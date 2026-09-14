#include "cx5_protocol.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
using namespace cx5;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
int main() {
    uint8_t blocks[max_mailboxes*mailbox_stride], input[max_command_bytes], output[max_command_bytes], entry[64];
    for (size_t i=0;i<sizeof(input);++i) input[i]=uint8_t(i*37+19);
    const uint64_t dma=0x12345678000ull;
    for (size_t n: {size_t(1),size_t(511),size_t(512),size_t(513),size_t(2048),size_t(8176)}) {
        memset(output,0xa5,sizeof(output));
        CHECK(prepare_mailboxes(blocks,sizeof(blocks),dma,input,n,43)==Error::none);
        size_t count=(n+511)/512;
        for (size_t b=0;b<count;++b) {
            CHECK(read_be64(blocks+b*1024+0x230)==(b+1<count ? dma+(b+1)*1024 : 0));
            CHECK(read_be32(blocks+b*1024+0x238)==b);
            uint8_t control=0,full=0;
            for (size_t j=0;j<576;++j) full^=blocks[b*1024+j];
            for (size_t j=512;j<575;++j) control^=blocks[b*1024+j];
            CHECK(control==255 && full==255);
        }
        CHECK(collect_mailboxes(blocks,sizeof(blocks),dma,output,n,43)==Error::none);
        CHECK(memcmp(input,output,n)==0 && output[n]==0xa5);
        blocks[(count-1)*1024+0x23d]^=1;
        memset(output,0xa5,sizeof(output));
        CHECK(collect_mailboxes(blocks,sizeof(blocks),dma,output,n,43)==Error::bad_mailbox);
        CHECK(output[0]==0xa5);
    }
    CHECK(prepare_mailboxes(blocks,1024,dma,input,513,1)==Error::invalid_length);
    CHECK(prepare_mailboxes(blocks,sizeof(blocks),UINT64_MAX-1023,input,513,1)==Error::invalid_length);
    CHECK(prepare_mailboxes(blocks,sizeof(blocks),dma,nullptr,2048,0)==Error::invalid_token);
    CHECK(encode_command(entry,input,16,4112,9,0,dma)==Error::none);
    CHECK(read_be64(entry+8)==0 && read_be64(entry+48)==dma && read_be32(entry+56)==4112);
    CHECK(entry[60]==9 && entry[63]==1);
    CHECK(encode_command(entry,input,17,16,9,0,0)==Error::invalid_address);
    CHECK(encode_command(entry,input,16,17,9,0,dma+1)==Error::invalid_address);
    CHECK(encode_command(entry,input,8193,16,9,dma,0)==Error::invalid_length);
    CHECK(mailbox_count(16)==0 && mailbox_count(528)==1 && mailbox_count(529)==2 && mailbox_count(8192)==16);
    std::printf("PASS %u mailbox boundary/integrity checks\n",checks);
}
