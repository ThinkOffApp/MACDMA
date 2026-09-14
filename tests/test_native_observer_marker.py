"""Exercise the real runner's acceptance gate, without hardware or SSH."""
import importlib.util
from pathlib import Path
import unittest

source=Path(__file__).resolve().parents[1]/'tools/native_cross_host.py'
spec=importlib.util.spec_from_file_location('native_cross_host', source)
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
ON='MCDMA_CQ_OBSERVER mapped=1 bytes=16384 readonly=1'
CONSUME='MCDMA_CQ_OBSERVER mapped=1 bytes=16384 readonly=1 consume=user'
RETIRED='MCDMA_CQ_CONSUME retired cq=3 reason=completion_mismatch'
OFF='MCDMA_CQ_OBSERVER mapped=0 reason=disabled'
FAILED='MCDMA_CQ_OBSERVER mapped=0 reason=mmap_failed'

def logs(marker,host='mac-test'):
    return [{'host':host,'endpoint_stderr':'diagnostic\n'+marker+'\n'}]

class ObserverMarker(unittest.TestCase):
    def test_user_post_gate(self):
        enabled='MCDMA_USER_POST enabled=1 uar_bytes=16384'
        queue='MCDMA_USER_POST qp=17 queue_mapped=1 bytes=16384'
        valid=enabled+'\n'+queue
        self.assertTrue(runner.user_post_marker(logs(valid),'mac-test','1'))
        self.assertFalse(runner.user_post_marker([],'mac-test','0'))
        for marker in ['',enabled,queue,valid+'\n'+queue,valid+'\nMCDMA_USER_POST enabled=0 reason=uar_mmap_failed',
                       valid.replace('16384','4096'),valid.replace('queue_mapped=1','queue_mapped=0')]:
            with self.subTest(marker=marker),self.assertRaises(ValueError):
                runner.user_post_marker(logs(marker),'mac-test','1')
        with self.assertRaises(ValueError):runner.user_post_marker(logs(valid,'linux-test'),'mac-test','1')
        with self.assertRaises(ValueError):runner.user_post_marker(logs(valid),'mac-test','0')
    def test_user_bf_gate(self):
        context='MCDMA_USER_BF mode=64 uar_wc=1'
        queue='MCDMA_USER_BF qp=17 bank_bytes=256 bytes=64 store=neon'
        valid=context+'\n'+queue
        self.assertTrue(runner.user_bf_marker(logs(valid),'mac-test','64'))
        self.assertTrue(runner.user_bf_marker(logs('MCDMA_USER_BF mode=128 uar_wc=1\nMCDMA_USER_BF qp=3 bank_bytes=256 bytes=128 store=scalar'),'mac-test','128'))
        self.assertTrue(runner.user_bf_marker(logs('MCDMA_USER_BF mode=64s uar_wc=1\nMCDMA_USER_BF qp=3 bank_bytes=128 bytes=64 store=scalar'),'mac-test','64s'))
        self.assertTrue(runner.user_bf_marker(logs('MCDMA_USER_BF mode=db uar_wc=1\nMCDMA_USER_BF qp=3 bank_bytes=256 bytes=0 store=doorbell'),'mac-test','db'))
        self.assertFalse(runner.user_bf_marker([],'mac-test','0'))
        # Other MCDMA lines of the same run never satisfy the BlueFlame gate.
        self.assertTrue(runner.user_bf_marker(logs('MCDMA_USER_POST enabled=1 uar_bytes=16384\n'+valid),'mac-test','64'))
        for marker in ['',context,queue,valid+'\n'+queue,
                       'MCDMA_USER_BF mode=64 uar_wc=0 reason=wc_mmap_failed\n'+queue.replace('bytes=64 store=neon','bytes=0 store=doorbell'),
                       valid.replace('mode=64','mode=128'),valid.replace('store=neon','store=scalar'),
                       valid.replace('bytes=64','bytes=128'),valid.replace('bank_bytes=256','bank_bytes=0'),
                       valid.replace('bank_bytes=256','bank_bytes=192'),
                       context+'\nMCDMA_USER_BF qp=17 bank_bytes=256 bytes=0 store=doorbell']:
            with self.subTest(marker=marker),self.assertRaises(ValueError):
                runner.user_bf_marker(logs(marker),'mac-test','64')
        # A 128-byte write fits a 128-byte bank; a 64-byte bank is not a bank.
        self.assertTrue(runner.user_bf_marker(logs('MCDMA_USER_BF mode=128 uar_wc=1\nMCDMA_USER_BF qp=3 bank_bytes=128 bytes=128 store=scalar'),'mac-test','128'))
        with self.assertRaises(ValueError):runner.user_bf_marker(logs('MCDMA_USER_BF mode=128 uar_wc=1\nMCDMA_USER_BF qp=3 bank_bytes=64 bytes=128 store=scalar'),'mac-test','128')
        with self.assertRaises(ValueError):runner.user_bf_marker(logs(valid,'linux-test'),'mac-test','64')
        with self.assertRaises(ValueError):runner.user_bf_marker(logs(valid),'mac-test','0')
        with self.assertRaises(ValueError):runner.user_bf_marker(logs(valid),'mac-test','32')
    def test_enabled(self):
        self.assertTrue(runner.observer_marker(logs(ON),'mac-test','1'))
    def test_disabled(self):
        self.assertFalse(runner.observer_marker(logs(OFF),'mac-test','0'))
    def test_consume(self):
        self.assertTrue(runner.observer_marker(logs(CONSUME),'mac-test','2'))
    def test_consume_marker_only_for_mode_two(self):
        for marker,mode in [(CONSUME,'1'),(ON,'2'),(CONSUME,'0')]:
            with self.subTest(marker=marker,mode=mode),self.assertRaises(ValueError):
                runner.observer_marker(logs(marker),'mac-test',mode)
    def test_retired_consumption_fails_run(self):
        with self.assertRaises(ValueError):runner.observer_marker(logs(CONSUME+'\n'+RETIRED),'mac-test','2')
    def test_missing_disabled_marker(self):
        with self.assertRaises(ValueError):runner.observer_marker([],'mac-test','0')
    def test_missing_enabled_marker(self):
        with self.assertRaises(ValueError):runner.observer_marker([],'mac-test','1')
    def test_wrong_arm(self):
        for marker,mode in [(ON,'0'),(OFF,'1'),(FAILED,'0'),(FAILED,'1')]:
            with self.subTest(marker=marker,mode=mode),self.assertRaises(ValueError):
                runner.observer_marker(logs(marker),'mac-test',mode)
    def test_wrong_host(self):
        with self.assertRaises(ValueError):runner.observer_marker(logs(ON,'linux-test'),'mac-test','1')
    def test_duplicate_and_contradictory(self):
        for marker in [ON+'\n'+ON,ON+'\n'+FAILED,OFF+'\n'+ON]:
            with self.subTest(marker=marker),self.assertRaises(ValueError):
                runner.observer_marker(logs(marker),'mac-test','1')
    def test_malformed(self):
        for marker in [ON+' extra',ON.replace('readonly=1','readonly=0'),ON.replace('16384','8192'),'prefix '+ON]:
            with self.subTest(marker=marker),self.assertRaises(ValueError):
                runner.observer_marker(logs(marker),'mac-test','1')

if __name__=='__main__':unittest.main()
