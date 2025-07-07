#ifndef __ARCH_RISCV_MMU_MPT_AND_CACHE_SMMPT52_HH__
#define __ARCH_RISCV_MMU_MPT_AND_CACHE_SMMPT52_HH__

#include <unordered_map>
#include "arch/riscv/isa.hh" //PrivilegeMode)，getMemPriv(tc, BaseMMU::Read);
#include <vector>
#include <optional>
#include <cstdint> //uint64_t （来自 stdint.h / cstdint 头文件）
#include <functional>
#include <cassert>
#include "cpu/base.hh"//cpu/thread_context.hh"只 forward declare 了 BaseCPU	，需要加 #include "cpu/base.hh"
//#include "arch/riscv/tlb.hh" //RiscvTLBParams SimObject 相关的 Params 类型，绝大多数都通过 include 相应模块的主类头文件自动引入。
#include "params/RiscvTLB.hh" //RiscvTLBParams
#include "sim/sim_object.hh"
#include "arch/riscv/utility.hh"
#include "base/types.hh"      // for Addr, uint64_t 等类型， 否则用不了!!    //typedef uint64_t Tick;
#include "arch/riscv/mmu.hh"  // for BaseMMU::Mode
#include "arch/riscv/pma_checker.hh"  // PMAChecker
#include "arch/riscv/pmp.hh"       // PMP
//#include "sim/thread_context.hh"   // ThreadContext //新版本 gem5（比如 22.x 之后，尤其是 RISCV 相关架构逐步完善以后），thread_context 已经统一放在 cpu/thread_context.hh。
#include "base/logging.hh"         // DPRINTF 等调试宏
#include "sim/serialize.hh"        // checkpoint 支持
#include "mem/request.hh"  //在 gem5 里，RequestPtr 是：using RequestPtr = std::shared_ptr<Request>;Request 这个类定义在：mem/request.hh
#include "arch/riscv/isa.hh"  //PrivilegeMode	
#include "sim/faults.hh"  //Fault	  fault不再riscvISA作用域，得用全名  gem5::Fault fault;
#include "sim/eventq.hh" //LambdaEvent
#include "cpu/thread_context.hh" //tc
#include "sim/core.hh"   //curTick()
#include "base/logging.hh" //宏	功能DPRINTF(...)	调试打印 (需要开启调试标志)panic(...)	触发严重错误中止运行warn(...)	警告信息inform(...)	普通信息输出
#include "base/statistics.hh" ////statistics::Scalar
#include "cpu/translation.hh" //translation  class DataTranslation : public BaseMMU::Translation

// 是否启用 MPT（默认启用，使用 -D__ARCH_RISCV_MMU_MPT_HH__ 禁用）																			 
//#ifndef __ARCH_RISCV_MMU_MPT_HH__
#define MPT_ENABLED 1
#include "sim/stat_control.hh" 
//#else
//#define MPT_ENABLED 0
//#endif

// 是否启用 MPT Cache（默认启用，使用 -D__ARCH_RISCV_MMU_MPT_CACHE_HH__ 禁用），前提是 MPT 启用																													
//#if MPT_ENABLED && !defined(__ARCH_RISCV_MMU_MPT_CACHE_HH__)
#define MPT_CACHE_ENABLED 1
#include "params/RiscvTLB.hh" //JJW2
//#else
//#define MPT_CACHE_ENABLED 0
//#endif


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace gem5 {
namespace RiscvISA {


// -----------------------------------------------------------------------------------------------------------------------------------------------------------
//General Info:
//一共有4个结构体，位于#if MPT_ENABLED - #endif 内部的有struct MPTE52和struct MPT。
//                 位于#if MPT_CACHE_ENABLED - #endif 内部的有struct MPTCacheEntry和class MPTCache52
//"A leaf MPTE provides access type permissions for sixteen pages in the address range determined by that MPTE."   -----manual
//编译时要加的信息有：是否启用mpt，是否启用mptcache，mptcache的size是多少
// -----------------------------------------------------------------------------------------------------------------------------------------------------------


#if MPT_ENABLED 
extern MPT globalMPT;

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
uint64_t getPageSizeForLevel(int level);
uint64_t getRegionSizeForLevel(int level);
uint8_t log2floor(uint64_t x);

/////////////////////////////////////////////////////////////////////////////////////////////////

// -----------------------------
// Smmp52 页表项 MPTE52 定义
// -----------------------------
struct MPTE52 {
    uint64_t raw;

    // 默认构造函数（无效项）
    MPTE52();

    // 用原始值构造
    MPTE52(uint64_t val);

    // 是否有效
    bool isValid() const;

    // 是否为叶子
    bool isLeaf() const;

    // N 位（bit 63）
    bool getN() const;

    // 下一层页表的物理页号（非叶子时使用）
    Addr nextLevelPPN() const;

    // 下一层页表物理地址（按 4KB 页对齐）
    Addr nextLevelPAddr() const;

    // 获取第 pi 个页的权限（pi ∈ [0, 15]）
    uint8_t perms(uint8_t pi) const;
};


// -----------------------------
// 工具函数：检查 MPTE 权限。命名空间级别工具函数声明,不在 struct 里面
// -----------------------------
// 根据访问模式（Read/Write/Exec）、偏移、层级等检查权限位
bool checkMPTEPermissions(const MPTE52 &mpte, BaseMMU::Mode mode, Addr range_offset, int level);









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

    MPT(); // 默认构造函数

    MPTE52 simulateLeafAllowAll() const;

    MPTE52 simulateNonLeaf(Addr nextLevelPPN) const;

    Addr allocMPTPage(const std::vector<MPTE52>& entries);

    Addr buildSimulatedMPTTree(int levels = MPT_LEVELS);

    uint64_t readMPTE(Addr paddr, ThreadContext *tc, PMAChecker *pma, PMP *pmp, int &accessCounter) const;


    // Smmp52 多级 MPT 遍历，根据虚拟地址返回 MPTE52（或无效项）
    MPTE52 walk(Addr vaddr, ThreadContext *tc, PMAChecker *pma, PMP *pmp, int &accessCounter) const;

    //all miss, 127*4; L3 hit , else miss,  127*3.   L3 L2 hit , l1 l0 miss, 127*2.   L3 L2 L1 hit , l0 miss 127

    //新增：异步延迟 walk 接口，127 cycle 后触发回调返回结果
    void walkDelayed(Addr vaddr,
                     ThreadContext *tc,
                     PMAChecker *pma, PMP *pmp,
                     std::function<void(MPTE52)> callback) const;

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
    Addr regionAlign(Addr pa, int level) const;

	// MPTCache 分级命中统计项
	mutable statistics::Scalar mptCacheL0Hits;
	mutable statistics::Scalar mptCacheL1Hits;
	mutable statistics::Scalar mptCacheL2Hits;
	mutable statistics::Scalar mptCacheL3Hits;
	mutable statistics::Scalar mptCacheSPHits;

	// MPTCache 分级未命中统计项
	mutable statistics::Scalar mptCacheL0Misses;
	mutable statistics::Scalar mptCacheL1Misses;
	mutable statistics::Scalar mptCacheL2Misses;
	mutable statistics::Scalar mptCacheL3Misses;
	mutable statistics::Scalar mptCacheSPMisses;

  public:
    MPTCache52(size_t capL0, size_t capL1, size_t capL2, size_t capL3, size_t capSP);
    MPTCache52();

	static int configuredSizeL0;
	static int configuredSizeL1;
	static int configuredSizeL2;
	static int configuredSizeL3;
	static int configuredSizeSP;
	
	static void configureSize(int sL0, int sL1, int sL2, int sL3, int sSP);

	void initMPTCacheFromParams(const RiscvTLBParams *params);

	// 非 const 版本：允许修改
	std::unordered_map<Addr, MPTCacheEntry>& getTableByLevel(int level);

	// const 版本：只读
	const std::unordered_map<Addr, MPTCacheEntry>& getTableByLevel(int level) const;

	// 允许修改
	size_t& getCapacityByLevel(int level);

	// 只读版本（如果需要在 const 函数中读取容量）
	size_t getCapacityByLevel(int level) const;


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
 
 




	void fetchDelayed(
        Addr pa,
        int level,
        const MPT &mpt,
        ThreadContext *tc,
        PMAChecker *pma, PMP *pmp,
        std::function<void(bool /*hit*/, MPTCacheEntry)> callback) const;
        //callback是一个函数指针的封装，类型是：std::function<void(bool, MPTCacheEntry)>


};

#endif // MPT_CACHE_ENABLED


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_MMU_MPT_AND_CACHE_SMMPT52_HH__