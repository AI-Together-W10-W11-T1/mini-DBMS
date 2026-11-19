import argparse
import json
import socket
import struct
import subprocess
import threading


def frame(kind, value):
    payload = value if isinstance(value, bytes) else json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode()
    return kind.encode() + struct.pack('!I', len(payload)) + payload


def exact(sock, length):
    data = b''
    while len(data) < length:
        part = sock.recv(length-len(data))
        if not part:
            raise AssertionError('unexpected EOF')
        data += part
    return data


def sdk_test(options):
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    errors = []
    event = {'version': 1, 'result': {'version': 1, 'type': 'query_result', 'statement_index': '1',
        'columns': [{'name': 'column1', 'type': 'INT64'}], 'rows': [['42']], 'row_count': '1'},
        'diagnostic': None, 'stop': False, 'health': 'healthy', 'stage': None}
    raw = json.dumps(event, separators=(',', ':')).encode()
    def peer():
        try:
            connection, _ = listener.accept()
            with connection:
                for attempt in range(2):
                    header = exact(connection, 5)
                    request = json.loads(exact(connection, struct.unpack('!I', header[1:])[0]))
                    assert header[:1] == b'Q' and request['sql'] == 'VALUES (42);'
                    output = frame('B', {'version': 1, 'type': 'begin', 'event_index': '1', 'bytes': str(len(raw))})
                    output += b''.join(frame('D', raw[i:i+1]) for i in range(len(raw)))
                    output += frame('E', {'version': 1, 'type': 'end', 'event_index': '1'})
                    output += frame('F', {'version': 1, 'type': 'done', 'status': 'ok', 'events': '1', 'exit_code': 0})
                    connection.sendall(output)
        except Exception as error:
            errors.append(repr(error))
        finally:
            listener.close()
    worker = threading.Thread(target=peer, daemon=True)
    worker.start()
    process = subprocess.run([options.unit, '--sdk', str(listener.getsockname()[1])], capture_output=True, timeout=10)
    worker.join(2)
    assert process.returncode == 0, (process.returncode, process.stdout, process.stderr, errors)
    assert not errors, errors
    sdk_contract_rejections(options)


def sdk_contract_rejections(options):
    query = {'version': 1, 'result': {'version': 1, 'type': 'query_result', 'statement_index': '1',
        'columns': [{'name': 'column1', 'type': 'INT64'}], 'rows': [['42']], 'row_count': '1'},
        'diagnostic': None, 'stop': False, 'health': 'healthy', 'stage': None}
    diagnostic = {'version': 1, 'type': 'diagnostic', 'code': 'UNKNOWN_TABLE', 'message': 'Missing table',
        'operation': 'run', 'phase': 'execute', 'source': None, 'path': None, 'statement_index': '1',
        'line': '1', 'column': '1', 'field_path': None, 'byte_offset': '0', 'committed': None,
        'rollback': 'not_needed', 'command': None, 'affected_rows': None, 'output_ready': None}
    cases = []
    for field, value in [('statement_index', '01'), ('row_count', '01'), ('rows', [['042']]), ('rows', [['-042']])]:
        event = dict(query, result=dict(query['result'], **{field: value}))
        cases.append(('noncanonical-result-'+field, event, 0))
    for field, value in [('statement_index', '0'), ('line', '0'), ('column', '0'), ('affected_rows', '00'),
                         ('phase', 'bogus'), ('source', 'bogus'), ('command', 'DROP')]:
        event = dict(query, result=None, diagnostic=dict(diagnostic, **{field: value}), stop=True)
        cases.append(('invalid-diagnostic-'+field, event, 4))
    failures = []
    for name, event, code in cases:
        raw = json.dumps(event, separators=(',', ':')).encode()
        output = frame('B', {'version': 1, 'type': 'begin', 'event_index': '1', 'bytes': str(len(raw))})
        output += frame('D', raw)
        output += frame('E', {'version': 1, 'type': 'end', 'event_index': '1'})
        output += frame('F', {'version': 1, 'type': 'done', 'status': 'error' if code else 'ok', 'events': '1', 'exit_code': code})
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        port = listener.getsockname()[1]
        def peer():
            try:
                connection, _ = listener.accept()
                with connection:
                    header = exact(connection, 5)
                    exact(connection, struct.unpack('!I', header[1:])[0])
                    connection.sendall(output)
            except (ConnectionError, OSError):
                pass
            finally:
                listener.close()
        worker = threading.Thread(target=peer, daemon=True)
        worker.start()
        process = subprocess.run([options.unit, '--sdk-reject', str(port)],
            capture_output=True, timeout=5)
        worker.join(1)
        if process.returncode:
            failures.append((name, process.returncode, process.stderr.decode(errors='replace')))
    assert not failures, failures


def sdk_faults(options):
    event = {'version': 1, 'result': {'version': 1, 'type': 'query_result', 'statement_index': '1',
        'columns': [{'name': 'column1', 'type': 'INT64'}], 'rows': [['42']], 'row_count': '1'},
        'diagnostic': None, 'stop': False, 'health': 'healthy', 'stage': None}
    raw = json.dumps(event, separators=(',', ':')).encode()
    begin = lambda size=len(raw), index='1': frame('B', {'version': 1, 'type': 'begin', 'event_index': index, 'bytes': str(size)})
    end = lambda index='1': frame('E', {'version': 1, 'type': 'end', 'event_index': index})
    whole = begin()+frame('D',raw)+end()
    done = frame('F', {'version': 1, 'type': 'done', 'status': 'ok', 'events': '1', 'exit_code': 0})
    cases = [
        ('data-before-begin', frame('D', raw), 0),
        ('short-header', b'B\x00', 0),
        ('truncated-payload', begin()[:-2], 0),
        ('short-event', begin(len(raw)+1)+frame('D',raw)+end(), 0),
        ('long-event', begin(len(raw)-1)+frame('D',raw)+end(), 0),
        ('bad-start-index', begin(index='2')+frame('D',raw)+end('2'), 0),
        ('bad-end-index', begin()+frame('D',raw)+end('2'), 0),
        ('missing-end', begin()+frame('D',raw), 0),
        ('missing-done', whole, 1),
        ('nested-begin', begin()+begin(), 0),
        ('duplicate-index', whole+whole+done, 1),
        ('empty-data', begin()+frame('D',b'')+frame('D',raw)+end(), 0),
        ('done-while-event', begin()+frame('D',raw)+done, 0),
        ('unknown-done-field', whole+frame('F', {'version': 1, 'type': 'done', 'status': 'ok', 'events': '1', 'exit_code': 0, 'x': 1}), 1),
        ('wrong-done-total', whole+frame('F', {'version': 1, 'type': 'done', 'status': 'ok', 'events': '2', 'exit_code': 0}), 1),
        ('bad-done-status', whole+frame('F', {'version': 1, 'type': 'done', 'status': 'error', 'events': '1', 'exit_code': 0}), 1),
        ('invalid-utf8', begin(1)+frame('D',b'\xff')+end(), 0),
        ('duplicate-event-field', begin(len(raw)+12)+frame('D',raw[:-1]+b',"version":1}')+end(), 0),
        ('event-cap', begin(4097), 0, 4096),
        ('frame-cap', b'B'+struct.pack('!I',4097), 0, 67108864, 4096),
        ('response-cap', begin(20000)+frame('D',b'x'*8192)+frame('D',b'x'*8192), 0, 67108864, 1048576, 16384),
    ]
    for key, value in [('version', 2), ('health','unknown'), ('stop', 0), ('stage','unknown')]:
        modified = dict(event, **{key:value})
        data = json.dumps(modified).encode()
        cases.append(('bad-'+key,begin(len(data))+frame('D',data)+end(),0))
    for key, value in [('rows', [['9223372036854775808']]), ('rows', [[]]), ('row_count','2'),
                       ('columns',[{'name':'x','type':'FLOAT'}]), ('statement_index','18446744073709551616')]:
        modified = dict(event, result=dict(event['result'], **{key:value}))
        data = json.dumps(modified).encode()
        cases.append(('bad-result-'+key,begin(len(data))+frame('D',data)+end(),0))
    cases.append(('receive-deadline',b'B',0,67108864,1048576,268435456,True))
    for case in cases:
        name, output, events, *caps = case
        limits = [67108864,1048576,268435456,False]
        limits[:len(caps)] = caps
        event_cap, frame_cap, total_cap, delay = limits
        listener = socket.socket()
        listener.bind(('127.0.0.1',0)); listener.listen()
        port = listener.getsockname()[1]
        errors = []
        def peer():
            try:
                connection, _ = listener.accept()
                with connection:
                    header = exact(connection,5)
                    exact(connection,struct.unpack('!I',header[1:])[0])
                    connection.sendall(output)
                    if delay:
                        time.sleep(.15)
            except (ConnectionError, OSError):
                pass  # native strict rejection may close before all malicious bytes are sent
            except Exception as error:
                errors.append(repr(error))
            finally:
                listener.close()
        worker = threading.Thread(target=peer,daemon=True)
        worker.start()
        process = subprocess.run([options.unit,'--sdk-error',str(port),str(events),'50',str(event_cap),str(frame_cap),str(total_cap)],
            capture_output=True,timeout=5)
        worker.join(1)
        assert process.returncode == 0 and not errors, (name,process.returncode,process.stderr,errors)


# All server cases use fresh native processes and a separately initialized persistent DB.
import concurrent.futures
import contextlib
import os
from pathlib import Path
import signal
import tempfile
import time


def read_frame(sock):
    header = exact(sock, 5)
    return chr(header[0]), exact(sock, struct.unpack('!I', header[1:])[0])


def send_query(sock, sql, split=None):
    request = frame('Q', {'version': 1, 'type': 'execute', 'sql': sql})
    if split is None:
        sock.sendall(request)
    else:
        for i in range(0, len(request), split):
            sock.sendall(request[i:i+split])


def response(sock, sizes=None):
    events = []
    total = 0
    while True:
        kind, payload = read_frame(sock)
        total += len(payload)
        if kind == 'F':
            done = json.loads(payload)
            assert done['events'] == str(len(events)), done
            return events, done, total
        assert kind == 'B', kind
        begin = json.loads(payload)
        if sizes is not None:
            sizes.append([int(begin["bytes"]), len(payload)])
        assert begin['event_index'] == str(len(events)+1), begin
        raw = b''
        while True:
            kind, payload = read_frame(sock)
            total += len(payload)
            if kind == 'D':
                raw += payload
                assert len(raw) <= int(begin['bytes'])
                continue
            assert kind == 'E' and len(raw) == int(begin['bytes']), (kind, len(raw), begin)
            assert json.loads(payload)['event_index'] == begin['event_index']
            events.append(json.loads(raw))
            if sizes is not None:
                sizes[-1].append(len(payload))
            break


class Database:
    def __init__(self, options, suffix=''):
        self.options = options
        root = Path(options.work)
        root.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(prefix='r'+suffix+'-', dir=root))
        self.schema, self.data = self.root/'s', self.root/'d'
        self.schema.mkdir()
        (self.schema/'users.json').write_text(json.dumps({'version': 1, 'name': 'users', 'columns': [
            {'name': 'id', 'type': 'INT64'}, {'name': 'name', 'type': 'TEXT'}]}), encoding='utf-8')
        init = subprocess.run([options.exe, 'init', '--schema-dir', str(self.schema), '--data-dir', str(self.data),
            '--format', 'jsonl'], capture_output=True, timeout=15)
        assert init.returncode == 0, (init.returncode, init.stderr)

    def local(self, sql):
        result = subprocess.run([self.options.exe, 'run', '--schema-dir', str(self.schema), '--data-dir', str(self.data),
            '--format', 'jsonl', '--stdin'], input=sql.encode(), capture_output=True, timeout=15)
        assert result.returncode == 0, (result.returncode, result.stderr)
        return [json.loads(line) for line in result.stdout.splitlines()]

    @contextlib.contextmanager
    def server(self, extra=(), fault=None, hold=0, sndbuf=0):
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            port = reserve.getsockname()[1]
        env = dict(os.environ)
        if fault:
            env['MINIDB_TEST_FAULT'] = fault
        if hold:
            env['MINIDB_TEST_SERVER_HOLD_MS'] = str(hold)
        if sndbuf:
            env['MINIDB_TEST_SEND_BUFFER'] = str(sndbuf)
        executable = self.options.fault_server if fault or hold or sndbuf else self.options.server
        process = subprocess.Popen([executable, '--schema-dir', str(self.schema), '--data-dir', str(self.data),
            '--port', str(port), *map(str, extra)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP, env=env)
        try:
            deadline = time.monotonic()+10
            while True:
                assert process.poll() is None, ('server exited', process.returncode, process.communicate())
                try:
                    probe = socket.create_connection(('127.0.0.1', port), timeout=.1)
                    probe.close()
                    break
                except OSError:
                    assert time.monotonic() < deadline, 'server startup timed out'
                    time.sleep(.01)
            yield port, process
        finally:
            if process.poll() is None:
                process.send_signal(signal.CTRL_BREAK_EVENT)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    raise AssertionError('server failed graceful join')
            out, err = process.communicate()
            assert process.returncode == 0, (process.returncode, out, err)

    def connect(self, port):
        sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        sock.settimeout(5)
        return sock

    def remote(self, port, sql, extra=(), stdin=True):
        args = [self.options.remote, '--host', '127.0.0.1', '--port', str(port), '--format', 'jsonl', *map(str, extra)]
        raw = sql.encode()
        if stdin:
            args += ['--stdin']
        else:
            path = self.root/'input.sql'
            path.write_bytes(raw)
            args += ['--file', str(path)]
            raw = None
        return subprocess.run(args, input=raw, capture_output=True, timeout=15)


def server_wire(options):
    db = Database(options)
    with db.server() as (port, _):
        for split in (1, 2, 3, 4, 5, 7, 64):
            with db.connect(port) as sock:
                send_query(sock, 'VALUES (42);VALUES (NULL);', split)
                events, done, _ = response(sock)
                assert done['exit_code'] == 0 and len(events) == 2
                assert events[0]['result']['rows'] == [['42']]
                assert events[1]['result']['rows'] == [[None]]
                send_query(sock, 'VALUES (-9223372036854775808);')
                assert response(sock)[0][0]['result']['rows'] == [['-9223372036854775808']]


def invalid_wire(options):
    db = Database(options)
    bad = [b'X\x00\x00\x00\x00', b'Q\xff\xff\xff\xff',
        frame('Q', b'{"version":1,"version":1,"type":"execute","sql":"INSERT INTO users(id) VALUES(9);"}'),
        frame('Q', {'version': 2, 'type': 'execute', 'sql': 'INSERT INTO users(id) VALUES(9);'}),
        frame('Q', {'version': 1, 'type': 'execute', 'sql': '', 'extra': True}),
        frame('Q', b'{"version":1,"type":"execute","sql":"\xff"}')]
    with db.server() as (port, _):
        for raw in bad:
            with db.connect(port) as sock:
                sock.sendall(raw)
                events, done, _ = response(sock)
                assert events[0]['diagnostic']['code'] == ('LIMIT_EXCEEDED' if raw[:5] == b'Q\xff\xff\xff\xff' else 'PROTOCOL_ERROR'), events
                assert events[0]['stage'] == 'protocol' and done['exit_code'] == 8
        for cut in range(1, 8):
            sock = db.connect(port)
            sock.sendall(frame('Q', {'version': 1, 'type': 'execute', 'sql': 'INSERT INTO users(id) VALUES(9);'})[:cut])
            sock.close()
        with db.connect(port) as sock:
            send_query(sock, 'SELECT id FROM users;')
            assert response(sock)[0][0]['result']['rows'] == []


def chunk_roundtrip(options):
    db = Database(options)
    text = "한글🙂'\\\n\t|"*6000
    sql = "INSERT INTO users(id,name) VALUES(9223372036854775807,'"+text.replace("'", "''")+"');SELECT * FROM users;"
    with db.server(['--frame-bytes', 4096, '--request-bytes', 4096]) as (port, _):
        # request cap is independent of returned event cap
        result = db.remote(port, 'VALUES (42);', ['--frame-bytes', 4096, '--request-bytes', 4096])
        assert result.returncode == 0, result.stderr
    # Full request fits, while every returned data frame is chunked at 64 KiB.
    with db.server() as (port, _):
        result = db.remote(port, sql)
        assert result.returncode == 0, result.stderr
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        assert rows[1]['rows'] == [['9223372036854775807', text]], rows[1]['row_count']
        with db.connect(port) as sock:
            send_query(sock, 'SELECT name FROM users;')
            events, done, _ = response(sock)
            assert events[0]['result']['rows'] == [[text]] and done['exit_code'] == 0


def sql_stop(options):
    db = Database(options)
    with db.server() as (port, _):
        with db.connect(port) as sock:
            send_query(sock, "INSERT INTO users(id) VALUES(1);INSERT INTO users(id,name) VALUES(2,9);INSERT INTO users(id) VALUES(3);")
            events, done, _ = response(sock)
            assert len(events) == 2 and events[0]['result']['committed'] is True
            assert events[1]['diagnostic']['code'] == 'TYPE_MISMATCH' and done['exit_code'] == 4
            send_query(sock, 'SELECT id FROM users;')
            assert response(sock)[0][0]['result']['rows'] == [['1']]


def pool(options):
    db = Database(options)
    with db.server(['--workers', 2, '--max-connections', 24]) as (port, _):
        idle = [db.connect(port) for _ in range(12)]
        try:
            def job(i):
                result = db.remote(port, f'INSERT INTO users(id) VALUES({i});')
                assert result.returncode == 0, result.stderr
            with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
                list(executor.map(job, range(1, 13)))
            native = subprocess.run([options.unit,'--sdk',str(port)],capture_output=True,timeout=5)
            assert native.returncode==0, native.stderr
            result = db.remote(port, 'SELECT COUNT(*) FROM users;')
            assert result.returncode == 0 and json.loads(result.stdout)['rows'] == [['12']], result.stderr
        finally:
            for sock in idle:
                sock.close()


def queue_limits(options):
    db = Database(options)
    with db.server(['--workers', 1, '--pending-jobs', 1], hold=300) as (port, _):
        with db.connect(port) as a, db.connect(port) as b, db.connect(port) as c:
            send_query(a, 'VALUES (1);')
            time.sleep(.08)
            send_query(b, 'VALUES (2);')
            time.sleep(.02)
            send_query(c, 'INSERT INTO users(id) VALUES(3);')
            events, done, _ = response(c)
            assert events[0]['diagnostic']['code'] == 'SERVER_BUSY' and events[0]['stage'] == 'queue' and done['exit_code'] == 8, events
            assert response(a)[1]['exit_code'] == 0
            assert response(b)[1]['exit_code'] == 0
    assert db.local('SELECT id FROM users;')[0]['rows'] == []


def ownership(options):
    db = Database(options)
    with db.server(['--workers', 2, '--max-connections', 4], hold=80) as (port, _):
        for i in range(12):
            sock = db.connect(port)
            send_query(sock, f'INSERT INTO users(id) VALUES({i+1});VALUES (9);')
            sock.close()
            with db.connect(port) as next_sock:
                send_query(next_sock, 'VALUES (42);')
                events, done, _ = response(next_sock)
                assert events[0]['result']['rows'] == [['42']] and len(events) == 1 and done['exit_code'] == 0


def disconnect(options):
    db = Database(options)
    text = 'x'*3000000
    db.local("INSERT INTO users(id,name) VALUES(1,'"+text+"');")
    with db.server(['--workers', 1], sndbuf=1024) as (port, _):
        sock = db.connect(port)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        send_query(sock, 'SELECT name FROM users;INSERT INTO users(id) VALUES(2);')
        kind, _ = read_frame(sock)
        assert kind == 'B'
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('hh', 1, 0))
        sock.close()
        time.sleep(.1)
        result = db.remote(port, 'SELECT id FROM users;')
        assert result.returncode == 0 and json.loads(result.stdout)['rows'] == [['1']], result.stderr


def states(options):
    for mode, code, health in [('cleanup', 5, 'stopped_cleanup'), ('rollback', 6, 'recovery_required')]:
        db = Database(options, mode)
        with db.server(fault=mode) as (port, _):
            result = db.remote(port, 'INSERT INTO users(id) VALUES(1);INSERT INTO users(id) VALUES(2);')
            assert result.returncode == code, (mode, result.returncode, result.stderr)
            diagnostics = [json.loads(line) for line in result.stderr.splitlines()]
            assert diagnostics and diagnostics[0]['committed'] == (True if mode == 'cleanup' else None)
            if mode == 'cleanup':
                assert json.loads(result.stdout)['affected_rows'] == '1'
            else:
                assert not result.stdout and diagnostics[0]['rollback'] == 'failed'
            with db.connect(port) as sock:
                send_query(sock, 'VALUES (42);')
                events, done, _ = response(sock)
                assert events[0]['health'] == health and done['exit_code'] == 6


def caps(options):
    exact_caps(options)
    db = Database(options)
    db.local("INSERT INTO users(id,name) VALUES(1,'"+'x'*6000+"');")
    with db.server(['--event-bytes', 4096]) as (port, _):
        result = db.remote(port, 'INSERT INTO users(id) VALUES(2);SELECT name FROM users;INSERT INTO users(id) VALUES(3);')
        assert result.returncode == 8, (result.returncode, result.stderr)
        assert json.loads(result.stdout)['committed'] is True
        assert json.loads(result.stderr)['code'] == 'LIMIT_EXCEEDED', result.stderr
    with db.server(['--response-bytes', 16384]) as (port, _):
        with db.connect(port) as sock:
            send_query(sock, 'INSERT INTO users(id) VALUES(4);INSERT INTO users(id) VALUES(5);')
            events, done, total = response(sock)
            assert len(events) == 1 and total <= 16384 and done['exit_code'] == 8
            assert events[0]['result']['committed'] is True and events[0]['diagnostic']['committed'] is True
            assert events[0]['diagnostic']['affected_rows'] == '1'
    assert db.local('SELECT id FROM users ORDER BY id;')[0]['rows'] == [['1'], ['2'], ['4']]


def exact_caps(options):
    db = Database(options,'exact')
    sql = "VALUES ('"+'x'*5000+"');"
    sizes = []
    with db.server() as (port, _):
        with db.connect(port) as sock:
            send_query(sock,sql)
            events, done, _ = response(sock,sizes)
            assert done['exit_code']==0
    event_size, begin_size, end_size = sizes[0]
    payload_size = event_size+begin_size+end_size
    for delta in [-1,0,1]:
        with db.server(['--event-bytes',event_size+delta]) as (port, _):
            with db.connect(port) as sock:
                send_query(sock,sql)
                events, done, _ = response(sock)
                assert done['exit_code']==(8 if delta<0 else 0), (delta,done)
        with db.server(['--response-bytes',16384+payload_size+delta]) as (port, _):
            with db.connect(port) as sock:
                send_query(sock,sql)
                events, done, total = response(sock)
                assert done['exit_code']==(8 if delta<0 else 0), (delta,done)
                assert total<=16384+payload_size+delta
    request_size = len(frame('Q',{'version':1,'type':'execute','sql':'VALUES (42);'}))-5
    for delta in [-1,0,1]:
        with db.server(['--request-bytes',request_size+delta,'--frame-bytes',4096]) as (port, _):
            with db.connect(port) as sock:
                send_query(sock,'VALUES (42);')
                events, done, _ = response(sock)
                assert done['exit_code']==(8 if delta<0 else 0)
                if delta<0:
                    assert events[0]['diagnostic']['code']=='LIMIT_EXCEEDED'


def configuration(options):
    db = Database(options)
    base = [options.server, '--schema-dir', str(db.schema), '--data-dir', str(db.data), '--port', '12345']
    for name, value in [('--workers', '0'), ('--max-connections', '0'), ('--pending-jobs', '0'), ('--workers', '-1'),
        ('--workers', '+1'), ('--workers', '18446744073709551616'), ('--workers', '18446744073709551615'),
        ('--pending-jobs', '18446744073709551615'), ('--max-connections', '1023'), ('--sql-timeout-ms', '18446744073709551615'),
        ('--frame-bytes', '4095'), ('--event-bytes', '4095'), ('--response-bytes', '16383'), ('--request-bytes', '0'),
        ('--frame-bytes', '4294967296'), ('--bind', 'localhost'), ('--idle-timeout-ms', '-1'), ('--unknown', '1')]:
        process = subprocess.run(base+[name, value], capture_output=True, timeout=5)
        assert process.returncode == 2, (name, value, process.returncode, process.stderr)
    for extra in [['--port', '1'], ['--workers', '1', '--workers', '2'], ['--workers']]:
        assert subprocess.run(base+extra, capture_output=True, timeout=5).returncode == 2
    for extra in [['--frame-bytes', '4095'], ['--response-bytes', '16383'], ['--port', '0'], ['--receive-timeout-ms', '-1']]:
        result = db.remote(1, '', extra)
        assert result.returncode == 2, (extra, result.returncode, result.stderr)
    invalid_host = subprocess.run([options.remote,'--host','localhost','--port','1','--stdin'],input=b'',capture_output=True,timeout=3)
    assert invalid_host.returncode==2, invalid_host.stderr
    with db.server(['--workers', 3, '--pending-jobs', 1, '--max-connections', 8,
        '--receive-timeout-ms', 0, '--send-timeout-ms', 0, '--queue-timeout-ms', 0, '--lock-timeout-ms', 0, '--idle-timeout-ms', 0]) as (port, _):
        assert db.remote(port, 'VALUES (42);').returncode == 0


def sdk_deadlines(options):
    # With connect timeout disabled, a refused local port must still report I/O failure.
    with socket.socket() as closed:
        closed.bind(('127.0.0.1',0))
        refused = closed.getsockname()[1]
    process = subprocess.run([options.unit,'--sdk-connect-fail',str(refused)], capture_output=True,timeout=3)
    assert process.returncode == 0, process.stderr
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024)
    listener.bind(('127.0.0.1',0)); listener.listen()
    port = listener.getsockname()[1]
    def stalled():
        connection, _ = listener.accept()
        with connection:
            time.sleep(.3)
        listener.close()
    thread = threading.Thread(target=stalled,daemon=True)
    thread.start()
    process = subprocess.run([options.unit,'--sdk-send-timeout',str(port)],capture_output=True,timeout=5)
    thread.join(1)
    assert process.returncode == 0, process.stderr


def deadlines(options):
    sdk_deadlines(options)
    db = Database(options)
    with db.server(['--receive-timeout-ms', 60, '--idle-timeout-ms', 100]) as (port, _):
        with db.connect(port) as sock:
            sock.sendall(b'Q')
            time.sleep(.08)
            events, done, _ = response(sock)
            assert events[0]['diagnostic']['code'] == 'TIMEOUT' and events[0]['stage'] == 'receive' and done['exit_code'] == 8
        with db.connect(port) as sock:
            time.sleep(.15)
            assert sock.recv(1) == b''
    for stage, workers, extra in [('queue', 1, ['--queue-timeout-ms', 40]), ('lock', 2, ['--lock-timeout-ms', 40])]:
        with db.server(['--workers', workers, *extra], hold=250) as (port, _):
            with db.connect(port) as a, db.connect(port) as b:
                send_query(a, 'VALUES (1);')
                time.sleep(.06)
                send_query(b, 'INSERT INTO users(id) VALUES(2);')
                events, done, _ = response(b)
                assert events[0]['stage'] == stage and events[0]['diagnostic']['code'] == 'TIMEOUT' and done['exit_code'] == 8, events
                assert response(a)[1]['exit_code'] == 0
    db.local("INSERT INTO users(id,name) VALUES(1,'"+'x'*1000000+"');")
    with db.server(['--send-timeout-ms',40],sndbuf=1024) as (port, _):
        with db.connect(port) as slow:
            slow.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024)
            send_query(slow,'SELECT name FROM users;INSERT INTO users(id) VALUES(2);')
            time.sleep(.2)
            slow.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1048576)
            received = b''
            while True:
                data = slow.recv(65536)
                if not data:
                    break
                received += data
            # Send deadline closes the transport before the large event completes.
            assert len(received)<1000000
        result = db.remote(port,'SELECT id FROM users;')
        assert result.returncode==0 and json.loads(result.stdout)['rows']==[['1']], result.stderr
    assert db.local('SELECT id FROM users;')[0]['rows'] == [['1']]


def shutdown_test(options):
    db = Database(options)
    with db.server(['--workers', 1], hold=150) as (port, process):
        idle = db.connect(port)
        partial = db.connect(port)
        partial.sendall(b'Q\x00')
        running = db.connect(port)
        queued = db.connect(port)
        send_query(running, 'INSERT INTO users(id) VALUES(1);INSERT INTO users(id) VALUES(2);')
        time.sleep(.05)
        send_query(queued, 'INSERT INTO users(id) VALUES(3);')
        process.send_signal(signal.CTRL_BREAK_EVENT)
        for sock in [idle, partial, running, queued]:
            try:
                while sock.recv(4096):
                    pass
            except (ConnectionError, socket.timeout):
                pass
            finally:
                sock.close()
        process.wait(timeout=5)
        assert process.returncode == 0
    rows = db.local('SELECT id FROM users;')[0]['rows']
    assert rows in ([], [['1']]), rows


def cli_test(options):
    db = Database(options)
    with db.server() as (port, _):
        for stdin in [True, False]:
            result = db.remote(port, "VALUES (42,'한글',NULL);", stdin=stdin)
            assert result.returncode == 0 and json.loads(result.stdout)['rows'] == [['42', '한글', None]], result.stderr
        result = db.remote(port, 'VALUES (42);', ['--format', 'table'])
        assert result.returncode == 2  # duplicate options are rejected
        process = subprocess.run([options.remote, '--host', '127.0.0.1', '--port', str(port), '--format', 'table', '--stdin'],
            input=b'VALUES (42);', capture_output=True, timeout=10)
        assert process.returncode == 0 and b'42' in process.stdout and b'row_count 1' in process.stdout
        process = subprocess.Popen([options.remote, '--host', '127.0.0.1', '--port', str(port), '--stdin'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        process.stdout.close()
        process.stdin.write(b'VALUES (42);')
        process.stdin.close()
        stderr = process.stderr.read()
        assert process.wait(timeout=10) == 7 and b'stdout write failed' in stderr
        process = subprocess.Popen([options.remote, '--host', '127.0.0.1', '--port', str(port), '--format', 'jsonl', '--stdin'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        process.stdout.close()
        process.stdin.write(b'INSERT INTO users(id) VALUES(9);')
        process.stdin.close()
        diagnostics = [json.loads(line) for line in process.stderr.read().splitlines()]
        assert process.wait(timeout=10)==7
        assert diagnostics[-1]['committed'] is True and diagnostics[-1]['affected_rows']=='1'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--unit', required=True)
    parser.add_argument('--case', required=True)
    parser.add_argument('--exe')
    parser.add_argument('--server')
    parser.add_argument('--fault-server')
    parser.add_argument('--remote')
    parser.add_argument('--work')
    options = parser.parse_args()
    cases = {'M12-SDK-01': sdk_test, 'M12-WIRE-01': server_wire, 'M12-WIRE-02': invalid_wire,
        'M12-CHUNK-01': chunk_roundtrip, 'M12-CHUNK-02': sdk_faults, 'M12-SQL-01': sql_stop,
        'M12-POOL-01': pool, 'M12-QUEUE-01': queue_limits, 'M12-OWN-01': ownership,
        'M12-DISCONNECT-01': disconnect, 'M12-STATE-01': states, 'M12-LIMIT-01': caps,
        'M12-CONFIG-01': configuration, 'M12-TIME-01': deadlines, 'M12-SHUTDOWN-01': shutdown_test,
        'M12-CLI-01': cli_test, 'M12-REGRESSION-01': server_wire}
    cases[options.case](options)
    print(options.case+' native acceptance passed')


if __name__ == '__main__':
    main()
