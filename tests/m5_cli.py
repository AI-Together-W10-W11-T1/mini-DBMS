"""M5 real CLI acceptance: fixed two-input INNER JOIN."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import subprocess
from milestone_cli import Fixture, check
from m2_cli import failure, snapshot
from m4_cli import query

JOIN = ' FROM users AS u INNER JOIN orders AS o ON u.id=o.user_id'

def baseline(f, users=True, orders=True):
    definition = {'version':1,'name':'orders','columns':[
        {'name':n,'type':t} for n,t in [('id','INT64'),('user_id','INT64'),('amount','INT64'),('label','TEXT')]]}
    (f.schema/'orders.json').write_text(json.dumps(definition), encoding='utf-8')
    result, out, err = f.invoke(f.args('init'))
    check(result.returncode == 0 and not err, 'two table init')
    if users:
        f.sql("INSERT INTO users(id,name,age,team) VALUES(1,'A',20,'x'),(2,'B',NULL,'x'),(3,NULL,30,NULL);")
    if orders:
        f.sql("INSERT INTO orders(id,user_id,amount,label) VALUES(10,1,5,'A'),(11,1,NULL,'B'),(12,2,7,''),(13,NULL,9,NULL),(14,99,8,'x');")

def pairs(f, tail=''):
    return query(f,'SELECT u.id,o.id'+JOIN+tail+';')['rows']

def join_01(f):
    baseline(f)
    for clause in [JOIN, JOIN.replace('u.id=o.user_id','o.user_id=u.id'),
                   ' FROM users INNER JOIN orders ON users.id=orders.user_id']:
        names = 'u.id,o.id' if ' AS ' in clause else 'users.id,orders.id'
        got=query(f,'SELECT '+names+clause+';')
        check(Counter(map(tuple,got['rows'])) == Counter([('1','10'),('1','11'),('2','12')]),'P1-SQL-040 all INT matches')

def join_02(f):
    baseline(f)
    f.sql("UPDATE orders SET label='x' WHERE id<=12;UPDATE orders SET label='unmatched' WHERE id=14;")
    got=query(f,'SELECT u.id,o.id FROM users AS u INNER JOIN orders AS o ON u.team=o.label;')
    check(Counter(map(tuple,got['rows'])) == Counter((str(a),str(b)) for a in [1,2] for b in [10,11,12]),'P1-SQL-041 2x3 duplicates')

def join_03(f):
    baseline(f,False,False)
    texts=['',' ','A','a','한','\r\n',None]
    for i,t in enumerate(texts,1):
        lit='NULL' if t is None else "'"+t.replace("'","''")+"'"
        f.sql(f'INSERT INTO users(id,name) VALUES({i},{lit});INSERT INTO orders(id,label) VALUES({10+i},{lit});')
    got=query(f,'SELECT u.id,o.id FROM users AS u INNER JOIN orders AS o ON u.name=o.label ORDER BY u.id;')
    check(got['rows']==[[str(i),str(10+i)] for i in range(1,7)],'byte exact TEXT, NULL excluded')
    f.sql('UPDATE users SET age=-9223372036854775808 WHERE id=1;UPDATE orders SET amount=-9223372036854775808 WHERE id=11;')
    check(query(f,'SELECT u.id FROM users AS u INNER JOIN orders AS o ON u.age=o.amount;')['rows']==[['1']],'INT64 minimum')

def join_04(f):
    baseline(f)
    got=query(f,'SELECT a.id AS a_id,b.id AS b_id FROM users AS a INNER JOIN users AS b ON a.team=b.team WHERE a.id<>b.id ORDER BY a_id,b_id;')
    check(got['rows']==[['1','2'],['2','1']],'P1-SQL-042 explicit selfjoin')
    failure(f,'SELECT * FROM users INNER JOIN users AS b ON users.id=b.id;','INVALID_JOIN')

def name_01(f):
    baseline(f)
    for sql,code in [('SELECT id'+JOIN+';','AMBIGUOUS_COLUMN'),('SELECT u.id'+JOIN+' WHERE id=1;','AMBIGUOUS_COLUMN'),
                     ('SELECT users.id'+JOIN+';','UNKNOWN_TABLE'),('SELECT z.id'+JOIN+';','UNKNOWN_TABLE'),
                     ('SELECT u.nope'+JOIN+';','UNKNOWN_COLUMN'),('SELECT u.id'+JOIN.replace('o.user_id','missing')+';','UNKNOWN_COLUMN'),
                     ('SELECT * FROM users AS u INNER JOIN orders AS U ON u.id=U.user_id;','DUPLICATE_TABLE_ALIAS')]:
        failure(f,sql,code)
    check(query(f,'SELECT USER_ID,Name'+JOIN+' WHERE o.id=10;')['rows']==[['1','A']],'unique case-insensitive inputs')

def name_02(f):
    baseline(f)
    got=query(f,'SELECT *'+JOIN+' ORDER BY u.id,o.id;')
    check([c['name'] for c in got['columns']]==['u.id','u.name','u.age','u.team','o.id','o.user_id','o.amount','o.label'],'P1-SQL-044 star names')
    check(query(f,'SELECT u.ID AS Chosen,o.ID'+JOIN+' ORDER BY chosen,o.id;')['columns'][0]['name']=='Chosen','AS preserves spelling')
    failure(f,'SELECT u.id AS dup,o.id AS DUP'+JOIN+';','DUPLICATE_OUTPUT_NAME')
    failure(f,'SELECT u.id,u.id'+JOIN+';','DUPLICATE_OUTPUT_NAME')
    failure(f,'SELECT u.id AS chosen'+JOIN+' ORDER BY u.id;','INVALID_ORDER_BY')
    check(query(f,'SELECT ID FROM users;')['columns']==[{'name':'id','type':'INT64'}],'single input original spelling')

def tail_01(f):
    baseline(f)
    got=query(f,'SELECT u.id AS uid,o.id AS oid,o.amount'+JOIN+' WHERE (u.age=20 OR u.age IS NULL) AND o.id<>10 ORDER BY o.amount NULLS FIRST,uid,oid OFFSET 1 ROWS FETCH FIRST 1 ROWS ONLY;')
    check(got['rows']==[['2','12','7']],'JOIN WHERE projection sort page')

def empty_01(f):
    for i,(left,right) in enumerate([(False,True),(True,False),(False,False),(True,True)]):
        g=Fixture(f.exe,f.work/str(i),f.fault_exe);baseline(g,left,right)
        got=query(g,'SELECT u.name,o.amount'+JOIN+' WHERE u.id=999 FETCH FIRST 0 ROWS ONLY;')
        check(got['rows']==[] and got['columns']==[{'name':'u.name','type':'TEXT'},{'name':'o.amount','type':'INT64'}],'typed empty')
        for sql,code in [('SELECT missing'+JOIN+' FETCH FIRST 0 ROWS ONLY;','UNKNOWN_COLUMN'),
                         ('SELECT u.id'+JOIN+' WHERE u.name=1 FETCH FIRST 0 ROWS ONLY;','TYPE_MISMATCH'),
                         ('SELECT u.id'+JOIN+' ORDER BY o.id FETCH FIRST 0 ROWS ONLY;','INVALID_ORDER_BY'),
                         ('SELECT u.id'+JOIN.replace('o.user_id','o.label')+' FETCH FIRST 0 ROWS ONLY;','TYPE_MISMATCH')]: failure(g,sql,code)

def error_01(f):
    baseline(f)
    for sql,code in [('SELECT u.id'+JOIN.replace('o.user_id','u.id')+';','INVALID_JOIN'),
                     ('SELECT u.id'+JOIN.replace('u.id=o.user_id','u.name=o.user_id')+';','TYPE_MISMATCH'),
                     ('SELECT u.id'+JOIN+' AND o.id=10;','UNSUPPORTED_FEATURE'),
                     ('SELECT u.id'+JOIN+' INNER JOIN users AS x ON x.id=u.id;','UNSUPPORTED_FEATURE'),
                     ('SELECT * FROM users LEFT OUTER JOIN orders ON users.id=orders.user_id;','UNSUPPORTED_FEATURE'),
                     ('SELECT * FROM users INNER JOIN orders;','SYNTAX_ERROR'),
                     ('SELECT u.id'+JOIN.replace('u.id=o.user_id','u.id o.user_id')+';','SYNTAX_ERROR'),
                     ('SELECT u.id'+JOIN.replace('u.id=o.user_id','u.id')+';','SYNTAX_ERROR'),
                     ('SELECT u.id'+JOIN.replace('u.id=o.user_id','u.id') ,'SYNTAX_ERROR'),
                     ('SELECT u.id'+JOIN.replace('u.id=o.user_id','u.id=')+';','SYNTAX_ERROR'),
                     ('SELECT * FROM users INNER JOIN orders ON users.id>orders.user_id;','INVALID_JOIN')]: failure(f,sql,code)

def resource_01(f):
    baseline(f)
    sql='SELECT u.name,o.label'+JOIN+' ORDER BY u.name,o.label;'
    before={p.name:p.read_bytes() for p in f.data.glob('*.tbl')}
    check(set(before)=={'table_users.tbl','table_orders.tbl'},'both persisted table snapshots present')
    for fault in ['join_width','join_row','join_copy_after_one']:
        env=dict(os.environ,MINIDB_TEST_FAULT=fault)
        args=f.args();args[0]=f.fault_exe
        p,out,err=f.invoke(args+['--stdin'],sql.encode(),env)
        check(p.returncode==4 and not out and len(err)==1 and err[0]['code']=='RESOURCE_ERROR',f'actual JOIN failure {fault}: {p.returncode} {out} {err}')
        check({p.name:p.read_bytes() for p in f.data.glob('*.tbl')}==before,'fault preserves both tables')
        query(f,sql)
    p,out,err=f.invoke(f.args()+['--stdin'],sql.encode(),dict(os.environ,MINIDB_TEST_FAULT='join_row'))
    check(p.returncode==0 and len(out)==1 and not err,'production ignores test hooks')

def ownership_01(f,unit_exe):
    p=subprocess.run([unit_exe],cwd=f.work,capture_output=True,timeout=30)
    combined=p.stdout+p.stderr
    check(p.returncode==0 and b'm5_join_owned_text_and_null' in combined and b'm5_join_checked_width' in combined and b'm5_join_result_size_boundaries' in combined and b'm5_partial_copy_preserves_catalog' in combined,'real new cmocka JOIN ownership and checked width cases: '+combined.decode(errors='replace'))

def cli_01(f):
    baseline(f)
    sql='SELECT u.id,o.id'+JOIN+' ORDER BY u.id,o.id;'
    a,_=f.sql(sql,file=True);b,_=f.sql(sql)
    check(a==b,'file stdin JSONL parity')
    p,out,err=f.invoke(f.args(fmt='table')+['--stdin'],sql.encode(),parse_stdout=False)
    check(p.returncode==0 and not err and p.stdout==b'statement 1 query_result\n"u.id":INT64|"o.id":INT64\n1|10\n1|11\n2|12\nrow_count 3\n','table exact bytes')
    out,err=f.sql('INSERT INTO users(id) VALUES(4);SELECT id'+JOIN+';INSERT INTO users(id) VALUES(5);',4)
    check(len(out)==1 and err[0]['statement_index']=='2' and err[0]['code']=='AMBIGUOUS_COLUMN','stop after prior success')
    check(query(f,'SELECT id FROM users ORDER BY id;')['rows']==[['1'],['2'],['3'],['4']],'commit retained later stopped')

def regression_01(f):
    baseline(f);before={p.name:p.read_bytes() for p in f.data.glob('*.tbl')}
    check(set(before)=={'table_users.tbl','table_orders.tbl'},'both persisted table snapshots present')
    pairs(f);query(f,'VALUES(2),(1) ORDER BY column1;')
    check({p.name:p.read_bytes() for p in f.data.glob('*.tbl')}==before,'both input files unchanged')
    f.sql("UPDATE users SET name='changed' WHERE id=1;DELETE FROM orders WHERE id=11;")
    check(pairs(f,' ORDER BY u.id,o.id')==[['1','10'],['2','12']],'mutations and restart JOIN')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--exe',required=True);parser.add_argument('--fault-exe',required=True)
    parser.add_argument('--unit-exe',required=True);parser.add_argument('--case',required=True);parser.add_argument('--work',required=True)
    a=parser.parse_args();f=Fixture(a.exe,a.work,a.fault_exe)
    name=a.case.removeprefix('M5-').lower().replace('-','_')
    if name=='ownership_01': ownership_01(f,a.unit_exe)
    else: globals()[name](f)
