#include "arch/riscv/mmu_mpt_and_mptcache-Smmpt52.hh"
namespace gem5 {
namespace RiscvISA {
	
#if MPT_ENABLED
MPT globalMPT;

  #if MPT_CACHE_ENABLED
  
	#include "params/RiscvTLB.hh"
	
	//int MPTCache52::configuredSize = MPT_CACHE_SIZE;
	int MPTCache52::configuredSizeL0 = 0;
	int MPTCache52::configuredSizeL1 = 0;
	int MPTCache52::configuredSizeL2 = 0;
	int MPTCache52::configuredSizeL3 = 0;
	int MPTCache52::configuredSizeSP = 0;

	
	void MPTCache52::configureSize(int sL0, int sL1, int sL2, int sL3, int sSP)
{
    configuredSizeL0 = sL0;
    configuredSizeL1 = sL1;
    configuredSizeL2 = sL2;
    configuredSizeL3 = sL3;
    configuredSizeSP = sSP;
}

	
	MPTCache52* globalMPTCache = nullptr;//用指针延迟构造 globalMPTCache//tlb.cc中都得改成箭头->(而不是.)来使用globalMPTCache 了
	
	//在 SimObject 初始化时调用，完成构造
	  void initMPTCacheFromParams(const RiscvTLBParams *params)
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
	
	
	

	
	
	
  #endif

#endif



} // namespace RiscvISA
} // namespace gem5