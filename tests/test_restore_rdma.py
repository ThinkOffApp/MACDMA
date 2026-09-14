"""Offline checks for interface selection and safe restore command construction."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('restore_rdma', Path(__file__).resolve().parents[1] / 'tools/restore-rdma.py')
restore = importlib.util.module_from_spec(spec)
spec.loader.exec_module(restore)

class RestoreTests(unittest.TestCase):
    # Synthetic locally administered addresses, unrelated to the test hardware.
    def test_eui64_and_scoped_neighbor(self):
        self.assertEqual(restore.link_local('02:00:00:00:00:01'), 'fe80::ff:fe00:1')
        commands = restore.plan('mcrdma7', '02:00:00:00:00:01', 'fe80::ff:fe00:2', '02:00:00:00:00:02')
        self.assertEqual(commands[0][3], 'fe80::ff:fe00:1')
        self.assertEqual(commands[2], ['/usr/sbin/ndp', '-s', 'fe80::ff:fe00:2%mcrdma7', '02:00:00:00:00:02'])

    def test_reject_management_or_injected_interface(self):
        for name in ('en0', 'mcrdma1;reboot', '-a', 'mcrdma'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                restore.plan(name, '02:00:00:00:00:01', 'fe80::ff:fe00:2', '02:00:00:00:00:02')

    def test_invalid_mac(self):
        for mac in ('00:00:00:00:00:00', 'ff:ff:ff:ff:ff:ff', '03:00:00:00:00:01', 'bad', '02:00:00:00:00:01;id'):
            with self.subTest(mac=mac), self.assertRaises(ValueError):
                restore.link_local(mac)

    def test_reject_unrelated_gid(self):
        for gid in ('::1', '2001:db8::2', 'fe80::ff:fe00:3', 'fe80::ff:fe00:2%en0'):
            with self.subTest(gid=gid), self.assertRaises(ValueError):
                restore.plan('mcrdma1', '02:00:00:00:00:01', gid, '02:00:00:00:00:02')

    def test_dry_run_never_invokes_host_commands(self):
        with patch.object(restore.subprocess, 'run') as run, patch.object(restore.subprocess, 'check_output') as read, patch('builtins.print'):
            result = restore.main(['--interface', 'mcrdma1', '--expected-mac', '02:00:00:00:00:01', '--peer-gid', 'fe80::ff:fe00:2', '--peer-mac', '02:00:00:00:00:02', '--dry-run'])
        self.assertEqual(result, 0)
        run.assert_not_called()
        read.assert_not_called()

if __name__ == '__main__':
    unittest.main()
