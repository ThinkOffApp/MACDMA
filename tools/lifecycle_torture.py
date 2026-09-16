#!/usr/bin/env python3
"""Lifecycle torture: kill a native Apple verbs client at random lifecycle
states, thousands of times, and check nothing leaks and the device stays up.

Each cycle runs client/lifecycle_client.c on the Mac over SSH against
peer/lifecycle_peer.c on the Linux peer, stops the client at a sampled
(state, exit mode) pair, then checks Mac health: the client process is gone,
the native checker still finds an active RoCE v2 port, IORegistry properties
are sampled for deltas, and a fresh clean cycle still succeeds.  Results are
JSON lines plus a summary under the --output path (keep it in ignored
results/; raw lines contain addresses, keys and device names).

Requires a loaded native CX5 provider and configured link-local addresses and
static neighbours.  Changes no network or driver settings; SSH carries only
descriptors, results and the external kill of the client process.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import random
import re
import selectors
import shlex
import subprocess
import sys
import threading
import time

STATES = ('open', 'pd', 'mr', 'cq', 'qp', 'init', 'rtr', 'rts', 'posted', 'polled', 'mapped')
MODES = ('kill', 'abort', 'exit-no-teardown', 'clean', 'hang')
STATE_INDEX = {name: index for index, name in enumerate(STATES)}
# Mac lines that carry key=value fields the records keep verbatim.
FIELD_LINES = ('CONFIG', 'WRITE', 'MAPPED', 'TEARDOWN', 'FAIL', 'HANG')
PEER_FIELD_LINES = ('PEER_UP', 'PEER_HEALTH', 'PEER_VERIFY', 'PEER_DONE', 'PEER_ERROR')
# Provider stderr markers that mean a requested userspace path silently fell back.
FALLBACK_MARKERS = ('enabled=0', 'queue_mapped=0', 'mapped=0 reason=mmap_failed', 'uar_wc=0')
IDENTIFIER = re.compile(r'[A-Za-z0-9_.:-]{1,64}')


def load_cross_host():
    """Reuse the endpoint descriptor and provider marker validators."""
    path = Path(__file__).resolve().with_name('native_cross_host.py')
    spec = importlib.util.spec_from_file_location('native_cross_host', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CampaignAbort(RuntimeError):
    """A failure that makes further cycles meaningless (peer gone, Mac unreachable)."""


class TransportError(RuntimeError):
    pass


# ----------------------------------------------------------------------------
# Sampling and parsing (pure; covered by tests/test_lifecycle_torture.py)

def plan(cycles, seed, states=STATES, modes=MODES):
    states, modes = tuple(states), tuple(modes)
    for name in states:
        if name not in STATE_INDEX:
            raise ValueError('Unknown lifecycle state: ' + name)
    for name in modes:
        if name not in MODES:
            raise ValueError('Unknown exit mode: ' + name)
    if cycles < 0 or not states or not modes:
        raise ValueError('Need at least one cycle, state and mode')
    generator = random.Random(seed)
    return [(generator.choice(states), generator.choice(modes)) for _ in range(cycles)]


def parse_fields(words):
    fields = {}
    for word in words:
        key, separator, value = word.partition('=')
        if not separator or not key:
            raise ValueError('Malformed key=value field: ' + word)
        fields[key] = int(value) if re.fullmatch(r'-?[0-9]+', value) else value
    return fields


def parse_line(text):
    """Classify one protocol line: returns (kind, payload) or raises ValueError."""
    words = text.split()
    if not words:
        raise ValueError('Empty protocol line')
    kind = words[0]
    if kind == 'STATE':
        if len(words) != 2 or words[1] not in STATE_INDEX:
            raise ValueError('Malformed STATE line: ' + text)
        return kind, words[1]
    if kind == 'PID':
        if len(words) != 2 or not re.fullmatch(r'[1-9][0-9]{0,9}', words[1]):
            raise ValueError('Malformed PID line: ' + text)
        return kind, int(words[1])
    if kind == 'READY':
        if len(words) != 1:
            raise ValueError('Malformed READY line: ' + text)
        return kind, None
    if kind == 'ENDPOINT':
        return kind, words[1:]
    if kind in FIELD_LINES or kind in PEER_FIELD_LINES:
        return kind, parse_fields(words[1:])
    raise ValueError('Unknown protocol line: ' + text)


def expected_states(stop_at):
    return list(STATES[:STATE_INDEX[stop_at] + 1])


def check_states(stop_at, mode, reached, mac):
    """Problems with what the client reported against what was requested."""
    problems = []
    expected = expected_states(stop_at)
    if reached[:len(expected)] != expected:
        problems.append('state_not_reached expected=%s reached=%s' % ('>'.join(expected), '>'.join(reached)))
    elif len(reached) > len(expected):
        problems.append('state_overshoot reached=%s' % '>'.join(reached))
    if 'FAIL' in mac:
        problems.append('client_failure ' + ' '.join('%s=%s' % item for item in mac['FAIL'].items()))
    if mode == 'clean':
        teardown = mac.get('TEARDOWN')
        if teardown is None or teardown.get('error') != 0 or teardown.get('unmap_error') != 0:
            problems.append('teardown_failed %r' % (teardown,))
        if mac.get('exit') != 0:
            problems.append('clean_exit_code=%r' % (mac.get('exit'),))
    elif mode == 'hang' and not mac.get('killed_externally'):
        problems.append('hang_not_killed')
    if 'polled' in reached and mac.get('WRITE', {}).get('status') != 0:
        problems.append('write_status=%r' % (mac.get('WRITE'),))
    return problems


def provider_fallbacks(stderr_lines):
    return [line for line in stderr_lines if line.startswith('MCDMA_')
            and any(marker in line for marker in FALLBACK_MARKERS)]


def parse_ioreg(text):
    """Numeric and boolean properties from `ioreg -r -c MCDMACX5Native -l`."""
    properties = {}
    for line in text.splitlines():
        match = re.match(r'\s*\|?\s*"([A-Za-z0-9_-]+)"\s*=\s*(-?[0-9]+|Yes|No)\s*$', line.rstrip('|').rstrip())
        if match:
            key, value = match.groups()
            properties[key] = {'Yes': True, 'No': False}.get(value, None)
            if properties[key] is None:
                properties[key] = int(value)
    return properties


def counter_deltas(before, after):
    deltas = {}
    for key in sorted(set(before) | set(after)):
        old, new = before.get(key), after.get(key)
        if old == new:
            continue
        if isinstance(old, int) and isinstance(new, int) and not isinstance(old, bool) and not isinstance(new, bool):
            deltas[key] = new - old
        else:
            deltas[key] = {'before': old, 'after': new}
    return deltas


def counter_health(properties):
    problems = []
    if not properties:
        problems.append('ioreg_no_properties')
        return problems
    if properties.get('MCDMAPortActive') is not True:
        problems.append('port_not_active')
    if properties.get('MCDMAQuarantined') is True:
        problems.append('driver_quarantined')
    if properties.get('MCDMAGidLive') is False:
        problems.append('gid_not_live')
    return problems


# ----------------------------------------------------------------------------
# Transports: real SSH, or local fakes for --self-test and unit tests

class SshTransport:
    def popen(self, host, command):
        return subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', host, shlex.join(command)],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def run(self, host, command, timeout=30):
        try:
            result = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', host, shlex.join(command)],
                                    capture_output=True, text=True, timeout=timeout)
        except (OSError, subprocess.SubprocessError) as error:
            raise TransportError('%s: %s' % (host, error))
        if result.returncode == 255:
            raise TransportError('%s: ssh transport failure: %s' % (host, result.stderr.strip()))
        return result.returncode, result.stdout, result.stderr


FAKE_CLIENT = r'''
import os, signal, sys, resource
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
STATES = %r
args = sys.argv[1:]
stop = args[args.index('--stop-at') + 1]
mode = args[args.index('--exit-mode') + 1]
seed = int(args[args.index('--pattern-seed') + 1]) if '--pattern-seed' in args else 0
payload = int(os.environ.get('MCDMA_PAYLOAD_BYTES', '4096'))
cq_map = os.environ.get('MCDMA_CQ_MAP', '0'); user_post = os.environ.get('MCDMA_USER_POST', '0')
user_bf = os.environ.get('MCDMA_USER_BF', '0')
def out(text):
    sys.stdout.write(text + '\n'); sys.stdout.flush()
def err(text):
    sys.stderr.write(text + '\n'); sys.stderr.flush()
out('PID %%d' %% os.getpid())
out('CONFIG stop_at=%%s exit_mode=%%s cq_map=%%s user_post=%%s user_bf=%%s payload_bytes=%%d path_mtu=1024 seed=%%d'
    %% (stop, mode, cq_map, user_post, user_bf, payload, seed))
def reached(name):
    out('STATE ' + name)
    if name != stop: return False
    if mode == 'kill': os.kill(os.getpid(), signal.SIGKILL)
    if mode == 'abort': os.kill(os.getpid(), signal.SIGABRT)
    if mode == 'exit-no-teardown': os._exit(0)
    if mode == 'hang':
        out('HANG pid=%%d' %% os.getpid())
        while True: signal.pause()
    return True
def teardown():
    out('TEARDOWN error=0 unmap_error=0'); return 0
if user_bf != '0': err('MCDMA_USER_BF mode=%%s uar_wc=1' %% user_bf)
if user_post == '1': err('MCDMA_USER_POST enabled=1 uar_bytes=16384')
if reached('open'): raise SystemExit(teardown())
if reached('pd'): raise SystemExit(teardown())
if reached('mr'): raise SystemExit(teardown())
if cq_map == '1': err('MCDMA_CQ_OBSERVER mapped=1 bytes=16384 readonly=1')
elif cq_map == '2' or user_post == '1': err('MCDMA_CQ_OBSERVER mapped=1 bytes=16384 readonly=1 consume=user')
else: err('MCDMA_CQ_OBSERVER mapped=0 reason=disabled')
if reached('cq'): raise SystemExit(teardown())
if user_post == '1': err('MCDMA_USER_POST qp=77 queue_mapped=1 bytes=16384')
if user_bf == '64': err('MCDMA_USER_BF qp=77 bank_bytes=256 bytes=64 store=neon')
if reached('qp'): raise SystemExit(teardown())
if reached('init'): raise SystemExit(teardown())
out('ENDPOINT 77 %%d 4660 1048576 16384 fe80::ff:fe00:1' %% ((0x200000 + seed) & 0xffffff))
line = sys.stdin.readline()
if len(line.split()) != 3: out('FAIL stage=peer_descriptor errno=71 text=EPROTO'); os._exit(2)
if reached('rtr'): raise SystemExit(teardown())
if reached('rts'): raise SystemExit(teardown())
out('READY')
words = sys.stdin.readline().split()
if len(words) != 5 or words[0] != 'INITIATE': out('FAIL stage=initiate errno=71 text=EPROTO'); os._exit(2)
if reached('posted'): raise SystemExit(teardown())
out('WRITE status=0 bytes=%%d' %% payload)
if reached('polled'): raise SystemExit(teardown())
out('MAPPED supported=1 cq=1 queue=1 uar=1 knob_held=%%d held=1' %% (user_post == '1' or cq_map != '0'))
if reached('mapped'): raise SystemExit(teardown())
raise SystemExit(teardown())
''' % (STATES,)

FAKE_PEER = r'''
import os, sys
payload = int(os.environ.get('MCDMA_PAYLOAD_BYTES', '4096'))
def out(text):
    sys.stdout.write(text + '\n'); sys.stdout.flush()
out('PEER_UP payload_bytes=%d path_mtu=1024' % payload)
cycle, connected, have_qp = 0, 0, 0
for line in sys.stdin:
    words = line.split()
    if not words: continue
    if words[0] == 'CYCLE':
        cycle, have_qp, connected = int(words[1]), 1, 0
        out('ENDPOINT 88 %d 8738 2097152 16384 fe80::ff:fe00:2' % ((0x100000 + cycle) & 0xffffff))
    elif words[0] == 'CONNECT':
        if not have_qp or len(words) != 4: out('PEER_ERROR connect_without_cycle'); raise SystemExit(2)
        connected = 1; out('READY')
    elif words[0] == 'VERIFY':
        out('PEER_VERIFY bytes=%d' % (payload if connected else 0))
    elif words[0] == 'FINISH':
        out('PEER_HEALTH cycle=%d connected=%d query=0 qp_state=%d cur_state=%d destroy=0 probe_cq=1 probe_qp=1 probe_mr=1 ok=1'
            % (cycle, connected, 3 if connected else 1, 3 if connected else 1))
        have_qp = connected = 0
    elif words[0] == 'QUIT':
        break
    else:
        out('PEER_ERROR unknown_command'); raise SystemExit(2)
out('PEER_DONE cleanup=0')
'''

FAKE_IOREG = '''+-o MCDMACX5Native  <class MCDMACX5Native, id 0x1000, registered, matched, active, busy 0>
    {
      "MCDMAPortActive" = %s
      "MCDMAGidLive" = Yes
      "MCDMAGidAdds" = %d
      "MCDMAGidDeletes" = 0
      "MCDMAUarPageBytes" = 16384
      "MCDMAEthernetMTU" = 9000
      "MCDMANativeVersion" = "0.1.16"
      "MCDMATransport" = "hardware RoCE v2; polled RC; static IPv6 neighbours"
    }
'''


class FakeTransport:
    """Local python stand-ins for the Mac client, the peer and the health commands.

    fail_checker_at / port_down_at inject a health failure on that ioreg or
    checker sample so stop-on-failure can be demonstrated without hardware.
    """
    def __init__(self, fail_checker_at=None, port_down_at=None):
        self.fail_checker_at, self.port_down_at = fail_checker_at, port_down_at
        self.checker_calls = self.ioreg_calls = 0
        self.commands = []

    def popen(self, host, command):
        self.commands.append((host, command))
        environment = dict(os.environ)
        index = 0
        if command and command[0] == 'env':
            index = 1
            while index < len(command) and '=' in command[index]:
                key, _, value = command[index].partition('=')
                environment[key] = value
                index += 1
        rest = command[index:]
        if '--stop-at' in rest:
            return subprocess.Popen([sys.executable, '-u', '-c', FAKE_CLIENT] + rest[1:], env=environment,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return subprocess.Popen([sys.executable, '-u', '-c', FAKE_PEER] + rest[1:], env=environment,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def run(self, host, command, timeout=30):
        self.commands.append((host, command))
        if command[0] == 'kill' and len(command) == 3:
            try:
                os.kill(int(command[2]), 9 if command[1] == '-KILL' else 0)
            except ProcessLookupError:
                return 1, '', 'No such process\n'
            return 0, '', ''
        if command[0] == 'ioreg':
            self.ioreg_calls += 1
            down = self.port_down_at is not None and self.ioreg_calls == self.port_down_at
            return 0, FAKE_IOREG % ('No' if down else 'Yes', self.ioreg_calls // 2), ''
        if command[0] == 'cat':
            return 0, ('RoCE v2\n' if command[1].endswith('/types/1') else 'fake-if1\n'), ''
        if command[0] == 'ndp':
            return 0, 'fe80::ff:fe00:2%fake-if0 02:00:00:00:00:02 fake-if0 permanent R\n', ''
        if command[0] == 'ip':
            return 0, 'fe80::ff:fe00:1 lladdr 02:00:00:00:00:01 PERMANENT\n', ''
        # The native checker: any other command is treated as it.
        self.checker_calls += 1
        if self.fail_checker_at is not None and self.checker_calls == self.fail_checker_at:
            return 2, 'native_cx5=1 native_cx5_active_ports=0 errors=1\n', ''
        return 0, 'native_cx5=1 native_cx5_active_ports=1 errors=0\n', ''


class Endpoint:
    """Line reader over a process with a bounded stderr drain; EOF is a value, not an error."""
    STDERR_LIMIT = 1024 * 1024

    def __init__(self, host, process):
        self.host, self.process, self.pending = host, process, b''
        self.lines = []
        self.stderr_data, self.stderr_overflow = bytearray(), False
        self.stderr_stop = threading.Event()
        os.set_blocking(self.process.stderr.fileno(), False)
        self.stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self.stderr_thread.start()

    def _drain_stderr(self):
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(self.process.stderr, selectors.EVENT_READ)
                while True:
                    if not selector.select(0.1):
                        if self.stderr_stop.is_set():
                            return
                        continue
                    try:
                        data = os.read(self.process.stderr.fileno(), 4096)
                    except BlockingIOError:
                        continue
                    if not data:
                        return
                    available = self.STDERR_LIMIT - len(self.stderr_data)
                    self.stderr_data.extend(data[:available])
                    self.stderr_overflow |= len(data) > available
        except (OSError, ValueError):
            return

    def line(self, timeout):
        """Next stdout line, None at EOF; raises TimeoutError."""
        deadline = time.monotonic() + timeout
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            while b'\n' not in self.pending:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not selector.select(remaining):
                    raise TimeoutError('%s: no response within %ss' % (self.host, timeout))
                data = os.read(self.process.stdout.fileno(), 4096)
                if not data:
                    if self.pending:
                        text, self.pending = self.pending.decode(errors='replace').strip(), b''
                        self.lines.append(text)
                        return text
                    return None
                self.pending += data
                if len(self.pending) > 65536:
                    raise CampaignAbort('%s: line exceeds protocol limit' % self.host)
        raw, self.pending = self.pending.split(b'\n', 1)
        text = raw.decode(errors='replace').strip()
        self.lines.append(text)
        return text

    def send(self, text):
        try:
            self.process.stdin.write((text + '\n').encode())
            self.process.stdin.flush()
            return True
        except (BrokenPipeError, OSError, ValueError):
            return False

    def stderr_lines(self):
        return bytes(self.stderr_data).decode(errors='replace').splitlines()

    def stop(self, timeout=15):
        try:
            self.process.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        try:
            code = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
            code = -1
        self.stderr_stop.set()
        self.stderr_thread.join(timeout=2)
        if not self.stderr_thread.is_alive():
            self.process.stderr.close()
        self.process.stdout.close()
        return code


# ----------------------------------------------------------------------------
# The campaign

class Campaign:
    def __init__(self, args, transport, cross=None, emit=print):
        self.args, self.transport, self.emit = args, transport, emit
        self.cross = cross or load_cross_host()
        self.peer = None
        self.peer_endpoint = None
        self.last_counters = None
        self.baseline_counters = None
        self.records = []
        self.health_cycle_serial = 0

    # Command lines --------------------------------------------------------
    def mac_environment(self):
        provider = self.args.mac_provider
        return ['env', 'IBV_DRIVERS=' + provider[:-len('-rdmav34.so')],
                'MCDMA_PAYLOAD_BYTES=' + str(self.args.payload_bytes),
                'MCDMA_PATH_MTU=' + str(self.args.path_mtu),
                'MCDMA_CQ_MAP=' + self.args.mac_cq_map,
                'MCDMA_USER_POST=' + self.args.mac_user_post,
                'MCDMA_USER_BF=' + self.args.mac_user_bf]

    def mac_command(self, state, mode, seed):
        return self.mac_environment() + [self.args.mac_client, self.args.mac_device,
                                         '--stop-at', state, '--exit-mode', mode,
                                         '--gid-index', str(self.args.mac_gid_index),
                                         '--pattern-seed', str(seed)]

    def peer_command(self):
        return ['env', 'MCDMA_PAYLOAD_BYTES=' + str(self.args.payload_bytes),
                'MCDMA_PATH_MTU=' + str(self.args.path_mtu),
                self.args.peer_client, self.args.peer_device, str(self.args.peer_gid_index)]

    def checker_command(self):
        if not self.args.mac_checker:
            return None
        return [self.args.mac_checker, '--provider', self.args.mac_provider, '--require-gid']

    @staticmethod
    def ioreg_command():
        return ['ioreg', '-r', '-c', 'MCDMACX5Native', '-l']

    # Helpers ----------------------------------------------------------------
    def run_mac(self, command, timeout=30):
        try:
            return self.transport.run(self.args.mac_host, command, timeout=timeout)
        except TransportError as error:
            raise CampaignAbort('mac_unreachable: ' + str(error))

    def run_peer(self, command, timeout=30):
        try:
            return self.transport.run(self.args.peer_host, command, timeout=timeout)
        except TransportError as error:
            raise CampaignAbort('peer_unreachable: ' + str(error))

    def external_kill(self, record):
        pid = record['mac'].get('pid')
        if not pid:
            record['errors'].append('external_kill_without_pid')
            return
        code, _, stderr = self.run_mac(['kill', '-KILL', str(pid)])
        record['mac']['killed_externally'] = code == 0
        if code:
            record['errors'].append('external_kill_failed: ' + stderr.strip())

    def peer_line(self, expect=None):
        try:
            text = self.peer.line(self.args.line_timeout)
        except TimeoutError as error:
            raise CampaignAbort(str(error))
        if text is None:
            raise CampaignAbort('peer exited: ' + ' '.join(self.peer.lines[-3:]))
        kind, payload = parse_line(text)
        if kind == 'PEER_ERROR':
            raise CampaignAbort('peer error: ' + text)
        if expect and kind != expect:
            raise CampaignAbort('peer sent %s while %s was expected' % (kind, expect))
        return kind, payload, text

    def start_peer(self):
        self.peer = Endpoint(self.args.peer_host, self.transport.popen(self.args.peer_host, self.peer_command()))
        _, fields, _ = self.peer_line('PEER_UP')
        if fields.get('payload_bytes') != self.args.payload_bytes:
            raise CampaignAbort('peer payload does not match the request: %r' % fields)

    def stop_peer(self):
        if not self.peer:
            return None
        self.peer.send('QUIT')
        try:
            done = self.peer.line(self.args.line_timeout)
        except (TimeoutError, CampaignAbort):
            done = None
        code = self.peer.stop()
        stderr = self.peer.stderr_lines()[-5:]
        self.peer = None
        return {'done': done, 'exit': code, 'stderr_tail': stderr}

    # One cycle ----------------------------------------------------------------
    def run_cycle(self, cycle, state, mode, purpose='torture'):
        record = {'cycle': cycle, 'state': state, 'mode': mode, 'purpose': purpose,
                  'started': time.time(), 'reached': [], 'errors': [], 'mac': {}, 'peer': {}}
        reached, errors, mac = record['reached'], record['errors'], record['mac']
        self.peer.send('CYCLE %d' % cycle)
        _, _, text = self.peer_line('ENDPOINT')
        try:
            remote, peer_hardware = self.cross.descriptor(text)
        except ValueError as error:
            raise CampaignAbort('peer descriptor: %s' % error)
        record['peer']['endpoint'] = {'qpn': int(remote[0]), 'gid': remote[5], 'hardware': peer_hardware}
        endpoint = Endpoint(self.args.mac_host, self.transport.popen(self.args.mac_host, self.mac_command(state, mode, cycle)))
        try:
            while True:
                try:
                    text = endpoint.line(self.args.line_timeout)
                except TimeoutError as error:
                    errors.append('mac_line_timeout: ' + str(error))
                    self.external_kill(record)
                    break
                if text is None:
                    break
                try:
                    kind, payload = parse_line(text)
                except ValueError as error:
                    errors.append('mac_protocol: ' + str(error))
                    continue
                if kind == 'PID':
                    mac['pid'] = payload
                elif kind == 'STATE':
                    reached.append(payload)
                    if payload == state and mode == 'hang':
                        self.external_kill(record)
                elif kind == 'ENDPOINT':
                    try:
                        local, hardware = self.cross.descriptor(text)
                    except ValueError as error:
                        errors.append('mac_descriptor: ' + str(error))
                        continue
                    mac['endpoint'] = {'qpn': int(local[0]), 'gid': local[5], 'hardware': hardware}
                    self.peer.send('CONNECT %s %s %s' % (local[0], local[1], local[5]))
                    self.peer_line('READY')
                    record['peer']['connected'] = True
                    if not endpoint.send(' '.join([remote[0], remote[1], remote[5]])):
                        errors.append('mac_stdin_closed_before_descriptor')
                elif kind == 'READY':
                    if not endpoint.send('INITIATE %s %s %s %d' % (remote[2], remote[3], remote[4], cycle)):
                        errors.append('mac_stdin_closed_before_initiate')
                elif kind in FIELD_LINES:
                    mac[kind] = payload
                else:
                    errors.append('unexpected_mac_line: ' + text)
        finally:
            mac['exit'] = endpoint.stop()
            stderr = endpoint.stderr_lines()
            mac['provider_markers'] = [line for line in stderr if line.startswith('MCDMA_')]
            mac['stderr_tail'] = stderr[-5:]
            mac['stderr_overflow'] = endpoint.stderr_overflow
        errors.extend(check_states(state, mode, reached, mac))
        fallbacks = provider_fallbacks(stderr)
        if fallbacks:
            errors.append('provider_fallback: ' + '; '.join(fallbacks))
        if 'polled' in reached:
            self.peer.send('VERIFY %d' % cycle)
            _, fields, _ = self.peer_line('PEER_VERIFY')
            record['peer']['verified_bytes'] = fields.get('bytes')
            if fields.get('bytes') != self.args.payload_bytes:
                errors.append('peer_verify_bytes=%r' % fields.get('bytes'))
        self.peer.send('FINISH')
        _, health, _ = self.peer_line('PEER_HEALTH')
        record['peer']['health'] = health
        if health.get('ok') != 1:
            errors.append('peer_unhealthy')
        if purpose == 'health':
            errors.extend(self.strict_markers(stderr))
        record['duration_s'] = round(time.time() - record['started'], 3)
        record['ok'] = not errors
        return record

    def strict_markers(self, stderr):
        """Exactly-once provider marker rules from the cross-host runner, for a full clean walk."""
        log = [{'host': self.args.mac_host, 'endpoint_stderr': '\n'.join(stderr) + '\n'}]
        problems = []
        for check, requested in ((self.cross.observer_marker, self.args.mac_cq_map),
                                 (self.cross.user_post_marker, self.args.mac_user_post),
                                 (self.cross.user_bf_marker, self.args.mac_user_bf)):
            try:
                check(log, self.args.mac_host, requested)
            except ValueError as error:
                problems.append('provider_marker: ' + str(error))
        return problems

    # Health --------------------------------------------------------------------
    def wait_process_gone(self, pid):
        deadline = time.monotonic() + self.args.linger_timeout
        while True:
            code, _, _ = self.run_mac(['kill', '-0', str(pid)])
            if code != 0:
                return True
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.5)

    def sample_counters(self):
        code, stdout, stderr = self.run_mac(self.ioreg_command())
        if code:
            raise CampaignAbort('ioreg failed: ' + stderr.strip())
        return parse_ioreg(stdout)

    def health_check(self, record, clean_cycle):
        result = {'errors': []}
        pid = record['mac'].get('pid')
        if pid:
            result['process_gone'] = self.wait_process_gone(pid)
            if not result['process_gone']:
                result['errors'].append('client_process_lingers pid=%d' % pid)
        command = self.checker_command()
        if command:
            code, stdout, stderr = self.run_mac(command)
            summary = [line for line in stdout.splitlines() if line.startswith('native_cx5=')]
            result['checker'] = {'exit': code, 'summary': summary[-1] if summary else stdout.strip()[-200:]}
            if code:
                result['errors'].append('checker_failed exit=%d %s' % (code, stderr.strip()[-200:]))
        counters = self.sample_counters()
        result['counters'] = {'delta_from_previous': counter_deltas(self.last_counters or {}, counters),
                              'delta_from_baseline': counter_deltas(self.baseline_counters or {}, counters)}
        result['errors'].extend(counter_health(counters))
        self.last_counters = counters
        if clean_cycle:
            self.health_cycle_serial += 1
            clean = self.run_cycle(1000000 + self.health_cycle_serial, 'mapped', 'clean', purpose='health')
            result['clean_cycle'] = clean
            if not clean['ok']:
                result['errors'].append('clean_cycle_failed: ' + '; '.join(clean['errors']))
        result['ok'] = not result['errors']
        return result

    # Preflight ---------------------------------------------------------------------
    def preflight(self):
        report = {}
        command = self.checker_command()
        if command:
            code, stdout, stderr = self.run_mac(command)
            if code:
                raise CampaignAbort('native checker failed before torture: %s%s' % (stdout, stderr))
            report['checker'] = stdout.strip().splitlines()[-1:]
        base = '/sys/class/infiniband/%s/ports/1/gid_attrs' % self.args.peer_device
        code, gid_type, _ = self.run_peer(['cat', '%s/types/%d' % (base, self.args.peer_gid_index)])
        code2, ndev, _ = self.run_peer(['cat', '%s/ndevs/%d' % (base, self.args.peer_gid_index)])
        if code or code2 or gid_type.strip() != 'RoCE v2' or ndev.strip() != self.args.peer_interface:
            raise CampaignAbort('Peer GID is not RoCE v2 on the selected interface')
        self.start_peer()
        # A clean walk to RTR reveals both descriptors without sending a packet:
        # the QP transitions are local, and no work is posted before the
        # static neighbours are confirmed below.
        probe = self.run_cycle(0, 'rtr', 'clean', purpose='preflight')
        report['probe'] = probe
        if not probe['ok'] or 'endpoint' not in probe['mac']:
            raise CampaignAbort('preflight walk to RTR failed: ' + '; '.join(probe['errors']))
        mac_endpoint, peer_endpoint = probe['mac']['endpoint'], probe['peer']['endpoint']
        _, neighbour, _ = self.run_mac(['ndp', '-n', peer_endpoint['gid'] + '%' + self.args.mac_interface])
        if peer_endpoint['hardware'] not in neighbour.lower():
            raise CampaignAbort('Mac needs the peer static IPv6 neighbour before QP connection')
        _, neighbour, _ = self.run_peer(['ip', '-6', 'neigh', 'show', 'to', mac_endpoint['gid'],
                                         'dev', self.args.peer_interface])
        if 'lladdr ' + mac_endpoint['hardware'] not in neighbour.lower():
            raise CampaignAbort('Linux peer needs the Mac static IPv6 neighbour before QP connection')
        self.baseline_counters = self.sample_counters()
        self.last_counters = self.baseline_counters
        problems = counter_health(self.baseline_counters)
        if problems:
            raise CampaignAbort('driver unhealthy before torture: ' + ', '.join(problems))
        report['baseline_counters'] = self.baseline_counters
        clean = self.run_cycle(1000000, 'mapped', 'clean', purpose='health')
        report['baseline_clean_cycle'] = clean
        if not clean['ok']:
            raise CampaignAbort('baseline clean cycle failed: ' + '; '.join(clean['errors']))
        return report

    # Whole campaign ---------------------------------------------------------------
    def run(self, pairs):
        started = time.time()
        summary = {'seed': self.args.seed, 'cycles_planned': len(pairs), 'cycles_run': 0, 'passed': 0, 'failed': 0,
                   'failures_by_state': {}, 'failures_by_mode': {}, 'failures_by_pair': {}, 'first_failure': None,
                   'health': {'checks': 0, 'failures': 0, 'clean_cycles': 0, 'clean_failures': 0,
                              'lingering_processes': 0, 'checker_failures': 0},
                   'peer_qp_states': {}, 'stopped_early': False, 'abort': None, 'preflight': None}
        output = self.args.output
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('w') as lines:
            try:
                summary['preflight'] = self.preflight()
                for index, (state, mode) in enumerate(pairs, 1):
                    record = self.run_cycle(index, state, mode)
                    clean_due = (self.args.health_every > 0 and index % self.args.health_every == 0) or not record['ok']
                    record['health'] = self.health_check(record, clean_due)
                    self.emit('cycle %d/%d %s/%s %s' % (index, len(pairs), state, mode,
                                                        'ok' if record['ok'] and record['health']['ok'] else
                                                        'FAIL ' + '; '.join(record['errors'] + record['health']['errors'])))
                    self.records.append(record)
                    lines.write(json.dumps(record, default=str) + '\n')
                    lines.flush()
                    self.tally(summary, record)
                    if not (record['ok'] and record['health']['ok']) and not self.args.keep_going:
                        summary['stopped_early'] = True
                        break
            except CampaignAbort as error:
                summary['abort'] = str(error)
                summary['stopped_early'] = True
                self.emit('ABORT ' + str(error))
            except KeyboardInterrupt:
                summary['abort'] = 'interrupted'
                summary['stopped_early'] = True
            finally:
                try:
                    summary['peer_shutdown'] = self.stop_peer()
                except CampaignAbort as error:
                    summary['peer_shutdown'] = str(error)
        summary['counters_baseline'] = self.baseline_counters
        summary['counters_final'] = self.last_counters
        summary['counters_total_delta'] = counter_deltas(self.baseline_counters or {}, self.last_counters or {})
        summary['duration_s'] = round(time.time() - started, 3)
        summary['passed_all'] = (summary['abort'] is None and summary['failed'] == 0
                                 and summary['cycles_run'] == summary['cycles_planned'])
        summary_path = output.with_suffix('.summary.json')
        summary_path.write_text(json.dumps(summary, indent=2, default=str) + '\n')
        return summary

    @staticmethod
    def tally(summary, record):
        summary['cycles_run'] += 1
        health = record.get('health', {})
        failed = not (record['ok'] and health.get('ok', True))
        summary['passed' if not failed else 'failed'] += 1
        pair = record['state'] + '/' + record['mode']
        if failed:
            for key, name in (('failures_by_state', record['state']), ('failures_by_mode', record['mode']),
                              ('failures_by_pair', pair)):
                summary[key][name] = summary[key].get(name, 0) + 1
            if summary['first_failure'] is None:
                summary['first_failure'] = {'cycle': record['cycle'], 'state': record['state'], 'mode': record['mode'],
                                            'errors': record['errors'] + health.get('errors', [])}
        state = record.get('peer', {}).get('health', {}).get('qp_state')
        if state is not None:
            summary['peer_qp_states'][str(state)] = summary['peer_qp_states'].get(str(state), 0) + 1
        if health:
            summary['health']['checks'] += 1
            summary['health']['failures'] += not health.get('ok', True)
            if health.get('process_gone') is False:
                summary['health']['lingering_processes'] += 1
            if health.get('checker', {}).get('exit'):
                summary['health']['checker_failures'] += 1
            if 'clean_cycle' in health:
                summary['health']['clean_cycles'] += 1
                summary['health']['clean_failures'] += not health['clean_cycle']['ok']


# ----------------------------------------------------------------------------
# Entry points

def build_parser():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ['mac-host', 'peer-host', 'mac-client', 'peer-client', 'mac-provider',
                 'mac-interface', 'peer-interface', 'mac-device', 'peer-device']:
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--mac-checker', help='Absolute path to build/cx5-native-check on the Mac (health check)')
    parser.add_argument('--peer-gid-index', type=int, required=True)
    parser.add_argument('--mac-gid-index', type=int, default=0, help='Apple GID table index (the checker uses 0)')
    parser.add_argument('--cycles', type=int, default=100)
    parser.add_argument('--seed', type=int, help='Sampling seed; recorded in the summary (default: random)')
    parser.add_argument('--states', default=','.join(STATES), help='Comma-separated subset of stop states')
    parser.add_argument('--modes', default=','.join(MODES), help='Comma-separated subset of exit modes')
    parser.add_argument('--health-every', type=int, default=1,
                        help='Run a full clean cycle after every N torture cycles (0: only after a failure)')
    parser.add_argument('--line-timeout', type=float, default=30)
    parser.add_argument('--linger-timeout', type=float, default=15,
                        help='Seconds a killed client may take to disappear before it counts as stuck')
    parser.add_argument('--keep-going', action='store_true', help='Continue past the first failure')
    parser.add_argument('--payload-bytes', type=int, choices=[1024, 4096], default=4096)
    parser.add_argument('--path-mtu', type=int, choices=[1024, 4096], default=1024)
    parser.add_argument('--mac-cq-map', choices=['0', '1', '2'], default='0')
    parser.add_argument('--mac-user-post', choices=['0', '1'], default='0')
    parser.add_argument('--mac-user-bf', choices=['0', '64', '128', '64s', 'db'], default='0')
    parser.add_argument('--output', type=Path, required=True, help='JSON lines file; summary goes next to it')
    parser.add_argument('--dry-run', action='store_true', help='Print the planned commands; no SSH')
    parser.add_argument('--self-test', action='store_true', help='Exercise the driver with fake local endpoints')
    return parser


def validate(parser, args):
    if args.mac_user_post == '1' and args.mac_cq_map != '2':
        parser.error('--mac-user-post 1 requires --mac-cq-map 2')
    if args.mac_user_bf != '0' and args.mac_user_post != '1':
        parser.error('--mac-user-bf requires --mac-user-post 1')
    for value in [args.mac_interface, args.peer_interface, args.mac_device, args.peer_device]:
        if not IDENTIFIER.fullmatch(value):
            parser.error('Invalid interface/device identifier')
    if not args.mac_provider.startswith('/') or not args.mac_provider.endswith('-rdmav34.so'):
        parser.error('Use the absolute path to libmcdma-rdmav34.so')
    if not 0 <= args.peer_gid_index <= 255 or not 0 <= args.mac_gid_index <= 255:
        parser.error('Invalid GID index')
    if args.cycles < 1:
        parser.error('--cycles must be positive')
    if args.health_every < 0:
        parser.error('--health-every must not be negative')
    args.state_list = [name for name in args.states.split(',') if name]
    args.mode_list = [name for name in args.modes.split(',') if name]
    try:
        plan(1, 0, args.state_list, args.mode_list)
    except ValueError as error:
        parser.error(str(error))
    if args.seed is None:
        args.seed = random.SystemRandom().getrandbits(32)


def dry_run(args, pairs, emit=print):
    campaign = Campaign(args, transport=None)
    emit('# dry run: seed=%d cycles=%d; nothing is executed' % (args.seed, len(pairs)))
    emit('# preflight')
    if campaign.checker_command():
        emit('%s: %s' % (args.mac_host, shlex.join(campaign.checker_command())))
    emit('%s: %s' % (args.peer_host, shlex.join(['cat', '/sys/class/infiniband/%s/ports/1/gid_attrs/types/%d'
                                                 % (args.peer_device, args.peer_gid_index)])))
    emit('%s (long-lived): %s' % (args.peer_host, shlex.join(campaign.peer_command())))
    emit('%s: %s' % (args.mac_host, shlex.join(campaign.mac_command('rtr', 'clean', 0))))
    emit('%s: %s' % (args.mac_host, shlex.join(campaign.ioreg_command())))
    emit('%s: %s' % (args.mac_host, shlex.join(campaign.mac_command('mapped', 'clean', 1000000))))
    for index, (state, mode) in enumerate(pairs, 1):
        emit('# cycle %d stop=%s mode=%s' % (index, state, mode))
        emit('%s: %s' % (args.mac_host, shlex.join(campaign.mac_command(state, mode, index))))
        if mode == 'hang':
            emit('%s: kill -KILL <pid from PID line>' % args.mac_host)
        emit('%s: kill -0 <pid>  # must fail within %ss' % (args.mac_host, args.linger_timeout))
        if campaign.checker_command():
            emit('%s: %s' % (args.mac_host, shlex.join(campaign.checker_command())))
        emit('%s: %s' % (args.mac_host, shlex.join(campaign.ioreg_command())))
        if args.health_every and index % args.health_every == 0:
            emit('%s: %s' % (args.mac_host, shlex.join(campaign.mac_command('mapped', 'clean', 1000000 + index))))
    emit('# results: %s and %s' % (args.output, args.output.with_suffix('.summary.json')))
    return 0


def fake_arguments(output, **overrides):
    """Argument set for the fake transport; names are placeholders, not hosts."""
    values = ['--mac-host', 'fake-mac', '--peer-host', 'fake-peer', '--mac-client', '/fake/lifecycle-client',
              '--peer-client', '/fake/lifecycle-peer', '--mac-provider', '/fake/libmcdma-rdmav34.so',
              '--mac-checker', '/fake/cx5-native-check', '--mac-interface', 'fake-if0', '--peer-interface', 'fake-if1',
              '--mac-device', 'rdma_fake0', '--peer-device', 'fake_0', '--peer-gid-index', '1',
              '--output', str(output), '--line-timeout', '20']
    for key, value in overrides.items():
        values += ['--' + key.replace('_', '-'), str(value)]
    parser = build_parser()
    args = parser.parse_args(values)
    validate(parser, args)
    return args


def self_test(scratch=None, pairs=None, emit=print):
    """Walk every (state, mode) pair with fake endpoints, then prove stop-on-failure."""
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        base = Path(scratch or directory)
        pairs = pairs or [(state, mode) for state in STATES for mode in MODES]
        args = fake_arguments(base / 'self-test.jsonl', seed=1, cycles=len(pairs), health_every=5,
                              mac_cq_map=2, mac_user_post=1, mac_user_bf=64)
        campaign = Campaign(args, FakeTransport(), emit=emit)
        summary = campaign.run(pairs)
        if not summary['passed_all']:
            emit(json.dumps(summary, indent=2, default=str))
            raise SystemExit('SELF_TEST failed: all-pairs walk did not pass')
        hang = [record for record in campaign.records if record['mode'] == 'hang']
        if not hang or not all(record['mac'].get('killed_externally') for record in hang):
            raise SystemExit('SELF_TEST failed: hang cycles were not killed externally')
        args = fake_arguments(base / 'self-test-fail.jsonl', seed=2, cycles=6, health_every=1)
        failing = Campaign(args, FakeTransport(fail_checker_at=4), emit=emit)  # preflight uses call 1; cycle 3 uses call 4
        summary = failing.run(plan(6, 2))
        if summary['cycles_run'] != 3 or summary['first_failure'] is None or summary['first_failure']['cycle'] != 3 \
                or not summary['stopped_early']:
            emit(json.dumps(summary, indent=2, default=str))
            raise SystemExit('SELF_TEST failed: did not stop on the first health failure')
        emit('SELF_TEST ok: %d pair cycles passed with fake endpoints; stop-on-failure confirmed at cycle 3' % len(pairs))
        return 0


def main(argv=None):
    parser = build_parser()
    arguments = sys.argv[1:] if argv is None else list(argv)
    if '--self-test' in arguments:
        return self_test()
    args = parser.parse_args(arguments)
    validate(parser, args)
    pairs = plan(args.cycles, args.seed, args.state_list, args.mode_list)
    if args.dry_run:
        return dry_run(args, pairs)
    campaign = Campaign(args, SshTransport())
    summary = campaign.run(pairs)
    print(json.dumps({key: summary[key] for key in ('seed', 'cycles_planned', 'cycles_run', 'passed', 'failed',
                                                    'failures_by_pair', 'first_failure', 'stopped_early', 'abort',
                                                    'counters_total_delta', 'passed_all')}, default=str))
    return 0 if summary['passed_all'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
