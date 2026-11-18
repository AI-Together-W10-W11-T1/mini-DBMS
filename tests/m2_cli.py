"""M2 executable contracts, using the same real-process fixtures as M1."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import threading

from milestone_cli import Fixture, check, persisted_ids, seeded


def snapshot(f):
    return (f.data / "table_users.tbl").read_bytes()


def rows(f, sql):
    out, err = f.sql(sql)
    check(not err and len(out) == 1 and out[0]["type"] == "query_result", "query result")
    return Counter(tuple(row) for row in out[0]["rows"])


def failure(f, sql, code):
    before = snapshot(f)
    out, err = f.sql(sql, 4)
    check(not out and len(err) == 1 and err[0]["code"] == code, f"expected {code}: {err}")
    check(snapshot(f) == before, "failed statement leaves file unchanged")


def baseline(f):
    f.init()
    f.sql("INSERT INTO users(id,name,age,team) VALUES"
          "(1,'Kim',20,'A'),(2,'Lee',NULL,'A'),(3,'kim',30,NULL),(4,'',10,'B');")


def decoded_rows(f):
    raw = snapshot(f)
    schema_length = struct.unpack_from("<Q", raw, 12)[0]
    schema = json.loads(raw[20:20 + schema_length])
    offset = 20 + schema_length
    result = []
    while offset < len(raw):
        length = struct.unpack_from("<Q", raw, offset)[0]
        offset += 8
        end = offset + length
        row = []
        for field in schema["columns"]:
            tag = raw[offset]
            offset += 1
            check(tag in (0, 1), "valid stored tag")
            value = None
            if tag and field["type"] == "INT64":
                value = struct.unpack_from("<q", raw, offset)[0]
                offset += 8
            elif tag:
                size = struct.unpack_from("<Q", raw, offset)[0]
                offset += 8
                value = raw[offset:offset + size].decode("utf-8")
                offset += size
            row.append(value)
        check(offset == end and end <= len(raw), "stored record boundary")
        result.append(row)
    return result


def insert_01(f):
    f.init()
    out, err = f.sql("INSERT INTO users(id,name) VALUES(5,'Kim'),(6,'Lee');", file=True)
    check(not err and out[0]["affected_rows"] == "2" and out[0]["committed"] is True, "batch count")
    check(rows(f, "SELECT * FROM users;") == Counter([("5", "Kim", None, None), ("6", "Lee", None, None)]), "restart batch")
    check(decoded_rows(f) == [[5, "Kim", None, None], [6, "Lee", None, None]], "independent stored decode")


def insert_02(f):
    f.init()
    failure(f, "INSERT INTO users(id,name) VALUES(5,'A'),(5,'B');", "DUPLICATE_ID")
    f.sql("INSERT INTO users(id) VALUES(5);")
    failure(f, "INSERT INTO users(id,name) VALUES(6,'A'),(5,'B');", "DUPLICATE_ID")
    check(persisted_ids(f) == [5], "no prefix rows committed")


def insert_03(f):
    f.init()
    for sql, code in [
        ("INSERT INTO users(id,name) VALUES(1,'A'),(2);", "COLUMN_COUNT_MISMATCH"),
        ("INSERT INTO users(id,name) VALUES(1,'A'),(2,'B',3);", "COLUMN_COUNT_MISMATCH"),
        ("INSERT INTO users(id,age) VALUES(1,1),(2,'20');", "TYPE_MISMATCH"),
        ("INSERT INTO users(id,age) VALUES(1,1),(2,9223372036854775808);", "INTEGER_RANGE"),
        ("INSERT INTO users(id,age) VALUES(1,1),(2,-9223372036854775809);", "INTEGER_RANGE"),
        ("INSERT INTO users(id) VALUES(1),(0);", "INVALID_ID"),
        ("INSERT INTO users(id) VALUES(1),(NULL);", "INVALID_ID"),
        ("INSERT INTO users(id) VALUES(1),('2');", "TYPE_MISMATCH"),
        ("INSERT INTO users(id,name,name) VALUES(1,'A','B');", "DUPLICATE_COLUMN"),
    ]:
        failure(f, sql, code)
    out, _ = f.sql("INSERT INTO users(id,age) VALUES(1,-9223372036854775808),(2,9223372036854775807);")
    check(out[0]["affected_rows"] == "2", "extreme batch count")
    check(rows(f, "SELECT age FROM users;") == Counter([("-9223372036854775808",), ("9223372036854775807",)]), "INT64 extremes")
    automatic = Fixture(f.exe, f.work / "automatic-id", f.fault_exe)
    automatic.init()
    automatic.sql("INSERT INTO users(name) VALUES('A'),('B');")
    check(persisted_ids(automatic) == [1, 2], "Phase2 omitted IDs generated in batch")


def batch_fault(f, mode, expected):
    env = os.environ.copy()
    env["MINIDB_TEST_FAULT"] = mode
    args = f.args() + ["--stdin"]
    args[0] = f.fault_exe
    result, out, err = f.invoke(args, b"INSERT INTO users(id,name) VALUES(2,'two'),(3,'three');INSERT INTO users(id) VALUES(4);", env)
    check(result.returncode == expected, f"batch fault {mode} exit {result.returncode}: {err}")
    return out, err


def insert_04(f):
    before = seeded(f)
    out, err = batch_fault(f, "append_write", 4)
    check(not out and err[0]["committed"] is False and err[0]["rollback"] == "confirmed", "batch rollback diagnostic")
    check(snapshot(f) == before and not (f.data / ".minidb.pending").exists(), "exact batch rollback")
    check(rows(f, "SELECT id FROM users;") == Counter([("1",)]), "restart confirmed rollback")
    broken = Fixture(f.exe, f.work / "broken", f.fault_exe)
    before = seeded(broken)
    out, err = batch_fault(broken, "rollback", 6)
    check(not out and err[0]["rollback"] == "failed", "batch unresolved rollback")
    raw = snapshot(broken)
    check(raw.startswith(before) and len(raw) > len(before), "unresolved tail preserved")
    check((broken.data / ".minidb.pending").exists(), "unresolved marker")
    out, err = broken.sql("SELECT id FROM users;", 3)
    check(not out and err[0]["code"] == "RECOVERY_REQUIRED" and snapshot(broken) == raw, "restart unresolved gate")


def insert_05(f):
    for mode, exit_code in [("cleanup", 5), ("stdout", 7), ("stdout_cleanup", 5)]:
        test = Fixture(f.exe, f.work / mode, f.fault_exe)
        seeded(test)
        out, err = batch_fault(test, mode, exit_code)
        check(persisted_ids(test) == [1, 2, 3], "full batch committed; following SQL absent")
        check(any(d["committed"] is True and d["affected_rows"] == "2" for d in err), "full committed diagnostic count")
        if mode == "cleanup":
            check(out[0]["affected_rows"] == "2" and out[0]["committed"] is True, "cleanup batch success")
        if mode == "stdout_cleanup":
            check({d["phase"] for d in err} >= {"cleanup", "output"}, "combined diagnostics")
    f.init()
    env = os.environ.copy()
    env["MINIDB_TEST_FAULT"] = "rollback"
    result, out, err = f.invoke(f.args() + ["--stdin"], b"INSERT INTO users(id) VALUES(8),(9);", env)
    check(result.returncode == 0 and not err and out[0]["affected_rows"] == "2", "production batch ignores fault")


def where_01(f):
    baseline(f)
    expected = {"=": [1], "<>": [3, 4], "<": [4], "<=": [1, 4], ">": [3], ">=": [1, 3]}
    for op, ids in expected.items():
        check(rows(f, f"SELECT id FROM users WHERE age{op}20;") == Counter((str(i),) for i in ids), f"comparison {op}")
    for predicate, ids in [("1=id", [1]), ("age=age", [1, 3, 4]), ("1<2", [1, 2, 3, 4]),
                           ("NULL=NULL", []), ("age=NULL", []), ("name='Kim'", [1]),
                           ("name='kim'", [3]), ("name=''", [4]), ("name=' '", []),
                           ("name<'Lee'", [1, 4]), ("'Kim'=name", [1])]:
        check(rows(f, "SELECT id FROM users WHERE " + predicate + ";") == Counter((str(i),) for i in ids), predicate)
    f.sql("INSERT INTO users(id,name) VALUES(5,'한'),(6,' ');")
    check(rows(f, "SELECT id FROM users WHERE name>'kim';") == Counter([("5",)]), "UTF8 byte order")
    check(rows(f, "SELECT id FROM users WHERE name=' ';") == Counter([("6",)]), "space distinct from empty")


def where_02(f):
    baseline(f)
    expressions = ["1=1", "1=0", "NULL=1"]
    # T/F/U 전 조합을 실제 SQL로 평가한다.
    and_result = [[True, False, None], [False, False, False], [None, False, None]]
    or_result = [[True, True, True], [True, False, None], [True, None, None]]
    for op, truth in [("AND", and_result), ("OR", or_result)]:
        for a in range(3):
            for b in range(3):
                got = rows(f, f"SELECT id FROM users WHERE ({expressions[a]}) {op} ({expressions[b]});")
                expected = Counter((str(i),) for i in [1, 2, 3, 4]) if truth[a][b] is True else Counter()
                check(got == expected, f"3-valued {a} {op} {b}")
    for predicate, ids in [("age IS NULL", [2]), ("age IS NOT NULL", [1, 3, 4]),
                           ("NULL IS NULL", [1, 2, 3, 4]), ("NULL IS NOT NULL", []),
                           ("1=1 OR 1=0 AND 1=0", [1, 2, 3, 4]),
                           ("(1=1 OR 1=0) AND 1=0", []),
                           ("(age>=20 OR age IS NULL) AND id<4", [1, 2, 3])]:
        check(rows(f, "SELECT id FROM users WHERE " + predicate + ";") == Counter((str(i),) for i in ids), predicate)


def where_03(f):
    f.init()
    for populated in [False, True]:
        if populated:
            f.sql("INSERT INTO users(id) VALUES(1);")
        for sql, code in [("SELECT id FROM users WHERE 1=0 AND age='20';", "TYPE_MISMATCH"),
                          ("SELECT id FROM users WHERE 1=1 OR missing=1;", "UNKNOWN_COLUMN"),
                          ("SELECT id FROM users WHERE '1'=1;", "TYPE_MISMATCH"),
                          ("SELECT missing FROM users;", "UNKNOWN_COLUMN"),
                          ("SELECT id FROM users WHERE age='20';", "TYPE_MISMATCH")]:
            failure(f, sql, code)
    failure(f, "SELECT id FROM missing_table;", "UNKNOWN_TABLE")
    for sql in ["SELECT id FROM users WHERE id=1 IS NULL;",
                "SELECT id FROM users WHERE id=1 WHERE id=2;",
                "SELECT id FROM users WHERE (id=1;",
                "SELECT id FROM users WHERE id=1 AND;"]:
        failure(f, sql, "SYNTAX_ERROR")
    for sql in ["SELECT id FROM users WHERE NOT id=1;",
                "SELECT id FROM users WHERE COUNT(*)>0;",
                "SELECT SUM(age) FROM users;"]:
        failure(f, sql, "UNSUPPORTED_FEATURE")
    out, err = f.sql("SELECT id FROM users ORDER BY id;")
    check(not err and out[0]["rows"] == [["1"]], "M4 enables previously deferred ORDER BY")


def name_01(f):
    baseline(f)
    out, _ = f.sql("SELECT u.id AS user_id,u.name AS user_name FROM users AS u WHERE u.id=1;")
    check(out[0]["columns"] == [{"name": "user_id", "type": "INT64"}, {"name": "user_name", "type": "TEXT"}], "output AS names")
    check(out[0]["rows"] == [["1", "Kim"]], "qualified alias filter")
    check(rows(f, "SELECT USERS.id FROM users WHERE users.name='Kim';") == Counter([("1",)]), "table qualification")
    declared = Fixture(f.exe, f.work / "declared", f.fault_exe)
    declared.definition["name"] = "Users"
    declared.definition["columns"] = [{"name": "Name", "type": "TEXT"}, {"name": "ID", "type": "INT64"}]
    declared.schema_file.write_text(json.dumps(declared.definition), encoding="utf-8")
    result, out, err = declared.invoke(declared.args("init"))
    check(result.returncode == 0 and not err and out[0]["tables"] ==
          [{"name": "Users", "action": "created", "row_count": "0"}], "declared schema init")
    declared.sql("INSERT INTO users(id,name) VALUES(1,'A'),(2,'B');")
    out, _ = declared.sql("SELECT U.name,U.id AS KeyId FROM USERS AS U WHERE u.id=1;")
    check(out[0]["columns"] == [{"name": "Name", "type": "TEXT"}, {"name": "KeyId", "type": "INT64"}], "declared defaults and chosen AS spelling")
    out, _ = declared.sql("SELECT * FROM users AS u WHERE u.id=2;")
    check(out[0]["columns"][0]["name"] == "Name" and out[0]["rows"] == [["B", "2"]], "star with alias")


def name_02(f):
    baseline(f)
    for sql, code in [("SELECT users.id FROM users AS u;", "UNKNOWN_TABLE"),
                      ("SELECT u.id FROM users AS u WHERE users.id=1;", "UNKNOWN_TABLE"),
                      ("SELECT id AS x FROM users WHERE x=1;", "UNKNOWN_COLUMN"),
                      ("SELECT id AS x,name AS X FROM users;", "DUPLICATE_OUTPUT_NAME"),
                      ("SELECT id,id FROM users;", "DUPLICATE_OUTPUT_NAME"),
                      ("SELECT u.missing FROM users AS u;", "UNKNOWN_COLUMN")]:
        failure(f, sql, code)
    check(rows(f, "SELECT id AS a,id AS b FROM users WHERE id=1;") == Counter([("1", "1")]), "same input distinct outputs")
    check(rows(f, "SELECT name AS id FROM users WHERE id=1;") == Counter([("Kim",)]), "WHERE keeps input id meaning")


def values_01(f):
    f.init()
    before = snapshot(f)
    out, err = f.sql("VALUES(1,'Kim'),(2,'Lee');", file=True)
    check(not err and out[0]["columns"] == [{"name": "column1", "type": "INT64"}, {"name": "column2", "type": "TEXT"}], "VALUES metadata")
    check(Counter(tuple(r) for r in out[0]["rows"]) == Counter([("1", "Kim"), ("2", "Lee")]), "VALUES rows")
    check(snapshot(f) == before and persisted_ids(f) == [], "VALUES no storage/ID mutation")
    f.sql("INSERT INTO users(id) VALUES(1);")


def values_02(f):
    f.init()
    for sql, types, expected in [("VALUES(NULL),(1);", ["INT64"], [[None], ["1"]]),
                                  ("VALUES(NULL),(NULL);", ["NULL"], [[None], [None]]),
                                  ("VALUES(NULL,NULL),(NULL,'한');", ["NULL", "TEXT"], [[None, None], [None, "한"]])]:
        out, _ = f.sql(sql)
        check([c["type"] for c in out[0]["columns"]] == types and out[0]["rows"] == expected, "NULL positional inference")
    result = subprocess.run(f.args(fmt="table") + ["--stdin"], input=b"VALUES(NULL),(NULL);", capture_output=True, timeout=15)
    check(result.returncode == 0 and result.stdout == b'statement 1 query_result\n"column1":NULL\nNULL\nNULL\nrow_count 2\n', "NULL-only table output")


def values_03(f):
    f.init()
    for sql, code in [("VALUES(1,'Kim'),(2);", "COLUMN_COUNT_MISMATCH"),
                      ("VALUES(1,'Kim'),(2,3);", "TYPE_MISMATCH"),
                      ("VALUES(1),(9223372036854775808);", "INTEGER_RANGE"),
                      ("VALUES(1),();", "SYNTAX_ERROR"),
                      ("VALUES(1),(2)", "SYNTAX_ERROR")]:
        failure(f, sql, code)


def regression_01(f):
    before = seeded(f)
    check(rows(f, "SELECT id,name FROM users WHERE id=1;") == Counter([("1", "original")]), "M1-compatible restart")
    out, err = f.sql("INSERT INTO users(id) VALUES(2),(3);VALUES(1,'a'),(2,'b');SELECT id FROM users WHERE id>=2;", file=True)
    check(not err and len(out) == 3 and out[0]["affected_rows"] == "2" and out[2]["row_count"] == "2", "mixed file statements")
    check(snapshot(f).startswith(before), "batch preserves existing record bytes")
    out, err = f.sql("INSERT INTO users(id) VALUES(4),(5);VALUES(1),('x');INSERT INTO users(id) VALUES(6);", 4)
    check(len(out) == 1 and err[0]["statement_index"] == "2" and persisted_ids(f) == [1, 2, 3, 4, 5], "prior batch retained after later error")
    proc = subprocess.Popen(f.args() + ["--stdin"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    lines = queue.Queue()
    threading.Thread(target=lambda: lines.put(proc.stdout.readline()), daemon=True).start()
    try:
        proc.stdin.write(b"VALUES(NULL),(1);")
        proc.stdin.flush()
        check(json.loads(lines.get(timeout=8))["row_count"] == "2", "VALUES output before stdin EOF")
        proc.stdin.close()
        check(proc.wait(timeout=8) == 0, "incremental VALUES exit")
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait(timeout=8)


CASES = {"M2-INSERT-01": insert_01, "M2-INSERT-02": insert_02, "M2-INSERT-03": insert_03,
         "M2-INSERT-04": insert_04, "M2-INSERT-05": insert_05,
         "M2-WHERE-01": where_01, "M2-WHERE-02": where_02, "M2-WHERE-03": where_03,
         "M2-NAME-01": name_01, "M2-NAME-02": name_02,
         "M2-VALUES-01": values_01, "M2-VALUES-02": values_02, "M2-VALUES-03": values_03,
         "M2-REGRESSION-01": regression_01}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--fault-exe", required=True)
    parser.add_argument("--case", choices=CASES, required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()
    CASES[args.case](Fixture(args.exe, Path(args.work), args.fault_exe))
