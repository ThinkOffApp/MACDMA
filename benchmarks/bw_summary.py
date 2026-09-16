#!/usr/bin/env python3
"""Summarise mcdma-bw CSVs into a markdown table with tunnel efficiency.

Measured initiator rows (warmup=0, side=initiator) are grouped by
(initiator, op, bytes, depth, qps, cq_per_qp); repeats give the median, minimum
and maximum goodput. Efficiency is only computed against an explicit
--ceiling-gbit, the measured or specified ceiling of the Thunderbolt PCIe
tunnel; no ceiling figure is built in.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys

KEY = ('initiator', 'op', 'bytes', 'depth', 'qps', 'cq_per_qp')
REQUIRED = {'side', 'warmup', 'op', 'bytes', 'depth', 'depth_effective', 'qps', 'cq_per_qp', 'total_bytes',
            'seconds', 'gbit', 'completions', 'errors', 'cpu_pct', 'responder_cpu_pct', 'verified_bytes',
            'mismatches', 'finish'}


def load_rows(paths):
    rows, sources = [], []
    for path in paths:
        data = Path(path).read_bytes()
        sources.append({'file': str(path), 'sha256': hashlib.sha256(data).hexdigest()})
        for row in csv.DictReader(data.decode().splitlines()):
            missing = REQUIRED - row.keys()
            if missing:
                raise ValueError(f'{path}: missing columns {sorted(missing)}')
            row.setdefault('initiator', 'unknown')
            row.setdefault('mode_confirmed', '1')
            rows.append(row)
    return rows, sources


def measured(rows):
    return [row for row in rows if row['side'] == 'initiator' and row['warmup'] == '0']


def efficiency_pct(gbit, ceiling):
    if ceiling is None:
        return None
    return gbit / ceiling * 100


def summarize(rows, ceiling=None):
    groups = {}
    for row in measured(rows):
        key = tuple(row[k] for k in KEY)
        groups.setdefault(key, []).append(row)
    summary = []
    for key in sorted(groups, key=lambda k: (k[0], k[1], int(k[2]), int(k[3]), int(k[4]), int(k[5]))):
        group = groups[key]
        gbits = [float(r['gbit']) for r in group]
        if any(not math.isfinite(g) or g < 0 for g in gbits):
            raise ValueError('Non-finite or negative goodput')
        entry = dict(zip(KEY, key))
        entry.update({
            'trials': len(group),
            'median_gbit': statistics.median(gbits), 'min_gbit': min(gbits), 'max_gbit': max(gbits),
            'median_seconds': statistics.median(float(r['seconds']) for r in group),
            'total_bytes': max(int(r['total_bytes']) for r in group),
            'depth_effective': min(int(r['depth_effective']) for r in group),
            'initiator_cpu_pct': statistics.median(float(r['cpu_pct']) for r in group),
            'responder_cpu_pct': statistics.median(float(r['responder_cpu_pct']) for r in group),
            'verified_bytes_min': min(int(r['verified_bytes']) for r in group),
            'mismatches': sum(int(r['mismatches']) for r in group),
            'errors': sum(int(r['errors']) for r in group),
            'finish': ','.join(sorted({r['finish'] for r in group})),
            'mode_confirmed': all(r.get('mode_confirmed', '1') == '1' for r in group),
            'efficiency_pct': efficiency_pct(statistics.median(gbits), ceiling),
        })
        summary.append(entry)
    return summary


def markdown(summary, ceiling=None):
    lines = ['| initiator | op | bytes | depth (eff.) | qps | cq | trials | median Gbit/s | min | max | efficiency | init CPU % | resp CPU % | verified B | mismatches | errors | finish |',
             '|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|']
    for e in summary:
        eff = 'n/a' if e['efficiency_pct'] is None else f"{e['efficiency_pct']:.1f}%"
        flag = '' if e['mode_confirmed'] else ' (mode unconfirmed)'
        lines.append(f"| {e['initiator']} | {e['op']} | {e['bytes']} | {e['depth']} ({e['depth_effective']}) | {e['qps']} | "
                     f"{'per-qp' if e['cq_per_qp'] == '1' else 'shared'} | {e['trials']} | {e['median_gbit']:.3f} | "
                     f"{e['min_gbit']:.3f} | {e['max_gbit']:.3f} | {eff} | {e['initiator_cpu_pct']:.0f} | {e['responder_cpu_pct']:.0f} | "
                     f"{e['verified_bytes_min']} | {e['mismatches']} | {e['errors']} | {e['finish']}{flag} |")
    if ceiling is None:
        lines.append('')
        lines.append('Efficiency requires `--ceiling-gbit`, the Thunderbolt PCIe tunnel ceiling for this setup; none is assumed.')
    else:
        lines.append('')
        lines.append(f'Efficiency = median goodput / {ceiling:g} Gbit/s ceiling (given by `--ceiling-gbit`, not measured here).')
    lines.append('Goodput is initiator-timed from the first post to the receiver-visible finish marker completion; CPU % is process user+system time over wall time of the trial, and a polling loop reports high values by design.')
    return '\n'.join(lines) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('paths', nargs='+', type=Path, help='CSV files or directories of CSVs from run_bw.py')
    parser.add_argument('--ceiling-gbit', type=float, help='tunnel ceiling in Gbit/s for the efficiency column')
    parser.add_argument('--output', type=Path, help='write the markdown table here as well')
    parser.add_argument('--json', type=Path, help='write the numeric summary here')
    args = parser.parse_args(argv)
    if args.ceiling_gbit is not None and (not math.isfinite(args.ceiling_gbit) or args.ceiling_gbit <= 0):
        parser.error('The ceiling must be positive and finite')
    files = []
    for path in args.paths:
        files.extend(sorted(path.glob('*.csv')) if path.is_dir() else [path])
    if not files:
        parser.error('No CSV files found')
    rows, sources = load_rows(files)
    summary = summarize(rows, args.ceiling_gbit)
    if not summary:
        print('No measured initiator rows found', file=sys.stderr)
        return 1
    text = markdown(summary, args.ceiling_gbit)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({'ceiling_gbit': args.ceiling_gbit, 'sources': sources, 'summary': summary}, indent=2) + '\n')
    print(text, end='')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
