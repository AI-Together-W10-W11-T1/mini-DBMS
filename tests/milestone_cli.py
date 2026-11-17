"""M1 executable acceptance slices. Each case owns its DB and uses actual processes."""
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import threading


def check(value, message):
    if not value:
        raise AssertionError(message)


class Fixture:
    def __init__(self, exe, work, fault_exe=None):
        self.exe, self.work = exe, Path(work)
        self.fault_exe = fault_exe
        self.work.mkdir(parents=True, exist_ok=True)
        self.schema, self.data = self.work / "schema", self.work / "data"
        # CTest 재실행은 새 작업 경로를 쓰므로 이전 성공 상태를 재사용하지 않는다.
        import tempfile
        self.work = Path(tempfile.mkdtemp(prefix="attempt-", dir=self.work))
        self.schema, self.data = self.work / "schema", self.work / "data"
        self.schema.mkdir()
        self.definition = {"version": 1, "name": "users", "columns": [
            {"name": n, "type": t} for n, t in
            [("id", "INT64"), ("name", "TEXT"), ("age", "INT64"), ("team", "TEXT")]]}
        self.schema_file = self.schema / "users.json"
        self.schema_file.write_text(json.dumps(self.definition), encoding="utf-8")

    def args(self, command="run", fmt="jsonl"):
        return [self.exe, command, "--schema-dir", str(self.schema),
                "--data-dir", str(self.data), "--format", fmt]

    def invoke(self, args, input=None, env=None, parse_stdout=True):
        result = subprocess.run(args, input=input, capture_output=True, timeout=15, env=env)
        return result, ([json.loads(line) for line in result.stdout.splitlines()] if parse_stdout else []), [
            json.loads(line) for line in result.stderr.splitlines()]

    def init(self):
        result, out, err = self.invoke(self.args("init"))
        check(result.returncode == 0, f"init failed {result.returncode}: {result.stderr!r}")
        check(not err and len(out) == 1 and out[0]["output_ready"] is True, "init wire")
        check(out[0]["tables"] == [{"name": "users", "action": "created", "row_count": "0"}], "init tables")

    def sql(self, sql, expected=0, file=False):
        raw = sql.encode("utf-8") if isinstance(sql, str) else sql
        args = self.args()
        if file:
            path = self.work / "input.sql"
            path.write_bytes(raw)
            args += ["--file", str(path)]
            raw = None
        else:
            args += ["--stdin"]
        result, out, err = self.invoke(args, raw)
        check(result.returncode == expected, f"SQL exit {result.returncode}, wanted {expected}: {result.stderr!r}")
        return out, err


def roundtrip(f):
    """P1-SQL-001 entirely: persistence, typed projection, omitted NULL columns."""
    f.init()
    out, err = f.sql("INSERT INTO users(id,name) VALUES(1,'Kim');", file=True)
    check(not err and out == [{"version": 1, "type": "statement_ok", "statement_index": "1",
          "command": "INSERT", "affected_rows": "1", "committed": True}], "INSERT wire")
    out, err = f.sql("SELECT id,name FROM users WHERE id=1;")
    check(not err and out[0]["columns"] == [{"name": "id", "type": "INT64"}, {"name": "name", "type": "TEXT"}], "typed columns")
    check(out[0]["rows"] == [["1", "Kim"]] and out[0]["row_count"] == "1", "restart SELECT")
    out, _ = f.sql("SELECT * FROM users;")
    check(out[0]["rows"] == [["1", "Kim", None, None]], "omitted columns NULL")
    p = subprocess.run(f.args(fmt="table") + ["--stdin"], input=b"SELECT id,name FROM users;", capture_output=True, timeout=15)
    check(p.returncode == 0 and p.stdout == b'statement 1 query_result\n"id":INT64|"name":TEXT\n1|"Kim"\nrow_count 1\n', "table bytes")
    out, _ = f.sql("SELECT id FROM users WHERE name='Kim';")
    check(out[0]["rows"] == [["1"]], "non-index TEXT equality")
    out, _ = f.sql("SELECT id FROM users WHERE name='kim';")
    check(out[0]["rows"] == [], "TEXT equality preserves case")
    middle = Fixture(f.exe, f.work / "middle-id", f.fault_exe)
    middle.definition["name"] = "Users"
    middle.definition["columns"] = [{"name": "Name", "type": "TEXT"}, {"name": "ID", "type": "INT64"}]
    middle.schema_file.write_text(json.dumps(middle.definition), encoding="utf-8")
    result, _, err = middle.invoke(middle.args("init"))
    check(result.returncode == 0 and not err, "ID may be in middle with preserved spelling")
    middle.sql("INSERT INTO USERS(id,name) VALUES(7,'x');")
    out, _ = middle.sql("SELECT * FROM users;")
    check(out[0]["columns"] == [{"name": "Name", "type": "TEXT"}, {"name": "ID", "type": "INT64"}]
          and out[0]["rows"] == [["x", "7"]], "declared order and case are preserved")


def literals(f):
    f.init()
    text = "O'Brien;/*x*/--y\r\n한글|\\n"
    sql = "\ufeff;-- comment\r\n/* comment */ INSERT INTO users(id,name,age) VALUES(1,'" + text.replace("'", "''") + "',-9223372036854775808);SELECT * FROM users;"
    out, err = f.sql(sql, file=True)
    check(not err and out[1]["rows"] == [["1", text, "-9223372036854775808", None]], "literal byte preservation")
    p = subprocess.run(f.args(fmt="table") + ["--stdin"], input=b"SELECT name FROM users;", capture_output=True, timeout=15)
    check(p.returncode == 0 and b"\\u007c" in p.stdout and b"\\\\u007c" not in p.stdout, "one literal backslash for table pipe escape")
    _, err = f.sql(b"INSERT INTO users(id,name) VALUES(2,'zero\x00byte');", 4)
    check(err[0]["code"] == "LEXICAL_ERROR", "SQL input NUL is lexical even in string")
    out, _ = f.sql("SELECT id FROM users;")
    check(out[0]["rows"] == [["1"]], "NUL rejected before mutation")


def stop_on_error(f):
    f.init()
    out, err = f.sql("INSERT INTO users(id) VALUES(1);\r\nINSERT INTO users(id,name) VALUES(2,9);INSERT INTO users(id) VALUES(3);", 4)
    check(len(out) == 1 and len(err) == 1, "stop on first error")
    check(err[0]["code"] == "TYPE_MISMATCH" and err[0]["statement_index"] == "2", "error identity")
    out, _ = f.sql("SELECT id FROM users;")
    check(out[0]["rows"] == [["1"]], "prior commit retained and later SQL stopped")
    _, err = f.sql("INSERT INTO users(id) VALUES(2)", 4)
    check(err[0]["code"] == "SYNTAX_ERROR", "missing final semicolon")
    _, err = f.sql("INSERT INTO users VALUES(2,'x',NULL,NULL);", 4)
    check(err[0]["code"] == "SYNTAX_ERROR", "INSERT column list is mandatory")
    _, err = f.sql("\r\nSELECT missing FROM users;", 4)
    check(err[0]["code"] == "UNKNOWN_COLUMN" and err[0]["line"] == "2"
          and err[0]["column"] == "1" and err[0]["committed"] is None, "SELECT diagnostic position and no DML state")
    empty = Fixture(f.exe, f.work / "empty-where", f.fault_exe)
    empty.init()
    for sql, code in [("SELECT id FROM users WHERE missing=1;", "UNKNOWN_COLUMN"),
                      ("SELECT id FROM users WHERE name=1;", "TYPE_MISMATCH")]:
        out, err = empty.sql(sql, 4)
        check(not out and err[0]["code"] == code, "WHERE validation precedes empty scan")


def invalid_id(f):
    f.init()
    for sql, code in [("INSERT INTO users(name) VALUES('x');", "ID_REQUIRED"),
                      ("INSERT INTO users(id) VALUES(0);", "INVALID_ID"),
                      ("INSERT INTO users(id) VALUES(NULL);", "INVALID_ID"),
                      ("INSERT INTO users(id) VALUES(-1);", "INVALID_ID")]:
        out, err = f.sql(sql, 4)
        check(not out and err[0]["code"] == code, code)
    f.sql("INSERT INTO users(id) VALUES(1);")
    _, err = f.sql("INSERT INTO users(id) VALUES(1);", 4)
    check(err[0]["code"] == "DUPLICATE_ID", "duplicate id")
    out, _ = f.sql("SELECT id FROM users;")
    check(out[0]["rows"] == [["1"]], "failed inserts unchanged")


def strict_schema(f):
    f.schema_file.write_text('{"version":1,"name":"users","columns":[{"name":"id","type":"INT64"}],"na\\u006de":"users"}', encoding="utf-8")
    result, out, err = f.invoke(f.args("init"))
    check(result.returncode == 3 and not out and err[0]["code"] == "SCHEMA_ERROR", "decoded duplicate key")
    check(not f.data.exists(), "invalid catalog must not create output")
    for index, definition in enumerate([
        {**f.definition, "version": 1.0},
        {**f.definition, "extra": True},
        {**f.definition, "columns": [{"name": "id", "type": "TEXT"}]},
        {**f.definition, "columns": [{"name": "id", "type": "INT64"}, {"name": "ABS", "type": "TEXT"}]},
        {**f.definition, "columns": [{"name": "id", "type": "INT64"}, {"name": "asc", "type": "TEXT"}]},
        {**f.definition, "columns": [{"name": "id", "type": "INT64", "nullable": False}]},
    ]):
        other = Fixture(f.exe, f.work / f"invalid-{index}", f.fault_exe)
        other.schema_file.write_text(json.dumps(definition), encoding="utf-8")
        result, out, err = other.invoke(other.args("init"))
        check(result.returncode == 3 and not out and err[0]["code"] == "SCHEMA_ERROR" and not other.data.exists(), "strict schema matrix")


def corruption(f):
    f.init()
    f.sql("INSERT INTO users(id) VALUES(1);")
    path = f.data / "table_users.tbl"
    before = path.read_bytes()
    path.write_bytes(before[:-1])
    out, err = f.sql("SELECT * FROM users;", 3)
    check(not out and err[0]["code"] == "STORAGE_FORMAT_ERROR", "truncated payload startup validation")
    check(path.read_bytes() == before[:-1], "corrupt file preserved")
    import struct
    record = 20 + struct.unpack_from("<Q", before, 12)[0]
    malformed = [before[:record] + struct.pack("<Q", (1 << 64) - 1),
                 before[:record + 8] + b"\x02" + before[record + 9:],
                 before[:record + 9] + struct.pack("<q", 0) + before[record + 17:],
                 before + before[record:]]
    for raw in malformed:
        path.write_bytes(raw)
        out, err = f.sql("", 3)
        check(not out and err[0]["code"] == "STORAGE_FORMAT_ERROR" and path.read_bytes() == raw,
              "startup validates size, null tags and IDs even with empty SQL")


def pipe(f):
    f.init()
    proc = subprocess.Popen(f.args() + ["--stdin"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    lines = queue.Queue()
    threading.Thread(target=lambda: lines.put(proc.stdout.readline()), daemon=True).start()
    try:
        proc.stdin.write(b"INSERT INTO users(id) VALUES(1);")
        proc.stdin.flush()
        line = lines.get(timeout=8)
        check(json.loads(line)["committed"] is True, "execute and flush before EOF")
        proc.stdin.close()
        check(proc.wait(timeout=8) == 0, "pipe exit")
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=8)


def fault_run(f, mode, expected):
    check(f.fault_exe is not None, "test-only executable required")
    env = os.environ.copy()
    env["MINIDB_TEST_FAULT"] = mode
    args = f.args() + ["--stdin"]
    args[0] = f.fault_exe
    result, out, err = f.invoke(args,
        b"INSERT INTO users(id,name) VALUES(2,'fault');INSERT INTO users(id) VALUES(3);", env,
        parse_stdout=mode not in ("stdout", "stdout_cleanup"))
    check(result.returncode == expected, f"fault {mode}: exit {result.returncode}, expected {expected}: {result.stderr!r}")
    return out, err


def persisted_ids(f):
    """독립 바이너리 해독: 차단된 DB도 실행 재개 없이 반영 사실을 확인한다."""
    import struct
    raw = (f.data / "table_users.tbl").read_bytes()
    check(raw[:8] == b"MINIDBMS" and struct.unpack_from("<I", raw, 8)[0] == 1, "storage header")
    length = struct.unpack_from("<Q", raw, 12)[0]
    schema = json.loads(raw[20:20 + length])
    offset, ids = 20 + length, []
    while offset < len(raw):
        size = struct.unpack_from("<Q", raw, offset)[0]
        offset += 8
        end = offset + size
        row = {}
        for column in schema["columns"]:
            tag = raw[offset]
            offset += 1
            check(tag in (0, 1), "record null tag")
            value = None
            if tag and column["type"] == "INT64":
                value = struct.unpack_from("<q", raw, offset)[0]
                offset += 8
            elif tag:
                text_length = struct.unpack_from("<Q", raw, offset)[0]
                offset += 8
                value = raw[offset:offset + text_length].decode("utf-8")
                offset += text_length
            row[column["name"].lower()] = value
        check(offset == end and offset <= len(raw), "record framing")
        ids.append(row["id"])
    return ids


def seeded(f):
    f.init()
    f.sql("INSERT INTO users(id,name) VALUES(1,'original');")
    return (f.data / "table_users.tbl").read_bytes()


def lease(f):
    f.init()
    proc = subprocess.Popen(f.args() + ["--stdin"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    lines = queue.Queue()
    threading.Thread(target=lambda: lines.put(proc.stdout.readline()), daemon=True).start()
    try:
        proc.stdin.write(b"SELECT id FROM users;")
        proc.stdin.flush()
        check(json.loads(lines.get(timeout=8))["row_count"] == "0", "first process owns live DB")
        # 경로 별칭도 동일 실제 디렉터리 사용권으로 처리한다.
        args = f.args() + ["--stdin"]
        args[args.index("--data-dir") + 1] = str(f.data) + os.sep + "."
        result, out, err = f.invoke(args, b"SELECT id FROM users;")
        check(result.returncode == 3 and not out and err[0]["code"] == "DATABASE_BUSY", "same-directory lease denied")
        other = Fixture(f.exe, f.work / "independent", f.fault_exe)
        other.init()
        other.sql("SELECT id FROM users;")
        proc.stdin.close()
        check(proc.wait(timeout=8) == 0, "owner exits cleanly")
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=8)
    f.sql("SELECT id FROM users;")


def pending(f):
    before = seeded(f)
    marker = f.data / ".minidb.pending"
    marker.write_bytes(b"unresolved operation")
    out, err = f.sql("INSERT INTO users(id) VALUES(2);", 3)
    check(not out and err[0]["code"] == "RECOVERY_REQUIRED", "pending marker startup gate")
    check(marker.read_bytes() == b"unresolved operation", "marker preserved")
    check((f.data / "table_users.tbl").read_bytes() == before, "pending DB unchanged")


def append_rollback(f):
    before = seeded(f)
    out, err = fault_run(f, "append_write", 4)
    check(not out and len(err) == 1 and err[0]["code"] == "IO_ERROR", "partial append failure")
    check(err[0]["committed"] is False and err[0]["rollback"] == "confirmed", "rollback confirmed")
    check((f.data / "table_users.tbl").read_bytes() == before, "exact original length and bytes restored")
    check(not (f.data / ".minidb.pending").exists(), "confirmed rollback removes marker")
    out, _ = f.sql("SELECT id FROM users;")
    check(out[0]["rows"] == [["1"]], "restart after rollback")


def rollback_failure(f):
    before = seeded(f)
    out, err = fault_run(f, "rollback", 6)
    check(not out and any(d["rollback"] == "failed" and d["committed"] is None for d in err), "failed rollback is unresolved")
    raw = (f.data / "table_users.tbl").read_bytes()
    check(raw.startswith(before) and len(raw) > len(before), "partial append preserved after failed truncate")
    marker = f.data / ".minidb.pending"
    check(marker.exists(), "failed rollback retains pending marker")
    out, err = f.sql("SELECT id FROM users;", 3)
    check(not out and err[0]["code"] == "RECOVERY_REQUIRED", "restart gate outranks corrupt tail")
    check((f.data / "table_users.tbl").read_bytes() == raw, "no automatic repair")


def cleanup_committed(f):
    seeded(f)
    out, err = fault_run(f, "cleanup", 5)
    check(len(out) == 1 and out[0]["type"] == "statement_ok" and out[0]["committed"] is True
          and out[0]["affected_rows"] == "1", "cleanup failure preserves successful DML result")
    check(any(d["phase"] == "cleanup" and d["committed"] is True and d["affected_rows"] == "1" for d in err), "cleanup diagnostic state")
    check(persisted_ids(f) == [1, 2], "commit retained and following SQL absent")
    check((f.data / ".minidb.pending").exists(), "cleanup marker retained")
    _, err = f.sql("SELECT id FROM users;", 3)
    check(err[0]["code"] == "RECOVERY_REQUIRED", "cleanup restart blocked")


def output_failure(f):
    # 실패 모드는 별도 실행 파일에서만 활성화된다.
    seeded(f)
    out, err = fault_run(f, "stdout", 7)
    check(any(d["code"] == "IO_ERROR" and d["phase"] == "output" and d["source"] == "stdout"
              and d["committed"] is True and d["command"] == "INSERT" and d["affected_rows"] == "1" for d in err), "stdout failure committed diagnostic")
    check(persisted_ids(f) == [1, 2], "stdout failure keeps commit and stops later SQL")
    check(not (f.data / ".minidb.pending").exists(), "output-only failure leaves healthy storage")
    both = Fixture(f.exe, f.work / "both", f.fault_exe)
    seeded(both)
    _, err = fault_run(both, "stdout_cleanup", 5)
    check({d["phase"] for d in err} >= {"cleanup", "output"}, "both diagnostics preserved with cleanup priority")
    check(persisted_ids(both) == [1, 2], "combined failure preserves only first commit")
    clean = Fixture(f.exe, f.work / "production", f.fault_exe)
    clean.init()
    env = os.environ.copy()
    env["MINIDB_TEST_FAULT"] = "rollback"
    result, out, err = clean.invoke(clean.args() + ["--stdin"], b"INSERT INTO users(id) VALUES(1);", env)
    check(result.returncode == 0 and not err and out[0]["committed"] is True, "production ignores test fault environment")


def read_ahead(f):
    f.init()
    out, err = f.sql(b"INSERT INTO users(id) VALUES(1);\r\nINSERT INTO users(id,name) VALUES(2,'\xff');INSERT INTO users(id) VALUES(3);", 4)
    check(len(out) == 1 and err[0]["code"] == "LEXICAL_ERROR" and err[0]["statement_index"] == "2", "later invalid UTF8 does not prevent first commit")
    check(persisted_ids(f) == [1], "read-ahead error stops third statement")
    proc = subprocess.Popen(f.args() + ["--stdin"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    lines = queue.Queue()
    threading.Thread(target=lambda: lines.put(proc.stdout.readline()), daemon=True).start()
    try:
        # BOM/다중바이트 UTF-8/실제 CRLF의 분할 읽기를 허용해야 한다.
        for chunk in [b"\xef", b"\xbb\xbfINSERT INTO users(id,name) VALUES(2,'", b"\xed", b"\x95\x9c\r", b"\n", b"');"]:
            proc.stdin.write(chunk)
            proc.stdin.flush()
        check(json.loads(lines.get(timeout=8))["affected_rows"] == "1", "split UTF8 input succeeds before EOF")
        proc.stdin.close()
        check(proc.wait(timeout=8) == 0, "split UTF8 exit")
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=8)
    out, _ = f.sql("SELECT name FROM users WHERE id=2;")
    check(out[0]["rows"] == [["한\r\n"]], "split UTF8 and CRLF bytes preserved")


def argv_wire(f):
    missing = f.work / "must-not-exist"
    for args in [["--help"], ["run", "--help"], ["init", "--help"], ["migrate", "--help"], ["--version"]]:
        result = subprocess.run([f.exe] + args, capture_output=True, timeout=15)
        check(result.returncode == 0 and result.stdout and not result.stderr, "standalone help/version")
        # 실제 읽기 전용 handle을 자식 stdout으로 상속한다. DML 실패 주입을 쓰지 않는다.
        readonly = f.work / "readonly-stdout.txt"
        sentinel = b"read-only stdout must stay unchanged"
        readonly.write_bytes(sentinel)
        with readonly.open("rb") as output:
            failed = subprocess.run([f.exe] + args, stdout=output, stderr=subprocess.PIPE, timeout=15)
        check(failed.returncode == 7 and failed.stderr.startswith(b"diagnostic code=IO_ERROR ")
              and b"phase=output" in failed.stderr and b'source="stdout"' in failed.stderr,
              "help/version real stdout failure must return 7 with output diagnostic")
        for fact in [b"committed=null", b"statement_index=null", b"command=null",
                     b"affected_rows=null", b"output_ready=null"]:
            check(fact in failed.stderr, "help/version output failure must not invent DB state")
        check(readonly.read_bytes() == sentinel, "read-only inherited stdout remains intact")
    check(not missing.exists() and not f.data.exists(), "help does not create DB")
    for args in [[], ["run"], ["RUN"], ["--help", "--format", "jsonl"],
                 ["run", "--schema-dir", "--stdin"], ["run", "--unknown"]]:
        result = subprocess.run([f.exe] + args, capture_output=True, timeout=15)
        check(result.returncode == 2 and not result.stdout and result.stderr.startswith(b"diagnostic code=ARGUMENT_ERROR "), "default human argument diagnostic")
    args = [f.exe, "run", "--schema-dir", str(missing), "--data-dir", str(missing), "--stdin", "--format", "jsonl"]
    result, out, err = f.invoke(args, b"")
    fields = {"version", "type", "code", "message", "operation", "phase", "source", "path", "statement_index", "line", "column", "field_path", "byte_offset", "committed", "rollback", "command", "affected_rows", "output_ready"}
    check(result.returncode == 3 and not out and len(err) == 1 and set(err[0]) == fields, "complete diagnostic wire fields")
    check(err[0]["operation"] == "run" and err[0]["phase"] == "prepare" and err[0]["statement_index"] is None
          and err[0]["line"] is None and err[0]["column"] is None, "no invented SQL location for startup failure")
    f.init()
    args = [f.exe, "run", "--schema-dir", str(f.schema), "--data-dir", str(f.data), "--stdin"]
    result = subprocess.run(args, input=b"SELECT id FROM users;", capture_output=True, timeout=15)
    check(result.returncode == 0 and result.stdout == b'statement 1 query_result\n"id":INT64\nrow_count 0\n' and not result.stderr, "default table wire exact LF")
    before = (f.data / "table_users.tbl").read_bytes()
    result, out, err = f.invoke(f.args("init"))
    check(result.returncode == 3 and not out and (f.data / "table_users.tbl").read_bytes() == before,
          "existing init never overwrites")
    unicode = Fixture(f.exe, f.work / "한글 경로", f.fault_exe)
    unicode.init()
    unicode.sql("INSERT INTO users(id,name) VALUES(1,'한글');")
    out, _ = unicode.sql("SELECT name FROM users;")
    check(out[0]["rows"] == [["한글"]], "Unicode argv paths and UTF8 data")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--fault-exe", required=True)
    options = parser.parse_args()
    globals()[options.case.replace("-", "_")](Fixture(options.exe, options.work, options.fault_exe))
