#!/usr/bin/env python3
"""Analyze Modbus bus gap patterns from the ESP32 /api/modbus/patterns endpoint.

Usage:
  # One-shot:
    curl -s --digest -u admin:Joba_Esp32-C40A24 http://192.168.1.173/api/modbus/patterns | python3 misc/analyze_patterns.py

  # Long-run collection (saves snapshots to a JSONL file):
    python3 misc/analyze_patterns.py --collect --interval 60 --duration 3600 --output misc/gap_data.jsonl

  # Analyze collected JSONL:
    python3 misc/analyze_patterns.py --analyze misc/gap_data.jsonl
"""

import argparse
import json
import sys
import time
import os
from datetime import datetime

# Device defaults
DEVICE_URL = "http://192.168.1.173"
AUTH_USER = "admin"
AUTH_PASS = "Joba_Esp32-C40A24"


def fetch_patterns(url, user, password):
    """Fetch /api/modbus/patterns using urllib with digest auth."""
    import urllib.request
    import urllib.error

    passman = urllib.request.HTTPPasswordMgrWithDefaultRealm()
    passman.add_password(None, url, user, password)
    auth_handler = urllib.request.HTTPDigestAuthHandler(passman)
    opener = urllib.request.build_opener(auth_handler)

    req = urllib.request.Request(f"{url}/api/modbus/patterns")
    resp = opener.open(req, timeout=5)
    return json.loads(resp.read())


def print_summary(d, file=sys.stdout):
    """Print a summary of patterns data."""
    uptime_s = d.get("uptimeMs", 0) / 1000

    print(f"\n{'='*60}", file=file)
    print(f"Uptime: {uptime_s:.0f}s ({uptime_s/60:.1f}min, {uptime_s/3600:.2f}hr)", file=file)

    # Byte stats
    bs = d.get("byteStats", {})
    print(f"\nByte Stats: {bs.get('totalBytes',0)} bytes, "
          f"{bs.get('validFrames',0)} valid / {bs.get('invalidFrames',0)} invalid frames, "
          f"{bs.get('bytesPerSec',0):.1f} B/s", file=file)

    # Transaction times
    tt = d.get("transactionTimes", {})
    if tt.get("count", 0) > 0:
        print(f"\nTransaction RTT: n={tt['count']}, "
              f"min={tt.get('minMs',0)}ms, max={tt.get('maxMs',0)}ms, "
              f"mean={tt.get('meanMs',0):.1f}ms, stddev={tt.get('stddevMs',0):.1f}ms", file=file)
        hist = tt.get("histogram", [])
        for b in hist:
            bar = '#' * min(b['count'], 60)
            print(f"  {b['label']:>12s}: {b['count']:5d} {bar}", file=file)

    # Gap histogram
    gh = d.get('gaps', {})
    if gh.get("count", 0) > 0:
        print(f"\nInter-frame Gaps: n={gh['count']}, "
              f"min={gh.get('minUs',0)}us, max={gh.get('maxUs',0)}us, "
              f"mean={gh.get('meanUs',0):.0f}us, stddev={gh.get('stddevUs',0):.0f}us", file=file)
        hist = gh.get("histogram", [])
        for b in hist:
            bar = '#' * min(b['count'], 60)
            print(f"  {b['label']:>12s}: {b['count']:5d} {bar}", file=file)

        # Highlight large gaps (>=50ms)
        large_count = sum(b['count'] for b in hist if any(
            x in b['label'] for x in ['50-100ms', '100-200ms', '200-500ms', '500ms-1s', '1-5s', '>=5s']))
        total = gh['count']
        print(f"\n  Large gaps (>=50ms): {large_count}/{total} = {100*large_count/total:.1f}%", file=file)

    # Detected cycle
    cycle = d.get('cycle', [])
    pos = d.get('cyclePosition', -1)
    print(f"\nDetected Cycle: {len(cycle)} steps, tracking pos: {pos}", file=file)

    # Step gaps - embedded in cycle entries as 'gap' field
    sg = [c.get('gap', {}) for c in cycle]
    has_gaps = any(g.get('count', 0) > 0 for g in sg)
    if has_gaps and cycle:
        print(f"\nCycle Step Gap Analysis (sorted by mean gap, descending):", file=file)
        print(f"  {'Step':>4s}  {'Unit':>4s}  {'FC':>2s}  {'Reg':>6s}  {'Qty':>3s}  "
              f"{'N':>5s}  {'Min':>6s}  {'Mean':>7s}  {'Max':>6s}  {'StdDev':>7s}  {'Visual'}", file=file)
        print(f"  {'----':>4s}  {'----':>4s}  {'--':>2s}  {'------':>6s}  {'---':>3s}  "
              f"{'-----':>5s}  {'------':>6s}  {'-------':>7s}  {'------':>6s}  {'-------':>7s}  {'------'}", file=file)

        ranked = sorted(enumerate(sg), key=lambda x: x[1].get('meanMs', 0), reverse=True)
        for i, g in ranked:
            if g.get('count', 0) == 0:
                continue
            c = cycle[i] if i < len(cycle) else {}
            mean = g.get('meanMs', 0)
            stddev = g.get('stddevMs', g.get('stddev', 0))
            bar_len = int(mean / 50)  # 50ms per char
            bar = '█' * min(bar_len, 40)
            print(f"  {i:4d}  {c.get('unitId','?'):>4}  {c.get('fc','?'):>2}  "
                  f"{c.get('startReg','?'):>6}  {c.get('qty','?'):>3}  "
                  f"{g.get('count',0):5d}  {g.get('minMs',0):5d}ms  {mean:6.1f}ms  "
                  f"{g.get('maxMs',0):5d}ms  {stddev:6.1f}ms  {bar}", file=file)

        # Identify best TX windows
        print(f"\n  === Best TX Windows (gaps > 100ms mean) ===", file=file)
        windows = [(i, g) for i, g in ranked
                   if g.get('meanMs', 0) > 100 and g.get('count', 0) >= 3]
        if windows:
            for i, g in windows:
                c = cycle[i] if i < len(cycle) else {}
                reliability = g.get('count', 0)
                min_gap = g.get('minMs', 0)
                print(f"  Step {i} (before unit={c.get('unitId','?')} reg={c.get('startReg','?')}): "
                      f"mean={g.get('meanMs',0):.0f}ms, min={min_gap}ms, n={reliability}", file=file)
                # Estimate if we can fit a request+response (typ 30-60ms at 9600)
                if min_gap > 60:
                    print(f"    -> ✓ Can likely fit a 9600-baud request+response ({min_gap}ms min gap)", file=file)
                elif min_gap > 30:
                    print(f"    -> ? Might fit a short request ({min_gap}ms min gap, tight)", file=file)
                else:
                    print(f"    -> ✗ Too tight even at minimum ({min_gap}ms min gap)", file=file)
        else:
            print(f"  (no gaps > 100ms with enough samples yet)", file=file)

    # Pattern entries summary
    entries = d.get("entries", [])
    if entries:
        print(f"\nRegister Patterns ({len(entries)} unique):", file=file)
        for e in sorted(entries, key=lambda x: x.get('count', 0), reverse=True):
            interval = e.get('interval', {})
            sg = e.get('successorGap', {})
            line = (f"  unit={e['unitId']} fc={e['fc']} reg={e['startReg']:>5d} qty={e['qty']:<3d} "
                    f"count={e['count']:4d}  interval: mean={interval.get('meanMs',0):.0f}ms "
                    f"stddev={interval.get('stddevMs',0):.0f}ms")
            if sg.get('count', 0) > 0:
                line += (f"  | gap-after: mean={sg.get('meanMs',0):.0f}ms "
                         f"min={sg.get('minMs',0)}ms max={sg.get('maxMs',0)}ms n={sg['count']}")
            print(line, file=file)

    # Successor gap ranking (key for TX window prediction!)
    entries_with_gaps = [e for e in entries if e.get('successorGap', {}).get('count', 0) >= 3]
    if entries_with_gaps:
        print(f"\n{'='*60}", file=file)
        print(f"=== TX Window Prediction (by successor gap after each register) ===", file=file)
        print(f"  'gap-after' = idle time between this transaction's response and the next request", file=file)
        print(file=file)
        ranked = sorted(entries_with_gaps, key=lambda x: x['successorGap']['meanMs'], reverse=True)
        for e in ranked:
            sg = e['successorGap']
            mean = sg['meanMs']
            minMs = sg['minMs']
            bar = '█' * min(int(mean / 20), 50)
            feasibility = ""
            if minMs > 60:
                feasibility = " ✓ SAFE for TX"
            elif minMs > 30:
                feasibility = " ? TIGHT for TX"
            else:
                feasibility = " ✗ too short"
            print(f"  unit={e['unitId']} fc={e['fc']} reg={e['startReg']:>5d} qty={e['qty']:<3d} | "
                  f"mean={mean:6.0f}ms min={minMs:4d}ms max={sg['maxMs']:4d}ms "
                  f"n={sg['count']:4d} {bar}{feasibility}", file=file)

    # Transitions 
    transitions = d.get("transitions", [])
    if transitions:
        print(f"\n{'='*60}", file=file)
        print(f"=== Transition Map (what follows what, with gap stats) ===", file=file)
        for t in transitions:
            fr = t.get('from', {})
            from_label = f"unit={fr.get('unit','?')} fc={fr.get('fc','?')} reg={fr.get('reg','?')}"
            tos = t.get('to', [])
            total = sum(s.get('count', 0) for s in tos)
            if total == 0:
                continue
            print(f"\n  {from_label} ({total} total):", file=file)
            for s in sorted(tos, key=lambda x: x.get('count', 0), reverse=True):
                pct = 100 * s['count'] / total
                gap = s.get('gap', {})
                gap_str = ""
                if gap:
                    gap_str = (f"  gap: mean={gap.get('meanMs',0):.0f}ms "
                              f"min={gap.get('minMs',0)}ms max={gap.get('maxMs',0)}ms "
                              f"std={gap.get('stddevMs',0):.0f}ms")
                    # Flag if this transition provides a TX window
                    min_gap = gap.get('minMs', 0)
                    if min_gap >= 60:
                        gap_str += " ✓TX"
                    elif min_gap >= 30:
                        gap_str += " ?TX"
                print(f"    → unit={s.get('unit','?')} fc={s.get('fc','?')} "
                      f"reg={s.get('reg','?')} qty={s.get('qty','?')}  "
                      f"({pct:4.1f}%, n={s['count']}){gap_str}", file=file)


def collect(args):
    """Long-run collection: periodically fetch and save patterns data."""
    outfile = args.output or "misc/gap_data.jsonl"
    interval = args.interval
    duration = args.duration
    url = args.url or DEVICE_URL
    user = args.user or AUTH_USER
    password = args.password or AUTH_PASS

    os.makedirs(os.path.dirname(outfile) or ".", exist_ok=True)

    start = time.time()
    end = start + duration if duration > 0 else float('inf')
    n = 0

    print(f"Collecting patterns from {url} every {interval}s", file=sys.stderr)
    if duration > 0:
        print(f"Duration: {duration}s ({duration/60:.0f}min, {duration/3600:.1f}hr)", file=sys.stderr)
    else:
        print(f"Duration: indefinite (Ctrl+C to stop)", file=sys.stderr)
    print(f"Output: {outfile}", file=sys.stderr)

    try:
        with open(outfile, 'a') as f:
            while time.time() < end:
                try:
                    data = fetch_patterns(url, user, password)
                    data['_collectedAt'] = datetime.utcnow().isoformat() + 'Z'
                    data['_elapsed'] = time.time() - start
                    json.dump(data, f)
                    f.write('\n')
                    f.flush()
                    n += 1

                    elapsed = time.time() - start
                    uptime = data.get('uptimeMs', 0) / 1000
                    gaps = data.get('gapHistogram', {}).get('count', 0)
                    cycle_len = len(data.get('cycle', []))
                    step_gaps = sum(1 for c in data.get('cycle', []) if c.get('gap', {}).get('count', 0) > 0)
                    pos = data.get('cyclePosition', -1)

                    print(f"[{n:4d}] {elapsed:.0f}s elapsed | "
                          f"uptime={uptime:.0f}s | gaps={gaps} | "
                          f"cycle={cycle_len} steps, pos={pos} | "
                          f"stepGaps={step_gaps}",
                          file=sys.stderr)

                    # Print summary every 10 samples
                    if n % 10 == 0:
                        print_summary(data, file=sys.stderr)

                except Exception as e:
                    print(f"[{n+1}] ERROR: {e}", file=sys.stderr)

                remaining = end - time.time()
                sleep_time = min(interval, remaining) if duration > 0 else interval
                if sleep_time > 0:
                    time.sleep(sleep_time)

    except KeyboardInterrupt:
        print(f"\nStopped after {n} samples, {time.time()-start:.0f}s", file=sys.stderr)

    print(f"\nCollected {n} snapshots to {outfile}", file=sys.stderr)


def analyze(filepath):
    """Analyze a JSONL file of collected snapshots."""
    snapshots = []
    with open(filepath) as f:
        for line in f:
            line = line.strip()
            if line:
                snapshots.append(json.loads(line))

    if not snapshots:
        print("No data found")
        return

    print(f"Loaded {len(snapshots)} snapshots from {filepath}")

    # Use the last snapshot for the most complete picture
    last = snapshots[-1]
    first = snapshots[0]
    total_elapsed = last.get('_elapsed', 0)
    print(f"Collection span: {total_elapsed:.0f}s ({total_elapsed/3600:.1f}hr)")
    print(f"Device uptime at end: {last.get('uptimeMs',0)/1000:.0f}s")

    print_summary(last)

    # Track cycle stability over time
    print(f"\n{'='*60}")
    print(f"Cycle Stability Over Time:")
    for i, s in enumerate(snapshots):
        cycle = s.get('cycle', [])
        pos = s.get('cyclePosition', -1)
        elapsed = s.get('_elapsed', 0)
        step_gaps = [c.get('gap', {}) for c in cycle]
        total_step_samples = sum(g.get('count', 0) for g in step_gaps)
        print(f"  [{i:3d}] t={elapsed:6.0f}s  cycle={len(cycle):2d} steps  "
              f"pos={pos:3d}  stepSamples={total_step_samples:5d}")

    # Gap distribution evolution
    print(f"\n{'='*60}")
    print(f"Gap Distribution Over Time (large gaps >=50ms):")
    for i, s in enumerate(snapshots):
        gh = s.get('gapHistogram', {})
        hist = gh.get('histogram', [])
        total = gh.get('count', 0)
        if total == 0:
            continue
        large = sum(b['count'] for b in hist if any(
            x in b['label'] for x in ['50-100ms', '100-200ms', '200-500ms', '500ms-1s', '1-5s', '>=5s']))
        pct = 100 * large / total
        elapsed = s.get('_elapsed', 0)
        print(f"  [{i:3d}] t={elapsed:6.0f}s  total={total:5d}  large={large:4d} ({pct:.1f}%)")


def main():
    parser = argparse.ArgumentParser(description="Modbus gap pattern analyzer")
    parser.add_argument('--collect', action='store_true', help='Run long collection')
    parser.add_argument('--analyze', type=str, help='Analyze JSONL file')
    parser.add_argument('--interval', type=int, default=60, help='Collection interval (seconds)')
    parser.add_argument('--duration', type=int, default=0, help='Collection duration (seconds, 0=indefinite)')
    parser.add_argument('--output', type=str, help='Output JSONL file')
    parser.add_argument('--url', type=str, help=f'Device URL (default: {DEVICE_URL})')
    parser.add_argument('--user', type=str, help=f'Auth user (default: {AUTH_USER})')
    parser.add_argument('--password', type=str, help=f'Auth password')

    args = parser.parse_args()

    if args.analyze:
        analyze(args.analyze)
    elif args.collect:
        collect(args)
    else:
        # Read from stdin (piped from curl)
        data = json.load(sys.stdin)
        print_summary(data)


if __name__ == '__main__':
    main()
