import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('benchmark', Path(__file__).resolve().parents[1] / 'scripts/analyze_benchmark.py')
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)


def frame(ms, chain='a', pid='42'):
    return dict(ProcessID=pid, SwapChainAddress=chain, MsBetweenPresents=str(ms))


class BenchmarkTests(unittest.TestCase):
    def test_constant_frames(self):
        result = b.frame_summary([frame(10)] * 100, 42)
        self.assertEqual(result['average_fps'], 100)
        self.assertEqual(result['one_percent_low_fps'], 100)
        self.assertEqual(result['p99_ms'], 10)

    def test_slowest_one_percent_and_percentile(self):
        result = b.frame_summary([frame(10)] * 99 + [frame(100)], 42)
        self.assertAlmostEqual(result['average_fps'], 1000 / 10.9)
        self.assertEqual(result['one_percent_low_fps'], 10)
        self.assertAlmostEqual(result['p99_ms'], 10.9)

    def test_processes_and_swapchains_never_mixed(self):
        rows = [frame(10)] * 3 + [frame(100, 'b')] * 2 + [frame(500, pid='99')] * 10
        self.assertEqual(b.frame_summary(rows, 42)['average_fps'], 100)
        self.assertEqual(b.frame_summary(rows, 42, 'b')['average_fps'], 10)

    def test_invalid_intervals(self):
        result = b.frame_summary([frame(v) for v in [0, -1, 'nan', 'inf', '', 10]], 42)
        self.assertEqual(result['discarded_intervals'], 5)
        with self.assertRaises(ValueError):
            b.frame_summary([frame(0)], 42)

    def test_resource_gaps_and_growth(self):
        rows = [dict(role='recorder', elapsed_s=str(t), private_bytes=str(m*1048576), working_set_bytes=str(1048576), cpu_percent='') for t,m in [(0,10),(30,10),(60,20),(90,20)]]
        r = b.resource_summary(rows)['recorder']
        self.assertIsNone(r['cpu_percent'])
        self.assertEqual(r['private_mib']['peak'], 20)
        self.assertEqual(r['private_mib_last_minus_first_30s'], 10)
        self.assertEqual(r['max_sample_gap_s'], 30)
        rows[0]['private_bytes'] = 'nan'
        with self.assertRaises(ValueError):
            b.resource_summary(rows)

    def test_incomplete_and_missing_schema(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'run.json').write_text(json.dumps(dict(schema=1, status='failed')))
            with self.assertRaises(ValueError):
                b.analyze(root)
            (root / 'frames.csv').write_text('FrameTime\n10\n')
            with self.assertRaises(ValueError):
                b.read_csv(root / 'frames.csv', {'MsBetweenPresents'})

    def test_full_run_and_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            meta = dict(schema=1, status='complete', metric='MsBetweenPresents', mode='baseline', game_pid=42, game_name='League of Legends', scenario='same scene', seconds=10, presentmon_sha256='abc', logical_processors=8)
            (root / 'run.json').write_text(json.dumps(meta))
            (root / 'frames.csv').write_text('ProcessID,SwapChainAddress,MsBetweenPresents\n' + '42,a,10\n'*1000)
            (root / 'resources.csv').write_text('role,elapsed_s,private_bytes,working_set_bytes,cpu_percent\n' + ''.join(f'game,{i},1048576,1048576,1\n' for i in range(10)))
            baseline = b.analyze(root)
            recording = json.loads(json.dumps(baseline))
            recording['metadata']['mode'] = 'recording'
            recording['frames']['average_fps'] = 90
            self.assertAlmostEqual(b.compare(baseline, recording)['average_fps_loss_percent'], 10)
            recording['metadata']['scenario'] = 'different'
            with self.assertRaises(ValueError):
                b.compare(baseline, recording)
            recording['metadata']['scenario'] = 'same scene'
            recording['warnings'] = ['partial capture']
            with self.assertRaises(ValueError):
                b.compare(baseline, recording)


if __name__ == '__main__':
    unittest.main()
