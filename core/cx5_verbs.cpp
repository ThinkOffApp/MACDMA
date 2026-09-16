#include "cx5_verbs.hpp"
#include <string.h>
namespace cx5 {
bool set_bits(uint8_t *p,size_t bytes,size_t offset,unsigned width,uint64_t value) {
    if (!p || !width || width>64 || bytes>SIZE_MAX/8 || offset>bytes*8 ||
        width>bytes*8-offset || (width<64 && (value>>width))) return false;
    for (unsigned bit=0;bit<width;++bit) {
        const size_t pos=offset+bit;
        const uint8_t mask=uint8_t(1u<<(7-pos%8));
        p[pos/8]=uint8_t((p[pos/8]&~mask) | (((value>>(width-1-bit))&1) ? mask : 0));
    }
    return true;
}
uint64_t get_bits(const uint8_t *p,size_t bytes,size_t offset,unsigned width) {
    if (!p || !width || width>64 || bytes>SIZE_MAX/8 || offset>bytes*8 || width>bytes*8-offset)
        return 0;
    uint64_t value=0;
    for (unsigned bit=0;bit<width;++bit) {
        const size_t pos=offset+bit;
        value=(value<<1)|((p[pos/8]>>(7-pos%8))&1);
    }
    return value;
}
bool create_mkey(uint8_t *p,size_t capacity,uint32_t pd,uint8_t key,uint64_t base,
                 uint64_t length,const uint64_t *pages,size_t count) {
    if (!p || !pages || !key || pd>0xffffff || !count || count>512 ||
        capacity<0x110+count*8 || (base&4095) || !length || length>count*4096 ||
        base>UINT64_MAX-length) return false;
    for (size_t i=0;i<count;++i) if (!pages[i] || (pages[i]&4095)) return false;
    memset(p,0,0x110+count*8); write_be32(p,0x2000000);
    auto *m=p+16;
    // MTT translation, local read/write and remote read/write, no atomics.
    set_bits(m,64,0x12,4,15); set_bits(m,64,0x16,2,1);
    set_bits(m,64,0x20,24,0xffffff); set_bits(m,64,0x38,8,key);
    set_bits(m,64,0x68,24,pd); set_bits(m,64,0x80,64,base);
    set_bits(m,64,0xc0,64,length);
    set_bits(m,64,0x1a0,32,(count+1)/2);
    set_bits(m,64,0x1db,5,12);
    write_be32(p+0x60,uint32_t((count+1)/2));
    for (size_t i=0;i<count;++i) write_be64(p+0x110+i*8,pages[i]);
    return true;
}
size_t create_user_mkey(uint8_t *p,size_t capacity,uint32_t pd,uint8_t key,uint64_t base,
                        uint64_t length,const uint64_t *pages,size_t count,uint32_t access,
                        unsigned log_page,bool relaxed_ordering) {
    if (!p || !pages || !key || pd>0xffffff || !count || count>max_mkey_pages || !length ||
        log_page<12 || log_page>30 ||
        (access&~7u) || ((access&2) && !(access&1)) || base>UINT64_MAX-length)
        return 0;
    const uint64_t page=uint64_t(1)<<log_page, mask=page-1, offset=base&mask;
    if (length>UINT64_MAX-offset-mask || length>uint64_t(count)*page-offset ||
        (length+offset+mask)/page!=count) return 0;
    const size_t padded=(count+1)&~size_t(1);
    const size_t bytes=0x110+padded*8;
    if (capacity<bytes) return 0;
    for (size_t i=0;i<count;++i) if (!pages[i] || (pages[i]&mask)) return 0;
    memset(p,0,bytes); write_be32(p,0x2000000);
    auto *m=p+16;
    set_bits(m,64,0x12,1,(access&2)!=0); // Remote write.
    set_bits(m,64,0x13,1,(access&4)!=0); // Remote read.
    set_bits(m,64,0x14,1,(access&1)!=0); // Local write.
    set_bits(m,64,0x15,1,1);            // Local read.
    set_bits(m,64,0x16,2,1);            // MTT translation.
    set_bits(m,64,0x20,24,0xffffff); set_bits(m,64,0x38,8,key);
    set_bits(m,64,0x68,24,pd); set_bits(m,64,0x80,64,base);
    set_bits(m,64,0xc0,64,length); set_bits(m,64,0x1a0,32,padded/2);
    set_bits(m,64,0x1da,6,log_page);
    if (relaxed_ordering) { set_bits(m,64,0xd,1,1); set_bits(m,64,0x1d9,1,1); }
    write_be32(p+0x60,uint32_t(padded/2));
    for (size_t i=0;i<count;++i) write_be64(p+0x110+i*8,pages[i]);
    return bytes;
}
namespace {
bool segments_valid(const Segment *s,size_t n,uint64_t start,uint64_t length) {
    if (!s || !n || !length || start>UINT64_MAX-length) return false;
    uint64_t expect=start&~uint64_t(4095);
    for (size_t i=0;i<n;++i) {
        if (s[i].va!=expect || !s[i].length || (s[i].length&4095) || !s[i].iova || (s[i].iova&4095) ||
            s[i].va>UINT64_MAX-s[i].length || s[i].iova>UINT64_MAX-s[i].length) return false;
        expect=s[i].va+s[i].length;
    }
    return expect>=start+length;
}
}
unsigned choose_log_page(const Segment *s,size_t n,uint64_t start,uint64_t length,uint64_t alias,size_t capacity) {
    if (!segments_valid(s,n,start,length) || ((start^alias)&4095)) return 0;
    unsigned best=0;
    for (unsigned log=12;log<=30;++log) {
        const uint64_t mask=(uint64_t(1)<<log)-1;
        // Misalignment at one size persists at every larger one, so stop early.
        if ((start^alias)&mask) break;
        bool fits=true;
        for (size_t i=0;i<n && fits;++i)
            fits=(s[i].va&mask)==(s[i].iova&mask) && (!i || !(s[i].va&mask));
        if (!fits) break;
        if (length>UINT64_MAX-(start&mask)-mask) break;
        if ((((start&mask)+length+mask)>>log)<=capacity) best=log;
    }
    return best;
}
size_t page_list(const Segment *s,size_t n,uint64_t start,uint64_t length,unsigned log,uint64_t *pages,size_t capacity) {
    if (!pages || log<12 || log>30 || !segments_valid(s,n,start,length)) return 0;
    const uint64_t page=uint64_t(1)<<log, mask=page-1;
    if (length>UINT64_MAX-(start&mask)-mask) return 0;
    const uint64_t entries=((start&mask)+length+mask)>>log;
    if (!entries || entries>capacity) return 0;
    const uint64_t base=start&~mask;
    size_t k=0;
    for (uint64_t i=0;i<entries;++i) {
        const uint64_t va=base+i*page;
        while (k+1<n && va>=s[k].va+s[k].length) ++k;
        if (va<s[k].va) {
            // Only the first page may begin before the first segment.
            if (k || s[k].va-va>=page || (s[k].va&mask)!=(s[k].iova&mask)) return 0;
            pages[i]=s[k].iova-(s[k].va-va);
        } else {
            if (va-s[k].va>=s[k].length || ((s[k].va^s[k].iova)&mask) || (k && (s[k].va&mask))) return 0;
            pages[i]=s[k].iova+(va-s[k].va);
        }
        if (pages[i]&mask) return 0;
    }
    return size_t(entries);
}
uint32_t request_bytes(const SendRequest &r) {
    const bool rdma=rdma_opcode(r.opcode);
    if (!rdma && r.opcode!=wqe_send && r.opcode!=wqe_send_imm) return 0;
    if (r.flags&~unsigned(send_flag_fence|send_flag_signaled|send_flag_solicited)) return 0;
    if (rdma && !r.rkey) return 0;
    uint64_t total=0;
    if (r.inline_data || r.inline_bytes) {
        // Inline never applies to a READ; the bytes share the WQEBB with the
        // control segment and, for WRITEs, the RDMA segment.
        if (!r.inline_data || r.sge_count || r.opcode==wqe_read) return 0;
        if (r.inline_bytes<1 || r.inline_bytes>(rdma ? max_inline_rdma : max_inline_send)) return 0;
        total=r.inline_bytes;
    } else {
        if (r.sge_count<1 || r.sge_count>(rdma ? max_rdma_sge : max_send_sge)) return 0;
        for (unsigned i=0;i<r.sge_count;++i) {
            const auto &s=r.sge[i];
            if (!s.length || !s.lkey || s.address>UINT64_MAX-s.length) return 0;
            total+=s.length;
        }
    }
    if (!total || total>0x7fffffff || (rdma && r.remote>UINT64_MAX-total)) return 0;
    return uint32_t(total);
}
bool encode_send_request(uint8_t *p,size_t capacity,uint32_t qpn,uint16_t producer,const SendRequest &r) {
    const uint32_t total=request_bytes(r);
    if (!p || capacity<64 || qpn>0xffffff || !total) return false;
    const bool rdma=rdma_opcode(r.opcode);
    const bool inlined=r.inline_data!=nullptr;
    const unsigned data_units=inlined ? (4+r.inline_bytes+15)/16 : r.sge_count;
    const unsigned ds=1+unsigned(rdma)+data_units;
    if (ds>4) return false;
    memset(p,0,64);
    write_be32(p,(uint32_t(producer)<<8)|r.opcode);
    write_be32(p+4,(qpn<<8)|ds);
    // fm_ce_se: always a completion (ce=2), plus the request's fence (fm=4)
    // and solicited-event bits in the hardware's positions.
    p[11]=uint8_t(8|((r.flags&send_flag_fence) ? 0x80 : 0)|((r.flags&send_flag_solicited) ? 0x02 : 0));
    if (immediate_opcode(r.opcode)) memcpy(p+12,&r.immediate,4);
    size_t at=16;
    if (rdma) { write_be64(p+16,r.remote); write_be32(p+24,r.rkey); at=32; }
    if (inlined) {
        write_be32(p+at,0x80000000u|r.inline_bytes);
        memcpy(p+at+4,r.inline_data,r.inline_bytes);
    } else for (unsigned i=0;i<r.sge_count;++i,at+=16) {
        write_be32(p+at,r.sge[i].length); write_be32(p+at+4,r.sge[i].lkey); write_be64(p+at+8,r.sge[i].address);
    }
    return true;
}
bool encode_wqe(uint8_t *p,size_t capacity,uint32_t qpn,uint16_t producer,uint8_t opcode,
                uint64_t local,uint32_t lkey,uint32_t length,uint64_t remote,uint32_t rkey) {
    SendRequest r; r.opcode=opcode; r.remote=remote; r.rkey=rkey;
    r.sge[0]={local,lkey,length}; r.sge_count=1;
    // The original one-SGE encoder accepted only WRITE, SEND and READ.
    if (opcode!=wqe_write && opcode!=wqe_send && opcode!=wqe_read) return false;
    return encode_send_request(p,capacity,qpn,producer,r);
}
bool encode_rc_transition(uint8_t *p,size_t capacity,uint16_t opcode,const RCConnection &c) {
    if (!p || capacity<272 || opcode<0x502 || opcode>0x504 ||
        c.qpn>0xffffff || c.pd>0xffffff || c.cq>0xffffff || c.remote_qpn>0xffffff ||
        c.send_psn>0xffffff || c.receive_psn>0xffffff || !c.doorbell || (c.doorbell&7) ||
        (c.access&~6u) || c.path_mtu<1 || c.path_mtu>5 || c.min_rnr_timer>31 ||
        c.retry_count>7 || c.rnr_retry>7 || c.timeout>31 || !c.hop_limit || c.log_ack_req_freq>15) return false;
    memset(p,0,272); write_be32(p,uint32_t(opcode)<<16); write_be32(p+8,c.qpn);
    auto *q=p+24;
    set_bits(q,232,0x13,2,3); // A single Ethernet path stays migrated.
    set_bits(q,232,0x28,24,c.pd);
    set_bits(q,232,0x3e8,24,c.cq); set_bits(q,232,0x4e8,24,c.cq);
    set_bits(q,232,0x380,4,c.log_ack_req_freq);
    if (opcode==0x502) {
        set_bits(q,232,0x490,2,c.access>>1);
        set_bits(q,232,0xc0+0x128,8,1);
        // RESET->INIT consumes the doorbell address even when CREATE_QP supplied it.
        set_bits(q,232,0x500,64,c.doorbell);
    } else if (opcode==0x503) {
        set_bits(q,232,0x40,3,c.path_mtu); set_bits(q,232,0x43,5,30);
        set_bits(q,232,0xa8,24,c.remote_qpn);
        set_bits(q,232,0x4a3,5,c.min_rnr_timer); set_bits(q,232,0x4a8,24,c.receive_psn);
        auto *path=q+24;
        // ADS.grh selects an InfiniBand path; RoCE uses the programmed source GID entry.
        set_bits(path,44,0x58,8,c.hop_limit);
        set_bits(path,44,0x110,16,0xc000|(c.qpn&0x3fff));
        set_bits(path,44,0x128,8,1);
        memcpy(path+16,c.remote_gid,16); memcpy(path+38,c.remote_mac,6);
    } else {
        set_bits(q,232,0x38d,3,c.retry_count); set_bits(q,232,0x390,3,c.rnr_retry);
        set_bits(q,232,0x3c8,24,c.send_psn);
        set_bits(q,232,0xc0+0x40,5,c.timeout);
    }
    return true;
}
CQResult decode_cqe(const uint8_t *p,uint32_t consumer,unsigned log_entries,Completion &out) {
    out={};
    if (!p || log_entries>23) return CQResult::unsupported;
    const uint8_t opcode=p[63]>>4;
    if (opcode==15 || (p[63]&1)!=((consumer>>log_entries)&1)) return CQResult::empty;
    out.qpn=read_be32(p+56)&0xffffff; out.bytes=read_be32(p+44);
    out.wqe_counter=uint16_t((uint16_t(p[60])<<8)|p[61]); out.opcode=opcode;
    memcpy(&out.immediate,p+40,4);
    if (opcode==13 || opcode==14) {
        out.syndrome=p[55]; out.vendor_syndrome=p[54]; return CQResult::error;
    }
    if (opcode==0 || opcode==1 || opcode==2 || opcode==3 || opcode==4)
        return CQResult::success;
    return CQResult::unsupported;
}
}
