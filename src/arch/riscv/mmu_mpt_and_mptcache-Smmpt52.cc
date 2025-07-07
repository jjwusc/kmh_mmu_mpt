#include "arch/riscv/mmu_mpt_and_mptcache-Smmpt52.hh"
//#include "base/logging.hh"         // 若用 panic 等
//#include "arch/riscv/mmu.hh"       // BaseMMU::Mode
//#include "arch/riscv/pma_checker.hh"
//#include "arch/riscv/pmp.hh"
//#include "cpu/thread_context.hh"
//#include "mem/request.hh"
#include "arch/riscv/tlb.hh"



namespace gem5 {
namespace RiscvISA {
	
#if MPT_ENABLED
// 获取当前层级的“单页大小”    运行时获取页大小, 普通的全局 helper 函数，不是属于某个类或结构体的成员函数，放在命名空间外部
uint64_t getPageSizeForLevel(int level) {
    switch (level) {
        case 0: return MPT_LEAF_L0_PAGE_SIZE;
        case 1: return MPT_LEAF_L1_PAGE_SIZE;
        case 2: return MPT_LEAF_L2_PAGE_SIZE;
        case 3: return MPT_LEAF_L3_PAGE_SIZE;
        default: return 0;//or panic
    }
}

// 获取当前层级的 MPTE 区域大小（16 个页）
uint64_t getRegionSizeForLevel(int level) {
    return MPT_NUM_PERMS * getPageSizeForLevel(level);
}

uint8_t log2floor(uint64_t x) {
    uint8_t r = 0;
    while (x >>= 1) ++r;
    return r;
}





MPTE52::MPTE52() : raw(0) {}// 默认构造函数（无效项）

MPTE52::MPTE52(uint64_t val) : raw(val) {}// 用原始值构造

bool MPTE52::isValid() const { return raw & 0x1; } // 是否有效

bool MPTE52::isLeaf() const { return raw & 0x2; } // 是否为叶子

bool MPTE52::getN() const { return (raw >> 63) & 0x1; } // N 位（bit 63）

// 下一层页表的物理页号（非叶子时使用）
Addr MPTE52::nextLevelPPN() const {
    return (raw >> 10) & 0x000FFFFFFFFFFFFF; // bits 10~61
}

// 下一层页表物理地址（按 4KB 页对齐）
Addr MPTE52::nextLevelPAddr() const {
    return nextLevelPPN() << 12;   //2^12=4KB
}

// 获取第 pi 个页的权限（pi ∈ [0, 15]）
uint8_t MPTE52::perms(uint8_t pi) const {
    if (pi >= MPT_NUM_PERMS) return 0;
    return (raw >> (2 + pi * MPT_PERM_BITS_PER_ENTRY)) & MPT_PERM_MASK;//2是因为最后两位分别是valid和leaf
}

//命名空间级别的工具函数，不在 struct 里面
bool checkMPTEPermissions(const MPTE52 &mpte, BaseMMU::Mode mode, Addr range_offset, int level)
{
    if (!mpte.isValid() || !mpte.isLeaf())
        return false;

    // 当前层的页大小
    uint64_t pageSize = getPageSizeForLevel(level);         // e.g. 2MB for level=1
    uint8_t pi = (range_offset / pageSize) & 0xF;            // 选择第几个页的权限

    uint8_t perm = mpte.perms(pi);

    switch (mode) {
        case BaseMMU::Read:    return perm & MPT_PERM_R;
        case BaseMMU::Write:   return perm & MPT_PERM_W;
        case BaseMMU::Execute: return perm & MPT_PERM_X;
        default: return false;
    }
}




MPT::MPT() : nextPPN(0x10000) {
    rootPPN = buildSimulatedMPTTree();
}

MPTE52 MPT::simulateLeafAllowAll() const {
    uint64_t raw = 0;
    raw |= 0x1; // V
    raw |= 0x2; // L
    for (int i = 0; i < MPT_NUM_PERMS; ++i) {    // 设置16个权限段，每段3bit，为 0b111（R/W/X）
        raw |= ((uint64_t)(MPT_PERM_R | MPT_PERM_W | MPT_PERM_X) << (2 + i * MPT_PERM_BITS_PER_ENTRY));
    }
    return MPTE52(raw);
}

MPTE52 MPT::simulateNonLeaf(Addr nextLevelPPN) const {
    uint64_t raw = 0;
    raw |= 0x1; // V
    raw |= (nextLevelPPN & 0x000FFFFFFFFFFFFF) << 10;
    return MPTE52(raw);
}

Addr MPT::allocMPTPage(const std::vector<MPTE52>& entries) {
    Addr ppn = nextPPN++;
    Addr baseAddr = ppn << 12;
    for (size_t i = 0; i < entries.size(); ++i) {
        Addr addr = baseAddr + i * MPT_MPTE_SIZE;
        simulatedMPTMemory[addr] = entries[i];
    }
    return ppn;
}

Addr MPT::buildSimulatedMPTTree(int levels) {
    assert(levels >= 1 && levels <= MPT_LEVELS);
    std::vector<MPTE52> leafEntries(512, simulateLeafAllowAll());
    Addr leafPPN = allocMPTPage(leafEntries);
    Addr lowerPPN = leafPPN;
    for (int l = 1; l < levels; ++l) {
        std::vector<MPTE52> levelEntries(512);
        levelEntries[0] = simulateNonLeaf(lowerPPN);
        Addr currentPPN = allocMPTPage(levelEntries);
        lowerPPN = currentPPN;
    }
    return lowerPPN;
}

uint64_t MPT::readMPTE(Addr paddr, ThreadContext *tc, PMAChecker *pma, PMP *pmp, int &accessCounter) const {
    // ① 构造 Request
    RequestPtr req = std::make_shared<Request>(
        paddr,
        sizeof(MPTE52),
        Request::PHYSICAL,
        tc->getCpuPtr()->thread[tc->threadId()]->getMasterId()
    );

    // ② PMA 检查
    pma->check(req);

    // ③ PMP 检查
    PrivilegeMode pmode = getMemPriv(tc, BaseMMU::Read);
    gem5::Fault fault = pmp->pmpCheck(req, BaseMMU::Read, pmode, tc);

    if (fault != NoFault) {
        panic("PMP blocked access to MPTE at 0x%lx\n", paddr);
    }

    // ④ 模拟 memory 读取
    auto it = simulatedMPTMemory.find(paddr);
    if (it != simulatedMPTMemory.end()) {
        accessCounter++;  // 每次模拟访问memory都累加
        return it->second.raw;
    } else {
        return 0;
    }
}



// Smmp52 多级 MPT 遍历，根据虚拟地址返回 MPTE52（或无效项）
MPTE52 MPT::walk(Addr vaddr, ThreadContext *tc, PMAChecker *pma, PMP *pmp, int &accessCounter) const {
    Addr base = rootPPN << 12;  // 页表基地址 = PPN × 4KB（页表页固定为 4KB）

    for (int level = MPT_LEVELS - 1; level >= 0; --level) {
        // 每级使用9-bit 索引（512 项）
        size_t shift = level * 9 + 12;
        size_t index = (vaddr >> shift) & 0x1FF;
        Addr paddr = base + index * MPT_MPTE_SIZE;

        // 读取 MPTE 项
        uint64_t raw = readMPTE(paddr, tc, pma, pmp, accessCounter);
        MPTE52 mpte(raw);

        if (!mpte.isValid()) {
            return MPTE52(); // 无效项
        }

        if (mpte.isLeaf()) {
            // 找到叶子项，直接返回
            return mpte;
        }

        // 否则继续下一级
        base = mpte.nextLevelPAddr();
    }

    //未找到叶子，返回无效项
    return MPTE52();
}

//all miss, 127*4; L3 hit , else miss,  127*3.   L3 L2 hit , l1 l0 miss, 127*2.   L3 L2 L1 hit , l0 miss 127

//新增：异步延迟 walk 接口，127 cycle 后触发回调返回结果
void MPT::walkDelayed(Addr vaddr,
                      ThreadContext *tc,
                      PMAChecker *pma, PMP *pmp,
                      std::function<void(MPTE52)> callback) const
{
    int accessCounter = 0;
    MPTE52 result = walk(vaddr, tc, pma, pmp, accessCounter); // 使用同步接口立即生成结果（只模拟“等这么久才交结果”）

    Tick delay = accessCounter  * 127 * SimClock::Int::ns(); // 模拟127 cycle

    // 延迟调用 callback，让请求等127个周期才拿到结果
    tc->getCpuPtr()->schedule(
        new LambdaEvent([=]() {
            callback(result);
        }),
        curTick() + delay);
}


#endif // MPT_ENABLED





























#if MPT_ENABLED
MPT globalMPT;


  #if MPT_CACHE_ENABLED
  
	#include "params/RiscvTLB.hh"
	
	
	
MPTCache52::MPTCache52(size_t capL0, size_t capL1, size_t capL2, size_t capL3, size_t capSP)
    : capacityL0(capL0),
      capacityL1(capL1),
      capacityL2(capL2),
      capacityL3(capL3),
      capacitySP(capSP)
{}

MPTCache52::MPTCache52()
    : capacityL0(configuredSizeL0),
      capacityL1(configuredSizeL1),
      capacityL2(configuredSizeL2),
      capacityL3(configuredSizeL3),
      capacitySP(configuredSizeSP)
{}

void MPTCache52::configureSize(int sL0, int sL1, int sL2, int sL3, int sSP) {
    configuredSizeL0 = sL0;
    configuredSizeL1 = sL1;
    configuredSizeL2 = sL2;
    configuredSizeL3 = sL3;
    configuredSizeSP = sSP;
}

Addr MPTCache52::regionAlign(Addr pa, int level) const {
    return pa & ~(getRegionSizeForLevel(level) - 1);
}

// 非 const 版本：允许修改
std::unordered_map<Addr, MPTCacheEntry>& MPTCache52::getTableByLevel(int level) {
    if (level == 0) return tableL0;
    else if (level == 1) return tableL1;
    else if (level == 2) return tableL2;
    else if (level == 3) return tableL3;
    else return tableSP;
}

// const 版本：只读
const std::unordered_map<Addr, MPTCacheEntry>& MPTCache52::getTableByLevel(int level) const {
    if (level == 0) return tableL0;
    else if (level == 1) return tableL1;
    else if (level == 2) return tableL2;
    else if (level == 3) return tableL3;
    else return tableSP;
}

// 允许修改
size_t& MPTCache52::getCapacityByLevel(int level) {
    if (level == 0) return capacityL0;
    else if (level == 1) return capacityL1;
    else if (level == 2) return capacityL2;
    else if (level == 3) return capacityL3;
    else return capacitySP;
}

// 只读版本（如果需要在 const 函数中读取容量）
size_t MPTCache52::getCapacityByLevel(int level) const {
    if (level == 0) return capacityL0;
    else if (level == 1) return capacityL1;
    else if (level == 2) return capacityL2;
    else if (level == 3) return capacityL3;
    else return capacitySP;
}

	
	
	
//int MPTCache52::configuredSize = MPT_CACHE_SIZE;
	int MPTCache52::configuredSizeL0 = 0;
	int MPTCache52::configuredSizeL1 = 0;
	int MPTCache52::configuredSizeL2 = 0;
	int MPTCache52::configuredSizeL3 = 0;
	int MPTCache52::configuredSizeSP = 0;

	

// 用指针延迟构造 globalMPTCache
// tlb.cc中都得改成箭头->(而不是.)来使用globalMPTCache 了
MPTCache52* globalMPTCache = nullptr;

// 在 SimObject 初始化时调用，完成构造
void MPTCache52::initMPTCacheFromParams(const RiscvTLBParams *params)
{
    // MPTCache52::configureSize(params->mptcache_size);       // 设置静态变量
    // globalMPTCache = new MPTCache52();                       // 延迟构造，使用配置值

    MPTCache52::configureSize(
        params->mptcache_l0_size,
        params->mptcache_l1_size,
        params->mptcache_l2_size,
        params->mptcache_l3_size,
        params->mptcache_sp_size
    );
    globalMPTCache = new MPTCache52();  // 默认构造函数会用上面这 5 个静态值

    //"在 gem5 中像这样用于全局单例（global singleton）的 new，不需要手动释放（不需要 delete）"
    DPRINTF(TLB, "Initialized globalMPTCache with size = %d\n", params->mptcache_size);
}

	  
	 
	/*	这样不行，没法做到在初始化时就用到py传来的参数
	int runtimeMPTCacheSize //= MPT_CACHE_SIZE;

	void initMPTCacheFromParams(const RiscvTLBParams *params)
	{
		runtimeMPTCacheSize = params->mptcache_size;
	}
	
	MPTCache52 globalMPTCache(runtimeMPTCacheSize);
	*/
	
	
	
	
void MPTCache52::fetchDelayed(
    Addr pa,
    int level,
    const MPT &mpt,
    ThreadContext *tc,
    PMAChecker *pma, PMP *pmp,
    std::function<void(bool /*hit*/, MPTCacheEntry)> callback) const
    //callback是一个函数指针的封装，类型是：std::function<void(bool, MPTCacheEntry)>
{
    Addr aligned = regionAlign(pa, level);
    auto& table = getTableByLevel(level);
    auto it = table.find(aligned);

    if (it != table.end() && it->second.valid) {
        // 命中：直接 10 cycle 延迟
        Tick delay = 10 * SimClock::Int::ns();
        MPTCacheEntry entry = it->second;

        // 统计命中
        //++globalMPT->mptCacheL1Misses;
        if (level == 0) ++globalMPT->mptCacheL0Hits;
        else if (level == 1) ++globalMPT->mptCacheL1Hits;
        else if (level == 2) ++globalMPT->mptCacheL2Hits;
        else if (level == 3) ++globalMPT->mptCacheL3Hits;
        else ++globalMPT->mptCacheSPHits;

        tc->getCpuPtr()->schedule(
            new LambdaEvent([=]() {
                callback(true, entry);// true表示命中
            }),
            curTick() + delay);
    } else {
        // 未命中：调用 mpt.walkDelayed() 模拟完整页表访问延迟
        mpt.walkDelayed(pa, tc, pma, pmp, 
            [=](MPTE52 mpte) {
                if (!mpte.isValid()) {
                    callback(false, {});
                    return;
                }

                // 插入缓存
                auto& table_mut = this->getTableByLevel(level);
                size_t& cap = this->getCapacityByLevel(level);

                if (table_mut.size() >= cap) {
                    auto randomIt = std::next(table_mut.begin(), rand() % table_mut.size());
                    table_mut.erase(randomIt);
                }
/*
                auto& table_mut = const_cast<MPTCache52*>(this)->getTableByLevel(level);
                size_t& cap = const_cast<MPTCache52*>(this)->getCapacityByLevel(level);
                if (table_mut.size() >= cap) {
                    auto randomIt = std::next(table_mut.begin(), rand() % table_mut.size());
                    table_mut.erase(randomIt);
                }
*/
                MPTCacheEntry entry = {
                    aligned, mpte, true, level, log2floor(getRegionSizeForLevel(level))
                };
                table_mut[aligned] = entry;

                // 统计未命中
                if (level == 0) ++globalMPT->mptCacheL0Misses;
                else if (level == 1) ++globalMPT->mptCacheL1Misses;
                else if (level == 2) ++globalMPT->mptCacheL2Misses;
                else if (level == 3) ++globalMPT->mptCacheL3Misses;
                else ++globalMPT->mptCacheSPMisses;

                callback(false, entry);
            }
        );
    }
}


	
	
	
  #endif//MPT_CACHE_ENABLED

#endif//MPT_ENABLED



} // namespace RiscvISA
} // namespace gem5