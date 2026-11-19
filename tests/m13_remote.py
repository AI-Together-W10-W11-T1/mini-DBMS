import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import threading
import time
from unittest.mock import patch

from m12_remote import Database, exact, frame, response, send_query


def timeout_wire(options):
    db = Database(options)
    for executable in (options.unit, options.legacy):
        with db.server(['--sql-timeout-ms', 10], hold=40) as (port, _):
            process = subprocess.run([executable, '--sdk-timeout', str(port)], capture_output=True, timeout=10)
            assert process.returncode == 0, (executable, process.stdout, process.stderr)
    with db.server(['--sql-timeout-ms', 10], hold=40) as (port, _):
        with db.connect(port) as connection:
            send_query(connection, 'VALUES(42);INSERT INTO users(id) VALUES(9);')
            events, done, _ = response(connection)
            assert len(events) == 1 and events[0]['stage'] == 'sql'
            assert events[0]['diagnostic']['code'] == 'TIMEOUT'
            assert events[0]['diagnostic']['committed'] is None
            assert events[0]['result'] is None and done['exit_code'] == 8
        result = db.remote(port, 'VALUES(42);')
        assert result.returncode == 8, result.stderr
    assert db.local('SELECT id FROM users;')[0]['rows'] == []
    # The legacy executable reproduces the pre-M13 strict whitelist. A complete
    # first event remains owned after the subsequent unknown-stage failure.
    for executable in (options.unit, options.legacy):
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        port = listener.getsockname()[1]
        errors = []
        success = {'version': 1, 'result': {'version': 1, 'type': 'query_result',
            'statement_index': '1', 'columns': [{'name': 'column1', 'type': 'INT64'}],
            'rows': [['42']], 'row_count': '1'}, 'diagnostic': None, 'stop': False,
            'health': 'healthy', 'stage': None}
        diagnostic = {'version': 1, 'type': 'diagnostic', 'code': 'TIMEOUT',
            'message': 'SQL timeout', 'operation': 'run', 'phase': 'execute',
            'source': None, 'path': None, 'statement_index': '2', 'line': '1',
            'column': '12', 'field_path': None, 'byte_offset': '11', 'committed': None,
            'rollback': 'not_needed', 'command': None, 'affected_rows': None, 'output_ready': None}
        timeout = {'version': 1, 'result': None, 'diagnostic': diagnostic,
                   'stop': True, 'health': 'healthy', 'stage': 'sql'}
        def peer():
            try:
                connection, _ = listener.accept()
                with connection:
                    header = exact(connection, 5)
                    exact(connection, int.from_bytes(header[1:], 'big'))
                    for index, event in enumerate((success, timeout), 1):
                        raw = json.dumps(event, separators=(',', ':')).encode()
                        connection.sendall(frame('B', {'version': 1, 'type': 'begin',
                            'event_index': str(index), 'bytes': str(len(raw))}) + frame('D', raw) +
                            frame('E', {'version': 1, 'type': 'end', 'event_index': str(index)}))
                    connection.sendall(frame('F', {'version': 1, 'type': 'done',
                        'status': 'error', 'events': '2', 'exit_code': 8}))
            except (BrokenPipeError, ConnectionResetError):
                pass
            except Exception as error:
                errors.append(repr(error))
            finally:
                listener.close()
        worker = threading.Thread(target=peer, daemon=True)
        worker.start()
        process = subprocess.run([executable, '--sdk-after-success', str(port)], capture_output=True, timeout=10)
        worker.join(2)
        assert not worker.is_alive() and not errors and process.returncode == 0, (process.stderr, errors)


def wait_marker(db, process):
    deadline = time.monotonic() + 5
    while not (db.data/'.minidb.pending').exists():
        assert process.poll() is None and time.monotonic() < deadline
        time.sleep(.002)


def lifetime(options):
    for shutdown in (False, True):
        db = Database(options)
        with patch.dict(os.environ, {'MINIDB_TEST_SERVER_HOLD_POINT': 'after-marker'}):
            with db.server(['--workers', 2], hold=200) as (port, process):
                connection = db.connect(port)
                send_query(connection, 'INSERT INTO users(id) VALUES(9);INSERT INTO users(id) VALUES(10);')
                wait_marker(db, process)
                if shutdown:
                    process.send_signal(signal.CTRL_BREAK_EVENT)
                    process.wait(timeout=5)
                    assert process.returncode == 0
                else:
                    connection.close()
                    deadline = time.monotonic() + 5
                    while (db.data/'.minidb.pending').exists():
                        assert time.monotonic() < deadline
                        time.sleep(.005)
                    result = db.remote(port, 'SELECT id FROM users;')
                    assert result.returncode == 0 and json.loads(result.stdout)['rows'] == [['9']], result.stderr
                connection.close()
        assert db.local('SELECT id FROM users;')[0]['rows'] == [['9']]
    db = Database(options)
    with db.server(['--sql-timeout-ms', 0], hold=100) as (port, process):
        connection = db.connect(port)
        send_query(connection, 'VALUES(1);INSERT INTO users(id) VALUES(99);')
        connection.close()
        # Repeated new connections exercise stale completion disposal and reuse.
        for _ in range(8):
            result = db.remote(port, 'VALUES(42);')
            assert result.returncode == 0
        assert process.poll() is None
    assert db.local('SELECT id FROM users;')[0]['rows'] == []


def regression(options):
    db = Database(options)
    db.local("INSERT INTO users(id,name) VALUES(1,'one'),(2,'two');")
    with db.server(['--workers', 4, '--sql-timeout-ms', 0]) as (port, _):
        def read(_):
            result = db.remote(port, 'SELECT id,name FROM users ORDER BY id;')
            assert result.returncode == 0, result.stderr
            assert json.loads(result.stdout)['rows'] == [['1', 'one'], ['2', 'two']]
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(read, range(8)))
        for executable in (options.unit, options.legacy):
            # Normal stages retain strict v1 wire compatibility.
            process = subprocess.run([executable, '--sdk-normal', str(port)], capture_output=True, timeout=10)
            assert process.returncode == 0, process.stderr
    assert db.local('SELECT id,name FROM users ORDER BY id;')[0]['rows'] == [['1', 'one'], ['2', 'two']]


def main():
    parser = argparse.ArgumentParser()
    for name in ('unit', 'legacy', 'exe', 'server', 'fault-server', 'remote', 'case', 'work'):
        parser.add_argument('--'+name, required=True)
    options = parser.parse_args()
    {'M13-LIFE-01': lifetime, 'M13-WIRE-01': timeout_wire,
     'M13-REGRESSION-01': regression}[options.case](options)
    print(options.case+' passed')


if __name__ == '__main__':
    main()
