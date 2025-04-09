#include "arch/riscv/mmu_mpt_and_mptcache-Smmpt52.hh"

#if MPT_ENABLED
MPT globalMPT;

  #if MPT_CACHE_ENABLED
  
	#include "params/RiscvTLB.hh"
	
	int MPTCache52::configuredSize = MPT_CACHE_SIZE;
	
	void MPTCache52::configureSize(int s) {
		configuredSize = s;
	}
	
	MPTCache52* globalMPTCache = nullptr;//用指针延迟构造 globalMPTCache//tlb.cc中都得改成->来使用globalMPTCache 了
	
	//在 SimObject 初始化时调用，完成构造
	  void initMPTCacheFromParams(const RiscvTLBParams *params)
	  {
		  MPTCache52::configureSize(params->mptcache_size);       // 设置静态变量
		  globalMPTCache = new MPTCache52();                       // 延迟构造，使用配置值
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



