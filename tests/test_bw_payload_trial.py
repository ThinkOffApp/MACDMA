"""Exercise complete payload trials without NICs or remote machines."""
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PayloadTrialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('C compiler and platform verbs library required')
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / 'trial'
        subprocess.run([compiler, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-D_DARWIN_C_SOURCE',
                        '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        str(ROOT / 'tests/bw_payload_trial.c'),
                        '-lrdma' if sys.platform == 'darwin' else '-libverbs',
                        '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def transfer(self, operation, data, fault='good', existing=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, dest, shared = (root / name for name in ('source', 'dest', 'shared'))
            source.write_bytes(data)
            shared.write_bytes(b'\x5a' * (2 * (16384 + 16384 + 64)))
            if existing is not None:
                dest.write_bytes(existing)
            processes, logs, partial = [], [[], []], [b'', b'']
            with selectors.DefaultSelector() as selector:
                try:
                    for index, role in enumerate(('initiator', 'responder')):
                        is_source = (role == 'initiator') == (operation == 'write')
                        command = [str(self.binary), role, operation, str(shared),
                                   str(source if is_source else dest), fault]
                        proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                                stderr=subprocess.STDOUT)
                        processes.append(proc)
                        selector.register(proc.stdout, selectors.EVENT_READ, index)
                    deadline = time.monotonic() + 12
                    while selector.get_map():
                        self.assertLess(time.monotonic(), deadline, logs)
                        for key, _ in selector.select(0.1):
                            index = key.data
                            chunk = os.read(key.fileobj.fileno(), 65536)
                            if not chunk:
                                selector.unregister(key.fileobj)
                                continue
                            partial[index] += chunk
                            while b'\n' in partial[index]:
                                line, partial[index] = partial[index].split(b'\n', 1)
                                logs[index].append(line.decode(errors='replace'))
                                if line.startswith(b'BW_MSG '):
                                    try:
                                        processes[1 - index].stdin.write(line[7:] + b'\n')
                                        processes[1 - index].stdin.flush()
                                    except BrokenPipeError:
                                        pass
                    codes = [p.wait(timeout=2) for p in processes]
                finally:
                    for p in processes:
                        if p.poll() is None:
                            p.kill()
                        p.wait()
                        for stream in (p.stdin, p.stdout):
                            try:
                                stream.close()
                            except BrokenPipeError:
                                pass
            return codes, dest.read_bytes() if dest.exists() else None, logs

    def test_complete_write_and_read_trials_verify_and_dump_once(self):
        for operation in ('write', 'read'):
            for data in (b'x', b'A' * 4096 + b'B' * 4096 + b'C' * 4096 + b'tail', b'Z' * 16384):
                with self.subTest(operation=operation, length=len(data)):
                    codes, landed, logs = self.transfer(operation, data)
                    self.assertEqual(codes, [0, 0], logs)
                    self.assertEqual(landed, data)
                    self.assertEqual(sum(line.startswith('BW_DUMP ') for log in logs for line in log), 1)
                    for log in logs:
                        results = [line for line in log if line.startswith('BW_RESULT ')]
                        self.assertEqual(len(results), 1, logs)
                        self.assertIn('measurement=resident-payload', results[0])
                    receiver = logs[1 if operation == 'write' else 0]
                    result = next(line for line in receiver if line.startswith('BW_RESULT '))
                    self.assertIn(f'verified_bytes={len(data)} ', result)
                    self.assertIn('mismatches=0 ', result)

    def test_corruption_guard_and_completion_failures_do_not_save_data(self):
        for operation in ('write', 'read'):
            for fault in ('corrupt', 'guard', 'completion'):
                with self.subTest(operation=operation, fault=fault):
                    codes, landed, logs = self.transfer(operation, b'payload' * 1700, fault)
                    self.assertTrue(all(code != 0 for code in codes), logs)
                    self.assertIsNone(landed, logs)
                    self.assertFalse(any(line.startswith('BW_DUMP ') for log in logs for line in log))

    def test_existing_destination_survives_complete_trial(self):
        for operation in ('write', 'read'):
            codes, landed, logs = self.transfer(operation, b'new data', existing=b'original')
            self.assertTrue(any(codes), logs)
            self.assertEqual(landed, b'original')


if __name__ == '__main__':
    unittest.main()
