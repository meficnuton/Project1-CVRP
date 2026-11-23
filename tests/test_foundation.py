"""Black-box tests; the oracle enumerates cuts without using solver code."""
import itertools
import math
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

EXE = str(Path(sys.argv.pop(1)).resolve())
BASE = [(90, 0, 0, 0), (40, 1, 0, 2), (5, 9, 0, 2), (83, 10, 0, 2), (3, 11, 0, 2)]


class Foundation(unittest.TestCase):
    def setUp(self):
        scratch = os.environ.get('TMPDIR')
        d = tempfile.TemporaryDirectory(dir=scratch if scratch and Path(scratch).is_dir() else None)
        self.addCleanup(d.cleanup)
        self.root = Path(d.name)
        self.vrp = self.root / 'case.vrp'
        self.sol = self.root / 'case.sol'
        self.write_vrp(BASE, 6)

    def write_vrp(self, nodes, cap):
        self.vrp.write_text('NAME : mini-k1\nTYPE : CVRP\n'
                            f'DIMENSION : {len(nodes)}\nCAPACITY : {cap}\n'
                            'EDGE_WEIGHT_TYPE : EUC_2D\nNODE_COORD_SECTION\n'
                            + ''.join(f'{i} {x} {y}\n' for i, x, y, _ in nodes[::-1])
                            + 'DEMAND_SECTION\n' + ''.join(f'{i} {q}\n' for i, _, _, q in nodes)
                            + f'DEPOT_SECTION\n{nodes[0][0]}\n-1\nEOF\n', encoding='ascii')

    def cli(self, *args, rejects=None):
        self.assertTrue(Path(EXE).is_file(), 'cvrp CLI not built')
        p = subprocess.run([EXE, *map(str, args)], capture_output=True, text=True, timeout=10)
        if rejects is None:
            self.assertEqual(p.returncode, 0, p.stderr)
        else:
            self.assertNotEqual(p.returncode, 0, p.stdout)
            self.assertIn(rejects, p.stderr)
        return p.stdout

    def solve(self, method, tour=None):
        extra = [] if tour is None else ['--tour', ','.join(map(str, tour))]
        self.cli('solve', self.vrp, '--method', method, '--out', self.sol, *extra)
        self.cli('validate', self.vrp, self.sol)
        return int(self.sol.read_text(encoding='ascii').splitlines()[-1].split()[1])

    def test_hand_example_and_original_ids(self):
        self.assertEqual(self.solve('split', [40, 5, 83, 3]), 24)
        self.assertEqual(self.sol.read_text(encoding='ascii').splitlines(),
                         ['Route #1: 40', 'Route #2: 5 83 3', 'Cost 24'])
        self.assertEqual(self.solve('savings'), 24)

    def test_capacity_boundary_and_rounding(self):
        self.write_vrp([(50, 0, 0, 0), (1, 2, 2, 6)], 6)
        self.assertEqual(self.solve('split', [1]), 6)  # two edges individually rounded
        self.write_vrp([(50, 0, 0, 0), (1, 2, 2, 7)], 6)
        self.cli('solve', self.vrp, '--method', 'savings', '--out', self.sol, rejects='demand')

    def test_invalid_inputs(self):
        raw = self.vrp.read_text(encoding='ascii')
        for old, new, err in [('EUC_2D', 'EXPLICIT', 'EDGE_WEIGHT_TYPE'),
                              ('83 2', '5 2', 'duplicate'), ('90\n-1', '90\n40\n-1', 'depot'),
                              ('DIMENSION : 5', 'DIMENSION : 6', 'DIMENSION'),
                              ('CAPACITY : 6', 'CAPACITY : 6\nDISTANCE : 99', 'unsupported')]:
            self.vrp.write_text(raw.replace(old, new), encoding='ascii')
            self.cli('solve', self.vrp, '--method', 'savings', '--out', self.sol, rejects=err)
        self.vrp.write_text(raw, encoding='ascii')
        for tour in ('40,5,83', '40,5,83,83', '40,5,83,90', '40,5,83,3,'):
            self.cli('solve', self.vrp, '--method', 'split', '--out', self.sol,
                     '--tour', tour, rejects='tour')

    def test_total_cost_exceeds_int32(self):
        ids = list(range(1, 1101))
        self.write_vrp([(9000, 0, 0, 0)] + [(i, 1_000_000, 0, 1) for i in ids], 1)
        self.assertEqual(self.solve('split', ids), 2_200_000_000)

    def test_validator_rejections(self):
        for routes, cost, err in [(['40', '5 83'], 0, 'missing'),
                                  (['40', '5 83 3', '40'], 24, 'duplicate'),
                                  (['40', '5 83 3 99'], 24, 'unknown'),
                                  (['40', '5 90 83 3'], 24, 'depot'),
                                  (['40 5 83 3'], 22, 'capacity'),
                                  (['40', '5 83 3'], 25, 'cost'),
                                  (['', '40', '5 83 3'], 24, 'empty')]:
            self.sol.write_text(''.join(f'Route #{j}: {r}\n' for j, r in enumerate(routes, 1))
                                + f'Cost {cost}\n', encoding='ascii')
            self.cli('validate', self.vrp, self.sol, rejects=err)

    def test_cvrplib_mode_is_explicit(self):
        self.write_vrp([(1, 0, 0, 0), (3, 3, 0, 1), (2, 1, 0, 1)], 2)
        self.sol.write_text('Route #1: 1 2\nCost 6\n', encoding='ascii')
        self.cli('validate', self.vrp, self.sol, rejects='depot')
        self.cli('validate', self.vrp, self.sol, '--ids', 'cvrplib-x')
        self.sol.write_text('Route #1: 1 2\nCost 6.000000\n', encoding='ascii')
        self.cli('validate', self.vrp, self.sol, '--ids', 'cvrplib-x')
        self.sol.write_text('Route #1: 1 2\nCost 6.5\n', encoding='ascii')
        self.cli('validate', self.vrp, self.sol, '--ids', 'cvrplib-x', rejects='cost')
        self.sol.write_text('Route #1: 1 2\nCost 6.\n', encoding='ascii')
        self.cli('validate', self.vrp, self.sol, '--ids', 'cvrplib-x', rejects='cost')
        self.sol.write_text('Route #1: 2 3\nCost 6.0\n', encoding='ascii')
        self.cli('validate', self.vrp, self.sol, rejects='cost')

    def test_oracle(self):
        rng = random.Random(20261121)
        trials = 0
        for case in range(6):
            nodes = [(77, 0, 0, 0)] + [(10+i*3, rng.randint(-9, 9), rng.randint(-9, 9), rng.randint(1, 6))
                                        for i in range(3 + case % 4)]
            cap = 7 + case % 3
            self.write_vrp(nodes, cap)
            coords = {i: (x, y) for i, x, y, _ in nodes}
            demands = {i: q for i, _, _, q in nodes}
            ids = tuple(i for i, _, _, _ in nodes[1:])
            for perm in itertools.islice(itertools.permutations(ids), 24):
                best = math.inf
                for cuts in itertools.product([False, True], repeat=len(perm)-1):
                    ix = [0] + [j for j, cut in enumerate(cuts, 1) if cut] + [len(perm)]
                    routes = [perm[a:b] for a, b in itertools.pairwise(ix)]
                    if all(sum(demands[i] for i in r) <= cap for r in routes):
                        cost = sum(math.floor(math.dist(coords[i], coords[j]) + .5)
                                   for r in routes for i, j in itertools.pairwise((77, *r, 77)))
                        best = min(best, cost)
                self.assertEqual(self.solve('split', perm), best, perm)
                trials += 1
        self.assertEqual(trials, 108)
        print(f'Oracle agreed on {trials} fixed permutations')


if __name__ == '__main__':
    unittest.main()
