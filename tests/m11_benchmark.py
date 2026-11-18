"""Eight small M11 acceptance cases using actual CLI and native benchmark processes."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location('benchmark', Path(__file__).resolve().parents[1] / 'scripts/m11_benchmark.py')
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


def rejects(action):
    try:
        action()
    except (ValueError, OSError):
        return
    raise AssertionError('invalid input unexpectedly accepted')


def native(args, *arguments, success=True):
    result = subprocess.run([args.bench, *map(str, arguments)], capture_output=True, timeout=60)
    assert (result.returncode == 0) == success, (arguments, result.returncode, result.stderr)
    if success:
        assert not result.stderr, result.stderr
        return json.loads(result.stdout)
    assert not result.stdout, 'failure must not emit successful measurement JSON'


def small(args, root, size=80):
    return benchmark.run_dataset(args.exe, args.bench, root / '한글 data', size)


def generate(args, root):
    for size in (80, 10000, 10001):
        a, b = root / f'a{size}.sql', root / f'b{size}.sql'
        first = benchmark.generate_sql(a, size)
        second = benchmark.generate_sql(b, size)
        assert first == second and a.read_bytes() == b.read_bytes(), 'deterministic SQL'
        statements = a.read_text(encoding='utf-8').splitlines()
        assert len(statements) == (size + 9999) // 10000
        offset = 1
        for statement in statements:
            assert statement.startswith('INSERT INTO bench_rows(scan_key,payload) VALUES')
            assert statement.endswith(';')
            expected_count = min(10000, size - offset + 1)
            expected = ','.join(f"({i},'row-{i:010d}')" for i in range(offset, offset + expected_count))
            assert statement == 'INSERT INTO bench_rows(scan_key,payload) VALUES' + expected + ';'
            offset += expected_count
        assert offset == size + 1
    rejects(lambda: benchmark.generate_sql(a, 10001))


def stored(size=80):
    header = json.dumps(benchmark.SCHEMA).encode('utf-8')
    data = b'MINIDBMS' + struct.pack('<IQ', 1, len(header)) + header
    frames = []
    for identifier in range(1, size + 1):
        text = f'row-{identifier:010d}'.encode('utf-8')
        body = b'\1' + struct.pack('<q', identifier) + b'\1' + struct.pack('<q', identifier)
        body += b'\1' + struct.pack('<Q', len(text)) + text
        frames.append(struct.pack('<Q', len(body)) + body)
    return data, frames


def decode(args, root):
    header, frames = stored()
    path = root / 'table.tbl'
    raw = header + b''.join(frames)
    path.write_bytes(raw)
    first = benchmark.decode_table(path, 80)
    assert first['rows'] == first['unique_ids'] == 80 and first['all_fields_verified']
    assert first == benchmark.decode_table(path, 80)
    variants = [raw[:-1], raw + b'\0', b'BADMAGIC' + raw[8:], raw[:12] + struct.pack('<Q', 2**64 - 1) + raw[20:]]
    duplicate = frames.copy()
    duplicate[70] = frames[69]
    variants.append(header + b''.join(duplicate))
    for field_offset in (8, 9, 18, 27, 40):
        bad = frames.copy()
        frame = bytearray(bad[70])
        frame[field_offset] ^= 0x80
        bad[70] = bytes(frame)
        variants.append(header + b''.join(bad))
    for variant in variants:
        path.write_bytes(variant)
        rejects(lambda: benchmark.decode_table(path, 80))


def oracle(args, root):
    assert native(args, '--selftest')['oracle_selftest'], 'real C oracle self-test'
    result = small(args, root)
    data = Path(result['paths']['data'])
    table = data / 'table_bench_rows.tbl'
    contents = table.read_bytes()
    assert b'row-0000000071' in contents
    table.write_bytes(contents.replace(b'row-0000000071', b'bad-0000000071'))
    native(args, '--schema-dir', result['paths']['schema'], '--data-dir', data, '--rows', 80, success=False)


def bench(args, root):
    result = small(args, root)
    measurement = result['measurement']
    benchmark.validate_measurements(measurement, 80)
    assert len(measurement['samples']) == 600
    assert len({sample['target'] for sample in measurement['samples'] if sample['hit']}) == 80
    assert len({sample['target'] for sample in measurement['samples'] if not sample['hit']}) == 20
    for method in ('id', 'scan_key'):
        assert measurement['statistics'][method]['count'] == 300
        assert measurement['statistics'][method + '_hit']['count'] == 240
        assert measurement['statistics'][method + '_miss']['count'] == 60


def statistics(args, root):
    assert native(args, '--selftest')['timer_selftest'], 'negative and reversed QPC intervals rejected'
    for values, expected in [([1, 2, 3, 4, 5], (3, 5)), (list(range(20)), (9, 18)),
                             ([8], (8, 8)), ([5, 0, 5, 2], (2, 5))]:
        assert benchmark.statistics(values) == {'count': len(values), 'p50_ns': expected[0], 'p95_ns': expected[1]}
    for values in ([], [-1], [True], [2**64], [1.0]):
        rejects(lambda: benchmark.statistics(values))
    pairs = [(0, 1), (1, 3), (10**7, 10**7), (2**63-1, 2**63-1),
             (2**63-1, 10**9), (18446744073, 1)]
    for ticks, frequency in pairs:
        expected = ticks * 10**9 // frequency
        assert benchmark.ticks_to_ns(ticks, frequency) == expected
        assert native(args, '--convert', ticks, frequency) == {'ns': expected}
    for ticks, frequency in [(-1, 1), (1, 0), (1, -1), (2**63-1, 1), (2**63, 10**9)]:
        rejects(lambda: benchmark.ticks_to_ns(ticks, frequency))
        native(args, '--convert', ticks, frequency, success=False)


def arguments(args, root):
    for size in (0, -1, 79, 1000001):
        rejects(lambda: benchmark.run_dataset(args.exe, args.bench, root / 'invalid', size))
        assert not (root / 'invalid').exists()
        native(args, '--schema-dir', root, '--data-dir', root, '--rows', size, success=False)
    native(args, '--rows', 80, success=False)
    existing = root / 'existing'
    existing.mkdir()
    sentinel = existing / 'sentinel'
    sentinel.write_text('preserve', encoding='ascii')
    rejects(lambda: benchmark.run_dataset(args.exe, args.bench, existing, 80))
    assert sentinel.read_text() == 'preserve' and list(existing.iterdir()) == [sentinel]
    rejects(lambda: benchmark.run_dataset(args.exe, args.bench, benchmark.ROOT / 'outside-cache', 80))
    native(args, '--schema-dir', root / 'absent', '--data-dir', root / 'missing', '--rows', 80, success=False)
    destination = root / 'failure'
    rejects(lambda: benchmark.run_dataset(args.exe, root / 'absent.exe', destination, 80))
    failure = json.loads((destination / 'report.json').read_text(encoding='utf-8'))
    assert failure['success'] is False and failure['errors']


def output(args, root):
    result = small(args, root)
    measurement = result['measurement']
    mutations = [lambda x: x['samples'].pop(), lambda x: x.update(frequency=0),
                 lambda x: x['samples'][511].update(ns=x['samples'][511]['ns']+1),
                 lambda x: x['samples'][511].update(target=9999),
                 lambda x: x['statistics']['id'].update(p95_ns=-1),
                 lambda x: x.update(db_open_count=2), lambda x: x.update(hits=79),
                 lambda x: x.update(db_open_count=True), lambda x: x.update(warmup_rounds=True),
                 lambda x: x['samples'][0].update(hit=1)]
    for mutate in mutations:
        wrong = copy.deepcopy(measurement)
        mutate(wrong)
        rejects(lambda: benchmark.validate_measurements(wrong, 80))
    assert result['success'] and not result['errors']
    assert all(record['exit_code'] == 0 for record in result['commands'])


def regression(args, root):
    result = small(args, root, 101)
    assert result['success'] and result['decoder']['rows'] == 101
    assert all(result['stages_seconds'][stage] >= 0 for stage in ('generation', 'init', 'cli_load', 'decode', 'benchmark_process'))
    path = Path(result['paths']['destination']) / 'report.json'
    saved = json.loads(path.read_text(encoding='utf-8'))
    assert saved == result
    benchmark.validate_measurements(saved['measurement'], 101)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--bench', required=True)
    parser.add_argument('--work', required=True)
    parser.add_argument('--case', required=True)
    args = parser.parse_args()
    Path(args.work).mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='attempt-', dir=args.work))
    cases = dict(zip(('GENERATE', 'DECODE', 'ORACLE', 'BENCH', 'STATISTICS', 'ARGUMENT', 'OUTPUT', 'REGRESSION'),
                     (generate, decode, oracle, bench, statistics, arguments, output, regression)))
    cases[args.case.split('-')[1]](args, root)


if __name__ == '__main__':
    main()
