import csv
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

EXE, INSTANCE = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])
sys.argv[1:] = []

class SearchTest(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([EXE, *map(str, args)], check=True, capture_output=True, text=True, timeout=15)

    def test_three_variants_are_feasible_and_reproducible(self):
        with tempfile.TemporaryDirectory() as d:
            for method in ('ga', 'multistart', 'ma'):
                outputs = []
                for rep in range(2):
                    sol = Path(d) / f'{method}-{rep}.sol'
                    log = sol.with_suffix('.csv')
                    self.run_cli('solve', INSTANCE, '--method', method, '--out', sol,
                                 '--steps', 30, '--seed', 17, '--log', log)
                    self.run_cli('validate', INSTANCE, sol)
                    with log.open(newline='') as f:
                        rows = list(csv.DictReader(f))
                    self.assertTrue(rows)
                    costs = [int(r['cost']) for r in rows]
                    times = [float(r['cpu_seconds']) for r in rows]
                    self.assertTrue(all(a > b for a, b in zip(costs, costs[1:])))
                    self.assertEqual(times, sorted(times))
                    self.assertEqual(costs[-1], int(sol.read_text().splitlines()[-1].split()[1]))
                    outputs.append((sol.read_bytes(), costs))
                self.assertEqual(*outputs)

if __name__ == '__main__':
    unittest.main()
