/*
 * Copyright (c) 2002-2005 The Regents of The University of Michigan
 * Copyright (c) 2007 MIPS Technologies, Inc.
 * Copyright (c) 2020 Barkhausen Institut
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef __ARCH_RISCV_PAGETABLE_H__
#define __ARCH_RISCV_PAGETABLE_H__

#include "base/bitunion.hh"
#include "base/logging.hh"
#include "base/trie.hh"
#include "base/types.hh"
#include "sim/serialize.hh"


namespace gem5
{

namespace RiscvISA {
	
// JJW: 

#include "arch/riscv/mmu_mpt_and_mptcache-Smmpt52.hh"
#include <type_traits>

#ifndef MPT_ENABLED
#define MPT_ENABLED 1
#endif


#if MPT_ENABLED 
inline int getPageShiftForLevel(int level)
{
	// 返回每个层级的页大小对应的 log2 值
    switch (level) {
        case 0: return 12; // log2(4KB)
        case 1: return 21; // log2(2MB)
        case 2: return 30; // log2(1GB)
        case 3: return 39; // log2(512GB)
        default: panic("Invalid MPT level: %d", level);
    }
}



//union类型写法

BitUnion32(MPTInfoRaw)
    Bitfield<0>        valid;
    Bitfield<1>        perm_r;
    Bitfield<2>        perm_w;
    Bitfield<3>        perm_x;
    Bitfield<4>        napot;           // N 位，napot 模式
    Bitfield<11, 6>    mptLogBytes;     // 6位最大63, 支持 log2(64EB) =48, 覆盖所有 napot 层级
    Bitfield<31, 12>   reserved;        // 剩余 bits 是保留位
EndBitUnion(MPTInfoRaw)

struct MPTInfoInTLB {
    MPTInfoRaw raw;

    MPTInfoInTLB() { raw = 0; }

    bool mptinfoTrust(uint8_t tlbLogBytes) const {
        return raw.valid() && (raw.mptLogBytes() >= tlbLogBytes);
    }

    static MPTInfoInTLB fromEntry(const MPTCacheEntry &entry, Addr rangeOffset) {
        // 根据 napot 与否判断是否使用 pi（子页索引）
        uint8_t pi = (rangeOffset >> getPageShiftForLevel(entry.level)) & 0xF;
        uint8_t perm = entry.mpte.perms(entry.mpte.getN() ? 0 : pi);

        MPTInfoInTLB info;
        info.raw = 0;
        info.raw.valid(entry.valid);
        info.raw.perm_r((perm & MPT_PERM_R) != 0);
        info.raw.perm_w((perm & MPT_PERM_W) != 0);
        info.raw.perm_x((perm & MPT_PERM_X) != 0);
        info.raw.napot(entry.mpte.getN());

        // 不再重复推导，直接使用已有值(在 mpt.cc 中或 tlb.cc 中已近分别考虑过 N 位开启后的等效 size)
        info.raw.mptLogBytes(entry.log2RegionSize);

        return info;
    }
};





/*uint32_t写法


//存入 TLB 的 MPT 相关信息（权限 + 粒度）
struct MPTInfoInTLB
{
    uint32_t valid         : 1;   // bit 0
    uint32_t perm_r        : 1;   // bit 1
    uint32_t perm_w        : 1;   // bit 2
    uint32_t perm_x        : 1;   // bit 3
    uint32_t napot         : 1;   // bit 4, 表示是否 napot 模式
    uint32_t mptLogBytes   : 6;   // bit 5~10，权限粒度 log2 区域大小    MPT 粒度，log2(region size)//6位最多为63. 2^63 ≈ 8EB  2^31  = 2GB. 6位足够
    uint32_t reserved      : 21;  // bit 11~31，保留


	
	// 默认构造（无效）
    //MPTInfoInTLB()
    //    : valid(0), perm_r(0), perm_w(0), perm_x(0),
    //      mptLogBytes(0), reserved(0) {}
	
	
    MPTInfoInTLB() = default;// 让编译器自动生成默认构造函数（支持noexcept推导）

    // 判断当前 TLB 项是否可用// 信任判断：mpt粒度是否 ≥ TLB 粒度
    bool mptinfoTrust(uint8_t tlbLogBytes) const {
        return this->valid && (this->mptLogBytes >= tlbLogBytes);
    }

	 // 静态函数，直接从一个 entry 构造出 info
	//调用方法：MPTInfoInTLB info = MPTInfoInTLB::fromEntry(entry, offset);
    static MPTInfoInTLB fromEntry(const MPTCacheEntry &entry, Addr rangeOffset) {
        uint8_t pi = (rangeOffset >> getPageShiftForLevel(entry.level)) & 0xF;
        uint8_t perm = entry.mpte.perms(entry.mpte.getN() ? 0 : pi);

        MPTInfoInTLB info;
        info.valid = entry.valid;
        info.perm_r = (perm & MPT_PERM_R) != 0;
        info.perm_w = (perm & MPT_PERM_W) != 0;
        info.perm_x = (perm & MPT_PERM_X) != 0;
        info.napot  = entry.mpte.getN();                    // N 位直接写入
        info.mptLogBytes = entry.log2RegionSize;            // 等效区域 log2 值
        info.reserved = 0;
        return info;
    }
};


*/





#endif //MPT_ENABLED










BitUnion64(SATP)
    Bitfield<63, 60> mode;
    Bitfield<59, 44> asid;
    Bitfield<43, 0> ppn;
EndBitUnion(SATP)

enum AddrXlateMode
{
    BARE = 0,
    SV39 = 8,
    SV48 = 9,
};

const Addr H_VADDR_BITS = 41;
// Sv39 paging
const Addr VADDR_BITS  = 39;
const Addr LEVEL_BITS  = 9;
const Addr LEVEL_MASK = ((1 << LEVEL_BITS) - 1);
const Addr PGMASK = ((1 << 12) - 1);
const Addr TWO_STAGE_L2_LEVEL_MASK = 0x7ff;
const Addr VPN_MASK = 0x1ff;
const Addr PGSHFT = 12;
const Addr PTESIZE = 8;
const Addr L2PageTypeNum = 4;
const Addr L2PageStoreTypeNum = 5;

const Addr L2TLB_BLK_OFFSET = 3;
const Addr VADDR_CHOOSE_MASK = 7;
const Addr l2tlbLineSize = 8;

const Addr preHitOnHitLNum = 500;
const double preHitOnHitPrecision = 0.08;
const double nextlinePrecision = 0.09;

const int L2L1CheckLevel = 2;
const int L2L2CheckLevel = 1;
const int L2L3CheckLevel = 0;


// L2L1 :L2TLB L1Page
// L2L2 :L2TLB L2Page
// L2L3 :L2TLB L3Page
// L2sp1 :L2TLB L1Page(leaf)
// L2sp2 :L2TLB L2Page(leaf)
enum l2TLBPage
{
    L_L2L1 =1,
    L_L2L2 ,
    L_L2L3 ,
    L_L2sp1,
    L_L2sp2

};
enum HTLBHitState
{
    H_L1miss = 0,
    h_l1AllstageHit,
    h_l1VSstageHit,
    h_l1GstageHit,
    h_l2VSstageHitEnd,
    h_l2VSstageHitContinue,
    h_l2GstageHitEnd,
    h_l2GstageHitContinue
};

enum TlbTranslateMode { direct = 0, vsstage, gstage, allstage };

enum TranslateMode
{
    defaultmode = 0,
    twoStageMode = 1

};

enum MMUMode { MMU_DIRECT = 0, MMU_TRANSLATE = 1, MMU_DYNAMIC = 2 };

BitUnion64(PTESv39)
    Bitfield<53, 10> ppn;
    Bitfield<53, 28> ppn2;
    Bitfield<27, 19> ppn1;
    Bitfield<18, 10> ppn0;
    Bitfield<7> d;
    Bitfield<6> a;
    Bitfield<5> g;
    Bitfield<4> u;
    Bitfield<3, 1> perm;
    Bitfield<3> x;
    Bitfield<2> w;
    Bitfield<1> r;
    Bitfield<0> v;
EndBitUnion(PTESv39)


BitUnion64(PTESv48)
    Bitfield<53, 10> ppn;
    Bitfield<53, 37> ppn3;
    Bitfield<36, 28> ppn2;
    Bitfield<27, 19> ppn1;
    Bitfield<18, 10> ppn0;
    Bitfield<7> d;
    Bitfield<6> a;
    Bitfield<5> g;
    Bitfield<4> u;
    Bitfield<3, 1> perm;
    Bitfield<3> x;
    Bitfield<2> w;
    Bitfield<1> r;
    Bitfield<0> v;
EndBitUnion(PTESv48)


BitUnion64(PTE)
    Bitfield<53, 10> ppn;
    Bitfield<7> d;
    Bitfield<6> a;
    Bitfield<5> g;
    Bitfield<4> u;
    Bitfield<3, 1> perm;
    Bitfield<3> x;
    Bitfield<2> w;
    Bitfield<1> r;
    Bitfield<0> v;
EndBitUnion(PTE)

struct TlbEntry;
//struct L2TlbEntry;
typedef Trie<Addr, TlbEntry> TlbEntryTrie;
//typedef Trie<Addr, L2TlbEntry> L2TlbEntryTrie;

struct TlbEntry : public Serializable
{
    // The base of the physical page.
    Addr paddr;

    // The beginning of the virtual page this entry maps.
    Addr vaddr;
    Addr gpaddr;
    // The size of the page this represents, in address bits.
    unsigned logBytes;
    //transalte mode
    //0:direct 1:vsstage 2:gstage 3:allstage
    uint8_t translateMode;
    //vsatp.asid or satp.asid
    uint16_t asid;
    // hgatp.vmid
    uint16_t vmid;

    PTESv39 pte;
    PTESv39 pteVS;

    TlbEntryTrie::Handle trieHandle;

    // A sequence number to keep track of LRU.
    uint64_t lruSeq;

    uint64_t level;
    uint64_t VSlevel;

    Addr index;

    bool isSquashed;

    bool used;
    bool isPre;
    bool fromForwardPreReq;
    bool fromBackPreReq;
    bool preSign;

	// New: 
#if MPT_ENABLED
    MPTInfoInTLB mptInfo;// JJW
#endif


    TlbEntry()
        : paddr(0),
          vaddr(0),
          gpaddr(0),
          logBytes(0),
          translateMode(0),
          asid(0),
          vmid(0),
          pte(),
          pteVS(),
          lruSeq(0),
          level(0),
          VSlevel(0),
          index(0),
          isSquashed(false),
          used(false),
          isPre(false),
          fromForwardPreReq(false),
          fromBackPreReq(false),
          preSign(false)
#if MPT_ENABLED
        , mptInfo()
#endif    //JJW
    {
    }

    // Return the page size in bytes
    Addr size() const
    {
        return (static_cast<Addr>(1) << logBytes);
    }

    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_PAGETABLE_H__


