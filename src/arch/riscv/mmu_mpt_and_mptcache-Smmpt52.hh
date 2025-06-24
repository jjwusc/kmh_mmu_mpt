#ifndef __ARCH_RISCV_MMU_MPT_AND_CACHE_SMMPT52_HH__
#define __ARCH_RISCV_MMU_MPT_AND_CACHE_SMMPT52_HH__

#include <unordered_map>
#include "arch/riscv/pma_checker.hh"
#include "arch/riscv/pmp.hh"
#include <vector>
#include <optional>
#include <cstdint>
#include <functional>
#include <cassert>
#include "params/RiscvTLB.hh"
#include "sim/sim_object.hh"
#include "arch/riscv/utility.hh"

// 是否启用 MPT（默认启用，使用 -D__ARCH_RISCV_MMU_MPT_HH__ 禁用）																			 
#ifndef __ARCH_RISCV_MMU_MPT_HH__
#define MPT_ENABLED 1
#include "sim/stat_control.hh" // 如果需要统计
#else
#define MPT_ENABLED 0
#endif

// 是否启用 MPT Cache（默认启用，使用 -D__ARCH_RISCV_MMU_MPT_CACHE_HH__ 禁用），前提是 MPT 启用																													
#if MPT_ENABLED && !defined(__ARCH_RISCV_MMU_MPT_CACHE_HH__)
#define MPT_CACHE_ENABLED 1
#include "params/RiscvTLB.hh" //JJW2
#else
#define MPT_CACHE_ENABLED 0
#endif


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////



// -----------------------------------------------------------------------------------------------------------------------------------------------------------
//General Info:
//一共有4个结构体，位于#if MPT_ENABLED - #endif 内部的有struct MPTE52和struct MPT。
//                 位于#if MPT_CACHE_ENABLED - #endif 内部的有struct MPTCacheEntry和class MPTCache52
//"A leaf MPTE provides access type permissions for sixteen pages in the address range determined by that MPTE."   -----manual
//编译时要加的信息有：是否启用mpt，是否启用mptcache，mptcache的size是多少
// -----------------------------------------------------------------------------------------------------------------------------------------------------------


#if MPT_ENABLED 


// -----------------------------
// MPT 权限位定义
// -----------------------------
#define MPT_PERM_R  (1 << 0)
#define MPT_PERM_W  (1 << 1)
#define MPT_PERM_X  (1 << 2)


// -----------------------------
// Smmp52 相关参数定义
// -----------------------------
#define MPT_LEVELS 4                 // Smmp52 使用 4 级页表（L3 → L0）
#define MPT_MPTE_SIZE 8              // 每个 MPTE 占 8 字节
#define MPT_NUM_PERMS 16             // 每个 MPTE 控制 16 个子页权限
#define MPT_PERM_BITS_PER_ENTRY 3    // 每个子页权限占 3 bit
#define MPT_PERM_MASK 0x7            // 低 3 位掩码

// 各层粒度：单位页大小（注意：不是 MPTE 粒度，是“单页粒度”）
#define MPT_LEAF_L0_PAGE_SIZE   (1UL << 12) // 4KB
#define MPT_LEAF_L1_PAGE_SIZE   (1UL << 21) // 2MB
#define MPT_LEAF_L2_PAGE_SIZE   (1UL << 30) // 1GB
#define MPT_LEAF_L3_PAGE_SIZE   (1UL << 39) // 512GB

// 每个 MPTE 覆盖范围（单位：字节） = 16 × 单页粒度
#define MPT_REGION_SIZE_L0   (MPT_NUM_PERMS * MPT_LEAF_L0_PAGE_SIZE) // 64KB
#define MPT_REGION_SIZE_L1   (MPT_NUM_PERMS * MPT_LEAF_L1_PAGE_SIZE) // 32MB
#define MPT_REGION_SIZE_L2   (MPT_NUM_PERMS * MPT_LEAF_L2_PAGE_SIZE) // 16GB
#define MPT_REGION_SIZE_L3   (MPT_NUM_PERMS * MPT_LEAF_L3_PAGE_SIZE) // 8TB     //我们香山没有这样的需求，不过smmpt52的最大支持是这样，将来可拓展。

// -----------------------------
// 工具函数区：根据层级获取页大小和区域大小
// -----------------------------

// 获取当前层级的“单页大小”    运行时获取页大小, 普通的全局 helper 函数，不是属于某个类或结构体的成员函数，放在命名空间外部
inline uint64_t getPageSizeForLevel(int level) {
    switch (level) {
        case 0: return MPT_LEAF_L0_PAGE_SIZE;
        case 1: return MPT_LEAF_L1_PAGE_SIZE;
        case 2: return MPT_LEAF_L2_PAGE_SIZE;
        case 3: return MPT_LEAF_L3_PAGE_SIZE;
        default: return 0; // or panic
    }
}

// 获取当前层级的 MPTE 区域大小（16 个页）
inline uint64_t getRegionSizeForLevel(int level) {
    return MPT_NUM_PERMS * getPageSizeForLevel(level);
}

inline uint8_t log2floor(uint64_t x) {
    uint8_t r = 0;
    while (x >>= 1) ++r;
    return r;
}

/////////////////////////////////////////////////////////////////////////////////////////////////


struct MPTE52 {
    uint64_t raw;

    MPTE52() : raw(0) {}// 默认构造函数（无效项）
    MPTE52(uint64_t val) : raw(val) {}// 用原始值构造
    bool isValid() const {       return raw & 0x1;    }// 是否有效
    bool isLeaf() const {        return raw & 0x2;    }// 是否为叶子
    bool getN() const {        return (raw >> 63) & 0x1;    }// N 位（bit 63）

    // 下一层页表的物理页号（非叶子时使用）
    Addr nextLevelPPN() const {
        return (raw >> 10) & 0x000FFFFFFFFFFFFF; // bits 10~61
    }

    // 下一层页表物理地址（按 4KB 页对齐）
    Addr nextLevelPAddr() const {
        return nextLevelPPN() << 12;   //2^12=4KB
    }

    // 获取第 pi 个页的权限（pi ∈ [0, 15]）
    uint8_t perms(uint8_t pi) const {
        if (pi >= MPT_NUM_PERMS) return 0;
        return (raw >> (2 + pi * MPT_PERM_BITS_PER_ENTRY)) & MPT_PERM_MASK;//2是因为最后两位分别是valid和leaf
    }
};


//命名空间级别的工具函数，不在 struct 里面
inline bool checkMPTEPermissions(const MPTE52 &mpte, BaseMMU::Mode mode, Addr range_offset, int level)
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




struct MPT {
    Addr rootPPN; // 根页表物理页号（页号单位）
/*//TODO：寄存器中获得The physical page number of the root memory protection table is stored in the mmpt register’s PPN field
    // 模拟 memory 访问，从物理地址加载一个 64-bit MPTE
    uint64_t readMPTE(Addr paddr) const {
        // TODO: 替换为 GEM5 中访问 memory 的实际方法//一般通过 port->read(...) 或某种 memory interface 读取内存
        //panic("readMPTE not implemented!");
        return 0;
    }
*/

//override:
    std::unordered_map<Addr, MPTE52> simulatedMPTMemory;
    Addr nextPPN;

    MPT() : nextPPN(0x10000) {
        rootPPN = buildSimulatedMPTTree();
    }

    MPTE52 simulateLeafAllowAll() const {
        uint64_t raw = 0;
        raw |= 0x1; // V
        raw |= 0x2; // L
        for (int i = 0; i < MPT_NUM_PERMS; ++i) {    // 设置16个权限段，每段3bit，为 0b111（R/W/X）

            raw |= ((uint64_t)(MPT_PERM_R | MPT_PERM_W | MPT_PERM_X) << (2 + i * MPT_PERM_BITS_PER_ENTRY));
        }
        return MPTE52(raw);
    }

    MPTE52 simulateNonLeaf(Addr nextLevelPPN) const {
        uint64_t raw = 0;
        raw |= 0x1; // V
        raw |= (nextLevelPPN & 0x000FFFFFFFFFFFFF) << 10;
        return MPTE52(raw);
    }

    Addr allocMPTPage(const std::vector<MPTE52>& entries) {
        Addr ppn = nextPPN++;
        Addr baseAddr = ppn << 12;
        for (size_t i = 0; i < entries.size(); ++i) {    

            Addr addr = baseAddr + i * MPT_MPTE_SIZE;
            simulatedMPTMemory[addr] = entries[i];
        }
        return ppn;
    }

    Addr buildSimulatedMPTTree(int levels = MPT_LEVELS) {
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
	uint64_t readMPTE(Addr paddr, ThreadContext *tc, PMAChecker *pma, PMP *pmp, int &accessCounter) const {
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
		Fault fault = pmp->pmpCheck(req, BaseMMU::Read, pmode, tc);

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
	
	MPTE52 walk(Addr vaddr, ThreadContext *tc, PMAChecker *pma, PMP *pmp, int &accessCounter) const {
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
	void walkDelayed(Addr vaddr,
                 ThreadContext *tc,
                 PMAChecker *pma, PMP *pmp,
                 std::function<void(MPTE52)> callback) const		 
    {
		    int accessCounter = 0;
			MPTE52 result = walk(vaddr, tc, pma, pmp, accessCounter);// 使用同步接口立即生成结果（只模拟“等这么久才交结果”）
			
        Tick delay = accessCounter  * 127 * SimClock::Int::ns(); // 模拟127 cycle

        // 延迟调用 callback，让请求等127个周期才拿到结果
        tc->getCpuPtr()->schedule(
            new LambdaEvent([=]() {
                callback(result);
            }),
            curTick() + delay);
    }
	
	
};



#endif // MPT_ENABLED




////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#if MPT_CACHE_ENABLED

#ifndef MPT_CACHE_SIZE
#define MPT_CACHE_SIZE 128    //MPT_CACHE_SIZE 默认为128. 需要在编译时自定义！
#endif

//extern int runtimeMPTCacheSize;//JJW2
//void initMPTCacheFromParams(const RiscvTLBParams *params);//JJW2

struct MPTCacheEntry {
    Addr tag;                  // region base（对齐后的地址）   目前这个tag用不上，用于查找的key是下面unordered map中的Addr，此处tag的用处为增加调试信息+以后扩展为set-ass时可用
    MPTE52 mpte;               // 缓存的 MPTE
    bool valid = false;

    //让 cache entry 自带粒度信息
    int level = -1;
    uint8_t log2RegionSize = 0;   //C++ 的 uint8_t 是8-bit
};



class MPTCache52 {
  private:
    size_t capacity;
	size_t capacityL0;
	size_t capacityL1;
	size_t capacityL2;
	size_t capacityL3;
	size_t capacitySP;

	static int configuredSize;//JJW2  用来存 param 传进来的值（类全局共享）
	
    std::unordered_map<Addr, MPTCacheEntry> table;
	
	std::unordered_map<Addr, MPTCacheEntry> tableL0;
	std::unordered_map<Addr, MPTCacheEntry> tableL1;
	std::unordered_map<Addr, MPTCacheEntry> tableL2;
	std::unordered_map<Addr, MPTCacheEntry> tableL3;
	std::unordered_map<Addr, MPTCacheEntry> tableSP;


    // 根据当前层级获取区域对齐地址（以 MPTE 粒度为单位）
    Addr regionAlign(Addr pa, int level) const {
        return pa & ~(getRegionSizeForLevel(level) - 1);
    }



		// MPTCache 分级命中统计项
		mutable Stats::Scalar mptCacheL0Hits;
		mutable Stats::Scalar mptCacheL1Hits;
		mutable Stats::Scalar mptCacheL2Hits;
		mutable Stats::Scalar mptCacheL3Hits;
		mutable Stats::Scalar mptCacheSPHits;

		// MPTCache 分级未命中统计项
		mutable Stats::Scalar mptCacheL0Misses;
		mutable Stats::Scalar mptCacheL1Misses;
		mutable Stats::Scalar mptCacheL2Misses;
		mutable Stats::Scalar mptCacheL3Misses;
		mutable Stats::Scalar mptCacheSPMisses;


	//void regStats(); // 不需要声明函数，在tlb.hh  tlb.cc中有对应的功能了




  public:

    MPTCache52(size_t capL0, size_t capL1, size_t capL2, size_t capL3, size_t capSP)
        : capacityL0(capL0),
          capacityL1(capL1),
          capacityL2(capL2),
          capacityL3(capL3),
          capacitySP(capSP)
    {}

	static int configuredSizeL0;
	static int configuredSizeL1;
	static int configuredSizeL2;
	static int configuredSizeL3;
	static int configuredSizeSP;
	
	MPTCache52()
    : capacityL0(configuredSizeL0),
      capacityL1(configuredSizeL1),
      capacityL2(configuredSizeL2),
      capacityL3(configuredSizeL3),
      capacitySP(configuredSizeSP)
		{}
		
	static void configureSize(int sL0, int sL1, int sL2, int sL3, int sSP) {
		configuredSizeL0 = sL0;
		configuredSizeL1 = sL1;
		configuredSizeL2 = sL2;
		configuredSizeL3 = sL3;
		configuredSizeSP = sSP;
	}
	
	void initMPTCacheFromParams(const RiscvTLBParams *params);

/* 
    //MPTCache52(size_t cap = MPT_CACHE_SIZE) : capacity(cap) {}
	MPTCache52(size_t cap) : capacity(cap) {}
	
	//JJW2 新增默认构造函数：从py参数传进来的静态值初始化
    MPTCache52() : capacity(configuredSize) {}
	static void configureSize(int s);
	void initMPTCacheFromParams(const RiscvTLBParams *params);

    // 查表：使用虚拟地址 + 层级作为对齐 key

		//lookup - 仅查找 MPTCache，若未命中不会触发 walk。
		//若需自动 walk 并插入，使用 fetch()。

    bool lookup(Addr pa, int level, MPTE52 &mpte) const {
        Addr aligned = regionAlign(pa, level);
        auto it = table.find(aligned);
        if (it != table.end() && it->second.valid) {
            mpte = it->second.mpte;
            return true;
        }
        return false;
    }

    // 插入：使用 mpte 覆盖的范围起始地址作为 key。  
	void insert(Addr pa, int level, const MPTE52 &mpte) {
		Addr aligned = regionAlign(pa, level);
		if (table.size() >= capacity) {
			// 随机选择一个要删除的 entry   。如果容量已满，随机删除一个现有 entry（无关位置，只为控制表大小，因为采用的实现方式是std::unordered map）
			auto randomIt = std::next(table.begin(), rand() % table.size());
			table.erase(randomIt);// 简单淘汰策略    //需要改为PLRU
		}
		table[aligned] = {
			aligned,
			mpte,
			true,
			level,
			log2floor(getRegionSizeForLevel(level))
		};
	}


    void invalidate(Addr pa, int level) {
        Addr aligned = regionAlign(pa, level);
        table.erase(aligned);
    }

    void clear() {
        table.clear();
    }
	
	
	bool fetch(Addr pa, int level, const MPT &mpt, MPTCacheEntry &entry) {
		Addr aligned = regionAlign(pa, level);
		auto it = table.find(aligned);
		if (it != table.end() && it->second.valid) {
			entry = it->second;
			
			
			
			return true;
		}

		// walk
		MPTE52 mpte = mpt.walk(pa);
		if (!mpte.isValid()) return false;

		// 构造 entry 并插入
		entry = {
			aligned,
			mpte,
			true,
			level,
			log2floor(getRegionSizeForLevel(level))
		};
		if (table.size() >= capacity) {//注：fetch函数为了避免再查一遍表，拿到之后就自动组装好了，然后自动将组装好的内联insert了，并没有调用insert函数
			auto randomIt = std::next(table.begin(), rand() % table.size());
			table.erase(randomIt);
		}
		table[aligned] = entry;
		
		

		return true;
	}
	
*/		
/* 	
	void fetchDelayed(
		Addr pa,
		int level,
		const MPT &mpt,
		ThreadContext *tc,
		std::function<void(bool , MPTCacheEntry)> callback) const
	{
		Addr aligned = regionAlign(pa, level);
		auto it = table.find(aligned);

		Tick delay;
		MPTCacheEntry entry;

		if (it != table.end() && it->second.valid) {
			// 命中：复制结果，模拟 10 cycle 延迟     //4
			delay = 10 * SimClock::Int::ns();
			entry = it->second;

			tc->getCpuPtr()->schedule(
				new LambdaEvent([=]() {
					callback(true, entry); // true表示命中
				}),
				curTick() + delay);
		} else {
			//未命中：需要走 MPT.walk()，模拟 127 cycle 延迟
			delay = 127 * SimClock::Int::ns();

			// 实际同步调用 walk（只是延迟结果交付）
			MPTE52 mpte = mpt.walk(pa);
			if (!mpte.isValid()) {
				// 即使无效，也要延迟后 callback → 表示失败
				tc->getCpuPtr()->schedule(
					new LambdaEvent([=]() {
						callback(false, {}); // false表示失败
					}),
					curTick() + delay);
				return;
			}

			// 构造 entry（直接插入缓存，等价于同步 fetch 做的事）
			entry = {
				aligned,
				mpte,
				true,
				level,
				log2floor(getRegionSizeForLevel(level))
			};

			// 为了保持 const 成员函数不动表（可以取消 const 后插入）
			const_cast<MPTCache52 *>(this)->insert(pa, level, mpte);

			// 延迟回调
			tc->getCpuPtr()->schedule(
				new LambdaEvent([=]() {
					callback(false, entry); // false 表示 miss 但成功 walk
				}),
				curTick() + delay);
		}
	}
*/	
 
 
	std::unordered_map<Addr, MPTCacheEntry>& getTableByLevel(int level) const {
		if (level == 0) return const_cast<MPTCache52*>(this)->tableL0;
		else if (level == 1) return const_cast<MPTCache52*>(this)->tableL1;
		else if (level == 2) return const_cast<MPTCache52*>(this)->tableL2;
		else if (level == 3) return const_cast<MPTCache52*>(this)->tableL3;
		else return const_cast<MPTCache52*>(this)->tableSP;
	}
	
	size_t& getCapacityByLevel(int level) {
		if (level == 0) return capacityL0;
		else if (level == 1) return capacityL1;
		else if (level == 2) return capacityL2;
		else if (level == 3) return capacityL3;
		else return capacitySP;
	}



	void fetchDelayed(
		Addr pa,
		int level,
		const MPT &mpt,
		ThreadContext *tc,
		PMAChecker *pma, PMP *pmp,
		std::function<void(bool /*hit*/, MPTCacheEntry)> callback) const
	{
		Addr aligned = regionAlign(pa, level);
		auto& table = getTableByLevel(level);
		auto it = table.find(aligned);

		if (it != table.end() && it->second.valid) {
			// 命中：直接 10 cycle 延迟
			Tick delay = 10 * SimClock::Int::ns();
			MPTCacheEntry entry = it->second;

			// 统计命中
			if (level == 0) ++mptCacheL0Hits;
			else if (level == 1) ++mptCacheL1Hits;
			else if (level == 2) ++mptCacheL2Hits;
			else if (level == 3) ++mptCacheL3Hits;
			else ++mptCacheSPHits;

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
					auto& table_mut = const_cast<MPTCache52*>(this)->getTableByLevel(level);
					size_t& cap = const_cast<MPTCache52*>(this)->getCapacityByLevel(level);
					if (table_mut.size() >= cap) {
						auto randomIt = std::next(table_mut.begin(), rand() % table_mut.size());
						table_mut.erase(randomIt);
					}
					MPTCacheEntry entry = {
						aligned, mpte, true, level, log2floor(getRegionSizeForLevel(level))
					};
					table_mut[aligned] = entry;

					// 统计未命中
					if (level == 0) ++mptCacheL0Misses;
					else if (level == 1) ++mptCacheL1Misses;
					else if (level == 2) ++mptCacheL2Misses;
					else if (level == 3) ++mptCacheL3Misses;
					else ++mptCacheSPMisses;

					callback(false, entry);
				}
			);
		}
	}


	
};

#endif // MPT_CACHE_ENABLED


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#endif // __ARCH_RISCV_MMU_MPT_AND_CACHE_SMMPT52_HH__