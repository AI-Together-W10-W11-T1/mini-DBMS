"""M7 UNION acceptance against native C23 entry points."""
import argparse
import os
import subprocess
from collections import Counter
from milestone_cli import Fixture, check
from m2_cli import failure, snapshot
from m4_cli import baseline, query
from m5_cli import baseline as join_baseline, JOIN


def union_01(f):
    f.init()
    got=query(f,"VALUES(1,'a'),(1,'a'),(1,'b'),(NULL,NULL) UNION VALUES(1,'a'),(NULL,NULL) ORDER BY column1,column2;")
    check(got['rows']==[['1','a'],['1','b'],[None,None]], 'full row NULL-aware dedup')
    check(query(f,"VALUES(''),(' '),('A'),('a'),('한') UNION VALUES('A') ORDER BY column1;")['rows']==[[''],[' '],['A'],['a'],['한']], 'UTF8 byte identity')


def all_01(f):
    f.init()
    check(query(f,'VALUES(1),(1),(NULL) UNION ALL VALUES(1),(NULL) ORDER BY column1;')['rows']==[['1'],['1'],['1'],[None],[None]], 'ALL multiplicity')


def mixed_01(f):
    f.init()
    check(query(f,'VALUES(1),(1) UNION ALL VALUES(1),(2) UNION VALUES(2) ORDER BY column1;')['rows']==[['1'],['2']], 'left association dedups accumulated ALL')
    check(query(f,'VALUES(1),(1) UNION VALUES(1) UNION ALL VALUES(1) ORDER BY column1;')['rows']==[['1'],['1']], 'later ALL retains duplicate')
    check(query(f,'VALUES(1),(1) UNION ALL VALUES(1) UNION ALL VALUES(1) UNION VALUES(1) UNION ALL VALUES(1) ORDER BY column1;')['rows']==[['1'],['1']], 'repeated ALL full accumulation dedup and last ALL')


def type_01(f):
    f.init()
    for sql,typ in [('VALUES(NULL) UNION VALUES(1);','INT64'),("VALUES(NULL) UNION ALL VALUES('x');",'TEXT'),('VALUES(NULL) UNION VALUES(NULL);','NULL')]:
        check(query(f,sql)['columns']==[{'name':'column1','type':typ}], 'NULL type inference')
    for sql,code in [("VALUES(NULL) UNION ALL VALUES(1) UNION VALUES('x');",'TYPE_MISMATCH'),('VALUES(1) UNION VALUES(1,2);','COLUMN_COUNT_MISMATCH')]: failure(f,sql,code)


def empty_01(f):
    f.init()
    check(query(f,'SELECT id AS initial_name FROM users UNION VALUES(NULL);')['columns']==[{'name':'initial_name','type':'INT64'}], 'typed empty first branch')
    failure(f,"SELECT id FROM users UNION VALUES('x') FETCH FIRST 0 ROWS ONLY;",'TYPE_MISMATCH')
    failure(f,'VALUES(1) UNION SELECT id,name FROM users FETCH FIRST 0 ROWS ONLY;','COLUMN_COUNT_MISMATCH')
    failure(f,'VALUES(1) UNION SELECT missing FROM users FETCH FIRST 0 ROWS ONLY;','UNKNOWN_COLUMN')
    check(query(f,'SELECT id FROM users UNION SELECT age FROM users;')['rows']==[], 'empty branches')
    failure(f,'SELECT id FROM users UNION SELECT name FROM users FETCH FIRST 0 ROWS ONLY;','TYPE_MISMATCH')
    failure(f,"VALUES(NULL) UNION VALUES('x') UNION VALUES(1) FETCH FIRST 0 ROWS ONLY;",'TYPE_MISMATCH')


def select_01(f):
    join_baseline(f)
    got=query(f,'SELECT u.id AS chosen'+JOIN+' UNION SELECT COUNT(*) FROM users UNION ALL VALUES(9) ORDER BY chosen;')
    check(got['rows']==[['1'],['2'],['3'],['9']], 'JOIN aggregate VALUES integration first name')
    check(query(f,'SELECT team AS t,COUNT(*) AS n FROM users GROUP BY team HAVING COUNT(*)>0 UNION VALUES(NULL,0) ORDER BY n,t;')['rows']==[[None,'0'],[None,'1'],['x','2']], 'group/having integration')


def tail_01(f):
    f.init()
    check(query(f,'VALUES(3),(1) UNION ALL VALUES(2),(NULL) ORDER BY column1 DESC NULLS FIRST OFFSET 1 ROWS FETCH FIRST 2 ROWS ONLY;')['rows']==[['3'],['2']], 'global tail')
    check(query(f,'VALUES(1) UNION VALUES(2) FETCH FIRST 0 ROWS ONLY;')['rows']==[], 'FETCH zero')
    failure(f,'SELECT id AS initial_name FROM users UNION SELECT age AS later_name FROM users ORDER BY later_name;','INVALID_ORDER_BY')
    failure(f,'VALUES(1) UNION VALUES(2) OFFSET 0 ROWS;','INVALID_PAGINATION')


def error_01(f):
    f.init()
    syntax=['VALUES(1) UNION;', 'VALUES(1) UNION ALL;', 'VALUES(1) UNION ALL ALL VALUES(2);', 'VALUES(1) UNION UNION VALUES(2);', 'VALUES(1) ORDER BY column1 UNION VALUES(2);', 'VALUES(1) FETCH FIRST 0 ROWS ONLY UNION VALUES(2);', 'VALUES(1) UNION VALUES(2) ORDER BY column1 UNION VALUES(3);', 'VALUES(1) UNION INSERT INTO users(id) VALUES(1);', 'VALUES(1) UNION SELECT;', 'VALUES(1) UNION VALUES;', 'VALUES(1) UNION VALUES(2)']
    for sql in syntax: failure(f,sql,'SYNTAX_ERROR')
    for sql in ['(VALUES(1)) UNION VALUES(2);','VALUES(1) UNION (VALUES(2));','VALUES(1) INTERSECT VALUES(2);','VALUES(1) EXCEPT VALUES(2);']: failure(f,sql,'UNSUPPORTED_FEATURE')
    failure(f,'VALUES(1) UNION VALUES(@);','LEXICAL_ERROR')


def overflow_01(f):
    f.init()
    # Initial Red exercised missing UNION. The native helper now checks actual arithmetic boundaries.
    check(query(f,'VALUES(1) UNION ALL VALUES(2);')['rows']==[['1'],['2']], 'UNION merge allocation path')
    p=subprocess.run([f.unit_exe],cwd=f.work,capture_output=True,timeout=30)
    check(p.returncode==0 and b'm7_union_size_boundaries' in p.stdout+p.stderr, 'actual checked merge helper boundaries')


def resource_01(f):
    baseline(f)
    before=snapshot(f)
    sql="SELECT name FROM users UNION VALUES('Kim'),('owned') ORDER BY name;"
    for name in ['union_merge','union_sort_keys','union_after_merge']:
        args=f.args();args[0]=f.fault_exe
        p,out,err=f.invoke(args+['--stdin'],sql.encode(),dict(os.environ,MINIDB_TEST_FAULT=name))
        check(p.returncode==4 and not out and len(err)==1 and err[0]['code']=='RESOURCE_ERROR', 'UNION fault '+name+str((p.returncode,out,err)))
        check(snapshot(f)==before,'failed union preserves catalog')
        query(f,sql)
    p,out,err=f.invoke(f.args()+['--stdin'],sql.encode(),dict(os.environ,MINIDB_TEST_FAULT='union_merge'))
    check(p.returncode==0 and len(out)==1 and not err,'production ignores hooks')


def ownership_01(f):
    p=subprocess.run([f.unit_exe],cwd=f.work,capture_output=True,timeout=30)
    check(p.returncode==0 and b'm7_union_owned_text_and_null' in p.stdout+p.stderr, 'native UNION lifetime '+(p.stdout+p.stderr).decode(errors='replace'))


def cli_01(f):
    baseline(f)
    sql='VALUES(2),(1) UNION VALUES(1) ORDER BY column1;'
    a,_=f.sql(sql,file=True);b,_=f.sql(sql);check(a==b,'file stdin parity')
    p,out,err=f.invoke(f.args(fmt='table')+['--stdin'],sql.encode(),parse_stdout=False)
    check(p.returncode==0 and not err and p.stdout==b'statement 1 query_result\n"column1":INT64\n1\n2\nrow_count 2\n','table bytes')
    out,err=f.sql("INSERT INTO users(id) VALUES(5);VALUES(1) UNION VALUES('x');INSERT INTO users(id) VALUES(6);",4)
    check(len(out)==1 and err[0]['code']=='TYPE_MISMATCH' and err[0]['statement_index']=='2','stop on union failure')
    check(query(f,'SELECT COUNT(*) FROM users;')['rows']==[['5']],'prior committed later stopped')


def regression_01(f):
    baseline(f)
    check(query(f,'SELECT COUNT(*) FROM users UNION VALUES(4);')['rows']==[['4']], 'aggregate dedup')
    f.sql("UPDATE users SET age=age+1 WHERE id=1;DELETE FROM users WHERE id=4;")
    check(query(f,'SELECT id FROM users UNION VALUES(9) ORDER BY id;')['rows']==[['1'],['2'],['3'],['9']], 'restart mutation integration')


if __name__=='__main__':
    p=argparse.ArgumentParser()
    for name in ['exe','fault-exe','unit-exe','case','work']: p.add_argument('--'+name,required=True)
    a=p.parse_args();f=Fixture(a.exe,a.work,a.fault_exe);f.unit_exe=a.unit_exe
    globals()[a.case.removeprefix('M7-').lower().replace('-','_')](f)
