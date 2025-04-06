#include "arch/riscv/mmu_mpt_and_mptcache-Smmpt52.hh"

#if MPT_ENABLED
MPT globalMPT;

  #if MPT_CACHE_ENABLED
  MPTCache52 globalMPTCache;
  #endif

#endif