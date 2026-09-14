// The userspace WQE/receive encoders must produce exactly the bytes the kernel
// encoder produces, and the doorbell helpers must publish in the right places.
#include "cx5_verbs.hpp"
#include "cx5_user_post.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main() {
    static uint8_t page[MCDMA_QUEUE_PAGE_BYTES], uar[16384]; unsigned checks=0;
    const uint32_t producers[]={0,1,31,32,63,65535,65536,0x12345};
    const uint8_t opcodes[]={MCDMA_WQE_WRITE,MCDMA_WQE_READ,MCDMA_WQE_SEND};
    for (uint32_t producer:producers) for (uint8_t opcode:opcodes) {
        uint8_t expected[64];
        assert(cx5::encode_wqe(expected,64,0x123456,uint16_t(producer),opcode,0x100000000ull,7,4096,0x200000000ull,9));
        const uint64_t doorbell=mcdma_encode_send_wqe(page,0x123456,producer,opcode,0x100000000ull,7,4096,0x200000000ull,9);
        assert(doorbell && !memcmp(page+MCDMA_SQ_OFFSET+(producer&31u)*64,expected,64) && !memcmp(&doorbell,expected,8));
        ++checks;
    }
    // Rejections mirror the kernel encoder.
    assert(!mcdma_encode_send_wqe(page,0x123456,0,MCDMA_WQE_WRITE,0x1000,7,0,0x2000,9));
    assert(!mcdma_encode_send_wqe(page,0x123456,0,MCDMA_WQE_WRITE,0x1000,0,64,0x2000,9));
    assert(!mcdma_encode_send_wqe(page,0x123456,0,MCDMA_WQE_READ,0x1000,7,64,0x2000,0));
    assert(!mcdma_encode_send_wqe(page,0x123456,0,0x42,0x1000,7,64,0x2000,9));
    assert(!mcdma_encode_send_wqe(page,0x1000000,0,MCDMA_WQE_SEND,0x1000,7,64,0,0));
    assert(!mcdma_encode_send_wqe(page,0x123456,0,MCDMA_WQE_WRITE,UINT64_MAX,7,64,0x2000,9));
    assert(!mcdma_encode_send_wqe(nullptr,0x123456,0,MCDMA_WQE_WRITE,0x1000,7,64,0x2000,9));
    // Receive entries: the kernel writes length, lkey, address big-endian.
    assert(mcdma_encode_recv_wqe(page,33,0x300000000ull,4096,11));
    const uint8_t *entry=page+MCDMA_RQ_OFFSET+(33&31u)*16;
    assert(cx5::read_be32(entry)==4096 && cx5::read_be32(entry+4)==11 && cx5::read_be64(entry+8)==0x300000000ull);
    assert(!mcdma_encode_recv_wqe(page,0,0x1000,0,11) && !mcdma_encode_recv_wqe(page,0,0x1000,64,0) &&
           !mcdma_encode_recv_wqe(page,0,UINT64_MAX,64,11));
    // Doorbell record and UAR register writes.
    const uint64_t doorbell=mcdma_encode_send_wqe(page,0x123456,5,MCDMA_WQE_WRITE,0x1000,7,64,0x2000,9);
    mcdma_ring_send(page,uar,6,doorbell);
    assert(cx5::read_be32(page+MCDMA_DBR_OFFSET+4)==6 && !memcmp(uar+MCDMA_UAR_DOORBELL,&doorbell,8));
    for (unsigned i=0;i<16384;++i) if (i<MCDMA_UAR_DOORBELL || i>=MCDMA_UAR_DOORBELL+8) assert(!uar[i]);
    mcdma_publish_recv(page,34);
    assert(cx5::read_be32(page+MCDMA_DBR_OFFSET)==34 && cx5::read_be32(page+MCDMA_DBR_OFFSET+4)==6);
    // Write-combined doorbell: same bytes, same places.
    memset(uar,0,sizeof(uar));
    mcdma_ring_send_wc(page,uar,7,doorbell);
    assert(cx5::read_be32(page+MCDMA_DBR_OFFSET+4)==7 && !memcmp(uar+MCDMA_UAR_DOORBELL,&doorbell,8));
    for (unsigned i=0;i<16384;++i) if (i<MCDMA_UAR_DOORBELL || i>=MCDMA_UAR_DOORBELL+8) assert(!uar[i]);
    // Capability block: beyond the queues and the doorbell record, valid only
    // with the magic, and a bank size only for a write-combined page.
    static_assert(MCDMA_INFO_OFFSET>=MCDMA_DBR_OFFSET+8 && MCDMA_INFO_OFFSET+32<=MCDMA_QUEUE_PAGE_BYTES);
    uint32_t flags=1,bank=1;
    assert(!mcdma_info_read(page,&flags,&bank));
    mcdma_info_write(page,MCDMA_INFO_KERNEL_BLUEFLAME,256);
    assert(mcdma_info_read(page,&flags,&bank) && flags==MCDMA_INFO_KERNEL_BLUEFLAME && bank==0);
    mcdma_info_write(page,MCDMA_INFO_UAR_WRITE_COMBINED|MCDMA_INFO_KERNEL_BLUEFLAME,256);
    assert(mcdma_info_read(page,&flags,&bank) && flags==3 && bank==256);
    assert(cx5::read_be32(page+MCDMA_DBR_OFFSET)==34 && cx5::read_be32(page+MCDMA_DBR_OFFSET+4)==7); // Untouched.
    mcdma_put_le32(page+MCDMA_INFO_OFFSET+8,192); assert(!mcdma_info_read(page,&flags,&bank));
    mcdma_put_le32(page+MCDMA_INFO_OFFSET+8,2048); assert(!mcdma_info_read(page,&flags,&bank));
    mcdma_put_le32(page+MCDMA_INFO_OFFSET+8,256); mcdma_put_le32(page+MCDMA_INFO_OFFSET+12,3); assert(!mcdma_info_read(page,&flags,&bank));
    mcdma_info_write(page,3,256); assert(mcdma_info_read(page,&flags,&bank));
    // BlueFlame push: exactly the WQEBB (or the WQEBB plus 64 zero bytes) at
    // the requested bank, the doorbell record first, nothing else touched.
    const uint32_t index=9;
    assert(mcdma_encode_send_wqe(page,0x123456,index,MCDMA_WQE_READ,0x1000,7,4096,0x2000,9));
    const uint8_t *wqe=page+MCDMA_SQ_OFFSET+(index&31u)*64;
    for (unsigned variant=0;variant<4;++variant) {
        const unsigned bytes=variant<2 ? 64 : 128; const int vector=variant&1;
        const uint32_t bank_offset=MCDMA_UAR_DOORBELL+(variant&1)*256;
        memset(uar,0x5a,sizeof(uar));
        mcdma_ring_send_bf(page,uar,index+1,index,bank_offset,bytes,vector);
        assert(cx5::read_be32(page+MCDMA_DBR_OFFSET+4)==index+1);
        assert(!memcmp(uar+bank_offset,wqe,64));
        for (unsigned i=64;i<bytes;++i) assert(uar[bank_offset+i]==0);
        for (unsigned i=0;i<16384;++i) if (i<bank_offset || i>=bank_offset+bytes) assert(uar[i]==0x5a);
        ++checks;
    }
    printf("PASS %u userspace WQE encodings identical to the kernel encoder, receive entries, doorbell publication, capability block and BlueFlame bank writes\n",checks);
    return 0;
}
