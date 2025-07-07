#include "arch/riscv/mmu.hh"
#include "arch/riscv/tlb.hh"
#include "arch/riscv/walker.hh"

namespace gem5
{
namespace RiscvISA {

MMU::MMU(const RiscvMMUParams &p)
    : BaseMMU(p), pma(p.pma_checker)
{}

TranslationGenPtr
MMU::translateFunctional(Addr start, Addr size, ThreadContext *tc,
                         Mode mode, Request::Flags flags)
{
    return TranslationGenPtr(new MMUTranslationGen(
        PageBytes, start, size, tc, this, mode, flags));
}

PrivilegeMode
MMU::getMemPriv(ThreadContext *tc, BaseMMU::Mode mode)
{
    return static_cast<TLB*>(dtb)->getMemPriv(tc, mode);
}

Walker*
MMU::getDataWalker()
{
    return static_cast<TLB*>(dtb)->getWalker();
}

PMP*
MMU::getPMP()
{
    return static_cast<TLB*>(dtb)->pmp;
}

void
MMU::setOldPriv(ThreadContext *tc)
{
    static_cast<TLB*>(dtb)->setOldPriv(tc);
    static_cast<TLB*>(itb)->setOldPriv(tc);
}

void
MMU::useNewPriv(ThreadContext *tc)
{
    static_cast<TLB*>(dtb)->useNewPriv(tc);
    static_cast<TLB*>(itb)->useNewPriv(tc);
}

void
MMU::takeOverFrom(BaseMMU *old_mmu)
{
    MMU *ommu = dynamic_cast<MMU*>(old_mmu);
    BaseMMU::takeOverFrom(ommu);
    pma->takeOverFrom(ommu->pma);
}

} // namespace RiscvISA
} // namespace gem5
