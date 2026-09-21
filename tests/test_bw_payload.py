"""Run the real payload helpers and WR builder with a memory-copy verbs double."""
import importlib.util
import pathlib
import shutil
import subprocess
import tempfile
import unittest
from types import SimpleNamespace

ROOT = pathlib.Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = load('payload_runner', 'benchmarks/run_bw.py')
summary = load('payload_summary', 'benchmarks/bw_summary.py')


class PayloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('C compiler and platform verbs library required')
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = pathlib.Path(cls.temp.name)
        source = cls.directory / 'payload.c'
        source.write_text(r'''
#include <infiniband/verbs.h>
#include <string.h>
#include <assert.h>
static int copy_post(struct ibv_qp *qp,struct ibv_send_wr *wr,struct ibv_send_wr **bad) {
    (void)qp; (void)bad;
    void *local=(void *)(uintptr_t)wr->sg_list->addr;
    void *remote=(void *)(uintptr_t)wr->wr.rdma.remote_addr;
    if (wr->opcode==IBV_WR_RDMA_READ) memcpy(local,remote,wr->sg_list->length);
    else { assert(wr->opcode==IBV_WR_RDMA_WRITE); memcpy(remote,local,wr->sg_list->length); }
    return 0;
}
#define ibv_post_send copy_post
#define main benchmark_main
#include "''' + str(ROOT / 'benchmarks/mcdma_bw.c') + r'''"
#undef main
int main(int argc,char **argv) {
    if (argc>1 && !strcmp(argv[1],"parse")) {
        struct bench b={0}; parse(&b,argc-1,argv+1); return 0;
    }
    assert(argc==6);
    const unsigned depth=(unsigned)atoi(argv[4]);
    const int is_read=!strcmp(argv[1],"read");
    struct bench source={0},receiver={0};
    source.o.bytes=receiver.o.bytes=4096;
    source.o.qps=receiver.o.qps=1;
    source.depth=receiver.depth=depth;
    source.o.payload_path=argv[2]; receiver.o.dump_path=argv[3];
    source.o.timeout_s=1;
    source.region_bytes=receiver.region_bytes=(uint64_t)depth*4096+GUARD_BYTES+FLAG_BYTES;
    source.region=malloc(source.region_bytes); receiver.region=malloc(receiver.region_bytes);
    memset(source.region,0x5a,source.region_bytes); memset(receiver.region,0x5a,receiver.region_bytes);
    assert(payload_crc64((const unsigned char *)"123456789",9)==UINT64_C(0x6c40df5f0b497347));
    prepare_payload(&source); /* stdin supplies PAYLOAD_READY */
    receiver.payload_len=source.payload_len; receiver.payload_crc=source.payload_crc;
    receiver.o.total=source.o.total;
    struct bench *initiator=is_read ? &receiver : &source;
    struct ibv_mr mr={0}; initiator->mr=&mr;
    initiator->o.op=is_read ? OP_READ : OP_WRITE;
    initiator->remote.addr=(uintptr_t)(is_read ? source.region : receiver.region);
    uint64_t wrs=0; plan_trial(initiator,&wrs);
    assert(wrs<=depth);
    for (uint64_t k=0;k<wrs;++k) assert(!post_data(initiator,0,k));
    if (!strcmp(argv[5],"corrupt")) receiver.region[receiver.payload_len-1]^=1;
    uint64_t verified=0;
    const uint64_t bad=verify_payload(&receiver,&verified);
    assert(verified==source.payload_len);
    assert(guard_intact(&source) && guard_intact(&receiver));
    if (bad) return 3;
    dump_landed(&receiver);
    free(source.region); free(receiver.region); return 0;
}
''')
        cls.binary = cls.directory / 'payload'
        import sys
        subprocess.run([compiler, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-D_DARWIN_C_SOURCE', '-O1',
                        '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        str(source), '-lrdma' if sys.platform == 'darwin' else '-libverbs',
                        '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def transfer(self, operation, data, depth=4, corrupt=False, existing=None):
        with tempfile.TemporaryDirectory() as directory:
            source, dest = pathlib.Path(directory) / 'source', pathlib.Path(directory) / 'landed'
            source.write_bytes(data)
            if existing is not None:
                dest.write_bytes(existing)
            result = subprocess.run([str(self.binary), operation, str(source), str(dest), str(depth),
                                     'corrupt' if corrupt else 'good'], input='PAYLOAD_READY\n',
                                    text=True, capture_output=True)
            return result, dest.read_bytes() if dest.exists() else None

    def test_multislot_and_partial_tail_round_trip_in_both_directions(self):
        data = b'A' * 4096 + b'B' * 4096 + b'C' * 4096 + b'partial last slot!'
        for op in ('write', 'read'):
            result, landed = self.transfer(op, data)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(landed, data)
        for data in (b'x', b'Z' * 16384):
            result, landed = self.transfer('write', data)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(landed, data)

    def test_oversize_empty_and_corrupt_payload_never_produce_a_dump(self):
        for data, depth, corrupt in ((b'x' * 12288, 1, False), (b'', 4, False), (b'x' * 12345, 4, True)):
            result, landed = self.transfer('write', data, depth, corrupt)
            self.assertNotEqual(result.returncode, 0)
            self.assertIsNone(landed)

    def test_existing_destination_is_preserved(self):
        result, landed = self.transfer('read', b'new data', existing=b'previous data')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(landed, b'previous data')

    def test_parse_rejects_ambiguous_or_unsupported_payload_modes(self):
        base = ['parse', '--device', 'test-device', '--role', 'initiator', '--op', 'write',
                '--payload', '/test/source', '--warmup', '0', '--repeats', '1']
        for extra in (['--warmup', '1'], ['--repeats', '2'], ['--qps', '2'], ['--op', 'send'],
                      ['--op', 'read'], ['--dump', '/test/dest']):
            result = subprocess.run([str(self.binary)] + base + extra, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, extra)
            self.assertIn('BW_ERROR', result.stdout)

    def test_runner_routes_files_by_data_direction(self):
        args = SimpleNamespace(total=65536, warmup=0, mtu=1024, finish='flag', timeout=30,
                               verify_bytes=4096, payload='/test/source', dump='/test/dest')
        for op in ('write', 'read'):
            config = dict(op=op, bytes=4096, depth=4, qps=1, cq_per_qp=0)
            for role in ('initiator', 'responder'):
                command = runner.program_arguments(config, role, 'test-device', 0, args)
                source = role == ('responder' if op == 'read' else 'initiator')
                self.assertIn('--payload' if source else '--dump', command)
                self.assertNotIn('--dump' if source else '--payload', command)

    def test_resident_payload_cannot_be_reported_as_sustained_bandwidth(self):
        with self.assertRaisesRegex(ValueError, 'not sustained'):
            summary.summarize([dict(side='initiator', warmup='0', measurement='resident-payload')])


if __name__ == '__main__':
    unittest.main()
