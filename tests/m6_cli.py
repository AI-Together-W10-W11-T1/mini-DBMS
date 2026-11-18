"""M6 aggregation acceptance through native CLI and lifetime unit tests."""
import argparse
from collections import Counter
import os
import subprocess
from milestone_cli import Fixture, check
from m2_cli import failure, snapshot
from m4_cli import baseline, query
from m5_cli import baseline as join_baseline, JOIN


def count_01(f):
    baseline(f)
    got=query(f,'SELECT COUNT(*),COUNT(age),COUNT(name) AS names FROM users;')
    check(got['rows']==[['4','3','4']], 'COUNT all/nonNULL')
    check(got['columns']==[{'name':'count_all','type':'INT64'},{'name':'count_age','type':'INT64'},{'name':'names','type':'INT64'}], 'COUNT default names')


def group_01(f):
    baseline(f)
    got=query(f,'SELECT team,COUNT(*) AS n,COUNT(age) AS known FROM users GROUP BY team ORDER BY team;')
    check(got['rows']==[['A','2','1'],['B','1','1'],[None,'1','1']], 'NULL groups and COUNT')
    check(query(f,'SELECT team FROM users GROUP BY team ORDER BY team;')['rows']==[['A'],['B'],[None]], 'GROUP without COUNT')


def group_02(f):
    f.init()
    vals=["''", "' '", "'A'", "'a'", "'한'", "NULL", "NULL"]
    f.sql('INSERT INTO users(id,team,age) VALUES'+','.join(f'({i},{v},{i%2})' for i,v in enumerate(vals,1))+';')
    check(len(query(f,'SELECT team FROM users GROUP BY team;')['rows'])==6,'TEXT bytes and NULL equality')
    check(len(query(f,'SELECT team,age,COUNT(*) FROM users GROUP BY team,age;')['rows'])==7,'multiple keys')
    check(len(query(f,'SELECT * FROM users GROUP BY id,name,age,team;')['rows'])==7,'star grouping')


def having_01(f):
    baseline(f)
    check(query(f,'SELECT team,COUNT(*) AS n FROM users GROUP BY team HAVING (COUNT(*)>=2 OR COUNT(age)=0) AND COUNT(*)<>0 ORDER BY team;')['rows']==[['A','2']], 'HAVING precedence and hidden count')
    check(query(f,'SELECT COUNT(*) FROM users HAVING COUNT(age)=3;')['rows']==[['4']], 'HAVING hidden count global')
    check(query(f,'SELECT COUNT(*) FROM users HAVING COUNT(*)<4;')['rows']==[], 'HAVING rejects')


def having_02(f):
    f.init()
    f.sql('INSERT INTO users(id,team) VALUES'+','.join(f"({i},'{team}')" for i,team in enumerate('A'*9+'B'*10+'C'*11,1))+';')
    check(query(f,'SELECT team FROM users GROUP BY team HAVING COUNT(*)>=10 AND COUNT(*)<=11 ORDER BY team;')['rows']==[['B'],['C']], '9/10/11 and HAVING-only COUNT')


def join_01(f):
    join_baseline(f)
    check(query(f,'SELECT u.team,COUNT(*),COUNT(o.amount)'+JOIN+' WHERE o.id<>11 GROUP BY u.team HAVING COUNT(*)>0;')['rows']==[['x','2','2']], 'JOIN WHERE aggregate')
    check(query(f,'SELECT COUNT(*)'+JOIN+';')['rows']==[['3']], 'joined multiplicity')


def tail_01(f):
    baseline(f)
    check(query(f,'SELECT team,COUNT(*) AS n FROM users GROUP BY team ORDER BY n DESC,team OFFSET 1 ROWS FETCH FIRST 1 ROWS ONLY;')['rows']==[['B','1']], 'aggregate sort/page')
    failure(f,'SELECT COUNT(*) FROM users ORDER BY age;','INVALID_ORDER_BY')


def empty_01(f):
    f.init()
    check(query(f,'SELECT COUNT(*),COUNT(age) FROM users;')['rows']==[['0','0']], 'global empty')
    check(query(f,'SELECT team,COUNT(*) FROM users GROUP BY team;')['rows']==[], 'grouped empty')
    check(query(f,'SELECT COUNT(*) FROM users HAVING COUNT(*)=0;')['rows']==[['0']], 'empty HAVING')
    check(query(f,'SELECT COUNT(*) FROM users FETCH FIRST 0 ROWS ONLY;')['rows']==[], 'zero fetch')
    failure(f,'SELECT name,COUNT(*) FROM users FETCH FIRST 0 ROWS ONLY;','INVALID_GROUPING')
    failure(f,'SELECT COUNT(missing) FROM users FETCH FIRST 0 ROWS ONLY;','UNKNOWN_COLUMN')


def error_01(f):
    baseline(f)
    cases=[('SELECT name,COUNT(*) FROM users;','INVALID_GROUPING'),
           ('SELECT * FROM users GROUP BY team;','INVALID_GROUPING'),
           ('SELECT team AS t,COUNT(*) FROM users GROUP BY t;','UNKNOWN_COLUMN'),
           ('SELECT team FROM users GROUP BY 1;','UNSUPPORTED_FEATURE'),
           ('SELECT id FROM users WHERE COUNT(*)>0;','UNSUPPORTED_FEATURE'),
           ('SELECT COUNT(1) FROM users;','UNSUPPORTED_FEATURE'),
           ('SELECT COUNT(NULL) FROM users;','UNSUPPORTED_FEATURE'),
           ('SELECT COUNT(DISTINCT age) FROM users;','UNSUPPORTED_FEATURE'),
           ('SELECT SUM(age) FROM users;','UNSUPPORTED_FEATURE'),
           ('SELECT COUNT(*) FROM users HAVING SUM(age)>0;','UNSUPPORTED_FEATURE'),
           ('SELECT COUNT(*) AS n FROM users HAVING n>0;','UNSUPPORTED_FEATURE'),
           ('SELECT COUNT(age),COUNT(age) FROM users;','DUPLICATE_OUTPUT_NAME'),
           ('SELECT COUNT(age) FROM users HAVING COUNT(age)>9223372036854775808;','INTEGER_RANGE')]
    cases += [('SELECT COUNT() FROM users;','SYNTAX_ERROR'),
              ('SELECT COUNT(;','SYNTAX_ERROR'),
              ('SELECT COUNT(*) FROM users HAVING;','SYNTAX_ERROR'),
              ('SELECT COUNT(*) FROM users HAVING COUNT(*)>;','SYNTAX_ERROR'),
              ('SELECT COUNT(*) FROM users HAVING COUNT(*)>0 AND;','SYNTAX_ERROR'),
              ('SELECT COUNT(age+1) FROM users;','UNSUPPORTED_FEATURE'),
              ('SELECT COUNT(*) FROM users HAVING COUNT(*)>0 GROUP BY team;','SYNTAX_ERROR')]
    eof_cases = [(sql[:-1],'SYNTAX_ERROR') for sql,code in cases[-7:]]
    cases += eof_cases
    cases += [('SELECT COUNT(','SYNTAX_ERROR'),
              ('SELECT COUNT(*) FROM users HAVING COUNT(*)>0 OR;','SYNTAX_ERROR'),
              ('SELECT COUNT(@) FROM users;','LEXICAL_ERROR'),
              ('SELECT COUNT(*) FROM users HAVING @;','LEXICAL_ERROR'),
              ('SELECT COUNT(*) FROM users HAVING COUNT(*)>@;','LEXICAL_ERROR')]
    cases += [('SELECT COUNT(*) FROM users HAVING ORDER BY count_all;','SYNTAX_ERROR'),
              ('SELECT COUNT(*) FROM users HAVING COUNT(*)> ORDER BY count_all;','SYNTAX_ERROR'),
              ('SELECT COUNT(*) FROM users HAVING COUNT(*)>0 AND FETCH FIRST 0 ROWS ONLY;','SYNTAX_ERROR')]
    boundaries = ['GROUP BY team', 'HAVING COUNT(*)>0', 'ORDER BY count_all',
                  'OFFSET 0 ROWS', 'FETCH FIRST 0 ROWS ONLY',
                  'UNION SELECT COUNT(*) FROM users', 'WHERE id=1', 'FROM users',
                  'INNER JOIN users AS u ON users.id=u.id',
                  'JOIN users AS u ON users.id=u.id', 'AND COUNT(*)>0', 'OR COUNT(*)>0',
                  'AS n', 'ON id=id']
    positions = ['HAVING ', 'HAVING COUNT(*)> ', 'HAVING COUNT(*)>0 AND ',
                 'HAVING COUNT(*)>0 OR ']
    for boundary in boundaries:
        for position in positions:
            cases.append(('SELECT COUNT(*) FROM users '+position+boundary+';', 'SYNTAX_ERROR'))
        cases.append(('SELECT COUNT('+boundary+') FROM users;', 'SYNTAX_ERROR'))
    cases += [('SELECT COUNT(*) FROM users HAVING age>0;','UNSUPPORTED_FEATURE'),
              ('SELECT COUNT(*) FROM users HAVING COUNT(*)>NULL;','UNSUPPORTED_FEATURE'),
              ('SELECT COUNT(*) FROM users HAVING COUNT(*)>age;','UNSUPPORTED_FEATURE'),
              ('SELECT COUNT(*) FROM users ORDER BY count_all UNION SELECT COUNT(*) FROM users;','SYNTAX_ERROR')]
    for sql,code in cases: failure(f,sql,code)
    check(query(f,'SELECT COUNT(*) FROM users UNION SELECT COUNT(*) FROM users;')['rows']==[['4']], 'aggregate UNION supported')


def fault(f,name,code):
    before=snapshot(f)
    args=f.args();args[0]=f.fault_exe
    p,out,err=f.invoke(args+['--stdin'],b'SELECT name,team,COUNT(*) FROM users GROUP BY name,team;',dict(os.environ,MINIDB_TEST_FAULT=name))
    check(p.returncode==4 and not out and len(err)==1 and err[0]['code']==code, f'{name} {p.returncode} {out} {err}')
    check(snapshot(f)==before,'failed aggregate preserves storage')


def overflow_01(f):
    baseline(f)
    fault(f,'aggregate_count_overflow','INTEGER_OVERFLOW')
    p=subprocess.run([f.unit_exe],cwd=f.work,capture_output=True,timeout=30)
    check(p.returncode==0 and b'm6_count_and_size_boundaries' in p.stdout+p.stderr,
          'actual increment MAX-1/MAX and group size boundary tests')


def resource_01(f):
    baseline(f)
    for name in ['aggregate_group','aggregate_key_after_one','aggregate_output_after_one']:
        fault(f,name,'RESOURCE_ERROR')
        query(f,'SELECT team,COUNT(*) FROM users GROUP BY team;')
    p,out,err=f.invoke(f.args()+['--stdin'],b'SELECT COUNT(*) FROM users;',dict(os.environ,MINIDB_TEST_FAULT='aggregate_count_overflow'))
    check(p.returncode==0 and not err and out[0]['rows']==[['4']], 'production ignores hooks')


def ownership_01(f,unit_exe):
    p=subprocess.run([unit_exe],cwd=f.work,capture_output=True,timeout=30)
    check(p.returncode==0 and all(name in p.stdout+p.stderr for name in [b'm6_group_owned_text_and_null',b'm6_partial_copy_preserves_catalog']),'native aggregation lifetime '+(p.stdout+p.stderr).decode(errors='replace'))


def cli_01(f):
    baseline(f)
    sql='SELECT COUNT(*) FROM users;'
    a,_=f.sql(sql,file=True);b,_=f.sql(sql)
    check(a==b,'file stdin parity')
    p,out,err=f.invoke(f.args(fmt='table')+['--stdin'],sql.encode(),parse_stdout=False)
    check(p.returncode==0 and not err and p.stdout==b'statement 1 query_result\n"count_all":INT64\n4\nrow_count 1\n','table bytes')
    out,err=f.sql('INSERT INTO users(id) VALUES(5);SELECT name,COUNT(*) FROM users;INSERT INTO users(id) VALUES(6);',4)
    check(len(out)==1 and err[0]['code']=='INVALID_GROUPING' and err[0]['statement_index']=='2','stop order')
    check(query(f,'SELECT COUNT(*) FROM users;')['rows']==[['5']],'prior committed')


def regression_01(f):
    baseline(f)
    query(f,'SELECT COUNT(*) FROM users;')
    f.sql("UPDATE users SET team='A' WHERE id=4;DELETE FROM users WHERE id=3;")
    check(query(f,'SELECT team,COUNT(*) FROM users GROUP BY team;')['rows']==[['A','3']], 'restart mutation integration')
    check(query(f,'VALUES(2),(1) ORDER BY column1;')['rows']==[['1'],['2']], 'VALUES retained')


if __name__=='__main__':
    p=argparse.ArgumentParser()
    for name in ['exe','fault-exe','unit-exe','case','work']: p.add_argument('--'+name,required=True)
    a=p.parse_args();f=Fixture(a.exe,a.work,a.fault_exe);f.unit_exe=a.unit_exe
    name=a.case.removeprefix('M6-').lower().replace('-','_')
    if name=='ownership_01': ownership_01(f,a.unit_exe)
    else: globals()[name](f)
