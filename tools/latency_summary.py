#!/usr/bin/env python3
"""Summarize measured CQ latency, retaining outliers and an optional target gate."""
import argparse
import collections
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics


def summarize(path):
    data = path.read_bytes()
    rows = list(csv.DictReader(data.decode().splitlines()))
    if not rows:
        raise ValueError('Empty latency trace')
    sizes={int(r['bytes']) for r in rows}
    if len(sizes)!=1 or not sizes <= {1024,4096}:
        raise ValueError('Trace must contain one supported payload size')
    payload_bytes=sizes.pop()
    result = {}
    for operation in ('write', 'read'):
        group = [r for r in rows if r.get('operation') == operation]
        if not group or [int(r['sample']) for r in group] != list(range(len(group))):
            raise ValueError('Missing, reordered or duplicate samples')
        values = sorted(int(r['completion_ns']) for r in group)
        if values[0] <= 0:
            raise ValueError('Nonpositive completion time')
        result[operation] = {
            'samples': len(values), 'bytes': payload_bytes,
            'median_us': statistics.median(values) / 1000,
            'p95_us': values[math.ceil(len(values) * .95) - 1] / 1000,
            'p99_us': values[math.ceil(len(values) * .99) - 1] / 1000,
            'min_us': values[0] / 1000, 'max_us': values[-1] / 1000,
            'below_25_us': sum(v < 25000 for v in values),
        }
        fields = {'post_ns', 'completion_wait_ns', 'poll_calls'}
        if fields & group[0].keys():
            for row in group:
                if (not fields <= row.keys() or
                        int(row['post_ns']) < 0 or int(row['completion_wait_ns']) < 0 or
                        int(row['post_ns']) + int(row['completion_wait_ns']) != int(row['completion_ns']) or
                        int(row['poll_calls']) < 1):
                    raise ValueError('Invalid timing decomposition')
            result[operation]['post_median_us'] = statistics.median(int(r['post_ns']) for r in group) / 1000
            result[operation]['wait_median_us'] = statistics.median(int(r['completion_wait_ns']) for r in group) / 1000
            result[operation]['poll_count_histogram'] = dict(sorted(collections.Counter(int(r['poll_calls']) for r in group).items()))
    if sum(r['samples'] for r in result.values()) != len(rows):
        raise ValueError('Unexpected operation in trace')
    return {'trace': str(path), 'sha256': hashlib.sha256(data).hexdigest(), 'operations': result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--max-median-us', type=float)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.max_median_us is not None and (not math.isfinite(args.max_median_us) or args.max_median_us <= 0):
        parser.error('The median target must be positive and finite')
    result = summarize(args.trace)
    passed = args.max_median_us is None or all(
        op['median_us'] <= args.max_median_us for op in result['operations'].values())
    result.update(max_median_us=args.max_median_us, median_target_passed=passed)
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    print(text, end='')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
