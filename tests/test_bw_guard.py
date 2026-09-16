"""Exercise the actual bandwidth canary checker without RDMA hardware."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class GuardTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'C compiler required')
    def test_payload_and_finish_are_excluded_but_guard_corruption_is_detected(self):
        source = (ROOT / 'benchmarks/mcdma_bw.c').read_text()
        start = source.index('static int guard_intact(')
        end = source.index('\n}', start) + 2
        program = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
enum { GUARD_BYTES=16384, FLAG_BYTES=64 };
struct bench {
    struct { unsigned qps, bytes; } o;
    unsigned depth;
    uint64_t region_bytes;
    unsigned char *region;
};
''' + source[start:end] + r'''
int main(void) {
    for (unsigned slack=0; slack<=2; ++slack) {
        struct bench b={.o={.qps=1,.bytes=4194304},.depth=1};
        uint64_t used=b.o.bytes;
        b.region_bytes=used+GUARD_BYTES+slack*4096;
        b.region=malloc(b.region_bytes);
        if (!b.region) return 1;
        memset(b.region,0x5a,b.region_bytes);
        memset(b.region,0xa5,used);
        memset(b.region+b.region_bytes-FLAG_BYTES,0xff,FLAG_BYTES);
        if (!guard_intact(&b)) return 2;
        uint64_t offsets[]={used,used+8192,b.region_bytes-FLAG_BYTES-1};
        for (unsigned n=0; n<3; ++n) {
            b.region[offsets[n]]=0;
            if (guard_intact(&b)) return 3;
            b.region[offsets[n]]=0x5a;
        }
        free(b.region);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / 'guard.c').write_text(program)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(path / 'guard.c'), '-o', str(path / 'guard')], check=True)
            result = subprocess.run([str(path / 'guard')])
            self.assertEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
