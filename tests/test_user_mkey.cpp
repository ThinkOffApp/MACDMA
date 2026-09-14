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
    assert(!cx5::create_user_mkey(wire,sizeof(wire),17,1,UINT64_MAX-1,4096,pages,1,7));
    assert(!cx5::create_user_mkey(wire,sizeof(wire),17,1,0,4097,pages,1,7));
    assert(!cx5::create_user_mkey(wire,sizeof(wire),17,1,1,4096,pages,1,7));
    assert(!cx5::create_user_mkey(wire,sizeof(wire),17,1,0,4096,pages,2,7));
    assert(!cx5::create_user_mkey(wire,0x118,17,1,0,4096,pages,1,7));
    pages[0]|=1;
    assert(!cx5::create_user_mkey(wire,sizeof(wire),17,1,0,4096,pages,1,7));
    printf("PASS %u user MKey permission/alignment cases plus six invalid bounds/mapping cases\n",cases);
}
