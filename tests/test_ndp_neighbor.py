"""Static neighbor checks use synthetic MACs and no network access."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


cross = load('ndp_cross', 'tools/native_cross_host.py')
bw = load('ndp_bw', 'benchmarks/run_bw.py')


class NeighborTests(unittest.TestCase):
    def check_both(self, output, expected):
        for module in (cross, bw.cross):
            with self.subTest(module=module.__name__, output=output):
                self.assertEqual(module.ndp_has_neighbor(
                    output, 'fe80::ff:fe00:a', 'test0', '02:00:00:00:00:0a'), expected)

    def test_padded_unpadded_and_uppercase(self):
        for mac in ('02:00:00:00:00:0a', '2:0:0:0:0:a', '02:0:00:0:00:0A'):
            self.check_both(f'fe80::ff:fe00:a%test0 {mac} test0 permanent R', True)
        self.check_both('fe80:0:0:0:0:ff:fe00:a 2:0:0:0:0:a test0 permanent S', True)

    def test_wrong_mac_cannot_match_a_suffix(self):
        self.check_both('fe80::ff:fe00:a%test0 12:0:0:0:0:a test0 permanent R', False)

    def test_requires_matching_gid_interface_and_static_entry(self):
        row = 'fe80::ff:fe00:a%test0 2:0:0:0:0:a test0 permanent R'
        for wrong in (row.replace('fe00:a', 'fe00:b'), row.replace('test0', 'test1'),
                      row.replace('permanent', '23h59m59s'), row.replace(' R', ' I')):
            self.check_both(wrong, False)

    def test_invalid_and_incomplete_octets_are_rejected(self):
        for mac in ('(incomplete)', '2:0:0:0:0', '002:0:0:0:0:a', '2:0:0:0:0:gg',
                    '2:0:0:0:0:a:0', 'x2:0:0:0:0:a'):
            self.check_both(f'fe80::ff:fe00:a {mac} test0 permanent R', False)
        self.check_both('', False)
        self.check_both('Neighbor Linklayer Address Netif Expire S Flags Prbs', False)

    def test_match_must_come_from_one_complete_row(self):
        self.check_both('fe80::ff:fe00:a 12:0:0:0:0:a test0 permanent R\n'
                        'fe80::ff:fe00:b 2:0:0:0:0:a test0 permanent R', False)
        self.check_both('Neighbor Linklayer Address Netif Expire S Flags Prbs\n'
                        'fe80::ff:fe00:a 2:0:0:0:0:a test0 permanent R', True)


if __name__ == '__main__':
    unittest.main()
