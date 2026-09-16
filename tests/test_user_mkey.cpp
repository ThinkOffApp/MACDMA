#include "cx5_verbs.hpp"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main() {
    uint8_t wire[8192]; uint64_t pages[512];
    for (size_t i=0;i<512;++i) pages[i]=0x400000+4096*i;
    unsigned cases=0;
    unsigned offsets[]={0,1,4095}, counts[]={1,2,3,511,512};
    // Exercise exact unaligned bounds, permissions and odd-sized MTT padding.
    for (uint32_t access=0;access<16;++access)
        for (unsigned offset: offsets)
            for (unsigned count: counts) {
                const uint64_t length=uint64_t(count)*4096-offset;
                memset(wire,0xa5,sizeof(wire));
                auto bytes=cx5::create_user_mkey(wire,sizeof(wire),17,0x39,
                    0x100000000ull+offset,length,pages,count,access);
                const bool allowed=!(access&~7u) && (!(access&2) || (access&1));
                assert(bool(bytes)==allowed); ++cases;
                if (!allowed) { assert(wire[0]==0xa5); continue; }
                assert(bytes==0x110+((count+1)&~1u)*8);
                assert(cx5::get_bits(wire+16,64,0x12,1)==bool(access&2));
                assert(cx5::get_bits(wire+16,64,0x13,1)==bool(access&4));
                assert(cx5::get_bits(wire+16,64,0x14,1)==bool(access&1));
                assert(cx5::get_bits(wire+16,64,0x15,1)==1);
                assert(cx5::read_be64(wire+32)==0x100000000ull+offset);
                assert(cx5::read_be64(wire+40)==length);
                assert(cx5::get_bits(wire+16,64,0x1da,6)==12);
                if (count&1) assert(cx5::read_be64(wire+0x110+count*8)==0);
                assert(wire[bytes]==0xa5);
            }
    // Relaxed ordering: MKC bits 0xd (write) and 0x1d9 (read) only on request.
    assert(cx5::create_user_mkey(wire,sizeof(wire),17,1,0x100000000ull,4096,pages,1,7));
    assert(cx5::get_bits(wire+16,64,0xd,1)==0 && cx5::get_bits(wire+16,64,0x1d9,1)==0);
    assert(cx5::create_user_mkey(wire,sizeof(wire),17,1,0x100000000ull,4096,pages,1,7,12,true));
    assert(cx5::get_bits(wire+16,64,0xd,1)==1 && cx5::get_bits(wire+16,64,0x1d9,1)==1);
    assert(cx5::get_bits(wire+16,64,0x1da,6)==12 && cx5::get_bits(wire+16,64,0x12,4)==0xf);
    assert(!cx5::create_user_mkey(wire,sizeof(wire),17,1,UINT64_MAX-1,4096,pages,1,7));
    assert(!cx5::create_user_mkey(wire,sizeof(wire),17,1,0,4097,pages,1,7));
    assert(!cx5::create_user_mkey(wire,sizeof(wire),17,1,1,4096,pages,1,7));
    assert(!cx5::create_user_mkey(wire,sizeof(wire),17,1,0,4096,pages,2,7));
    assert(!cx5::create_user_mkey(wire,0x118,17,1,0,4096,pages,1,7));
    pages[0]|=1;
    assert(!cx5::create_user_mkey(wire,sizeof(wire),17,1,0,4096,pages,1,7));
    pages[0]&=~uint64_t(1);
    // Larger MKey pages: the list shrinks, alignment is checked at that size
    // and the range must still be covered exactly.
    {
        uint64_t big[4]={0x200000,0x400000,0xa00000,0xc00000};
        const uint64_t base=0x100000000ull+0x1234;
        assert(cx5::create_user_mkey(wire,sizeof(wire),17,0x39,base,4*0x200000-0x1234,big,4,7,21)==0x110+4*8);
        assert(cx5::get_bits(wire+16,64,0x1da,6)==21 && cx5::read_be32(wire+0x60)==2);
        assert(cx5::read_be64(wire+0x110+3*8)==0xc00000);
        assert(!cx5::create_user_mkey(wire,sizeof(wire),17,0x39,base,4*0x200000-0x1234,big,3,7,21));   // Too short a list.
        assert(!cx5::create_user_mkey(wire,sizeof(wire),17,0x39,base,4*0x200000-0x1234,big,4,7,22));   // 0x200000 is not 4 MiB aligned.
        assert(!cx5::create_user_mkey(wire,sizeof(wire),17,0x39,base,4096,big,1,7,11));
        assert(!cx5::create_user_mkey(wire,sizeof(wire),17,0x39,base,4096,big,1,7,31));
        assert(cx5::create_user_mkey(wire,sizeof(wire),17,0x39,base,4096,big,1,7,21)==0x110+2*8);
    }
    // Page-size selection from IOMMU segments.
    {
        uint64_t list[64];
        // One contiguous 64 MiB mapping whose device and process offsets agree
        // up to 16 MiB: four 16 MiB pages describe it.
        cx5::Segment one{0x1000000,0x90000000,64<<20};
        assert(cx5::choose_log_page(&one,1,0x1000000,64<<20,0x1000000,64)==24);
        assert(cx5::page_list(&one,1,0x1000000,64<<20,24,list,64)==4 && list[0]==0x90000000 && list[3]==0x93000000);
        // The same range at 4 KiB pages needs 16384 entries: too many.
        assert(cx5::page_list(&one,1,0x1000000,64<<20,12,list,64)==0);
        assert(cx5::choose_log_page(&one,1,0x1000000,64<<20,0x1000000,3)==0);
        // An HCA address that only shares the 16 KiB offset caps the page size.
        assert(cx5::choose_log_page(&one,1,0x1000000,64<<20,0x1004000,64)==0);
        assert(cx5::choose_log_page(&one,1,0x1000000,1<<20,0x1004000,64)==14);
        assert(cx5::page_list(&one,1,0x1000000,1<<20,14,list,64)==64 && list[63]==0x90000000+63*16384);
        // A range starting inside a page: the mapping begins at its first
        // 4 KiB page and the first large entry is the page below it.
        cx5::Segment inner{0x1001000,0x90001000,(64<<20)-0x1000};
        assert(cx5::choose_log_page(&inner,1,0x1000000+0x1234,8192,0x1000000+0x1234,64)==24);
        assert(cx5::page_list(&inner,1,0x1000000+0x1234,8192,24,list,64)==1 && list[0]==0x90000000);
        assert(cx5::page_list(&inner,1,0x1000000+0x1234,8192,12,list,64)==3 && list[0]==0x90001000 && list[2]==0x90003000);
        assert(cx5::choose_log_page(&one,1,0x1000000+0x1234,8192,0x1000000+0x1234,64)==0); // Wrong first page.
        // Two device-discontiguous pieces: the boundary must be page aligned,
        // and the device offset within the page must match on both.
        cx5::Segment two[2]={{0x1000000,0x90000000,0x200000},{0x1200000,0xa0000000,0x200000}};
        assert(cx5::choose_log_page(two,2,0x1000000,0x400000,0x1000000,64)==21);
        assert(cx5::page_list(two,2,0x1000000,0x400000,21,list,64)==2 && list[0]==0x90000000 && list[1]==0xa0000000);
        cx5::Segment skew[2]={{0x1000000,0x90000000,0x201000},{0x1201000,0xa0000000,0x1ff000}};
        assert(cx5::choose_log_page(skew,2,0x1000000,0x400000,0x1000000,2048)==12);
        cx5::Segment offset[2]={{0x1000000,0x90000000,0x200000},{0x1200000,0xa0001000,0x200000}};
        assert(cx5::choose_log_page(offset,2,0x1000000,0x400000,0x1000000,2048)==12); // Only 4 KiB pages fit a skewed device offset.
        // Malformed segment lists are refused.
        cx5::Segment gap[2]={{0x1000000,0x90000000,0x100000},{0x1200000,0xa0000000,0x200000}};
        assert(cx5::choose_log_page(gap,2,0x1000000,0x300000,0x1000000,64)==0);
        cx5::Segment shortlist{0x1000000,0x90000000,0x100000};
        assert(cx5::choose_log_page(&shortlist,1,0x1000000,0x200000,0x1000000,64)==0);
        cx5::Segment unaligned{0x1000000,0x90000800,0x200000};
        assert(cx5::choose_log_page(&unaligned,1,0x1000000,0x200000,0x1000000,64)==0);
        assert(cx5::choose_log_page(nullptr,0,0x1000000,0x200000,0x1000000,64)==0);
    }
    printf("PASS %u user MKey permission/alignment cases, large-page keys and page-size selection from IOMMU segments\n",cases);
}
