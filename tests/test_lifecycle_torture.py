"""Offline checks for the lifecycle torture driver: sampling, log parsing,
counter deltas, stop-on-failure and dry-run output.  No SSH, RDMA or hardware;
the campaign tests drive local python stand-ins for both endpoints."""
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('lifecycle_torture', Path(__file__).resolve().parents[1] / 'tools/lifecycle_torture.py')
torture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(torture)

IOREG_SAMPLE = '''+-o MCDMACX5Native  <class MCDMACX5Native, id 0x100000abc, registered, matched, active, busy 0 (12 ms), retain 9>
    {
      "MCDMAPortActive" = Yes
      "MCDMAGidLive" = Yes
      "MCDMAGidAdds" = 3
      "MCDMAGidDeletes" = 1
      "MCDMAUarPageBytes" = 16384
      "MCDMATransport" = "hardware RoCE v2; polled RC; static IPv6 neighbours"
      "MCDMANativeBuild" = "26A428"
      "IOProbeScore" = 20000
    }
'''


class SamplingTests(unittest.TestCase):
    def test_seeded_plan_is_deterministic_and_valid(self):
        first, second = torture.plan(200, 7), torture.plan(200, 7)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 200)
        self.assertTrue(all(state in torture.STATES and mode in torture.MODES for state, mode in first))
        self.assertGreater(len(set(first)), 30)
        self.assertNotEqual(first, torture.plan(200, 8))

    def test_subset_and_rejection(self):
        pairs = torture.plan(20, 1, states=['rts', 'mapped'], modes=['hang'])
        self.assertTrue(all(state in ('rts', 'mapped') and mode == 'hang' for state, mode in pairs))
        with self.assertRaises(ValueError):
            torture.plan(1, 1, states=['reset'])
        with self.assertRaises(ValueError):
            torture.plan(1, 1, modes=['segv'])
        with self.assertRaises(ValueError):
            torture.plan(1, 1, states=[])


class ParsingTests(unittest.TestCase):
    def test_protocol_lines(self):
        self.assertEqual(torture.parse_line('STATE rts'), ('STATE', 'rts'))
        self.assertEqual(torture.parse_line('PID 4242'), ('PID', 4242))
        self.assertEqual(torture.parse_line('READY'), ('READY', None))
        kind, fields = torture.parse_line('WRITE status=0 bytes=4096')
        self.assertEqual((kind, fields), ('WRITE', {'status': 0, 'bytes': 4096}))
        kind, fields = torture.parse_line('PEER_HEALTH cycle=3 connected=1 query=0 qp_state=3 cur_state=3 destroy=0 probe_cq=1 probe_qp=1 probe_mr=1 ok=1')
        self.assertEqual(kind, 'PEER_HEALTH')
        self.assertEqual(fields['qp_state'], 3)
        self.assertEqual(fields['ok'], 1)
        kind, fields = torture.parse_line('FAIL stage=modify_rtr errno=22 text=Invalid')
        self.assertEqual(fields['stage'], 'modify_rtr')
        self.assertEqual(torture.parse_line('ENDPOINT 1 2 3 4 16384 fe80::1')[1], ['1', '2', '3', '4', '16384', 'fe80::1'])
        for bad in ('STATE reset', 'STATE', 'PID 0', 'PID x', 'READY now', 'WRITE status', 'BOGUS a=1', ''):
            with self.subTest(line=bad), self.assertRaises(ValueError):
                torture.parse_line(bad)

    def test_state_consistency(self):
        walk = torture.expected_states('rtr')
        self.assertEqual(walk, ['open', 'pd', 'mr', 'cq', 'qp', 'init', 'rtr'])
        self.assertEqual(torture.check_states('rtr', 'kill', walk, {'exit': -9}), [])
        self.assertTrue(any(p.startswith('state_not_reached') for p in torture.check_states('rtr', 'kill', walk[:-1], {})))
        self.assertTrue(any(p.startswith('state_overshoot') for p in torture.check_states('rtr', 'kill', walk + ['rts'], {})))
        clean_ok = {'exit': 0, 'TEARDOWN': {'error': 0, 'unmap_error': 0}}
        self.assertEqual(torture.check_states('rtr', 'clean', walk, clean_ok), [])
        self.assertTrue(any(p.startswith('teardown_failed') for p in torture.check_states('rtr', 'clean', walk, {'exit': 0})))
        self.assertTrue(any(p.startswith('clean_exit_code') for p in torture.check_states('rtr', 'clean', walk, dict(clean_ok, exit=3))))
        self.assertIn('hang_not_killed', torture.check_states('rtr', 'hang', walk, {}))
        self.assertEqual(torture.check_states('rtr', 'hang', walk, {'killed_externally': True}), [])
        full = torture.expected_states('polled')
        self.assertTrue(any(p.startswith('write_status') for p in torture.check_states('polled', 'kill', full, {'WRITE': {'status': 12, 'bytes': 0}})))
        self.assertTrue(any(p.startswith('client_failure') for p in torture.check_states('rtr', 'kill', walk, {'FAIL': {'stage': 'x', 'errno': 5}})))

    def test_provider_fallbacks(self):
        lines = ['MCDMA_CQ_OBSERVER mapped=0 reason=disabled', 'MCDMA_USER_POST enabled=1 uar_bytes=16384',
                 'MCDMA_USER_POST qp=5 queue_mapped=0 reason=mmap', 'BENCHMARK_CONFIG payload_bytes=4096 enabled=0']
        self.assertEqual(torture.provider_fallbacks(lines), ['MCDMA_USER_POST qp=5 queue_mapped=0 reason=mmap'])


class CounterTests(unittest.TestCase):
    def test_parse_ioreg_numbers_and_booleans_only(self):
        properties = torture.parse_ioreg(IOREG_SAMPLE)
        self.assertEqual(properties, {'MCDMAPortActive': True, 'MCDMAGidLive': True, 'MCDMAGidAdds': 3,
                                      'MCDMAGidDeletes': 1, 'MCDMAUarPageBytes': 16384, 'IOProbeScore': 20000})
        self.assertEqual(torture.parse_ioreg(''), {})

    def test_deltas_and_health(self):
        before = torture.parse_ioreg(IOREG_SAMPLE)
        after = dict(before, MCDMAGidAdds=5, MCDMAPortActive=False, MCDMAQuarantined=True)
        deltas = torture.counter_deltas(before, after)
        self.assertEqual(deltas, {'MCDMAGidAdds': 2, 'MCDMAPortActive': {'before': True, 'after': False},
                                  'MCDMAQuarantined': {'before': None, 'after': True}})
        self.assertEqual(torture.counter_deltas(before, before), {})
        self.assertEqual(torture.counter_health(before), [])
        self.assertEqual(torture.counter_health(after), ['port_not_active', 'driver_quarantined'])
        self.assertEqual(torture.counter_health({}), ['ioreg_no_properties'])


class CampaignTests(unittest.TestCase):
    def campaign(self, transport, cycles, seed=3, **overrides):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        args = torture.fake_arguments(Path(directory.name) / 'run.jsonl', seed=seed, cycles=cycles, **overrides)
        return torture.Campaign(args, transport, emit=lambda *_: None), args

    def test_fake_walk_covers_every_mode_and_records_lines(self):
        pairs = [('open', 'kill'), ('mapped', 'abort'), ('rts', 'exit-no-teardown'), ('polled', 'clean'), ('init', 'hang')]
        campaign, args = self.campaign(torture.FakeTransport(), len(pairs), health_every=2)
        summary = campaign.run(pairs)
        self.assertTrue(summary['passed_all'], summary)
        records = [json.loads(line) for line in args.output.read_text().splitlines()]
        self.assertEqual([(r['state'], r['mode']) for r in records], pairs)
        self.assertEqual(records[0]['reached'], ['open'])
        self.assertEqual(records[1]['reached'], list(torture.STATES))
        self.assertTrue(records[4]['mac']['killed_externally'])
        self.assertEqual(records[3]['peer']['verified_bytes'], 4096)
        self.assertIn('clean_cycle', records[1]['health'])
        self.assertNotIn('clean_cycle', records[0]['health'])
        self.assertTrue(all(r['health']['process_gone'] for r in records))
        written = json.loads(args.output.with_suffix('.summary.json').read_text())
        self.assertEqual(written['cycles_run'], 5)
        self.assertEqual(written['health']['clean_cycles'], 2)

    def test_stops_on_first_health_failure_unless_keep_going(self):
        pairs = torture.plan(5, 11)
        campaign, _ = self.campaign(torture.FakeTransport(fail_checker_at=3), len(pairs), health_every=0)
        summary = campaign.run(pairs)
        self.assertEqual(summary['cycles_run'], 2)
        self.assertTrue(summary['stopped_early'])
        self.assertEqual(summary['first_failure']['cycle'], 2)
        self.assertTrue(any(e.startswith('checker_failed') for e in summary['first_failure']['errors']))
        self.assertEqual(summary['health']['checker_failures'], 1)
        self.assertFalse(summary['passed_all'])
        campaign, _ = self.campaign(torture.FakeTransport(port_down_at=3), len(pairs), health_every=0)
        campaign.args.keep_going = True
        summary = campaign.run(pairs)
        self.assertEqual(summary['cycles_run'], 5)
        self.assertEqual(summary['failed'], 1)
        self.assertIn('port_not_active', summary['first_failure']['errors'])
        self.assertEqual(summary['failures_by_pair'], {pairs[1][0] + '/' + pairs[1][1]: 1})

    def test_provider_markers_enforced_on_clean_walk(self):
        campaign, _ = self.campaign(torture.FakeTransport(), 2, health_every=1, mac_cq_map=2, mac_user_post=1, mac_user_bf=64)
        summary = campaign.run([('rts', 'kill'), ('mapped', 'clean')])
        self.assertTrue(summary['passed_all'], summary)
        markers = campaign.records[1]['mac']['provider_markers']
        self.assertIn('MCDMA_USER_POST enabled=1 uar_bytes=16384', markers)
        self.assertEqual(summary['counters_total_delta'].get('MCDMAGidAdds'), summary['counters_final']['MCDMAGidAdds'])


class EntryPointTests(unittest.TestCase):
    ARGS = ['--mac-host', 'mac-example', '--peer-host', 'peer-example', '--mac-client', '/opt/mcdma/lifecycle-client',
            '--peer-client', '/opt/mcdma/lifecycle-peer', '--mac-provider', '/opt/mcdma/libmcdma-rdmav34.so',
            '--mac-checker', '/opt/mcdma/cx5-native-check', '--mac-interface', 'mcrdma9', '--peer-interface', 'enp9s0',
            '--mac-device', 'rdma_mcrdma9', '--peer-device', 'rocep9s0', '--peer-gid-index', '1']

    def test_dry_run_prints_plan_without_ssh(self):
        with tempfile.TemporaryDirectory() as directory:
            output = str(Path(directory) / 'plan.jsonl')
            with patch.object(torture.subprocess, 'Popen') as popen, patch.object(torture.subprocess, 'run') as run, \
                    patch('sys.stdout', new_callable=io.StringIO) as stdout:
                code = torture.main(self.ARGS + ['--cycles', '4', '--seed', '5', '--states', 'rts,mapped', '--modes', 'kill,hang',
                                                 '--mac-cq-map', '2', '--mac-user-post', '1', '--output', output, '--dry-run'])
            self.assertEqual(code, 0)
            popen.assert_not_called()
            run.assert_not_called()
            text = stdout.getvalue()
            self.assertEqual(text.count('# cycle '), 4)
            self.assertIn('MCDMA_USER_POST=1', text)
            self.assertIn('IBV_DRIVERS=/opt/mcdma/libmcdma', text)
            for state, mode in torture.plan(4, 5, ['rts', 'mapped'], ['kill', 'hang']):
                self.assertIn('--stop-at %s --exit-mode %s' % (state, mode), text)
            self.assertIn('cx5-native-check --provider /opt/mcdma/libmcdma-rdmav34.so --require-gid', text)
            self.assertIn('ioreg -r -c MCDMACX5Native -l', text)
            self.assertFalse(Path(output).exists())

    def test_argument_validation(self):
        for extra in (['--mac-user-post', '1'], ['--mac-user-bf', '64'], ['--cycles', '0'],
                      ['--states', 'reset'], ['--mac-device', 'bad device']):
            with self.subTest(extra=extra), self.assertRaises(SystemExit), patch('sys.stderr', new_callable=io.StringIO):
                torture.main(self.ARGS + ['--output', 'x.jsonl', '--dry-run'] + extra)

    def test_self_test_passes(self):
        pairs = [('open', 'kill'), ('cq', 'abort'), ('rtr', 'exit-no-teardown'), ('mapped', 'clean'), ('posted', 'hang'),
                 ('qp', 'hang'), ('polled', 'kill')]
        self.assertEqual(torture.self_test(pairs=pairs, emit=lambda *_: None), 0)


if __name__ == '__main__':
    unittest.main()
