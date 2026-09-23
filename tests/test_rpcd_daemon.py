"""Exercise mcdma-rpcd's failure paths offline: the real daemon sources built against a stub verbs library."""
import mmap
import os
import pathlib
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MIB = 1 << 20
SOURCES = ['rpc/rpcd_common.c', 'rpc/rpcd_verbs.c', 'rpc/rpcd_listen.c', 'rpc/rpcd_connect.c']


@unittest.skipUnless(shutil.which('cc'), 'C compiler required')
class DaemonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.mkdtemp(prefix='rpcd-', dir='/tmp')
        flags = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 f'-DMCDMA_RPC_BOX_DIR="{cls.work}"', f'-DMCDMA_RPC_LOCK_DIR="{cls.work}"',
                 '-DMCDMA_RPC_HANDSHAKE_S=1']
        stub = ['tests/rpcd_stub_verbs.c']
        cls.daemon = os.path.join(cls.work, 'rpcd-stub')
        cls.harness = os.path.join(cls.work, 'connect-harness')
        subprocess.run([*flags, 'rpc/mcdma-rpcd.c', *SOURCES, *stub, '-o', cls.daemon], cwd=ROOT, check=True)
        subprocess.run([*flags, 'tests/test_rpcd_connect.c', *SOURCES, *stub, '-o', cls.harness], cwd=ROOT,
                       check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def connect_scenario(self, mode):
        done = subprocess.run([self.harness, mode], capture_output=True, text=True, timeout=30)
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])

    def test_a_request_staged_the_moment_the_link_is_up_is_served(self):
        self.connect_scenario('race')

    def test_shutdown_completes_when_the_peer_never_answers_hello(self):
        self.connect_scenario('hang')

    def test_an_oversized_pull_reply_drops_the_link_instead_of_crashing(self):
        self.connect_scenario('pullcrash')

    def test_sigterm_during_a_call_stops_the_daemon_the_orderly_way(self):
        self.connect_scenario('sigterm')

    # Listen end: the daemon under test, driven over its control port and socket.

    def start(self, name):
        port = free_port()
        sock = os.path.join(self.work, name + '.sock')
        env = dict(os.environ, MCDMA_RPCD_SOCKET=sock, STUB_NOCOPY='1')
        daemon = subprocess.Popen([self.daemon, 'listen', name, 'stub0', '0', '4096', f'127.0.0.1:{port}'],
                                  stderr=subprocess.PIPE, env=env, text=True)
        self.addCleanup(stop, daemon, sock)
        deadline = time.monotonic() + 5
        while command(sock, 'STATUS') is None:
            self.assertLess(time.monotonic(), deadline, 'the listen daemon never answered')
            time.sleep(0.05)
        return daemon, port, sock, os.path.join(self.work, 'mcdma-rpc.' + name)

    def test_a_second_daemon_for_the_same_link_leaves_the_live_one_alone(self):
        live, port, sock, box = self.start('dup')
        with open(box, 'r+b') as stream, mmap.mmap(stream.fileno(), 8 * MIB) as mailbox:
            mailbox[4096:4102] = b'MARKER'
            env = dict(os.environ, MCDMA_RPCD_SOCKET=os.path.join(self.work, 'dup2.sock'), STUB_NOCOPY='1')
            second = subprocess.run([self.daemon, 'listen', 'dup', 'stub0', '0', '4096', f'127.0.0.1:{free_port()}'],
                                    capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(second.returncode, 2)
            self.assertIn('another mcdma-rpcd serves link dup', second.stderr)
            self.assertEqual(bytes(mailbox[4096:4102]), b'MARKER')
        self.assertIsNone(live.poll())
        self.assertIn('PEER dup down', command(sock, 'STATUS'))

    def test_silent_socket_clients_cannot_hold_shutdown_off(self):
        daemon, _, sock, _ = self.start('quiet')
        idle = []
        for _ in range(4):
            client = socket.socket(socket.AF_UNIX)
            client.connect(sock)
            idle.append(client)
        try:
            deadline = time.monotonic() + 10
            while command(sock, 'SHUTDOWN') != 'BYE':
                self.assertLess(time.monotonic(), deadline, 'SHUTDOWN stayed locked out')
                time.sleep(0.2)
            self.assertEqual(daemon.wait(timeout=10), 0)
        finally:
            for client in idle:
                client.close()

    def test_an_idle_control_connection_gives_way_to_the_real_connect_end(self):
        _, port, _, _ = self.start('idle')
        squatter = socket.create_connection(('127.0.0.1', port))
        try:
            time.sleep(1.5)
            with socket.create_connection(('127.0.0.1', port), timeout=5) as mac:
                mac.sendall(hello())
                self.assertTrue(mac.recv(200).startswith(b'HELLO '))
        finally:
            squatter.close()

    def test_a_failed_second_hello_disarms_the_link(self):
        daemon, port, sock, box = self.start('rehello')
        with socket.create_connection(('127.0.0.1', port), timeout=5) as mac, register(sock) as service:
            mac.sendall(hello())
            self.assertTrue(mac.recv(200).startswith(b'HELLO '))
            mac.sendall(b'READY\n')
            mac.sendall(hello('not-a-gid'))
            self.assertEqual(mac.recv(200).strip(), b'ERR qp')
            with open(box, 'r+b') as stream, mmap.mmap(stream.fileno(), 8 * MIB) as mailbox:
                struct.pack_into('<Q', mailbox, 4 * MIB + 128, (1 << 32) | 16)
                time.sleep(0.5)
            self.assertIsNone(daemon.poll(), 'the daemon died writing a reply without a queue pair')
            self.assertIsNotNone(service)

    def test_losing_the_link_ends_the_service_registration(self):
        _, port, sock, _ = self.start('loss')
        with register(sock) as service:
            mac = socket.create_connection(('127.0.0.1', port), timeout=5)
            mac.sendall(hello())
            self.assertTrue(mac.recv(200).startswith(b'HELLO '))
            mac.sendall(b'READY\n')
            time.sleep(0.3)
            mac.close()
            service.settimeout(5)
            self.assertEqual(service.recv(16), b'BYE\n')

    def test_sigterm_stops_cleanly_and_sighup_is_ignored(self):
        daemon, _, sock, box = self.start('signals')
        daemon.send_signal(signal.SIGHUP)
        time.sleep(0.3)
        self.assertIsNone(daemon.poll(), 'SIGHUP from a closed terminal must not stop the daemon')
        daemon.send_signal(signal.SIGTERM)
        self.assertEqual(daemon.wait(timeout=10), 0)
        self.assertFalse(os.path.exists(sock))
        self.assertFalse(os.path.exists(box))


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def hello(gid='fe80::1'):
    return f'HELLO 1 5 6 {gid} DIRECT {4 * MIB} {4 * MIB} 1 99 12345\n'.encode()


def command(sock, text):
    try:
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(2)
            client.connect(sock)
            client.sendall(text.encode() + b'\n')
            answer = b''
            while chunk := client.recv(4096):
                answer += chunk
            return answer.decode().strip()
    except OSError:
        return None


class register:
    """A service registered with the listen daemon, as an application would."""

    def __init__(self, sock):
        self.client = socket.socket(socket.AF_UNIX)
        self.client.connect(sock)
        self.client.sendall(b'MODE poll\n')
        self.client.settimeout(5)
        if self.client.recv(3) != b'OK\n':
            raise AssertionError('the service was not registered')

    def __enter__(self):
        return self.client

    def __exit__(self, *_):
        self.client.close()


def stop(daemon, sock):
    if daemon.poll() is None:
        command(sock, 'SHUTDOWN')
        try:
            daemon.wait(timeout=10)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
    daemon.stderr.close()


if __name__ == '__main__':
    unittest.main()
