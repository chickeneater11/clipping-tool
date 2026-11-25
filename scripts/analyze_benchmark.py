"""Analyze our PresentMon v1 CSV and sampled process resources; standard library only."""
import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lo, hi = math.floor(position), math.ceil(position)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (position - lo)


def frame_summary(rows, pid, swapchain=None):
    groups = defaultdict(list)
    discarded = 0
    for row in rows:
        if int(row['ProcessID']) != pid:
            continue
        try:
            value = float(row['MsBetweenPresents'])
        except (ValueError, TypeError):
            discarded += 1
            continue
        if not math.isfinite(value) or value <= 0:
            discarded += 1
            continue
        groups[row['SwapChainAddress']].append(value)
    if not groups or (swapchain is not None and swapchain not in groups):
        raise ValueError('No valid frame intervals for the requested process/swapchain')
    chosen = swapchain if swapchain is not None else max(groups, key=lambda key: len(groups[key]))
    values = groups[chosen]
    slowest = sorted(values, reverse=True)[:max(1, math.ceil(len(values) * .01))]
    return dict(swapchain=chosen, swapchain_sample_counts={k: len(v) for k, v in groups.items()},
                valid_intervals=len(values), discarded_intervals=discarded,
                interval_seconds=sum(values) / 1000, average_fps=1000 / statistics.mean(values),
                one_percent_low_fps=1000 / statistics.mean(slowest),
                p95_ms=percentile(values, .95), p99_ms=percentile(values, .99))


def resource_summary(rows):
    groups = defaultdict(list)
    for row in rows:
        groups[row['role']].append(row)
    result = {}
    for role, samples in groups.items():
        result[role] = {'samples': len(samples)}
        for column, scale in [('private_bytes', 1048576), ('working_set_bytes', 1048576), ('cpu_percent', 1)]:
            values = [float(r[column]) / scale for r in samples if r[column] != '']
            if any(not math.isfinite(v) or v < 0 for v in values):
                raise ValueError('Invalid resource measurement')
            name = column.replace('_bytes', '_mib')
            result[role][name] = {'median': statistics.median(values), 'peak': max(values)} if values else None
        ordered = sorted(samples, key=lambda r: float(r['elapsed_s']))
        times = [float(r['elapsed_s']) for r in ordered]
        if any(not math.isfinite(t) or t < 0 for t in times):
            raise ValueError('Invalid resource timestamp')
        result[role]['max_sample_gap_s'] = max((b-a for a,b in zip(times, times[1:])), default=0)
        # Window medians describe growth; they do not diagnose a leak.
        if times[-1] - times[0] >= 60:
            first = [float(r['private_bytes']) / 1048576 for r in ordered if float(r['elapsed_s']) <= times[0]+30]
            last = [float(r['private_bytes']) / 1048576 for r in ordered if float(r['elapsed_s']) >= times[-1]-30]
            result[role]['private_mib_last_minus_first_30s'] = statistics.median(last)-statistics.median(first)
    return result


def read_csv(path, required):
    with path.open(encoding='utf-8-sig', newline='') as stream:
        reader = csv.DictReader(stream)
        if not required.issubset(reader.fieldnames or []):
            raise ValueError(f'{path.name}: missing columns {sorted(required)}; use the capture script with --v1_metrics')
        return list(reader)


def analyze(folder, swapchain=None):
    folder = Path(folder)
    meta = json.loads((folder / 'run.json').read_text(encoding='utf-8-sig'))
    if meta.get('schema') != 1 or meta.get('status') != 'complete' or meta.get('metric') != 'MsBetweenPresents':
        raise ValueError('Run is incomplete or uses an unsupported schema/metric')
    frames = frame_summary(read_csv(folder / 'frames.csv', {'ProcessID', 'SwapChainAddress', 'MsBetweenPresents'}), meta['game_pid'], swapchain)
    resources = resource_summary(read_csv(folder / 'resources.csv', {'role', 'elapsed_s', 'private_bytes', 'working_set_bytes', 'cpu_percent'}))
    required = {'game'} if meta['mode'] == 'baseline' else {'game', 'recorder'}
    if not required.issubset(resources):
        raise ValueError('Missing process resource samples')
    warnings = []
    if len(frames['swapchain_sample_counts']) > 1:
        warnings.append('Multiple swapchains: selected largest unless explicitly overridden; verify it is gameplay.')
    if frames['interval_seconds'] < meta['seconds'] * .9:
        warnings.append('Frame intervals cover less than 90% of requested duration; inspect capture before comparing.')
    if any(r['max_sample_gap_s'] > 2.5 for r in resources.values()):
        warnings.append('Resource sampling gaps exceed 2.5 seconds; sampled peaks may miss spikes.')
    return dict(folder=str(folder), metadata=meta, frames=frames, resources=resources, warnings=warnings)


def compare(baseline, recording):
    a, b = baseline['metadata'], recording['metadata']
    if a['mode'] != 'baseline' or b['mode'] not in ('recording', 'recording-saves'):
        raise ValueError('Comparison requires baseline first, then recording or recording-saves')
    for key in ('scenario', 'game_name', 'seconds', 'presentmon_sha256', 'metric', 'logical_processors'):
        if a[key] != b[key]:
            raise ValueError(f'Comparison settings differ: {key}')
    if baseline['warnings'] or recording['warnings']:
        raise ValueError('Resolve run warnings before comparing; inspect swapchain selection and capture coverage')
    af, bf = baseline['frames'], recording['frames']
    return dict(average_fps_loss_percent=100 * (1-bf['average_fps']/af['average_fps']),
                one_percent_low_loss_percent=100 * (1-bf['one_percent_low_fps']/af['one_percent_low_fps']),
                p99_increase_ms=bf['p99_ms']-af['p99_ms'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runs', nargs='+', help='One run, or baseline followed by recording')
    parser.add_argument('--swapchain', help='Explicit swapchain address (single-run analysis only)')
    args = parser.parse_args()
    if len(args.runs) not in (1, 2) or (args.swapchain and len(args.runs) != 1):
        parser.error('Provide one run or a baseline/recording pair; --swapchain requires one run')
    try:
        runs = [analyze(path, args.swapchain) for path in args.runs]
        report = {'runs': runs}
        if len(runs) == 2:
            report['comparison'] = compare(*runs)
        print(json.dumps(report, indent=2, allow_nan=False))
    except (ValueError, KeyError, OSError, TypeError) as exc:
        parser.exit(1, f'Cannot analyze: {exc}\n')


if __name__ == '__main__':
    main()
