"""M4 sorting and pagination contracts against real CLI processes."""
import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess

from milestone_cli import Fixture, check
from m2_cli import failure, snapshot, decoded_rows


def baseline(f):
    f.init()
    f.sql("INSERT INTO users(id,name,age,team) VALUES"
          "(3,'kim',30,NULL),(1,'Kim',20,'A'),(4,'',-1,'B'),(2,'Lee',NULL,'A');")


def query(f, sql):
    before = snapshot(f)
    out, err = f.sql(sql)
    check(not err and len(out) == 1 and out[0]['type'] == 'query_result', 'one query result')
    check(out[0]['row_count'] == str(len(out[0]['rows'])), 'returned row count')
    check(snapshot(f) == before, 'query preserves stored bytes')
    return out[0]


def ids(result):
    return [int(row[0]) for row in result['rows']]


def order_01(f):
    baseline(f)
    for keys, expected in [('id', [1,2,3,4]), ('id DESC', [4,3,2,1]),
                           ('team,id', [1,2,4,3]), ('team ASC,id DESC', [2,1,4,3]),
                           ('team DESC,id ASC', [4,1,2,3])]:
        check(ids(query(f, 'SELECT id,team FROM users ORDER BY '+keys+';')) == expected, keys)
    check(ids(query(f, 'SELECT id FROM users WHERE (age>=20 OR age IS NULL) AND id<4 ORDER BY id;'))
          == [1,2,3], 'P1-SQL-028 sorted expanded predicate')
    tied = query(f, 'SELECT team FROM users ORDER BY team;')['rows']
    check(tied == [['A'],['A'],['B'],[None]], 'ties impose no extra ordering contract')


def order_02(f):
    baseline(f)
    for direction, nonnull in [('ASC',[1,2,4]), ('DESC',[4,1,2])]:
        for suffix, first in [('',False), (' NULLS LAST',False), (' NULLS FIRST',True)]:
            expected = [3]+nonnull if first else nonnull+[3]
            got = query(f, f'SELECT id,team FROM users ORDER BY team {direction}{suffix},id ASC;')
            check(ids(got) == expected, direction+suffix)
    got = query(f, 'VALUES(NULL,NULL,3),(1,NULL,2),(1,2,1),(NULL,2,4) '
                'ORDER BY column1 DESC NULLS FIRST,column2 ASC NULLS LAST,column3;')
    check(got['rows'] == [[None,'2','4'],[None,None,'3'],['1','2','1'],['1',None,'2']], 'per-key NULL placement')


def order_03(f):
    f.init()
    texts = ['한','a','A','', ' ', "O'Brien", '\r\n', 'aa', '가']
    literal = ','.join("('%s',%d)" % (s.replace("'","''"), i) for i,s in enumerate(texts))
    for direction, reverse in [('ASC',False),('DESC',True)]:
        got = query(f, f'VALUES{literal} ORDER BY column1 {direction},column2;')
        check([r[0] for r in got['rows']] == sorted(texts,key=lambda s:s.encode('utf-8'),reverse=reverse),
              'UTF8 numeric-independent byte order '+direction)
        got = query(f, 'VALUES(-9223372036854775808),(9223372036854775807),(0),(-1),(1) '
                    f'ORDER BY column1 {direction};')
        expected = sorted([-(2**63),2**63-1,0,-1,1],reverse=reverse)
        check([int(r[0]) for r in got['rows']] == expected, 'safe INT64 comparison '+direction)


def order_04(f):
    baseline(f)
    got = query(f, 'SELECT name AS id FROM users ORDER BY ID;')
    check(got['rows'] == [[''],['Kim'],['Lee'],['kim']], 'AS replaces input name')
    for sql in ['SELECT name AS id FROM users ORDER BY name;',
                'SELECT name FROM users ORDER BY id;',
                'SELECT id FROM users ORDER BY users.id;',
                'SELECT u.id AS key_id FROM users AS u ORDER BY u.id;',
                'SELECT id FROM users ORDER BY 1;']:
        failure(f,sql,'INVALID_ORDER_BY')
    failure(f,'SELECT id,id FROM users ORDER BY id;','DUPLICATE_OUTPUT_NAME')
    declared = Fixture(f.exe, f.work/'declared', f.fault_exe)
    declared.definition['name'] = 'Users'
    declared.definition['columns'] = [{'name':'Name','type':'TEXT'},{'name':'ID','type':'INT64'}]
    declared.schema_file.write_text(json.dumps(declared.definition),encoding='utf-8')
    result,_,err = declared.invoke(declared.args('init'))
    check(result.returncode == 0 and not err,'declared schema init')
    declared.sql("INSERT INTO users(id,name) VALUES(2,'B'),(1,'A');")
    got = query(declared,'SELECT name,id AS KeyId FROM users ORDER BY NAME,keyid DESC;')
    check(got['columns'] == [{'name':'Name','type':'TEXT'},{'name':'KeyId','type':'INT64'}]
          and got['rows'] == [['A','1'],['B','2']], 'casefold binding, original output spelling')


def page_01(f):
    baseline(f)
    for tail, expected in [('ORDER BY team,id',[1,2,4,3]),
                           ('ORDER BY team,id OFFSET 1 ROWS',[2,4,3]),
                           ('ORDER BY team,id FETCH FIRST 2 ROWS ONLY',[1,2]),
                           ('ORDER BY team,id OFFSET 1 ROWS FETCH FIRST 2 ROWS ONLY',[2,4])]:
        check(ids(query(f,'SELECT id,team FROM users '+tail+';')) == expected,tail)


def page_02(f):
    baseline(f)
    for tail, expected in [('OFFSET 0 ROWS',[1,2,3,4]),('FETCH FIRST 0 ROWS ONLY',[]),
                           ('OFFSET 4 ROWS',[]),('OFFSET 5 ROWS',[]),
                           ('OFFSET 9223372036854775807 ROWS',[]),
                           ('FETCH FIRST 9223372036854775807 ROWS ONLY',[1,2,3,4]),
                           ('OFFSET 1 ROWS FETCH FIRST 9223372036854775807 ROWS ONLY',[2,3,4]),
                           ('OFFSET 9223372036854775807 ROWS FETCH FIRST 9223372036854775807 ROWS ONLY',[])]:
        got = query(f,'SELECT id FROM users ORDER BY id '+tail+';')
        check(ids(got) == expected and got['columns'] == [{'name':'id','type':'INT64'}], tail)
    empty = Fixture(f.exe,f.work/'empty',f.fault_exe)
    empty.init()
    for tail in ['ORDER BY id', 'ORDER BY id OFFSET 0 ROWS FETCH FIRST 0 ROWS ONLY',
                 'FETCH FIRST 9223372036854775807 ROWS ONLY']:
        got = query(empty,'SELECT id FROM users '+tail+';')
        check(got['rows'] == [] and got['columns'][0]['type'] == 'INT64','typed empty output')


def page_03(f):
    baseline(f)
    source = Counter((str(i),) for i in range(1,5))
    for n, count in [(0,0),(1,1),(2,2),(5,4),(9223372036854775807,4)]:
        got = query(f,f'SELECT id FROM users FETCH FIRST {n} ROWS ONLY;')
        actual = Counter(map(tuple,got['rows']))
        check(len(got['rows']) == count and all(actual[row] <= source[row] for row in actual),
              'unordered FETCH count and subset, no identity promise')
    failure(f,'SELECT id FROM users OFFSET 0 ROWS;','INVALID_PAGINATION')
    failure(f,'SELECT id FROM users OFFSET 1 ROWS FETCH FIRST 1 ROWS ONLY;','INVALID_PAGINATION')


def error_01(f):
    baseline(f)
    for count in ['-1','9223372036854775808','-9223372036854775809']:
        failure(f,f'SELECT id FROM users ORDER BY id OFFSET {count} ROWS;','INVALID_PAGINATION')
        failure(f,f'SELECT id FROM users FETCH FIRST {count} ROWS ONLY;','INVALID_PAGINATION')
    for tail in ['ORDER id','ORDER BY','ORDER BY id,','ORDER BY id NULLS','ORDER BY id NULLS MIDDLE',
                 'OFFSET 0','FETCH 1 ROWS ONLY','FETCH FIRST 1 ROWS','FETCH FIRST 1 ONLY',
                 'FETCH FIRST ROWS ONLY','FETCH FIRST 1+1 ROWS ONLY',
                 'ORDER BY id FETCH FIRST 1 ROWS ONLY OFFSET 0 ROWS',
                 'OFFSET 0 ROWS ORDER BY id','ORDER BY id ORDER BY id',
                 'ORDER BY id OFFSET 0 ROWS OFFSET 1 ROWS',
                 'FETCH FIRST 1 ROWS ONLY FETCH FIRST 1 ROWS ONLY',
                 'ORDER BY id ASC DESC','ORDER BY id NULLS FIRST ASC']:
        failure(f,'SELECT id FROM users '+tail+';','SYNTAX_ERROR')
    failure(f,'VALUES(1) ORDER BY column1 UNION VALUES(2);','SYNTAX_ERROR')
    failure(f,'VALUES(1) FETCH FIRST 1 ROWS ONLY UNION VALUES(2);','SYNTAX_ERROR')
    # 조회 끝절이 변경문 finish_query에 새로 허용되어서는 안 된다.
    for sql in ['UPDATE users SET age=1 WHERE id=1 ORDER BY id;',
                'DELETE FROM users WHERE id=1 FETCH FIRST 1 ROWS ONLY;']:
        failure(f,sql,'UNSUPPORTED_FEATURE')


def error_02(f):
    f.init()
    for suffix, page in [('', ''), (' WHERE id<0', ''),
                         (' WHERE id<0', ' FETCH FIRST 0 ROWS ONLY')]:
        for sql, code in [('SELECT id AS x FROM users'+suffix+' ORDER BY id'+page+';','INVALID_ORDER_BY'),
                          ('SELECT id FROM users'+suffix+' ORDER BY missing'+page+';','INVALID_ORDER_BY'),
                          ('SELECT id FROM users'+suffix+' FETCH FIRST -1 ROWS ONLY;','INVALID_PAGINATION')]:
            failure(f,sql,code)
    failure(f,"SELECT id FROM users WHERE age='20' ORDER BY id FETCH FIRST 0 ROWS ONLY;",'TYPE_MISMATCH')
    f.sql('INSERT INTO users(id) VALUES(1);')
    failure(f,'SELECT id FROM users WHERE id<0 ORDER BY missing FETCH FIRST 0 ROWS ONLY;','INVALID_ORDER_BY')


def values_01(f):
    f.init()
    got = query(f,"VALUES(NULL,'z'),(2,NULL),(1,'a'),(2,'b') ORDER BY column1 DESC NULLS FIRST,column2 "
                'OFFSET 1 ROWS FETCH FIRST 2 ROWS ONLY;')
    check(got['rows'] == [['2','b'],['2',None]] and got['columns'] ==
          [{'name':'column1','type':'INT64'},{'name':'column2','type':'TEXT'}], 'VALUES common tail')
    got = query(f,'VALUES(NULL,2),(NULL,1) ORDER BY column1 DESC NULLS FIRST,column2;')
    check(got['rows'] == [[None,'1'],[None,'2']] and got['columns'][0]['type'] == 'NULL','all NULL ordering')
    got = query(f,"VALUES(NULL),('a') ORDER BY column1 FETCH FIRST 0 ROWS ONLY;")
    check(got['rows'] == [] and got['columns'] == [{'name':'column1','type':'TEXT'}], 'empty page inferred metadata')
    check(decoded_rows(f) == [],'VALUES never persists')


def values_02(f):
    f.init()
    for limit in [0,1]:
        for sql,code in [("VALUES(1,'A'),(2,3)",'TYPE_MISMATCH'),
                         ("VALUES(1,'A'),(2)",'COLUMN_COUNT_MISMATCH'),
                         ('VALUES(1),(9223372036854775808)','INTEGER_RANGE')]:
            failure(f,sql+f' FETCH FIRST {limit} ROWS ONLY;',code)
    failure(f,'VALUES(1),(2) ORDER BY missing FETCH FIRST 0 ROWS ONLY;','INVALID_ORDER_BY')


def cli_01(f):
    for file in [False,True]:
        for fmt in ['jsonl','table']:
            x = Fixture(f.exe,f.work/f'{file}-{fmt}',f.fault_exe)
            baseline(x)
            before = snapshot(x)
            valid = 'SELECT id,team FROM users ORDER BY team,id OFFSET 1 ROWS FETCH FIRST 2 ROWS ONLY;'
            args = x.args(fmt=fmt)
            raw = valid.encode()
            if file:
                p=x.work/'input.sql';p.write_bytes(raw);args += ['--file',str(p)];raw=None
            else:
                args += ['--stdin']
            result,out,err = x.invoke(args,raw,parse_stdout=fmt=='jsonl')
            check(result.returncode == 0 and not err and snapshot(x)==before,'CLI mode success preserves bytes')
            if fmt == 'jsonl':
                check(out[0]['rows']==[['2','A'],['4','B']] and out[0]['row_count']=='2','JSONL ordered page')
            else:
                check(result.stdout == b'statement 1 query_result\n"id":INT64|"team":TEXT\n2|"A"\n4|"B"\nrow_count 2\n', 'table ordered page bytes')
            sql = "INSERT INTO users(id) VALUES(5);"+valid+'\r\nSELECT id FROM users ORDER BY missing;INSERT INTO users(id) VALUES(6);'
            args = x.args(fmt=fmt);raw=sql.encode()
            if file:
                p.write_bytes(raw);args += ['--file',str(p)];raw=None
            else:
                args += ['--stdin']
            if fmt=='jsonl':
                result,out,err = x.invoke(args,raw)
                check(result.returncode==4 and len(err)==1 and err[0]['code']=='INVALID_ORDER_BY'
                      and err[0]['statement_index']=='3' and err[0]['line']=='2'
                      and err[0]['column']=='1','JSONL stop/error statement position')
                check(len(out)==2 and out[1]['statement_index']=='2','no partial bad query result')
            else:
                result = subprocess.run(args,input=raw,capture_output=True,timeout=15)
                check(result.returncode==4 and len(result.stderr.splitlines())==1
                      and result.stderr.startswith(b'diagnostic code=INVALID_ORDER_BY operation=run phase=execute ')
                      and b' statement_index="3" line="2" column="1" ' in result.stderr,
                      'table stop/error statement position')
                check(b'statement 3' not in result.stdout and b'statement 2 query_result' in result.stdout,'table stops before bad result')
            check(sorted(r[0] for r in decoded_rows(x))==[1,2,3,4,5],'prior mutation retained, following absent')


def regression_01(f):
    baseline(f)
    original = decoded_rows(f)
    query(f,'SELECT id,name FROM users ORDER BY name DESC,id OFFSET 1 ROWS;')
    check(decoded_rows(f)==original,'sort does not reorder storage')
    f.sql("UPDATE users SET name='changed' WHERE id=2;DELETE FROM users WHERE id=1;INSERT INTO users(id) VALUES(1);")
    got=query(f,'SELECT id,name FROM users ORDER BY id DESC FETCH FIRST 3 ROWS ONLY;')
    check(got['rows']==[['4',''],['3','kim'],['2','changed']],'M3 mutations and restart sorted query')


CASES = {'M4-ORDER-01':order_01,'M4-ORDER-02':order_02,'M4-ORDER-03':order_03,'M4-ORDER-04':order_04,
         'M4-PAGE-01':page_01,'M4-PAGE-02':page_02,'M4-PAGE-03':page_03,
         'M4-ERROR-01':error_01,'M4-ERROR-02':error_02,'M4-VALUES-01':values_01,
         'M4-VALUES-02':values_02,'M4-CLI-01':cli_01,'M4-REGRESSION-01':regression_01}


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--exe',required=True)
    parser.add_argument('--fault-exe',required=True)
    parser.add_argument('--case',choices=CASES,required=True)
    parser.add_argument('--work',required=True)
    args=parser.parse_args()
    CASES[args.case](Fixture(args.exe,Path(args.work),args.fault_exe))
