"""M11 deterministic SQL loading and independent binary/timing evidence checks."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone, timedelta

ROOT = Path(__file__).resolve().parents[1]
BATCH = 10000
SEED = 17
SCHEMA = {'version': 1, 'name': 'bench_rows', 'columns': [
    {'name': 'id', 'type': 'INT64'}, {'name': 'scan_key', 'type': 'INT64'},
    {'name': 'payload', 'type': 'TEXT'}]}
PROJECTION = 'id,scan_key,payload'
MAX_U64 = (1 << 64) - 1


def payload(identifier):
    return f'row-{identifier:010d}'


def validate_size(size):
    if type(size) is not int or not 80 <= size <= 1000000:
        raise ValueError('rows must be an integer between 80 and 1000000')


def generate_sql(path, size):
    validate_size(size)
    digest = hashlib.sha256()
    with Path(path).open('xb') as stream:
        for start in range(1, size + 1, BATCH):
            last = min(start + BATCH, size + 1)
            statement = 'INSERT INTO bench_rows(scan_key,payload) VALUES' + ','.join(
                f"({identifier},'{payload(identifier)}')" for identifier in range(start, last)) + ';\n'
            data = statement.encode('utf-8')
            stream.write(data)
            digest.update(data)
    return {'sha256': digest.hexdigest(), 'statements': math.ceil(size / BATCH),
            'bytes': Path(path).stat().st_size}


def decode_table(path, size):
    """저장 구현을 호출하지 않고 모든 프레임과 필드를 기대 행에 대조한다."""
    validate_size(size)
    digest = hashlib.sha256()
    file_size = Path(path).stat().st_size
    with Path(path).open('rb') as stream:
        def take(length):
            if length < 0 or length > file_size - stream.tell():
                raise ValueError('truncated or oversized binary field')
            data = stream.read(length)
            if len(data) != length:
                raise ValueError('short binary read')
            return data

        if take(8) != b'MINIDBMS' or struct.unpack('<I', take(4))[0] != 1:
            raise ValueError('invalid storage header')
        def unique_object(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError('duplicate stored schema key')
                result[key] = value
            return result

        header = json.loads(take(struct.unpack('<Q', take(8))[0]).decode('utf-8'), object_pairs_hook=unique_object)
        if json.dumps(header, sort_keys=True) != json.dumps(SCHEMA, sort_keys=True):
            raise ValueError('stored schema differs from workload')
        count = 0
        for expected_id in range(1, size + 1):
            length = struct.unpack('<Q', take(8))[0]
            body = take(length)
            expected_text = payload(expected_id).encode('utf-8')
            expected = (b'\1' + struct.pack('<q', expected_id)) * 2
            expected += b'\1' + struct.pack('<Q', len(expected_text)) + expected_text
            if body != expected:
                raise ValueError(f'row {expected_id} has wrong ID, scan_key, payload or framing')
            digest.update(struct.pack('<Q', length) + body)
            count += 1
        if stream.read(1):
            raise ValueError('unexpected rows or trailing bytes')
    return {'rows': count, 'unique_ids': count, 'sha256_records': digest.hexdigest(),
            'bytes': file_size, 'all_fields_verified': True}


def ticks_to_ns(ticks, frequency):
    if (type(ticks) is not int or type(frequency) is not int or ticks < 0 or
            ticks > (1 << 63) - 1 or frequency <= 0 or frequency > (1 << 63) - 1):
        raise ValueError('invalid QPC ticks or frequency')
    value = ticks * 1000000000 // frequency
    if value > MAX_U64:
        raise ValueError('QPC nanoseconds overflow')
    return value


def statistics(values):
    if not values or any(type(value) is not int or value < 0 or value > MAX_U64 for value in values):
        raise ValueError('statistics requires unsigned integer samples')
    ordered = sorted(values)
    return {'count': len(ordered), 'p50_ns': ordered[(len(ordered) * 50 + 99) // 100 - 1],
            'p95_ns': ordered[(len(ordered) * 95 + 99) // 100 - 1]}


def target_value(index, size):
    if index < 80:
        return 1 + (((index * 37 + SEED) % 80) * (size - 1)) // 79
    return size + index - 79


def validate_measurements(report, size):
    validate_size(size)
    if not isinstance(report, dict):
        raise ValueError('benchmark report must be an object')
    expected_metadata = {'rows': size, 'seed': SEED, 'targets': 100, 'hits': 80, 'misses': 20,
                         'warmup_rounds': 1, 'measured_rounds': 3, 'projection': PROJECTION,
                         'db_open_count': 1, 'timing_boundary': 'request_open_through_result_and_eof'}
    if any(type(report.get(key)) is not type(value) or report.get(key) != value
           for key, value in expected_metadata.items()):
        raise ValueError('benchmark workload metadata mismatch')
    frequency = report.get('frequency')
    if type(report.get('open_ns')) is not int or report['open_ns'] != ticks_to_ns(report.get('open_ticks'), frequency):
        raise ValueError('DB open timing mismatch')
    samples = report.get('samples')
    if not isinstance(samples, list) or len(samples) != 600:
        raise ValueError('exactly 600 measured samples required')
    groups = {method + suffix: [] for method in ('id', 'scan_key') for suffix in ('', '_hit', '_miss')}
    for position, sample in enumerate(samples):
        if not isinstance(sample, dict):
            raise ValueError('benchmark sample must be an object')
        pair = position // 2
        round_index, target_index = divmod(pair, 100)
        first = 'id' if ((round_index + 1) * 100 + target_index) % 2 == 0 else 'scan_key'
        method = first if position % 2 == 0 else ('scan_key' if first == 'id' else 'id')
        expected = {'round': round_index + 1, 'target_index': target_index,
                    'target': target_value(target_index, size), 'hit': target_index < 80,
                    'method': method, 'pair_order': position % 2}
        if any(type(sample.get(key)) is not type(value) or sample.get(key) != value for key, value in expected.items()):
            raise ValueError('sample workload/order mismatch')
        elapsed = ticks_to_ns(sample.get('ticks'), frequency)
        if type(sample.get('ns')) is not int or sample['ns'] != elapsed:
            raise ValueError('sample nanosecond conversion mismatch')
        groups[method].append(elapsed)
        groups[method + ('_hit' if sample['hit'] else '_miss')].append(elapsed)
    summaries = {key: statistics(values) for key, values in groups.items()}
    supplied = report.get('statistics')
    if not isinstance(supplied, dict) or supplied.keys() != summaries.keys():
        raise ValueError('aggregate statistics mismatch')
    for key, summary in summaries.items():
        values = supplied[key]
        if (not isinstance(values, dict) or values.keys() != summary.keys() or
                any(type(values[name]) is not int or values[name] != expected for name, expected in summary.items())):
            raise ValueError('aggregate statistics mismatch')
    return summaries


def run_dataset(exe, bench, destination, size):
    validate_size(size)
    destination = Path(destination).resolve()
    cache = (ROOT / '.cache').resolve()
    if cache not in destination.parents:
        raise ValueError('destination must be a fresh directory under project .cache')
    destination.mkdir(parents=True, exist_ok=False)
    schema, data = destination / 'schema', destination / 'data'
    schema.mkdir()
    (schema / 'bench_rows.json').write_text(json.dumps(SCHEMA), encoding='utf-8')
    report = {'success': False, 'rows': size, 'batch_size': BATCH, 'seed': SEED,
              'created_at': datetime.now(timezone(timedelta(hours=9))).isoformat(),
              'commands': [], 'stages_seconds': {}, 'paths': {'destination': str(destination),
              'schema': str(schema), 'data': str(data), 'sql': str(destination / 'input.sql')},
              'tools': {'python': sys.version, 'exe': str(Path(exe).resolve()),
                        'bench': str(Path(bench).resolve())}, 'errors': []}
    report_path = destination / 'report.json'

    def invoke(name, command):
        started = time.perf_counter()
        stdout, stderr = destination / (name + '.stdout'), destination / (name + '.stderr')
        with stdout.open('xb') as output, stderr.open('xb') as error:
            process = subprocess.run(command, stdout=output, stderr=error)
        report['stages_seconds'][name] = time.perf_counter() - started
        report['commands'].append({'name': name, 'argv': command, 'exit_code': process.returncode,
                                   'stdout': str(stdout), 'stderr': str(stderr)})
        if process.returncode or stderr.stat().st_size:
            raise ValueError(f'{name} failed, see {stderr}')
        return stdout

    try:
        report['provenance_sha256'] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in [Path(exe), Path(bench), ROOT / 'tools/c-toolchain-lock.json',
                         ROOT / 'scripts/m11_benchmark.py', ROOT / 'tools/bench_m11.c',
                         ROOT / 'include/minidb.h', *sorted((ROOT / 'src').glob('*.c'))]}
        started = time.perf_counter()
        report['generation'] = generate_sql(destination / 'input.sql', size)
        report['stages_seconds']['generation'] = time.perf_counter() - started
        common = ['--schema-dir', str(schema), '--data-dir', str(data), '--format', 'jsonl']
        invoke('init', [str(exe), 'init'] + common)
        load_output = invoke('cli_load', [str(exe), 'run'] + common + ['--file', str(destination / 'input.sql')])
        records = [json.loads(line) for line in load_output.read_text(encoding='utf-8').splitlines()]
        counts = [min(BATCH, size - start) for start in range(0, size, BATCH)]
        if len(records) != len(counts) or any(record.get('type') != 'statement_ok' or
                record.get('affected_rows') != str(count) or record.get('committed') is not True
                for record, count in zip(records, counts)):
            raise ValueError('CLI insertion acknowledgement mismatch')
        started = time.perf_counter()
        report['decoder'] = decode_table(data / 'table_bench_rows.tbl', size)
        report['stages_seconds']['decode'] = time.perf_counter() - started
        benchmark_output = invoke('benchmark_process', [str(bench), '--schema-dir', str(schema),
                                                       '--data-dir', str(data), '--rows', str(size)])
        report['measurement'] = json.loads(benchmark_output.read_text(encoding='utf-8'))
        validate_measurements(report['measurement'], size)
        report['success'] = True
    except (OSError, ValueError, KeyError, TypeError) as error:
        report['errors'].append(str(error))
        raise
    finally:
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--bench', required=True, type=Path)
    parser.add_argument('--destination', required=True, type=Path)
    parser.add_argument('--rows', required=True, type=int)
    options = parser.parse_args()
    try:
        report = run_dataset(options.exe.resolve(), options.bench.resolve(), options.destination, options.rows)
        print(json.dumps({'success': True, 'rows': options.rows, 'stages_seconds': report['stages_seconds'],
                          'report': str(options.destination.resolve() / 'report.json')}), flush=True)
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
