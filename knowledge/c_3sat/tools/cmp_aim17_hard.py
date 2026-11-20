#!/usr/bin/env python3
"""Compare 3sat inits and an optional DPLL solver on the 17 hard AIM-yes cases.

Usage:
  python3 tools/cmp_aim17_hard.py [--aim DIR] [--bin ./build/3sat] [--dpll ./build/dpll_solve]
                                  [--steps 100000] [--restarts 20] [--jobs 8]
The 17 instances are the AIM yes1 set previously unsolved by all starts at 100k×20.
"""
import argparse, json, os, subprocess, tempfile, time
from concurrent.futures import ThreadPoolExecutor, as_completed

NAMES = [
 'aim-50-1_6-yes1-1.cnf','aim-50-1_6-yes1-2.cnf',
 'aim-100-1_6-yes1-1.cnf','aim-100-1_6-yes1-2.cnf','aim-100-1_6-yes1-3.cnf','aim-100-1_6-yes1-4.cnf',
 'aim-100-2_0-yes1-1.cnf','aim-100-2_0-yes1-2.cnf','aim-100-2_0-yes1-4.cnf',
 'aim-200-1_6-yes1-1.cnf','aim-200-1_6-yes1-2.cnf','aim-200-1_6-yes1-3.cnf','aim-200-1_6-yes1-4.cnf',
 'aim-200-2_0-yes1-1.cnf','aim-200-2_0-yes1-2.cnf','aim-200-2_0-yes1-3.cnf','aim-200-2_0-yes1-4.cnf',
]
CONFIGS = [
    ('majority', []),
    ('terminator-fwd', ['--init','terminator','--terminator-order','forward']),
    ('terminator-rev', ['--init','terminator','--terminator-order','reverse']),
    ('terminator-rand', ['--init','terminator','--terminator-order','random']),
    ('likho', ['--init','likho']),
    ('gob-sign', ['--init','gob-sign']),
]


def run_3sat(binp, path, extra, steps, restarts):
    fd, outp = tempfile.mkstemp(prefix='h17_', suffix='.json', dir='/tmp')
    os.close(fd)
    try:
        cmd = [binp, path, '--steps', str(steps), '--restarts', str(restarts), '--out', outp] + extra
        t0 = time.time(); subprocess.run(cmd, capture_output=True, text=True); dt = time.time() - t0
        with open(outp) as f: j = json.load(f)
        return j.get('status'), j.get('steps'), j.get('best_unsatisfied'), dt
    finally:
        try: os.unlink(outp)
        except OSError: pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--aim', default='/tmp/opencode/cnf/ex/aim')
    ap.add_argument('--bin', default='./build/3sat')
    ap.add_argument('--dpll')
    ap.add_argument('--steps', type=int, default=100000)
    ap.add_argument('--restarts', type=int, default=20)
    ap.add_argument('--jobs', type=int, default=8)
    args = ap.parse_args()

    results = {}
    tasks = [(n,lbl,ex) for n in NAMES for lbl,ex in CONFIGS]
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_3sat, args.bin, os.path.join(args.aim,n), ex, args.steps, args.restarts): (n,lbl)
                for n,lbl,ex in tasks}
        for fut in as_completed(futs):
            n,lbl = futs[fut]; results.setdefault(n,{})[lbl] = fut.result()

    dpll_res = {}
    if args.dpll:
        for n in NAMES:
            path = os.path.join(args.aim, n)
            t0=time.time(); p=subprocess.run([args.dpll,path],capture_output=True,text=True); dt=time.time()-t0
            st='SAT' if 'STATUS SAT' in p.stdout else ('UNSAT' if 'STATUS UNSAT' in p.stdout else 'ERR')
            dpll_res[n]=(st,dt)

    labels = [c[0] for c in CONFIGS]
    hdr = 'instance'.ljust(22)
    if args.dpll: hdr += ' ' + 'dpll'.ljust(12)
    for lbl in labels: hdr += ' ' + lbl.ljust(18)
    print(hdr); print('-'*len(hdr))
    for name in NAMES:
        short = name.replace('aim-','').replace('.cnf','')
        row = short.ljust(22)
        if args.dpll:
            st,dt = dpll_res[name]; row += ' ' + f"{st}/{dt:.3f}s".ljust(12)
        for lbl in labels:
            st,steps,best,dt = results[name][lbl]
            cell = f"SAT s={steps} {dt:.2f}s" if st=='SAT' else f"UNK b={best} {dt:.2f}s"
            row += ' ' + cell.ljust(18)
        print(row)
    if args.dpll:
        print('\ndpll solved', sum(1 for n in NAMES if dpll_res[n][0]=='SAT'), '/', len(NAMES))
    for lbl in labels:
        s=sum(1 for n in NAMES if results[n][lbl][0]=='SAT')
        b1=sum(1 for n in NAMES if results[n][lbl][0]!='SAT' and results[n][lbl][2]==1)
        print(lbl,'solved',s,'best=1',b1)


if __name__ == '__main__':
    main()
