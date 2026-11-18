"""M10 integration assertions through the actual CLI and native index tests."""
import argparse
import json
import os
from pathlib import Path
import subprocess

from milestone_cli import Fixture, check, persisted_ids
from m2_cli import rows, failure, snapshot, decoded_rows


def native(f, kind):
    result = subprocess.run([f.unit_exe, str(f.schema), str(f.data), kind], capture_output=True, timeout=30)
    check(result.returncode == 0, f'native {kind}: {result.stdout!r} {result.stderr!r}')


def auto_01(f):
    f.init()
    f.sql("INSERT INTO users(name) VALUES('a'),('b');INSERT INTO users(id,name) VALUES(10,'x');INSERT INTO users(name) VALUES('c');")
    check(persisted_ids(f) == [1, 2, 10, 11], "automatic IDs follow existing maximum and VALUES order")
    middle = Fixture(f.exe, f.work / 'middle', f.fault_exe)
    middle.definition['columns'] = [{'name': 'Name', 'type': 'TEXT'}, {'name': 'ID', 'type': 'INT64'}]
    middle.schema_file.write_text(json.dumps(middle.definition), encoding='utf-8')
    middle.init()
    middle.sql("INSERT INTO users(name) VALUES('a'),('b');")
    check(decoded_rows(middle) == [['a', 1], ['b', 2]], "middle ID schema")


def auto_02(f):
    f.init()
    f.sql("INSERT INTO users(id) VALUES(4),(9);DELETE FROM users WHERE id=9;INSERT INTO users(name) VALUES('reuse');")
    check(persisted_ids(f) == [4, 5], "deleted maximum reuse")
    f.sql("INSERT INTO users(name) VALUES('restart');")
    check(persisted_ids(f) == [4, 5, 6], "restart derives maximum")
    other = Fixture(f.exe, f.work / 'independent', f.fault_exe)
    definition = dict(other.definition, name='orders')
    (other.schema / 'orders.json').write_text(json.dumps(definition), encoding='utf-8')
    result, _, err = other.invoke(other.args('init'))
    check(result.returncode == 0 and not err, 'two table init')
    out, _ = other.sql("INSERT INTO users(id) VALUES(40);INSERT INTO orders(name) VALUES('first');INSERT INTO users(name) VALUES('next');SELECT id FROM orders;SELECT id FROM users WHERE id=41;")
    check(out[-2]['rows'] == [['1']] and out[-1]['rows'] == [['41']], 'table independent maxima')


def auto_03(f):
    f.init()
    f.sql("INSERT INTO users(id) VALUES(9223372036854775806);")
    failure(f, "INSERT INTO users(name) VALUES('a'),('b');", 'INTEGER_RANGE')
    failure(f, "INSERT INTO users(age) VALUES(1),('bad');", 'INTEGER_RANGE')
    f.sql("INSERT INTO users(name) VALUES('last');")
    failure(f, "INSERT INTO users(name) VALUES('overflow');", 'INTEGER_RANGE')


def insert_01(f):
    f.init()
    f.sql("INSERT INTO users(id) VALUES(7);")
    for sql, code in [("INSERT INTO users(id) VALUES(8),(7);", 'DUPLICATE_ID'),
                      ("INSERT INTO users(id) VALUES(8),(8);", 'DUPLICATE_ID'),
                      ("INSERT INTO users(id,age) VALUES(8,1),(9,'x');", 'TYPE_MISMATCH')]:
        failure(f, sql, code)
    f.sql("INSERT INTO users(name) VALUES('ok');")
    check(persisted_ids(f) == [7, 8], "failed statement has no allocation effect")


def lookup(f, kind):
    f.init()
    f.sql("INSERT INTO users(id,name,age) VALUES(1,'a',10),(3,'b',NULL),(9,'c',30);")
    if kind == 1:
        for predicate in ['id=3', '3=id', 'u.id=3', '3=u.id']:
            check(rows(f, f'SELECT u.id FROM users AS u WHERE {predicate};') == {('3',): 1}, predicate)
        check(not rows(f, 'SELECT id FROM users WHERE id=7;'), 'missing candidate')
        env = dict(os.environ)
        args = f.args() + ['--stdin']
        args[0] = f.fault_exe
        result, out, err = f.invoke(args, b'SELECT id FROM users WHERE id=3;', env)
        check(result.returncode == 0 and not err and out[0]['rows'] == [['3']], 'index lookup instrumentation')
        # Test hook turns an actual index access into an observable diagnostic.
        env['MINIDB_TEST_FAULT'] = 'index_lookup_fail'
        result, out, err = f.invoke(args, b'SELECT id FROM users WHERE id=3;', env)
        check(result.returncode == 4 and err[0]['code'] == 'RESOURCE_ERROR', 'actual indexed access observed')
        native(f, 'lookup')
    elif kind == 2:
        for predicate, expected in [('id=3 AND age IS NULL', [('3',)]), ('id=3 AND age=1', []),
                                    ('id=3 AND id=9', []), ('age IS NULL AND 3=id', [('3',)])]:
            check(rows(f, f'SELECT id FROM users WHERE {predicate};') == {r: 1 for r in expected}, predicate)
    else:
        check(rows(f, 'SELECT id FROM users WHERE id=1 OR id=9;') == {('1',): 1, ('9',): 1}, 'OR scan')
        failure(f, 'SELECT missing FROM users WHERE id=999;', 'UNKNOWN_COLUMN')
        failure(f, "SELECT id FROM users WHERE id=999 AND age='x';", 'TYPE_MISMATCH')
        env = dict(os.environ, MINIDB_TEST_FAULT='index_lookup_fail')
        args = f.args() + ['--stdin']; args[0] = f.fault_exe
        result, out, err = f.invoke(args, b'SELECT id FROM users WHERE id=1 OR id=9;', env)
        check(result.returncode == 0 and not err and len(out[0]['rows']) == 2, 'OR fallback avoids index')
        check(not rows(f, 'SELECT id FROM users WHERE id=3 AND age=NULL;'), 'candidate preserves UNKNOWN')
        f.sql('DELETE FROM users WHERE 1=1;')
        failure(f, "SELECT id FROM users WHERE id=999 AND age='x';", 'TYPE_MISMATCH')


def update_01(f):
    f.init()
    out, _ = f.sql("INSERT INTO users(id,name) VALUES(9,'a'),(2,'b');UPDATE users SET name='new' WHERE id=2;SELECT name FROM users WHERE id=2;")
    check(out[-1]['rows'] == [['new']], 'updated indexed row')
    native(f, 'update')


def delete_01(f):
    f.init()
    out, _ = f.sql("INSERT INTO users(id) VALUES(9),(2),(7),(4);DELETE FROM users WHERE id=9 OR id=7;SELECT id FROM users WHERE id=4;DELETE FROM users WHERE id=2;SELECT id FROM users WHERE id=4;DELETE FROM users WHERE 1=1;INSERT INTO users(name) VALUES('reset');SELECT id FROM users WHERE id=1;")
    check(out[2]['rows'] == [['4']] and out[4]['rows'] == [['4']] and out[-1]['rows'] == [['1']], 'delete remaps and clears index')


def delete_02(f):
    f.init()
    f.sql('INSERT INTO users(id) VALUES(1),(2),(3);')
    before = snapshot(f)
    for fault in ['index_delete_alloc', 'replace_temp_write', 'replace_other']:
        env = dict(os.environ, MINIDB_TEST_FAULT=fault)
        args = f.args() + ['--stdin']; args[0] = f.fault_exe
        result, out, err = f.invoke(args, b'DELETE FROM users WHERE id=2;', env)
        check(result.returncode == 4 and not out and err, f'{fault} rejected')
        check(snapshot(f) == before, 'delete failure preserves bytes')
        check(rows(f, 'SELECT id FROM users WHERE id=2;') == {('2',): 1}, 'delete failure preserves index')
    native(f, 'delete')


def storage_01(f):
    f.init()
    f.sql('INSERT INTO users(id) VALUES(1);')
    before = snapshot(f)
    env = dict(os.environ, MINIDB_TEST_FAULT='append_write')
    args = f.args() + ['--stdin']; args[0] = f.fault_exe
    result, _, err = f.invoke(args, b"INSERT INTO users(name) VALUES('a'),('b');", env)
    check(result.returncode == 4 and err[0]['rollback'] == 'confirmed' and snapshot(f) == before, 'append rollback')
    f.sql("INSERT INTO users(name) VALUES('retry');")
    check(persisted_ids(f) == [1, 2], 'retry auto ID')
    native(f, 'storage')


def startup_01(f):
    f.init()
    f.sql('INSERT INTO users(id) VALUES(1),(3),(8);')
    check(rows(f, 'SELECT id FROM users WHERE id=8;') == {('8',): 1}, 'startup index')
    native(f, 'startup')
    raw = snapshot(f)
    schema_length = int.from_bytes(raw[12:20], 'little')
    first = 20 + schema_length
    length = int.from_bytes(raw[first:first + 8], 'little') + 8
    (f.data / 'table_users.tbl').write_bytes(raw + raw[first:first + length])
    _, err = f.sql('SELECT id FROM users;', 3)
    check(err[0]['code'] == 'STORAGE_FORMAT_ERROR', 'late duplicate remains corruption')


def migrate_01(f):
    from m8_cli import prepare, migrate, rows as migrated_rows
    prepare(f, [('Name', 'TEXT'), ('ID', 'INT64'), ('age', 'INT64'), ('team', 'TEXT')],
            "INSERT INTO users(id,name) VALUES(3,'a'),(8,'b');")
    migrate(f)
    check([r[1] for r in migrated_rows(f)] == ['3', '8'], 'migration IDs preserved')
    args = [f.exe, 'run', '--schema-dir', str(f.newschema), '--data-dir', str(f.target), '--format', 'jsonl', '--stdin']
    result, out, err = f.invoke(args, b"INSERT INTO users(name) VALUES('new');SELECT name FROM users WHERE id=9;")
    check(result.returncode == 0 and not err and out[-1]['rows'] == [['new']], 'migration rebuilt lookup and auto ID')


CASES = {'M10-STARTUP-01': startup_01, 'M10-AUTO-01': auto_01, 'M10-AUTO-02': auto_02,
         'M10-AUTO-03': auto_03, 'M10-INSERT-01': insert_01,
         **{f'M10-LOOKUP-{i:02}': lambda f, i=i: lookup(f, i) for i in range(1, 4)},
         'M10-UPDATE-01': update_01, 'M10-DELETE-01': delete_01, 'M10-DELETE-02': delete_02,
         'M10-STORAGE-01': storage_01, 'M10-RESTART-01': auto_02, 'M10-MIGRATE-01': migrate_01}

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for option in ['exe', 'fault-exe', 'unit-exe', 'case', 'work']:
        parser.add_argument('--' + option, required=True)
    a = parser.parse_args()
    fixture = Fixture(a.exe, Path(a.work), a.fault_exe)
    fixture.unit_exe = a.unit_exe
    CASES[a.case](fixture)
