"""Contract counterexamples for the runtime parser, independent log fixtures."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

path = Path(__file__).with_name('check_i3_logs.py')
spec = importlib.util.spec_from_file_location('judge', path)
judge = importlib.util.module_from_spec(spec); spec.loader.exec_module(judge)
ADMIT = ('[INFO] [admit] op=BUILD path=single gpu=0 db=1 idx=2 algo=auto shape_class=checked '
         'auto_probe=na n=10 dim=8 gd=4 igd=8 final=80 scratch=20 needed=100 limit=1000 budget=1000 '
         'pending_before=0 pending_after=0 quota=1000 headroom_raw=1000 free_net=1000 pool_used=0 '
         'logical_used=0 evicted=0 verdict=reject reason=AR6 res_bytes=0 res_n=0 lru_free=0 '
         'elig_free=0 pin_bytes=0 policy=managed overcommit=off reserved=100 phys_free=100 '
         'lw_ub=200 lw_pending=0 host_guard=50 self_res=0 mono_us=1 '
         'pool_need=100 headroom=0 pool_quota=1000 pending=0')
# AR8: pending 300 + pool_used 500 + pool_need 100 + headroom 101 = 1001 > 1000.
# pool_used is the existing [admit] field (the AR8 snapshot); free_net and
# headroom_raw follow so the free_net identity still holds.
AR8 = (ADMIT.replace('reason=AR6', 'reason=AR8').replace('pool_used=0 ', 'pool_used=500 ')
       .replace('headroom=0 pool_quota=1000 pending=0', 'headroom=101 pool_quota=1000 pending=300'))
HNSW = ('[INFO] [hnsw_route] phase=forecast requested_algo=auto route=ace_disk overflow=ace '
        'overflowed=1 status=0 rows=100 est_host=200 est_gpu=300 host_budget=100 '
        'gpu_budget=100 partitions=2 disk_need=500')
OOM = ('[WARN] [build_oom] op=BUILD gpu=0 db=1 idx=2 needed=800 '
       'action=fail_no_evict mono_us=1 failed_bytes=4096')

class Logs(unittest.TestCase):
    def test_only_logger_prefix_at_start_is_consumed(self):
        self.assertEqual(judge.parse_line(OOM)[0], 'build_oom')
        for text in ('fixture says ' + OOM, 'test output: ' + HNSW,
                     OOM.removeprefix('[WARN] '), '  ' + OOM,
                     '[DEBUG] description: ' + OOM):
            with self.subTest(text=text):
                self.assertIsNone(judge.parse_line(text))
    def test_failed_upstream_bytes_and_unknown(self):
        self.assertEqual(judge.parse_line(OOM)[1]['failed_bytes'], '4096')
        self.assertEqual(judge.parse_line(OOM.replace('4096','-1'))[1]['failed_bytes'], '-1')
    def test_failed_bytes_missing_or_zero_rejected(self):
        for text in [OOM.replace(' failed_bytes=4096',''),
                     OOM.replace('failed_bytes=4096','failed_bytes=0'),
                     OOM.replace('failed_bytes=4096','failed_bytes=-2')]:
            with self.subTest(text=text),self.assertRaises(ValueError):
                judge.parse_line(text)
    def test_fixed_examples_and_hnsw_optional_error(self):
        self.assertEqual(judge.parse_line(ADMIT)[1]['reason'],'AR6')
        self.assertEqual(len(judge.parse_line(HNSW)[1]),13)
        bad=HNSW.replace('status=0','status=1')+' error=not enough regular disk'
        self.assertEqual(judge.parse_line(bad)[1]['error'],'not enough regular disk')
        self.assertEqual(len(judge.parse_line(bad)[1]),14)
    def test_old_12_fields_and_extra_field_rejected(self):
        for text in (HNSW.replace('phase=forecast ',''),HNSW+' unexpected=1'):
            with self.assertRaises(ValueError):judge.parse_line(text)
    def test_missing_reordered_and_wrong_reason(self):
        variants=[ADMIT.replace(' lw_pending=0',''),
                  ADMIT.replace('reserved=100 phys_free=100','phys_free=100 reserved=100'),
                  ADMIT.replace('needed=100','needed=101'),
                  ADMIT.replace('reason=AR6','reason=AR7'),
                  ADMIT.replace('evicted=0','evicted=1'),
                  ADMIT.replace('free_net=1000','free_net=999')]
        for text in variants:
            with self.subTest(text=text):
                with self.assertRaises(ValueError):judge.parse_line(text)
    def test_ar8_fields_and_order(self):
        self.assertEqual(judge.parse_line(AR8)[1]['reason'], 'AR8')
        # The four AR8 fields are appended after mono_us, in this order.
        self.assertEqual(list(judge.parse_line(AR8)[1])[-5:],
                         ['mono_us', 'pool_need', 'headroom', 'pool_quota', 'pending'])
        variants=[AR8.replace('headroom=101','headroom=100'),          # == quota admits
                  AR8.replace('pool_used=500 ','pool_used=499 '),       # pool_used counts
                  AR8.replace('pool_need=100','pool_need=300'),         # != needed (lw_ub added)
                  AR8.replace('evicted=0','evicted=1'),                 # evicted before AR8
                  AR8.replace(' pending=300',''),                       # field missing
                  AR8.replace('reason=AR8','reason=AR6')]               # AR6 while AR8 applies
        for text in variants:
            with self.subTest(text=text):
                with self.assertRaises(ValueError):judge.parse_line(text)
    def test_automatic_load_admit_forbidden(self):
        load=ADMIT.replace('op=BUILD path=single','op=LOAD path=reload').replace('verdict=reject reason=AR6','verdict=admit reason=none').replace('policy=managed overcommit=off','policy=na overcommit=na')
        with self.assertRaises(ValueError):judge.parse_line(load)
        self.assertEqual(judge.parse_line(load.replace('path=reload','path=load'))[0],'admit')
    def test_missing_families_never_pass(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'raw.log';p.write_text(ADMIT+'\n'+HNSW+'\n')
            result=judge.evaluate([p]);self.assertEqual(result['status'],'INCOMPLETE')
            self.assertIn('build_peak',result['missing'])

if __name__=='__main__':unittest.main()
