"""Compare raw/RTK output; Python 3.9 stdlib only. Does not configure agents."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rtk', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / '.cache/tooling-eval')
    args = parser.parse_args()
    rtk = args.rtk.resolve(strict=True)
    output = args.output.resolve()
    # Keep tool databases and generated fixtures inside the ignored workspace cache.
    cache = (ROOT / '.cache').resolve()
    if cache not in output.parents:
        parser.error('--output must be a child of the project .cache directory')
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PYTHONIOENCODING='utf-8', PYTHONUTF8='1', NO_COLOR='1',
               RTK_DB_PATH=str(output / 'history.db'),
               RTK_RECALL_DB=str(output / 'recall.db'),
               CLAUDE_CONFIG_DIR=str(output / 'claude-config'))

    def run(command):
        started = time.perf_counter()
        proc = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, timeout=60)
        return {'argv': command, 'exit_code': proc.returncode,
                'ms': round((time.perf_counter() - started) * 1000, 3),
                'bytes': len(proc.stdout) + len(proc.stderr),
                'stdout': proc.stdout.decode('utf-8', errors='replace'),
                'stderr': proc.stderr.decode('utf-8', errors='replace')}

    fixture = output / 'diagnostic_fixture.py'
    fixture.write_text(
        'import sys\n'
        'print("PASS control")\n'
        'if sys.argv[1] == "fail":\n'
        '    print("WARNING repeat\\n" * 4, file=sys.stderr)\n'
        '    print("fixture.py:42 DIAG 한글 " + "x" * 1500 + " LONG_END", file=sys.stderr)\n'
        '    sys.exit(7)\n', encoding='utf-8')
    git = ['git', '-c', 'safe.directory=' + ROOT.as_posix()]
    workloads = [
        # 새 프로젝트의 파일 상태나 앱 계약에 기대지 않고 도구 자체를 측정한다.
        ('git-status', git + ['status', '--short'], 'git', []),
        ('tools-search', ['rg', '-n', 'def status|def wiki_status|def changed',
                          'scripts/dev_tools.py'], 'rg',
         ['def status', 'def wiki_status', 'def changed']),
        ('wiki-check', [sys.executable, 'scripts/dev_tools.py', 'wiki-check'], 'test',
         ['changed_sources', 'page_changed']),
        ('unittest', [sys.executable, '-m', 'unittest', 'discover', '-s', 'scripts',
                      '-p', 'test_*.py'], 'test', ['Ran ', ' tests', 'OK']),
    ]
    controls = [
        ('control-success', [sys.executable, str(fixture), 'pass'], 'test', ['PASS control']),
        ('control-failure', [sys.executable, str(fixture), 'fail'], 'test',
         ['fixture.py:42', 'DIAG', '한글', 'LONG_END', 'WARNING repeat']),
    ]
    results = []
    for name, raw_cmd, mode, markers in workloads + controls:
        wrapped = [str(rtk)] + (['test'] + raw_cmd if mode == 'test' else raw_cmd)
        rounds = []
        for index in range(3):
            pair = {}
            for variant in (['raw', 'rtk'] if index % 2 == 0 else ['rtk', 'raw']):
                pair[variant] = run(raw_cmd if variant == 'raw' else wrapped)
            raw_text = pair['raw']['stdout'] + pair['raw']['stderr']
            filtered = pair['rtk']['stdout'] + pair['rtk']['stderr']
            missing_raw = [m for m in markers if m not in raw_text]
            if missing_raw:
                raise RuntimeError(name + ': invalid baseline markers ' + repr(missing_raw))
            missing = [m for m in markers if m not in filtered]
            pair['missing_markers'] = missing
            pair['exit_matches'] = pair['raw']['exit_code'] == pair['rtk']['exit_code']
            # Conservative fallback cost: the agent needs to read the full raw result.
            pair['net_bytes'] = pair['rtk']['bytes'] + (pair['raw']['bytes'] if missing else 0)
            rounds.append(pair)
        results.append({'name': name, 'control': name.startswith('control-'),
                        'rounds': rounds,
                        'raw_bytes': statistics.median(r['raw']['bytes'] for r in rounds),
                        'filtered_bytes': statistics.median(r['rtk']['bytes'] for r in rounds),
                        'net_bytes': statistics.median(r['net_bytes'] for r in rounds),
                        'raw_ms': statistics.median(r['raw']['ms'] for r in rounds),
                        'rtk_ms': statistics.median(r['rtk']['ms'] for r in rounds)})
    work = [r for r in results if not r['control']]
    raw_bytes = sum(r['raw_bytes'] for r in work)
    net_bytes = sum(r['net_bytes'] for r in work)
    savings = round(100 * (raw_bytes - net_bytes) / raw_bytes, 2)
    exit_ok = all(p['exit_matches'] for r in results for p in r['rounds'])
    diagnostics_ok = all(not p['missing_markers'] for r in results for p in r['rounds'])
    gain = run([str(rtk), 'gain', '--all', '--format', 'json'])
    (output / 'gain.json').write_text(gain['stdout'], encoding='utf-8')
    report = {'python': platform.python_version(), 'rtk_version': run([str(rtk), '--version']),
              'rtk_sha256': hashlib.sha256(rtk.read_bytes()).hexdigest(),
              'raw_bytes': raw_bytes, 'net_bytes': net_bytes, 'savings_pct': savings,
              'exit_codes_match': exit_ok, 'diagnostics_preserved': diagnostics_ok,
              'adopt': savings >= 30 and exit_ok and diagnostics_ok,
              'gain_exit_code': gain['exit_code'], 'results': results,
              'limitations': 'Output efficiency only; whole-session tokens and billing unmeasured. '
                              'Gain includes controls; headline savings exclude controls.'}
    (output / 'results.json').write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                         encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k not in ('results', 'rtk_version')},
                     ensure_ascii=True, indent=2))
    for row in results:
        print('{}: raw={} filtered={} net={} bytes; raw={:.1f} rtk={:.1f} ms'.format(
            row['name'], row['raw_bytes'], row['filtered_bytes'], row['net_bytes'],
            row['raw_ms'], row['rtk_ms']))


if __name__ == '__main__':
    main()
