"""M3 contracts against real executables and independent persisted-byte decoding."""
import argparse
from collections import Counter
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import queue
import subprocess
import threading

from milestone_cli import Fixture, check, seeded
from m2_cli import baseline, decoded_rows, rows, snapshot


def success(f, sql, command, count):
    out, err = f.sql(sql)
    check(not err and out == [{"version": 1, "type": "statement_ok", "statement_index": "1",
          "command": command, "affected_rows": str(count), "committed": True}], f"{command} wire")


def failure(f, sql, code, command="UPDATE"):
    before = snapshot(f)
    out, err = f.sql(sql, 4)
    check(not out and len(err) == 1 and err[0]["code"] == code, f"expected {code}: {err}")
    check(err[0]["command"] == command and err[0]["committed"] is False, "DML failure identity")
    check(snapshot(f) == before, "no partial change")


def update_01(f):
    baseline(f)
    success(f, "UPDATE users SET name='한글 O''Brien;|\r\n',age=NULL,team='' WHERE id=1;", "UPDATE", 1)
    expected = [[1, "한글 O'Brien;|\r\n", None, ""], [2, "Lee", None, "A"], [3, "kim", 30, None], [4, "", 10, "B"]]
    check(decoded_rows(f) == expected, "decoded complete replacement")
    check(rows(f, "SELECT name,age,team FROM users WHERE id=1;") == Counter([(expected[0][1], None, "")]), "restart values")
    x = Fixture(f.exe, f.work / 'two-tables', f.fault_exe)
    (x.schema / 'orders.json').write_text(json.dumps({'version':1,'name':'orders','columns':[
        {'name':'id','type':'INT64'},{'name':'amount','type':'INT64'}]}), encoding='utf-8')
    result, _, err = x.invoke(x.args('init'))
    check(result.returncode == 0 and not err, 'two table init')
    x.sql("INSERT INTO users(id,name) VALUES(1,'u');INSERT INTO orders(id,amount) VALUES(1,100);")
    other_before = (x.data / 'table_orders.tbl').read_bytes()
    x.sql("UPDATE users SET name='changed' WHERE id=1;DELETE FROM users WHERE id=1;")
    check((x.data / 'table_orders.tbl').read_bytes() == other_before, 'other table bytes untouched')


def update_02(f):
    baseline(f)
    success(f, "UPDATE users SET name=team,team=name WHERE id<=2;", "UPDATE", 2)
    check(rows(f, "SELECT name,team FROM users WHERE id<=2;") == Counter([("A", "Kim"), ("A", "Lee")]), "old-row swap")
    success(f, "UPDATE users SET name='new',team=name WHERE id=1;", "UPDATE", 1)
    check(rows(f, "SELECT name,team FROM users WHERE id=1;") == Counter([("new", "A")]), "all sources old")


def update_03(f):
    baseline(f)
    for expr, expected in [("(age+2)-1", 21), ("1-2-3", -4), ("1-(2-3)", 2),
                           ("1+-2", -1), ("1- -2", 3), ("1-(-2)", 3),
                           ("-9223372036854775808", -(2**63)), ("9223372036854775807+0", 2**63-1),
                           ("-9223372036854775808+1", -(2**63)+1), ("(+1) + ( -2 )", -1)]:
        success(f, f"UPDATE users SET age={expr} WHERE id=1;", "UPDATE", 1)
        check(rows(f, "SELECT age FROM users WHERE id=1;") == Counter([(str(expected),)]), expr)
    success(f, "UPDATE users SET age=1-- comment\n+2 WHERE id=1;", "UPDATE", 1)
    check(rows(f, "SELECT age FROM users WHERE id=1;") == Counter([("3",)]), "line comment preserved")
    success(f, "UPDATE users SET age=" + '(' * 2048 + '1' + ')' * 2048 + " WHERE id=1;", "UPDATE", 1)
    success(f, "UPDATE users SET age=" + '+'.join(['1'] * 512) + " WHERE id=1;", "UPDATE", 1)
    check(rows(f, "SELECT age FROM users WHERE id=1;") == Counter([('512',)]), 'deep heap expression stacks')
    f.sql("VALUES(-9223372036854775808),(+1);INSERT INTO users(id,age) VALUES(5,-9223372036854775808);")


def update_04(f):
    baseline(f)
    success(f, "UPDATE users SET age=age+1 WHERE age IS NULL OR (id=3 AND NULL=1);", "UPDATE", 1)
    success(f, "UPDATE users SET age=NULL+1,name=NULL,team=users.name WHERE users.id=1;", "UPDATE", 1)
    check(rows(f, "SELECT name,age,team FROM users WHERE id<=2;") == Counter([(None,None,"Kim"),("Lee",None,"A")]), "NULL typed propagation")
    failure(f, "UPDATE users SET age=name+NULL WHERE 1=0;", "TYPE_MISMATCH")


def update_05(f):
    baseline(f)
    for sql, code in [("SET id=id WHERE id=999", "READ_ONLY_COLUMN"),
                      ("SET name='a',NAME='b' WHERE 1=0", "DUPLICATE_COLUMN"),
                      ("SET missing=1 WHERE 1=0", "UNKNOWN_COLUMN"),
                      ("SET age=missing WHERE 1=0", "UNKNOWN_COLUMN"),
                      ("SET age=other.age WHERE 1=0", "UNKNOWN_TABLE"),
                      ("SET age=name+1 WHERE 1=0", "TYPE_MISMATCH"),
                      ("SET name=age WHERE 1=0", "TYPE_MISMATCH"),
                      ("SET age=1 WHERE 1=0 AND name=1", "TYPE_MISMATCH")]:
        failure(f, "UPDATE users " + sql + ";", code)
    success(f, "DELETE FROM users WHERE 1=1;", "DELETE", 4)
    for sql, code in [("SET id=id WHERE id=999", "READ_ONLY_COLUMN"),
                      ("SET name=name,name='b' WHERE 1=0", "DUPLICATE_COLUMN"),
                      ("SET age=missing WHERE 1=0", "UNKNOWN_COLUMN"),
                      ("SET age=name+1 WHERE 1=0", "TYPE_MISMATCH")]:
        failure(f, "UPDATE users " + sql + ";", code)


def update_06(f):
    f.init()
    f.sql("INSERT INTO users(id,age) VALUES(1,0),(2,9223372036854775807);")
    for expr in ["age+1", "(9223372036854775807+1)-1", "-9223372036854775808-1",
                 "0-(-9223372036854775808)", "NULL+(9223372036854775807+1)"]:
        failure(f, f"UPDATE users SET age={expr} WHERE id>0;", "INTEGER_OVERFLOW")
    check(rows(f, "SELECT age FROM users;") == Counter([("0",), (str(2**63-1),)]), "all rows original")
    failure(f, "UPDATE users SET age=9223372036854775808 WHERE 1=0;", "INTEGER_RANGE")


def update_07(f):
    baseline(f)
    success(f, "UPDATE users SET name=name WHERE id<=2;", "UPDATE", 2)
    success(f, "UPDATE users SET age=age+1 WHERE id=999;", "UPDATE", 0)
    success(f, "DELETE FROM users WHERE 1=1;", "DELETE", 4)
    success(f, "UPDATE users SET name=NULL WHERE id>0;", "UPDATE", 0)


def update_08(f):
    baseline(f)
    for sql, code in [("UPDATE users SET name='X';", "SYNTAX_ERROR"),
                      ("UPDATE users SET WHERE id=1;", "SYNTAX_ERROR"),
                      ("UPDATE users SET age=1, WHERE id=1;", "SYNTAX_ERROR"),
                      ("UPDATE users SET age=(1+) WHERE id=1;", "SYNTAX_ERROR"),
                      ("UPDATE users SET age=() WHERE id=1;", "SYNTAX_ERROR"),
                      ("UPDATE users SET age=- 1 WHERE id=1;", "LEXICAL_ERROR"),
                      ("UPDATE users SET age=1*2 WHERE id=1;", "UNSUPPORTED_FEATURE"),
                      ("UPDATE users SET age=COUNT(*) WHERE id=1;", "UNSUPPORTED_FEATURE"),
                      ("UPDATE users AS u SET name='x' WHERE id=1;", "UNSUPPORTED_FEATURE"),
                      ("UPDATE users SET age=1 WHERE id=1 WHERE id=2;", "SYNTAX_ERROR")]:
        failure(f, sql, code)


def delete_01(f):
    baseline(f)
    success(f, "DELETE FROM users WHERE age IS NULL;", "DELETE", 1)
    success(f, "DELETE FROM users WHERE (users.id>=3 AND age<30) OR NULL=1;", "DELETE", 1)
    check(rows(f, "SELECT id FROM users;") == Counter([("1",),("3",)]), "only TRUE removed")
    success(f, "DELETE FROM users WHERE id=999;", "DELETE", 0)


def delete_02(f):
    baseline(f)
    success(f, "DELETE FROM users WHERE id=2;", "DELETE", 1)
    success(f, "INSERT INTO users(id,name) VALUES(2,'Reused');", "INSERT", 1)
    success(f, "DELETE FROM users WHERE 1=1;", "DELETE", 4)
    check(decoded_rows(f) == [] and rows(f, "SELECT * FROM users;") == Counter(), "header-only table")
    success(f, "INSERT INTO users(id,name) VALUES(1,'again');", "INSERT", 1)
    check(decoded_rows(f) == [[1,"again",None,None]], "append after deleting all")


def delete_03(f):
    baseline(f)
    for sql, code in [("DELETE FROM users;", "SYNTAX_ERROR"),
                      ("DELETE FROM missing_table WHERE id=1;", "UNKNOWN_TABLE"),
                      ("DELETE FROM users WHERE missing=1;", "UNKNOWN_COLUMN"),
                      ("DELETE FROM users WHERE other.id=1;", "UNKNOWN_TABLE"),
                      ("DELETE FROM users WHERE 1=0 AND name=1;", "TYPE_MISMATCH"),
                      ("DELETE FROM users AS u WHERE u.id=1;", "UNSUPPORTED_FEATURE")]:
        failure(f, sql, code, "DELETE")
    success(f, "DELETE FROM users WHERE 1=1;", "DELETE", 4)
    failure(f, "DELETE FROM users WHERE missing=1;", "UNKNOWN_COLUMN", "DELETE")


def fault(f, mode, command="UPDATE", expected=4, fmt="jsonl", production=False):
    env = os.environ.copy()
    env["MINIDB_TEST_FAULT"] = mode
    args = f.args(fmt=fmt) + ["--stdin"]
    if not production:
        args[0] = f.fault_exe
    sql = ("UPDATE users SET name='changed' WHERE id<=2;" if command == "UPDATE" else
           "DELETE FROM users WHERE id<=2;") + "INSERT INTO users(id) VALUES(99);"
    result = subprocess.run(args, input=sql.encode(), env=env, capture_output=True, timeout=15)
    check(result.returncode == expected, f"{command} {mode} exit {result.returncode} wanted {expected}: {result.stderr!r}")
    out = [json.loads(x) for x in result.stdout.splitlines()] if fmt == "jsonl" and mode not in ("stdout","stdout_cleanup") else []
    err = [json.loads(x) for x in result.stderr.splitlines()] if fmt == "jsonl" else []
    return result, out, err


def isolated(f, name):
    other = Fixture(f.exe, f.work / name, f.fault_exe)
    baseline(other)
    return other


def runtime_read_denial(f, command):
    """준비 완료 뒤 실제 OS 공유 거절을 일으켜 실행 단계 진단을 검증한다."""
    x = isolated(f, 'runtime-read-' + command)
    path = x.data / 'table_users.tbl'
    before = path.read_bytes()
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = wintypes.HANDLE(-1).value
    process = subprocess.Popen(x.args() + ['--stdin'], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        process.stdin.write(b'SELECT id FROM users WHERE id=1;\n')
        process.stdin.flush()
        ready = queue.Queue()
        threading.Thread(target=lambda: ready.put(process.stdout.readline()), daemon=True).start()
        check(json.loads(ready.get(timeout=15))['rows'] == [['1']], 'prepare and first query complete')
        handle = kernel.CreateFileW(str(path), 0x80000000, 0, None, 3, 0x80, None)
        check(handle != wintypes.HANDLE(-1).value, 'actual exclusive read handle')
        sql = ("UPDATE users SET name='new' WHERE id=1;" if command == 'UPDATE' else
               "DELETE FROM users WHERE id=1;")
        process.stdin.write(sql.encode())
        process.stdin.flush()
        process.stdin.close()
        process.stdin = None
        out, err = process.communicate(timeout=15)
    finally:
        if handle != wintypes.HANDLE(-1).value:
            check(kernel.CloseHandle(handle), 'release exclusive read handle')
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=15)
    check(process.returncode == 4 and not out, 'runtime staging read failed before mutation')
    diagnostic = json.loads(err)
    check(diagnostic['code'] == 'IO_ERROR' and diagnostic['phase'] == 'execute' and
          diagnostic['source'] == 'data', 'runtime file read uses execution/data context')
    check(Path(diagnostic['path']).resolve() == path.resolve() and
          diagnostic['command'] == command and diagnostic['statement_index'] == '2' and
          diagnostic['committed'] is False, 'runtime failure identity and actual path')
    check(path.read_bytes() == before, 'runtime read failure preserves original bytes')


def store_01(f):
    for command in ["UPDATE", "DELETE"]:
        for mode in ["replace_marker_create", "replace_marker_write", "replace_marker_flush", "replace_marker_close",
                     "replace_temp_create", "replace_temp_write", "replace_resource", "replace_temp_flush", "replace_temp_close"]:
            x = isolated(f, command + mode)
            before = snapshot(x)
            _, out, err = fault(x, mode, command)
            check(not out and err[0]["code"] == ("RESOURCE_ERROR" if mode == "replace_resource" else "IO_ERROR"), "precommit cause")
            check(err[0]["committed"] is False and err[0]["command"] == command, "precommit wire")
            check(snapshot(x) == before and rows(x, "SELECT id FROM users;") == Counter([(str(i),) for i in range(1,5)]), "precommit old bytes and restart")
        runtime_read_denial(f, command)


def store_02(f):
    for command in ["UPDATE", "DELETE"]:
        for mode in ["replace_1175", "replace_1176", "replace_other"]:
            x = isolated(f, command + mode)
            before = snapshot(x)
            _, out, err = fault(x, mode, command)
            check(not out and err[0]["code"] == "IO_ERROR" and err[0]["committed"] is False, "preserved names error")
            check(snapshot(x) == before, "original bytes")
            x.sql("SELECT * FROM users;")
            check(not (x.data / '.minidb.pending').exists() and not list(x.data.glob('*.bak')) and not list(x.data.glob('*.tmp')), "confirmed cleanup")


def store_03(f):
    for command in ["UPDATE", "DELETE"]:
        x = isolated(f, command)
        before = snapshot(x)
        _, out, err = fault(x, "replace_1177", command)
        check(not out and err[0]["rollback"] == "confirmed" and err[0]["committed"] is False, "1177 restored")
        check(snapshot(x) == before, "1177 exact original")
        x.sql("SELECT * FROM users;")


def store_04(f):
    for command in ['UPDATE','DELETE']:
        for mode in ["replace_unexpected", "replace_state", "replace_content", "replace_restore", "replace_restore_check", "replace_pre_cleanup"]:
            x = isolated(f, command + mode)
            before = snapshot(x)
            _, out, err = fault(x, mode, command, 6)
            check(not out and err[0]["code"] == "RECOVERY_REQUIRED" and err[0]["committed"] is None, "unresolved state")
            check((x.data / '.minidb.pending').exists() and (x.data / 'table_users.tbl.tmp').exists(), "marker and temp retained")
            if mode == "replace_unexpected":
                check(snapshot(x) == b"unexpected-main", "unexpected main preserved")
                check((x.data / 'table_users.tbl.bak').read_bytes() == before, "backup retained")
            elif mode == "replace_restore":
                check(not (x.data / 'table_users.tbl').exists(), "no fabricated restore")
                check((x.data / 'table_users.tbl.bak').read_bytes() == before, "original backup retained")
                check(err[0]['rollback'] == 'failed', 'failed restore status')
            else:
                check(snapshot(x) == before, 'original retained on check/cleanup failure')
                check(err[0]['rollback'] == 'unknown', 'unverified state is unknown')
            _, errors = x.sql("SELECT * FROM users;", 3)
            check(errors[0]["code"] == "RECOVERY_REQUIRED", "restart blocked")


def store_05(f):
    for command in ["UPDATE", "DELETE"]:
        for mode in ["replace_backup_cleanup", "replace_marker_cleanup", "cleanup"]:
            x = isolated(f, command + mode)
            _, out, err = fault(x, mode, command, 5)
            check(out[0]["committed"] is True and out[0]["affected_rows"] == "2", "committed count")
            check(err[0]["phase"] == "cleanup" and err[0]["command"] == command and err[0]["committed"] is True, "cleanup identity")
            decoded = decoded_rows(x)
            check([r[0] for r in decoded] == ([1,2,3,4] if command == "UPDATE" else [3,4]), "new disk and no99")
            if command == "UPDATE":
                check([r[1] for r in decoded[:2]] == ["changed","changed"], "committed new values")
            x.sql("SELECT * FROM users;", 3)


def store_06(f):
    for command in ["UPDATE", "DELETE"]:
        for fmt in ["jsonl", "table"]:
            for mode, code in [("stdout",7),("stdout_cleanup",5)]:
                x = isolated(f, command + fmt + mode)
                result, _, err = fault(x, mode, command, code, fmt)
                check(99 not in [r[0] for r in decoded_rows(x)], "later SQL stopped")
                if fmt == "jsonl":
                    check(any(e["phase"] == "output" and e["command"] == command and e["committed"] is True and e["affected_rows"] == "2" for e in err), "stdout commit diagnostic")
                else:
                    check(b'phase=output' in result.stderr and b'committed=true' in result.stderr, "table output diagnostic")


def store_07(f):
    for command in ["UPDATE", "DELETE"]:
        for mode in ["replace_1177","replace_resource","replace_temp_write","replace_backup_cleanup","replace_unexpected"]:
            x = isolated(f, command + mode)
            _, out, err = fault(x, mode, command, 0, production=True)
            check(not err and len(out) == 2 and out[0]["affected_rows"] == "2", "production ignores hooks")
            check(99 in [r[0] for r in decoded_rows(x)], "production continued")


def cli_01(f):
    for fmt in ["jsonl", "table"]:
        for use_file in [False, True]:
            x = isolated(f, fmt + str(use_file))
            sql = "UPDATE users SET name=name WHERE id=1;DELETE FROM users WHERE id=2;UPDATE users SET age=name+1 WHERE id=1;INSERT INTO users(id) VALUES(99);"
            args = x.args(fmt=fmt)
            raw = sql.encode()
            if use_file:
                path = x.work / 'input.sql'; path.write_bytes(raw)
                args += ['--file',str(path)]; raw = None
            else:
                args += ['--stdin']
            result = subprocess.run(args, input=raw, capture_output=True, timeout=15)
            check(result.returncode == 4, "mixed batch stops")
            check([r[0] for r in decoded_rows(x)] == [1,3,4], "earlier commits retained")
            if fmt == "jsonl":
                out = [json.loads(s) for s in result.stdout.splitlines()]
                err = json.loads(result.stderr)
                check([o['command'] for o in out] == ['UPDATE','DELETE'] and [o['statement_index'] for o in out] == ['1','2'], "ordered DML wire")
                check(err['command'] == 'UPDATE' and err['statement_index'] == '3' and err['committed'] is False, "later error identity")


def regression_01(f):
    seeded(f)
    success(f, "UPDATE users SET name='M3' WHERE id=1;", "UPDATE", 1)
    success(f, "DELETE FROM users WHERE id=1;", "DELETE", 1)
    success(f, "INSERT INTO users(id,name) VALUES(1,'reused');", "INSERT", 1)
    x = Fixture(f.exe, f.work / 'middle-id', f.fault_exe)
    x.definition['columns'] = [{'name':'Name','type':'TEXT'},{'name':'ID','type':'INT64'},{'name':'Age','type':'INT64'}]
    x.schema_file.write_text(json.dumps(x.definition), encoding='utf-8')
    x.init()
    x.sql("INSERT INTO users(id,name,age) VALUES(1,'x',20),(2,'y',30);UPDATE users SET age=age+1 WHERE id=1;DELETE FROM users WHERE id=2;INSERT INTO users(id,name) VALUES(2,'z');")
    check(rows(x,"SELECT * FROM users;") == Counter([('x','1','21'),('z','2',None)]), "nonfirst id preserved")


CASES = {**{f'M3-UPDATE-{i:02}':globals()[f'update_{i:02}'] for i in range(1,9)},
         **{f'M3-DELETE-{i:02}':globals()[f'delete_{i:02}'] for i in range(1,4)},
         **{f'M3-STORE-{i:02}':globals()[f'store_{i:02}'] for i in range(1,8)},
         'M3-CLI-01':cli_01, 'M3-REGRESSION-01':regression_01}

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe',required=True)
    parser.add_argument('--fault-exe',required=True)
    parser.add_argument('--case',choices=CASES,required=True)
    parser.add_argument('--work',required=True)
    a = parser.parse_args()
    CASES[a.case](Fixture(a.exe,Path(a.work),a.fault_exe))
