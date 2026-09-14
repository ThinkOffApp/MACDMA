#include "cx5_verbs.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace cx5;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
int main() {
    uint8_t wire[512]{}; uint64_t pages[4]={0x8000,0x9000,0xa000,0xb000};
    CHECK(create_mkey(wire,sizeof(wire),0x12345,0x37,0x100000000ull,16384,pages,4));
    CHECK(read_be32(wire)==0x02000000 && read_be32(wire+0x60)==2);
    CHECK(get_bits(wire+16,64,0x12,4)==15 && get_bits(wire+16,64,0x16,2)==1);
    CHECK(read_be32(wire+20)==0xffffff37 && (read_be32(wire+28)&0xffffff)==0x12345);
    CHECK(read_be64(wire+32)==0x100000000ull && read_be64(wire+40)==16384);
    CHECK(read_be64(wire+0x128)==pages[3]);
    CHECK(!create_mkey(wire,32,1,1,0x1000,1,pages,1));
    CHECK(!create_mkey(wire,sizeof(wire),0x1000000,1,0x1000,1,pages,1));
    pages[0]++; CHECK(!create_mkey(wire,sizeof(wire),1,1,0x1000,1,pages,1));
    CHECK(encode_wqe(wire,sizeof(wire),0x123456,0xffff,8,0x11223344,0x1234,4096,0x55667788,0x5678));
    CHECK(read_be32(wire)==0x00ffff08 && read_be32(wire+4)==0x12345603 && wire[11]==8);
    CHECK(read_be64(wire+16)==0x55667788 && read_be32(wire+24)==0x5678);
    CHECK(read_be32(wire+32)==4096 && read_be32(wire+36)==0x1234 && read_be64(wire+40)==0x11223344);
    CHECK(!encode_wqe(wire,sizeof(wire),1,0,0x10,1,1,1,UINT64_MAX,1));
    CHECK(!encode_wqe(wire,sizeof(wire),1,0,0x42,1,1,1,1,1));
    RCConnection rc{0x123456,0x456,0x789,0xabcdef,0x123456,0x654321,0x12345000,{}, {}};
    rc.remote_gid[0]=0xfe; rc.remote_gid[1]=0x80; rc.remote_gid[15]=0x23;
    const uint8_t peer_mac[6]={2,3,4,5,6,7}; memcpy(rc.remote_mac,peer_mac,6);
    CHECK(encode_rc_transition(wire,sizeof(wire),0x502,rc));
    CHECK(read_be32(wire)==0x05020000 && read_be32(wire+8)==rc.qpn);
    CHECK(read_be64(wire+24+0xa0)==rc.doorbell);
    CHECK(get_bits(wire+24,232,0x490,2)==3 && get_bits(wire+24,232,0x13,2)==3);
    CHECK(encode_rc_transition(wire,sizeof(wire),0x503,rc));
    CHECK(wire[48+5]==0); // Regression: InfiniBand GRH/MLID bits must be zero for RoCE.
    CHECK(memcmp(wire+64,rc.remote_gid,16)==0 && memcmp(wire+86,peer_mac,6)==0);
    CHECK(get_bits(wire+24,232,0xa8,24)==rc.remote_qpn && get_bits(wire+24,232,0x4a8,24)==rc.receive_psn);
    CHECK(get_bits(wire+24,232,0x28,24)==rc.pd && get_bits(wire+24,232,0x3e8,24)==rc.cq);
    CHECK(encode_rc_transition(wire,sizeof(wire),0x504,rc));
    CHECK(get_bits(wire+24,232,0x3c8,24)==rc.send_psn && get_bits(wire+48,44,0x40,5)==14);
    CHECK(!encode_rc_transition(wire,271,0x503,rc) && !encode_rc_transition(wire,512,0x505,rc));
    for (uint8_t mtu=1;mtu<=5;++mtu) {
        rc.path_mtu=mtu; CHECK(encode_rc_transition(wire,sizeof(wire),0x503,rc));
        CHECK(get_bits(wire+24,232,0x40,3)==mtu);
    }
    rc.path_mtu=6; CHECK(!encode_rc_transition(wire,sizeof(wire),0x503,rc));
    rc.path_mtu=0; CHECK(!encode_rc_transition(wire,sizeof(wire),0x503,rc));
    rc.path_mtu=3;
    rc.doorbell++; CHECK(!encode_rc_transition(wire,sizeof(wire),0x502,rc));
    Completion c{}; memset(wire,0,64); wire[63]=0xf1;
    CHECK(decode_cqe(wire,0,5,c)==CQResult::empty);
    wire[63]=0; write_be32(wire+56,0x123456); wire[60]=0xab; wire[61]=0xcd;
    CHECK(decode_cqe(wire,0,5,c)==CQResult::success && c.qpn==0x123456 && c.wqe_counter==0xabcd);
    CHECK(decode_cqe(wire,32,5,c)==CQResult::empty);
    wire[63]=1; CHECK(decode_cqe(wire,32,5,c)==CQResult::success);
    wire[63]=0xd1; wire[55]=5; wire[54]=7;
    CHECK(decode_cqe(wire,32,5,c)==CQResult::error && c.syndrome==5 && c.vendor_syndrome==7);
    memset(wire,0xa5,sizeof(wire)); uint8_t saved[512]; memcpy(saved,wire,512);
    CHECK(!set_bits(wire,512,4095,2,0) && memcmp(saved,wire,512)==0);
    CHECK(!set_bits(wire,512,0,3,8) && memcmp(saved,wire,512)==0);
    for (unsigned offset=0;offset<64;++offset) for (unsigned width=1;width<=64;++width) {
        uint64_t value=0x55aa0123456789abull;
        if (width<64) value&=(uint64_t(1)<<width)-1;
        CHECK(set_bits(wire,sizeof(wire),offset,width,value));
        CHECK(get_bits(wire,sizeof(wire),offset,width)==value);
    }
    std::printf("PASS %u memory-key/WQE/CQE boundary checks\n",checks);
}
