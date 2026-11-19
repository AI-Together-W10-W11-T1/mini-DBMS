"""M14: real C SDK load, local queue observations and independent evidence checks."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time

from m11_benchmark import SCHEMA, generate_sql, payload, statistics, ticks_to_ns

ROOT = Path(__file__).resolve().parents[1]
WORKLOADS = ('id', 'scan_key', 'mixed')
CASES = ('SDK-01', 'QUEUE-01', 'STATISTICS-01', 'MIXED-01', 'FAILURE-01', 'MEMORY-01', 'OUTPUT-01', 'REGRESSION-01')


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON field')
        result[key] = value
    return result


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique_object,
                      parse_constant=lambda text: (_ for _ in ()).throw(ValueError('nonfinite JSON number')))


def integer(value, minimum=0, maximum=(1 << 63)-1):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError('invalid integer evidence')
    return value


def boolean(value):
    if type(value) is not bool:
        raise ValueError('invalid boolean evidence')
    return value


def summarize(values):
    return statistics(values) if values else {'count': 0, 'p50_ns': None, 'p95_ns': None}


def validate_native(raw, config):
    if not isinstance(raw, dict) or raw.get('version') != 1 or type(raw.get('version')) is not int:
        raise ValueError('native report version')
    for key in ('connections', 'rows', 'warmup', 'requests'):
        if integer(raw.get(key), 0) != config[key]:
            raise ValueError('native configuration mismatch')
    if raw.get('workload') != config['workload']:
        raise ValueError('native workload mismatch')
    frequency = integer(raw.get('frequency'), 1)
    start, end = integer(raw.get('start'), 1), integer(raw.get('end'), 1)
    if end <= start:
        raise ValueError('invalid common measurement interval')
    if not boolean(raw.get('valid')) or boolean(raw.get('memory_overflow')):
        raise ValueError('native capture incomplete')
    connections, warmup, requests = (config[k] for k in ('connections', 'warmup', 'requests'))
    samples = raw.get('samples')
    if not isinstance(samples, list) or len(samples) != connections*(warmup+requests):
        raise ValueError('native sample count mismatch')
    expected_payloads = {client+1: payload(client+1) for client in range(connections)}
    counts = Counter(planned=connections*requests)
    warm_counts = Counter(planned=connections*warmup)
    success_ns = []
    all_latencies = []
    complete = True
    max_end = 0
    for position, sample in enumerate(samples):
        client, offset = divmod(position, warmup+requests)
        phase, sequence = (0, offset) if offset < warmup else (1, offset-warmup)
        for key, expected in (('client', client), ('phase', phase), ('sequence', sequence)):
            if integer(sample.get(key)) != expected:
                raise ValueError('native sample schedule mismatch')
        update = config['workload'] == 'mixed' and sequence % 20 == 19
        if boolean(sample.get('update')) != update:
            raise ValueError('operation schedule mismatch')
        attempted, confirmed, unknown = (boolean(sample.get(key)) for key in ('attempted', 'confirmed', 'unconfirmed'))
        begin, finish = integer(sample.get('begin')), integer(sample.get('end'))
        status = sample.get('status')
        code = sample.get('code')
        if not isinstance(code, str) or len(code) > 39 or any(c not in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789' for c in code):
            raise ValueError('invalid outcome code')
        if status not in ('ok', 'error', 'transport', 'invalid', 'skipped'):
            raise ValueError('unknown sample outcome')
        if attempted != (status != 'skipped') or (confirmed or unknown) and not update or confirmed and unknown:
            raise ValueError('inconsistent attempt/confirmation')
        if not attempted:
            if begin or finish or confirmed or unknown:
                raise ValueError('unattempted sample has timing or effects')
            complete = False
        else:
            if not begin or finish < begin or phase == 1 and not start <= begin <= finish <= end or phase == 0 and finish > start:
                raise ValueError('sample outside phase interval')
            ticks_to_ns(finish-begin, frequency)
            if phase:
                max_end = max(max_end, finish)
                all_latencies.append({'client': client, 'sequence': sequence, 'ns': ticks_to_ns(finish-begin, frequency), 'status': status})
        if status == 'ok' and (code or update and not confirmed) or status == 'error' and not code:
            raise ValueError('success/error confirmation mismatch')
        if status in ('transport', 'invalid') or unknown:
            complete = False
        if confirmed:
            expected_payloads[client+1] = f'm14-c{client}-p{phase}-n{sequence}'
        counter = counts if phase else warm_counts
        counter['attempted' if attempted else 'unattempted'] += 1
        counter[status] += 1
        counter['planned_updates'] += update
        counter['attempted_updates'] += update and attempted
        counter['confirmed_updates'] += confirmed
        counter['unconfirmed_updates'] += unknown
        if phase and status == 'ok':
            success_ns.append(ticks_to_ns(finish-begin, frequency))
    if max_end != end:
        raise ValueError('common end is not last measured completion')
    clients = raw.get('clients')
    if not isinstance(clients, list) or len(clients) != connections:
        raise ValueError('client report count mismatch')
    for client, record in enumerate(clients):
        if integer(record.get('client')) != client or record.get('final_payload') != expected_payloads[client+1]:
            raise ValueError('confirmed final payload mismatch')
        if not isinstance(record.get('connect_code'), str):
            raise ValueError('invalid connect outcome')
        if record['connect_code']:
            complete = False
    memory = raw.get('memory')
    if not isinstance(memory, list) or len(memory) < 2:
        raise ValueError('missing memory boundary observations')
    in_window = []
    last = 0
    for sample in memory:
        tick = integer(sample.get('tick'), 1)
        if tick < last or not boolean(sample.get('ok')):
            raise ValueError('failed or reversed memory observation')
        integer(sample.get('working_set'), 1)
        integer(sample.get('private_bytes'), 1)
        if start <= tick <= end:
            in_window.append(sample)
        last = tick
    if memory[0]['tick'] > start or memory[-1]['tick'] < end:
        raise ValueError('memory observations do not bracket measurement')
    elapsed_ns = ticks_to_ns(end-start, frequency)
    if not elapsed_ns:
        raise ValueError('zero elapsed measurement')
    memory_summary = {'samples': len(memory), 'in_window_samples': len(in_window),
        'boundary_samples': len(memory)-len(in_window), 'requested_interval_ms': 10,
        'max_observed_interval_ns': max(ticks_to_ns(b['tick']-a['tick'], frequency) for a, b in zip(memory, memory[1:])),
        'observed_working_set_bytes': max((s['working_set'] for s in in_window), default=None),
        'observed_private_bytes': max((s['private_bytes'] for s in in_window), default=None)}
    return {'complete': complete, 'all_requests_succeeded': complete and counts['ok'] == counts['planned'] and warm_counts['ok'] == warm_counts['planned'],
        'counts': dict(counts), 'warmup_counts': dict(warm_counts), 'elapsed_ns': elapsed_ns,
        'throughput_requests_per_second': len(success_ns)*1e9/elapsed_ns, 'latency': summarize(success_ns),
        'latencies': all_latencies, 'memory': memory_summary, 'expected_payloads': expected_payloads}


def validate_queue(trace, raw):
    if not isinstance(trace, dict) or type(trace.get('version')) is not int or trace['version'] != 1 or not boolean(trace.get('valid')):
        raise ValueError('invalid or incomplete queue trace')
    if integer(trace.get('frequency'), 1) != raw['frequency'] or integer(trace.get('overflow')):
        raise ValueError('queue clock mismatch or buffer overflow')
    count = {key: integer(trace.get(key)) for key in ('enqueued', 'dequeued', 'removed', 'rejected')}
    if count['enqueued'] != count['dequeued']+count['removed']:
        raise ValueError('queue accounting imbalance')
    samples = trace.get('samples')
    if not isinstance(samples, list) or len(samples) != count['enqueued']+count['rejected'] or len(samples) > integer(trace.get('capacity'), 1):
        raise ValueError('queue sample count mismatch')
    realized = Counter()
    selected = Counter()
    waits = {'sql': [], 'eof': [], 'error': []}
    identities = set()
    request_groups = {}
    for index, sample in enumerate(samples, 1):
        if integer(sample.get('id'), 1) != index:
            raise ValueError('queue job identity missing/duplicated')
        identity = tuple(integer(sample.get(key), 1) for key in ('generation', 'request', 'event_index'))
        if identity in identities:
            raise ValueError('persistent request identity reused')
        identities.add(identity)
        request_groups.setdefault(identity[:2], []).append(sample)
        enqueue, dequeue, finish = (integer(sample.get(key)) for key in ('enqueue', 'dequeue', 'finish'))
        outcome = sample.get('outcome')
        if not enqueue or outcome not in ('sql', 'eof', 'error', 'removed', 'rejected'):
            raise ValueError('unfinished queue sample')
        measured = raw['start'] <= enqueue <= raw['end']
        if outcome in waits:
            realized['dequeued'] += 1
            if not enqueue <= dequeue <= finish or not dequeue:
                raise ValueError('invalid queue time ordering')
            if measured:
                if finish > raw['end']:
                    raise ValueError('measured queue job finished beyond client completion')
                waits[outcome].append(ticks_to_ns(dequeue-enqueue, raw['frequency']))
        elif outcome == 'removed':
            realized['removed'] += 1
            if dequeue or finish < enqueue:
                raise ValueError('removed job fabricated wait')
        else:
            realized['rejected'] += 1
            if dequeue or finish:
                raise ValueError('rejected job fabricated wait')
        if measured:
            selected[outcome] += 1
    if any(realized[key] != count[key] for key in ('dequeued', 'removed', 'rejected')):
        raise ValueError('queue counters differ from raw outcomes')
    # 큐 내부 합계만으로는 누락된 요청을 찾을 수 없다. SDK 시도와 요청별 terminal을 교차 대조한다.
    native_phases = [Counter(), Counter()]
    for sample in raw['samples']:
        if sample['attempted']:
            native_phases[sample['phase']][sample['status']] += 1
    observed_phases = [Counter(), Counter()]
    for jobs in request_groups.values():
        jobs.sort(key=lambda sample: sample['event_index'])
        if [job['event_index'] for job in jobs] != list(range(1, len(jobs)+1)):
            raise ValueError('queue request event sequence is incomplete')
        pattern = [job['outcome'] for job in jobs]
        if pattern == ['sql', 'eof']:
            status = 'ok'
        elif pattern in (['rejected'], ['removed'], ['error'], ['sql', 'rejected'], ['sql', 'removed'], ['sql', 'error']):
            status = 'error'
        else:
            raise ValueError('queue request terminal pattern is invalid')
        first, last = jobs[0], jobs[-1]
        terminal = last['finish'] or last['enqueue']
        if first['enqueue'] < raw['start']:
            phase = 0
            if terminal > raw['start']:
                raise ValueError('warmup queue request crosses measurement barrier')
        else:
            phase = 1
            if first['enqueue'] > raw['end'] or terminal > raw['end']:
                raise ValueError('queue request outside measurement interval')
        if any(a['finish'] > b['enqueue'] for a, b in zip(jobs, jobs[1:])):
            raise ValueError('queue request jobs overlap or reverse')
        observed_phases[phase][status] += 1
    for native, observed in zip(native_phases, observed_phases):
        if sum(native.values()) != sum(observed.values()) or native['ok'] != observed['ok'] or native['error'] != observed['error']:
            raise ValueError('queue request groups do not reconcile with SDK phase outcomes')
    return {'all_counts': count, 'measured_counts': dict(selected), 'wait': {key: summarize(values) for key, values in waits.items()},
            'note': 'Direct enqueue-to-dequeue time; SQL, EOF and error jobs separate. Not DB gate wait.'}


def verify_data(path, rows, changed):
    """독립 decoder로 전체 행의 framing/ID/scan_key/payload를 대조한다."""
    data = Path(path).read_bytes()
    offset = 0
    def take(size):
        nonlocal offset
        if size < 0 or offset+size > len(data):
            raise ValueError('truncated storage frame')
        result = data[offset:offset+size]
        offset += size
        return result
    if take(8) != b'MINIDBMS' or struct.unpack('<I', take(4))[0] != 1:
        raise ValueError('storage header mismatch')
    header = json.loads(take(struct.unpack('<Q', take(8))[0]), object_pairs_hook=unique_object)
    if json.dumps(header, sort_keys=True) != json.dumps(SCHEMA, sort_keys=True):
        raise ValueError('stored schema mismatch')
    for identifier in range(1, rows+1):
        actual = take(struct.unpack('<Q', take(8))[0])
        text = changed.get(identifier, payload(identifier)).encode()
        expected = (b'\1'+struct.pack('<q', identifier))*2+b'\1'+struct.pack('<Q', len(text))+text
        if actual != expected:
            raise ValueError(f'final row {identifier} differs from confirmed mutations')
    if offset != len(data):
        raise ValueError('unexpected final rows/trailing bytes')
    return {'rows': rows, 'all_fields_verified': True, 'sha256': hashlib.sha256(data).hexdigest()}


def invoke(command, directory, name, timeout=120):
    with (directory/(name+'.stdout')).open('xb') as out, (directory/(name+'.stderr')).open('xb') as err:
        process = subprocess.run(list(map(str, command)), stdout=out, stderr=err, timeout=timeout)
    return process.returncode


def prepare_fixture(exe, directory, rows):
    directory.mkdir(exist_ok=False)
    schema, data = directory/'schema', directory/'data'
    schema.mkdir()
    (schema/'bench_rows.json').write_text(json.dumps(SCHEMA), encoding='utf-8')
    generate_sql(directory/'input.sql', rows)
    common = ['--schema-dir', schema, '--data-dir', data, '--format', 'jsonl']
    if invoke([exe, 'init', *common], directory, 'init') or invoke([exe, 'run', *common, '--file', directory/'input.sql'], directory, 'load'):
        raise ValueError('fixture initialization failed')
    verify_data(data/'table_bench_rows.tbl', rows, {})
    return schema, data


def run_case(server, bench, schema, base_data, directory, config, server_options=(), environment=None):
    directory.mkdir(exist_ok=False)
    data = directory/'data'
    shutil.copytree(base_data, data)
    trace_path, raw_path = directory/'queue.json', directory/'native.json'
    report = {'valid': False, 'complete': False, 'all_requests_succeeded': False, 'config': config, 'errors': [], 'commands': []}
    process = None
    try:
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            port = reserve.getsockname()[1]
        command = list(map(str, [server, '--schema-dir', schema, '--data-dir', data, '--port', port, '--queue-metrics', trace_path, *server_options]))
        report['commands'].append(command)
        with (directory/'server.stdout').open('xb') as out, (directory/'server.stderr').open('xb') as err:
            process = subprocess.Popen(command, stdout=out, stderr=err, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP, env=environment)
            deadline = time.monotonic()+15
            while True:
                if process.poll() is not None:
                    raise ValueError('server preparation failed')
                try:
                    probe = socket.create_connection(('127.0.0.1', port), timeout=.1)
                    probe.close()
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise ValueError('server readiness timeout')
                    time.sleep(.01)
            command = list(map(str, [bench, '--port', port, '--server-pid', process.pid, '--connections', config['connections'],
                '--rows', config['rows'], '--warmup', config['warmup'], '--requests', config['requests'], '--workload', config['workload'], '--output', raw_path]))
            report['commands'].append(command)
            report['native_exit_code'] = invoke(command, directory, 'bench')
            process.send_signal(signal.CTRL_BREAK_EVENT)
            report['server_exit_code'] = process.wait(timeout=15)
        raw, trace = read_json(raw_path), read_json(trace_path)
        measurement = validate_native(raw, config)
        report['measurement'] = measurement
        report['queue'] = validate_queue(trace, raw)
        report['oracle'] = verify_data(data/'table_bench_rows.tbl', config['rows'], measurement['expected_payloads'] if config['workload'] == 'mixed' else {})
        report['complete'] = measurement['complete']
        report['valid'] = measurement['complete'] and report['native_exit_code'] == report['server_exit_code'] == 0
        report['all_requests_succeeded'] = report['valid'] and measurement['all_requests_succeeded']
        report['raw_sha256'] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (raw_path, trace_path)}
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        report['errors'].append(str(error))
    finally:
        if process and process.poll() is None:
            try:
                process.send_signal(signal.CTRL_BREAK_EVENT)
                process.wait(timeout=15)
            except (OSError, subprocess.TimeoutExpired):
                process.kill()
                process.wait()
                report['errors'].append('server required forced termination; trace is incomplete')
        (directory/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    return report


def run_matrix(exe, server, bench, destination, rows=10000, connections=(1,4,16,32), repeats=3, warmup=20, requests=40, workloads=WORKLOADS):
    integer(rows, 80, 1000000)
    integer(repeats, 1, 20)
    integer(warmup, 0, 10000)
    integer(requests, 1, 10000)
    if not connections or len(set(connections)) != len(connections) or any(type(c) is not int or not 1 <= c <= 32 for c in connections):
        raise ValueError('invalid connection matrix')
    if not workloads or len(set(workloads)) != len(workloads) or any(w not in WORKLOADS for w in workloads) or 'mixed' in workloads and (warmup % 20 or requests % 20):
        raise ValueError('invalid workload matrix or mixed95/5 request counts')
    if max(connections)*(warmup+requests) > 262144:
        raise ValueError('native sample limit exceeded')
    destination = Path(destination).resolve()
    if (ROOT/'.cache').resolve() not in destination.parents:
        raise ValueError('destination must be fresh under project .cache')
    destination.mkdir(parents=True, exist_ok=False)
    report = {'valid': False, 'complete': False, 'all_requests_succeeded': False, 'rows': rows, 'cases': [], 'errors': [],
        'defaults': {'max_connections': 32, 'workers': 4, 'pending_jobs': 16}, 'python': sys.version,
        'limitations': ['Debug or recorded build configuration; loopback shares CPU with client.', 'Fixed-count short runs, not steady-state or statistical significance.',
            'Queue observations include instrumentation overhead. No DB gate-wait claim.', 'Memory maxima are observed samples, not absolute peaks or leak proof.', 'Startup/connect/warmup excluded from SQL timing.']}
    try:
        paths = [Path(exe), Path(server), Path(bench), ROOT/'src/server.c', ROOT/'tools/bench_m14.c', ROOT/'scripts/m14_benchmark.py', ROOT/'tools/c-toolchain-lock.json']
        report['provenance_sha256'] = {str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
        cache = Path(server).resolve().parent/'CMakeCache.txt'
        report['build_configuration'] = next((line.split('=', 1)[1] for line in cache.read_text().splitlines() if line.startswith('CMAKE_BUILD_TYPE:')), 'unknown') if cache.exists() else 'unknown'
        schema, base_data = prepare_fixture(exe, destination/'fixture', rows)
        for workload in workloads:
            for count in connections:
                for repeat in range(repeats):
                    name = f'{workload}-c{count}-r{repeat+1}'
                    config = {'rows': rows, 'connections': count, 'warmup': warmup, 'requests': requests, 'workload': workload}
                    case = run_case(server, bench, schema, base_data, destination/name, config)
                    report['cases'].append({'name': name, 'report': str(destination/name/'report.json'), 'valid': case['valid'],
                        'all_requests_succeeded': case['all_requests_succeeded'], 'measurement': case.get('measurement'), 'queue': case.get('queue'), 'errors': case['errors']})
                    print(f"{name}: valid={case['valid']} all_requests_succeeded={case['all_requests_succeeded']}", flush=True)
        report['complete'] = len(report['cases']) == len(workloads)*len(connections)*repeats
        report['valid'] = report['complete'] and all(case['valid'] for case in report['cases'])
        report['all_requests_succeeded'] = report['valid'] and all(case['all_requests_succeeded'] for case in report['cases'])
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        report['errors'].append(str(error))
    finally:
        (destination/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser()
    for option in ('exe', 'server', 'bench', 'destination'):
        parser.add_argument('--'+option, required=True)
    parser.add_argument('--rows', type=int, default=10000)
    parser.add_argument('--connections', type=int, nargs='+', default=[1,4,16,32])
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--warmup', type=int, default=20)
    parser.add_argument('--requests', type=int, default=40)
    parser.add_argument('--workloads', choices=WORKLOADS, nargs='+', default=list(WORKLOADS))
    args = parser.parse_args()
    try:
        result = run_matrix(args.exe, args.server, args.bench, args.destination, args.rows, args.connections, args.repeats, args.warmup, args.requests, args.workloads)
        return 0 if result['valid'] else 1
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
