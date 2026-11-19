import argparse
import concurrent.futures
import json
import copy
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
import m14_benchmark as benchmark
from m11_benchmark import statistics, ticks_to_ns

from m12_remote import Database, response, send_query


def queue_test(options):
    db = Database(options)
    trace = db.root/'queue.json'
    with db.server(['--queue-metrics', trace]) as (port, _):
        def request(index):
            with db.connect(port) as connection:
                send_query(connection, f'VALUES({index});')
                events, done, _ = response(connection)
                assert events[0]['result']['rows'] == [[str(index)]] and done['exit_code'] == 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            list(pool.map(request, range(3)))
    report = json.loads(trace.read_text())
    assert len(report['samples']) == 6, 'actual successful SQL/EOF queue jobs must be observed exactly once'
    assert report['valid'] and report['enqueued'] == report['dequeued'] == 6 and not report['removed']
    assert sorted(s['outcome'] for s in report['samples']) == ['eof']*3+['sql']*3
    for sample in report['samples']:
        assert sample['enqueue'] <= sample['dequeue'] <= sample['finish']
        assert ticks_to_ns(sample['dequeue']-sample['enqueue'], report['frequency']) >= 0
    # 실제 큐 경로가 고정 상한을 넘으면 원본 trace에 손실을 표시하고 정상 종료로 숨기지 않는다.
    env = dict(os.environ, MINIDB_TEST_METRICS_SMALL='1')
    original = os.environ.get('MINIDB_TEST_METRICS_SMALL')
    os.environ['MINIDB_TEST_METRICS_SMALL'] = '1'
    try:
        try:
            with db.server(['--queue-metrics', db.root/'overflow.json'], hold=1) as (port, _):
                with db.connect(port) as connection:
                    send_query(connection, 'VALUES(1);')
                    assert response(connection)[1]['exit_code'] == 0
        except AssertionError as error:
            assert '3' in str(error), error
    finally:
        if original is None:
            os.environ.pop('MINIDB_TEST_METRICS_SMALL', None)
        else:
            os.environ['MINIDB_TEST_METRICS_SMALL'] = original
    overflow = benchmark.read_json(db.root/'overflow.json')
    assert overflow['overflow'] > 0 and overflow['valid'] is False


def known_report():
    config = {'connections': 1, 'rows': 80, 'warmup': 0, 'requests': 20, 'workload': 'mixed'}
    samples = [{'client': 0, 'phase': 1, 'sequence': i, 'update': i == 19, 'attempted': True,
        'begin': 100+i*1000, 'end': 600+i*1000, 'status': 'ok', 'code': '', 'confirmed': i == 19, 'unconfirmed': False} for i in range(20)]
    raw = {'version': 1, 'valid': True, 'frequency': 1000000000, 'start': 100, 'end': 19600, **config,
        'memory_overflow': False, 'clients': [{'client': 0, 'connect_code': '', 'final_payload': 'm14-c0-p1-n19'}], 'samples': samples,
        'memory': [{'tick': tick, 'ok': True, 'working_set': 1000, 'private_bytes': 2000} for tick in (90, 200, 19600, 19700)]}
    return config, raw


def rejects(call):
    try:
        call()
    except (ValueError, TypeError, KeyError):
        return
    raise AssertionError('corrupted evidence was accepted')


def statistics_test():
    config, raw = known_report()
    result = benchmark.validate_native(raw, config)
    assert result['latency'] == {'count': 20, 'p50_ns': 500, 'p95_ns': 500}
    assert result['elapsed_ns'] == 19500 and result['counts']['planned_updates'] == 1
    assert result['throughput_requests_per_second'] == 20*1e9/19500
    assert statistics([1, 3, 2, 100])['p95_ns'] == 100
    assert ticks_to_ns(1, 3) == 333333333
    for bad in (True, -1, 1.5, 1 << 64):
        rejects(lambda: ticks_to_ns(bad, 1000))
    mutations = [lambda r: r['samples'].pop(), lambda r: r.update(frequency=True),
        lambda r: r['samples'][0].update(begin=99), lambda r: r['samples'][0].update(confirmed=True),
        lambda r: r['samples'][19].update(update=False), lambda r: r.update(end=19601),
        lambda r: r['clients'][0].update(final_payload='guessed'), lambda r: r['memory'][0].update(ok=False)]
    for mutation in mutations:
        bad = copy.deepcopy(raw)
        mutation(bad)
        rejects(lambda: benchmark.validate_native(bad, config))
    trace = {'version': 1, 'valid': True, 'frequency': raw['frequency'], 'capacity': 2, 'enqueued': 2, 'dequeued': 2, 'removed': 0, 'rejected': 0, 'overflow': 0,
        'samples': [{'id': 1, 'generation': 2, 'request': 1, 'event_index': 1, 'enqueue': 101, 'dequeue': 108, 'finish': 120, 'outcome': 'sql'},
                    {'id': 2, 'generation': 2, 'request': 1, 'event_index': 2, 'enqueue': 121, 'dequeue': 125, 'finish': 130, 'outcome': 'eof'}]}
    queue_raw = dict(raw, samples=[{'phase': 1, 'attempted': True, 'status': 'ok'}])
    queue = benchmark.validate_queue(trace, queue_raw)
    assert queue['wait']['sql']['p95_ns'] == 7 and queue['wait']['eof']['p95_ns'] == 4
    for mutation in (lambda t: t.update(enqueued=3), lambda t: t['samples'][1].update(id=1),
                     lambda t: t['samples'][0].update(dequeue=100), lambda t: t.update(overflow=1),
                     lambda t: t.update(valid=False), lambda t: t['samples'][1].update(event_index=1)):
        bad = copy.deepcopy(trace)
        mutation(bad)
        rejects(lambda: benchmark.validate_queue(bad, queue_raw))
    queue_crosscheck_test()


def queue_crosscheck_test():
    # 독립 fixture: 성공/최초 busy/SQL 뒤 EOF busy를 모두 포함한 세 요청.
    raw = {'frequency': 1000000000, 'start': 100, 'end': 1000,
        'samples': [{'phase': 0, 'attempted': True, 'status': 'ok'},
                    {'phase': 1, 'attempted': True, 'status': 'error'},
                    {'phase': 1, 'attempted': True, 'status': 'error'}]}
    trace = {'version': 1, 'valid': True, 'frequency': raw['frequency'], 'capacity': 10,
        'enqueued': 3, 'dequeued': 3, 'removed': 0, 'rejected': 2, 'overflow': 0, 'samples': []}
    outcomes = [(1,1,'sql',10),(1,2,'eof',20),(2,1,'rejected',110),(3,1,'sql',120),(3,2,'rejected',130)]
    for index, (request,event,outcome,tick) in enumerate(outcomes, 1):
        trace['samples'].append({'id': index, 'generation': 1, 'request': request, 'event_index': event,
            'enqueue': tick, 'dequeue': 0 if outcome == 'rejected' else tick+1,
            'finish': 0 if outcome == 'rejected' else tick+2, 'outcome': outcome})
    assert benchmark.validate_queue(trace, raw)['measured_counts'] == {'rejected': 2, 'sql': 1}
    empty = copy.deepcopy(trace)
    empty.update(enqueued=0,dequeued=0,rejected=0,samples=[])
    rejects(lambda: benchmark.validate_queue(empty, raw))
    deleted = copy.deepcopy(trace)
    deleted['samples'] = deleted['samples'][2:]
    for index, sample in enumerate(deleted['samples'], 1):
        sample['id'] = index
    deleted.update(enqueued=1,dequeued=1)
    rejects(lambda: benchmark.validate_queue(deleted, raw))
    shifted = copy.deepcopy(trace)
    for sample in shifted['samples']:
        for key in ('enqueue', 'dequeue', 'finish'):
            if sample[key]:
                sample[key] += 10000
    rejects(lambda: benchmark.validate_queue(shifted, raw))


def smoke(options, workloads, connections=(2,)):
    root = Path(options.work)
    root.mkdir(parents=True, exist_ok=True)
    destination = Path(tempfile.mkdtemp(prefix='parent-', dir=root))/'measurement'
    report = benchmark.run_matrix(options.exe, options.server, options.bench, destination,
        rows=80, connections=connections, repeats=1, warmup=20, requests=20, workloads=workloads)
    assert report['valid'], report
    return report


def failure_test(options):
    # 실제 SDK에 전송 뒤 단절을 주고 첫 실패+남은 미시도를 각각 보존한다.
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    port = listener.getsockname()[1]
    def peer():
        connection, _ = listener.accept()
        with connection:
            connection.recv(65536)
        listener.close()
    worker = threading.Thread(target=peer)
    worker.start()
    root = Path(options.work)
    root.mkdir(parents=True, exist_ok=True)
    raw_path = Path(tempfile.mkdtemp(dir=root))/'failure.json'
    command = [options.bench, '--port', str(port), '--server-pid', str(os.getpid()), '--connections', '1', '--rows', '80',
        '--warmup', '0', '--requests', '20', '--workload', 'id', '--output', str(raw_path)]
    process = subprocess.run(command, capture_output=True, timeout=15)
    worker.join(2)
    assert not worker.is_alive() and process.returncode == 0, process.stderr
    raw = benchmark.read_json(raw_path)
    config = {'connections': 1, 'rows': 80, 'warmup': 0, 'requests': 20, 'workload': 'id'}
    checked = benchmark.validate_native(raw, config)
    assert checked['complete'] is False and checked['all_requests_succeeded'] is False
    assert checked['counts']['attempted'] == 1 and checked['counts']['unattempted'] == 19
    assert checked['counts']['transport'] == 1 and checked['counts']['skipped'] == 19


def output_test(options):
    root = Path(options.work)
    root.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(dir=root))
    existing = directory/'native.json'
    existing.write_bytes(b'preserve-me')
    command = [options.bench, '--port', '1', '--server-pid', str(os.getpid()), '--workload', 'id', '--output', str(existing)]
    assert subprocess.run(command, capture_output=True).returncode == 3
    assert existing.read_bytes() == b'preserve-me'
    assert subprocess.run(command+['--requests', '-1'], capture_output=True).returncode == 2
    assert subprocess.run(command+['--connections', '33'], capture_output=True).returncode == 2
    db = Database(options)
    trace = db.root/'preserve.json'
    trace.write_bytes(b'keep')
    command = [options.server, '--schema-dir', str(db.schema), '--data-dir', str(db.data), '--port', '15432', '--queue-metrics', str(trace)]
    assert subprocess.run(command, capture_output=True).returncode == 3
    assert trace.read_bytes() == b'keep'
    rejects(lambda: benchmark.run_matrix(options.exe, options.server, options.bench, directory, rows=True))
    rejects(lambda: benchmark.read_json(write_duplicate(directory)))


def write_duplicate(directory):
    path = directory/'duplicate.json'
    path.write_text('{"a":1,"a":2}', encoding='utf-8')
    return path


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('exe','server','fault-server','remote','bench','case','work'):
        parser.add_argument('--'+name, required=True)
    options = parser.parse_args()
    if options.case == 'M14-QUEUE-01':
        queue_test(options)
    elif options.case == 'M14-STATISTICS-01':
        statistics_test()
    elif options.case == 'M14-FAILURE-01':
        failure_test(options)
    elif options.case == 'M14-OUTPUT-01':
        output_test(options)
    else:
        report = smoke(options, ('mixed',) if options.case == 'M14-MIXED-01' else ('id', 'scan_key') if options.case == 'M14-SDK-01' else ('id',))
        if options.case == 'M14-MEMORY-01':
            summary = report['cases'][0]['measurement']['memory']
            assert summary['samples'] >= 2 and summary['boundary_samples'] >= 2
        if options.case == 'M14-MIXED-01':
            summary = report['cases'][0]['measurement']
            assert summary['counts']['planned_updates'] == 2 and summary['counts']['confirmed_updates'] == 2
        if options.case == 'M14-REGRESSION-01':
            db = Database(options)
            with db.server() as (port, _):
                with db.connect(port) as connection:
                    send_query(connection, 'VALUES(42);')
                    assert response(connection)[0][0]['result']['rows'] == [['42']]
