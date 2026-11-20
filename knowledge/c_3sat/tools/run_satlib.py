#!/usr/bin/env python3
"""Run 3sat over every .cnf under a directory tree and summarize by subdir.

Usage:
  python3 tools/run_satlib.py [DIR] [--bin ./build/3sat] [--steps N] [--restarts N]
                              [--jobs J] [extra 3sat args...]
Defaults: DIR=/tmp/opencode/cnf/ex, bin=./build/3sat, budget = solver defaults.
UNKNOWN = budget exhausted (not proven UNSAT).
"""
import argparse, json, os, subprocess, tempfile
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed


def run_one(binp, path, root, extra):
    rel = os.path.relpath(path, root)
    d = rel.split(os.sep, 1)[0]
    fd, outp = tempfile.mkstemp(prefix='sl_', suffix='.json', dir='/tmp')
    os.close(fd)
    try:
        subprocess.run([binp, path, '--out', outp] + extra, capture_output=True, text=True)
        try:
            with open(outp) as f:
                j = json.load(f)
            st = j.get('status', 'ERR')
        except Exception:
            st = 'ERR'
        return d, rel, st
    finally:
        try:
            os.unlink(outp)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', nargs='?', default='/tmp/opencode/cnf/ex')
    ap.add_argument('--bin', default='./build/3sat')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--steps', type=int)
    ap.add_argument('--restarts', type=int)
    args, rest = ap.parse_known_args()
    extra = list(rest)
    if args.steps is not None:
        extra += ['--steps', str(args.steps)]
    if args.restarts is not None:
        extra += ['--restarts', str(args.restarts)]

    files = []
    for dp, _, fns in os.walk(args.dir):
        for fn in fns:
            if fn.endswith('.cnf'):
                files.append(os.path.join(dp, fn))
    files.sort()
    total = len(files)
    print('TOTAL', total, flush=True)

    counts = defaultdict(lambda: defaultdict(int))
    done = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_one, args.bin, f, args.dir, extra): f for f in files}
        for fut in as_completed(futs):
            d, rel, st = fut.result()
            counts[d][st] += 1
            done += 1
            if done % 200 == 0 or done == total:
                print(f'PROGRESS {done}/{total}', flush=True)

    print('\nSUMMARY')
    grand = defaultdict(int)
    for d in sorted(counts):
        row = counts[d]
        parts = ' '.join(f'{k}={row[k]}' for k in sorted(row))
        print(f'{d}: {parts}')
        for k, v in row.items():
            grand[k] += v
    print('ALL:', ' '.join(f'{k}={grand[k]}' for k in sorted(grand)))


if __name__ == '__main__':
    main()
