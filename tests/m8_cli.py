"""M8 real-process migration acceptance. Every variant owns a fresh directory."""
import argparse
import json
import os
import subprocess
from pathlib import Path
from milestone_cli import Fixture, check


def prepare(f, columns=None, sql="INSERT INTO users(id,name,age,team) VALUES(1,'owned',-9223372036854775808,NULL),(2,NULL,9223372036854775807,'x');"):
    f.init()
    f.sql(sql)
    f.old = f.data
    f.before = {p.name:p.read_bytes() for p in f.old.glob('*.tbl')}
    f.newschema = f.work / 'new-schema'
    f.newschema.mkdir()
    definition = dict(f.definition)
    if columns is not None:
        definition['columns'] = [{'name':n,'type':t} for n,t in columns]
    (f.newschema/'users.json').write_text(json.dumps(definition),encoding='utf-8')
    f.target=f.work/'output'
    f.schemas_before={path:path.read_bytes() for folder in [f.schema,f.newschema] for path in folder.glob('*.json')}


def migrate(f, plan=None, fault=None, expected=0, code=None):
    args=[f.fault_exe if fault else f.exe,'migrate','--source-data-dir',str(f.old),'--schema-dir',str(f.newschema),'--data-dir',str(f.target),'--format','jsonl']
    if plan is not None:
        path=f.work/'plan.json'
        path.write_bytes(plan if isinstance(plan,bytes) else json.dumps(plan).encode())
        args+=['--plan',str(path)]
    p,out,err=f.invoke(args,env=dict(os.environ,MINIDB_TEST_FAULT=fault) if fault else None)
    check(p.returncode==expected,f'migrate exit {p.returncode}, expected {expected}: {p.stderr!r}')
    check({p.name:p.read_bytes() for p in f.old.glob('*.tbl')}==f.before,'source bytes preserved')
    check(all(path.read_bytes()==raw for path,raw in f.schemas_before.items()),'external schema bytes preserved')
    if expected:
        check(not out and len(err)==1,'failure has one diagnostic and no partial success')
        if code: check(err[0]['code']==code,str(err))
        if expected==7: check(err[0]['output_ready'] is True,'stdout failed after publication')
        else: check(err[0]['output_ready'] is False,'management failure output_ready false')
    else:
        check(not err and len(out)==1 and out[0]['output_ready'] is True,'whole operation ready')
    return out,err


def rows(f):
    p,out,err=f.invoke([f.exe,'run','--schema-dir',str(f.newschema),'--data-dir',str(f.target),'--stdin','--format','jsonl'],b'SELECT * FROM users ORDER BY id;')
    check(p.returncode==0 and not err,'restart with target schema '+str(err))
    check(out[0]['rows']==stored_rows(f.target/'table_users.tbl'),'independent binary decode matches query rows')
    return out[0]['rows']


def stored_rows(path):
    import struct
    raw=path.read_bytes()
    check(raw[:8]==b'MINIDBMS' and struct.unpack_from('<I',raw,8)[0]==1,'independent format version')
    size=struct.unpack_from('<Q',raw,12)[0]
    definition=json.loads(raw[20:20+size]);position=20+size;result=[]
    while position<len(raw):
        frame=struct.unpack_from('<Q',raw,position)[0];position+=8;end=position+frame;row=[]
        for column in definition['columns']:
            tag=raw[position];position+=1;check(tag in [0,1],'independent tag')
            if not tag:row.append(None)
            elif column['type']=='INT64':row.append(str(struct.unpack_from('<q',raw,position)[0]));position+=8
            else:
                length=struct.unpack_from('<Q',raw,position)[0];position+=8
                row.append(raw[position:position+length].decode('utf-8'));position+=length
        check(position==end and end<=len(raw),'independent exact record framing')
        result.append(row)
    id_column=next(i for i,c in enumerate(definition['columns']) if c['name'].lower()=='id')
    return sorted(result,key=lambda row:int(row[id_column]))


def map_01(f):
    prepare(f,[('renamed','TEXT'),('id','INT64'),('fresh','TEXT'),('team','TEXT')])
    plan={'version':1,'tables':[{'name':'users','map':[{'target':'renamed','source':'name'}],'drop':['age']}]}
    out,_=migrate(f,plan)
    check(out[0]['tables']==[{'name':'users','action':'converted','row_count':'2'}],'converted management event')
    check(rows(f)==[['owned','1',None,None],[None,'2',None,'x']],'reordered mapped owned TEXT NULL and ID')


def type_01(f):
    prepare(f,[('id','INT64'),('name','TEXT'),('age','TEXT'),('team','TEXT')])
    migrate(f)
    check(rows(f)==[['1','owned','-9223372036854775808',None],['2',None,'9223372036854775807','x']],'INT64 exact endpoints to TEXT')
    v=child(f,'decimal');prepare(v,[('id','INT64'),('name','TEXT'),('age','TEXT'),('team','TEXT')],"INSERT INTO users(id,age) VALUES(1,0),(2,-42),(3,42),(4,NULL);")
    migrate(v)
    check([r[2] for r in rows(v)]==['0','-42','42',None],'canonical zero signed numbers NULL')


def type_03(f):
    invalid=['9223372036854775808','-9223372036854775809','',' ','+','-','1 2','1.0','1e2','0x1','\v1','1\f','\u00a01','1\u2003']
    for i,value in enumerate(invalid):
        v=child(f,'number'+str(i))
        prepare(v,[('id','INT64'),('name','INT64'),('age','INT64'),('team','TEXT')],"INSERT INTO users(id,name) VALUES(1,'9223372036854775807'),(2,'"+value+"');")
        migrate(v,expected=4,code='MIGRATION_ERROR')
        blocked(v)


def ownership_01(f):
    map_01(f)
    # Migration-produced TEXT and NULL must survive engine teardown and a fresh process.
    check(rows(f)==[['owned','1',None,None],[None,'2',None,'x']],'repeat independent restart ownership')
    p=subprocess.run([f.unit_exe],cwd=f.work,capture_output=True,timeout=30)
    check(p.returncode==0 and b'm8_migration_owned_events_and_text' in p.stdout+p.stderr,'native migration API lifetime '+str(p.stdout+p.stderr))


def child(f, label):
    variant = Fixture(f.exe, f.work.parent / label, f.fault_exe)
    variant.unit_exe = f.unit_exe
    return variant


def blocked(f):
    p,out,err=f.invoke([f.exe,'run','--schema-dir',str(f.newschema),'--data-dir',str(f.target),'--stdin','--format','jsonl'],b'VALUES(1);')
    check(p.returncode==3 and not out and len(err)==1,'incomplete output remains blocked '+str(err))


def plan_01(f):
    for i,plan in enumerate([None,b'\xef\xbb\xbf{"version":1,"tables":[]}',{'version':1,'tables':[{'name':'users'}]}]):
        v=child(f,'valid'+str(i));prepare(v,sql='')
        out,_=migrate(v,plan)
        check(out[0]['tables']==[{'name':'users','action':'copied','row_count':'0'}] and rows(v)==[],'empty/default plan')


def plan_02(f):
    invalid=[b'',b'[]',b'{}',b'{"version":1}',b'{"version":1,"tables":null}',
        b'{"version":true,"tables":[]}',b'{"version":2,"tables":[]}',
        b'{"version":1,"tables":[],"extra":0}',b'{"version":1,"version":1,"tables":[]}',
        b'{"version":1,"v\\u0065rsion":1,"tables":[]}',b'{"Version":1,"tables":[]}',
        b'{"version":1,"tables":[{}]}',b'{"version":1,"tables":[{"name":null}]}',
        b'{"version":1,"tables":[{"name":"users","map":null}]}',
        b'{"version":1,"tables":[{"name":"users","drop":null}]}',
        b'{"version":1,"tables":[{"name":"users","map":[{"source":"id"}]}]}',
        b'{"version":1,"tables":[{"name":"users","map":[{"target":"id","source":"id","source":"id"}]}]}',
        b'{"version":1,"tables":[{"name":"users","drop":[0]}]}',
        b'{"version":1,"tables":[],}',b'/*comment*/{"version":1,"tables":[]}',
        b'{"version":1,"tables":[],"\xff":0}',b'{"version":1,"tables":[{"name":"\\ud800"}]}',
        b'{"version":1,"tables":[],"tables\\u0000":[]}']
    for i,plan in enumerate(invalid):
        v=child(f,'bad'+str(i));prepare(v)
        # Structure errors take priority even when both directories are unavailable.
        v.old=v.work/'missing-source';v.before={};v.newschema=v.work/'missing-schema'
        migrate(v,plan,expected=2,code='ARGUMENT_ERROR')
        check(not v.target.exists(),'structure rejected before creation')


def plan_03(f):
    for i,name in enumerate(['','1bad','select','users\x00tail','absent']):
        v=child(f,'name'+str(i));prepare(v)
        migrate(v,{'version':1,'tables':[{'name':name}]},expected=3,code='MIGRATION_ERROR')
        check(not v.target.exists(),'semantic plan no output')
    v=child(f,'unreadable');prepare(v)
    args=[v.exe,'migrate','--source-data-dir',str(v.old),'--schema-dir',str(v.newschema),'--data-dir',str(v.target),'--plan',str(v.work/'missing.json'),'--format','jsonl']
    p,out,err=v.invoke(args)
    check(p.returncode==3 and not out and err[0]['code']=='IO_ERROR' and err[0]['source']=='plan','plan access error')


def source_01(f):
    import struct
    prepare(f)
    f.schemas_before.pop(f.schema_file)
    f.schema_file.unlink()
    migrate(f)
    check(rows(f)[0][1]=='owned','source headers need no old JSON')
    for i,kind in enumerate(['magic','version','schema','late','id','length']):
        v=child(f,'corrupt'+str(i));prepare(v)
        path=v.old/'table_users.tbl';raw=bytearray(path.read_bytes())
        start=20+struct.unpack_from('<Q',raw,12)[0]
        if kind=='magic':raw[0]=0
        elif kind=='version':struct.pack_into('<I',raw,8,2)
        elif kind=='schema':raw[20]=0
        elif kind=='late':raw=raw[:-1]
        elif kind=='id':struct.pack_into('<q',raw,start+9,0)
        else:struct.pack_into('<Q',raw,start,(1<<64)-1)
        path.write_bytes(raw);v.before={path.name:bytes(raw)}
        migrate(v,expected=3,code='STORAGE_FORMAT_ERROR')
        check(not v.target.exists(),'all source rows validated before creation')
    # Independent sharing violation covers the real source file access diagnostic.
    import ctypes
    from ctypes import wintypes
    v=child(f,'unreadable');prepare(v)
    create=ctypes.windll.kernel32.CreateFileW
    create.argtypes=[wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE]
    create.restype=wintypes.HANDLE
    close=ctypes.windll.kernel32.CloseHandle
    close.argtypes=[wintypes.HANDLE];close.restype=wintypes.BOOL
    handle=create(str(v.old/'table_users.tbl'),0x80000000,0,None,3,0x80,None)
    check(handle not in [None,ctypes.c_void_p(-1).value],'hold actual source table share denial')
    try:
        p,out,err=v.invoke([v.exe,'migrate','--source-data-dir',str(v.old),'--schema-dir',str(v.newschema),'--data-dir',str(v.target),'--format','jsonl'])
        check(p.returncode==3 and not out and err[0]['code']=='IO_ERROR' and err[0]['source']=='data','source file access diagnostic')
    finally:
        check(close(handle),'close source sharing fixture')
    check(not v.target.exists() and {p.name:p.read_bytes() for p in v.old.glob('*.tbl')}==v.before,'unreadable source untouched and no output')


def map_02(f):
    invalid=[{'map':[{'target':'name','source':'team'}]},
        {'map':[{'target':'name','source':'absent'}]},
        {'map':[{'target':'absent','source':'name'}]},
        {'map':[{'target':'name','source':'name'},{'target':'NAME','source':'team'}]},
        {'drop':['name']},{'drop':['absent']},{'drop':['name','NAME']},
        {'map':[{'target':'name','source':'name\x00tail'}]}]
    for i,entry in enumerate(invalid):
        v=child(f,'mapping'+str(i));prepare(v)
        migrate(v,{'version':1,'tables':[dict(name='users',**entry)]},expected=3,code='MIGRATION_ERROR')
        check(not v.target.exists(),'invalid mapping no output')
    v=child(f,'duplicates');prepare(v)
    migrate(v,{'version':1,'tables':[{'name':'users'},{'name':'USERS'}]},expected=3,code='MIGRATION_ERROR')
    v=child(f,'set');prepare(v)
    extra=dict(v.definition);extra['name']='other'
    (v.newschema/'other.json').write_text(json.dumps(extra),encoding='utf-8')
    migrate(v,expected=3,code='MIGRATION_ERROR')
    v=child(f,'missingdrop');prepare(v,[('id','INT64')])
    migrate(v,expected=3,code='MIGRATION_ERROR')


def map_03(f):
    prepare(f,[('name','TEXT'),('ID','INT64'),('age','INT64'),('team','TEXT')])
    migrate(f)
    check(rows(f)==[['owned','1','-9223372036854775808',None],[None,'2','9223372036854775807','x']],'ID spelling position values')
    for i,entry in enumerate([{'map':[{'target':'id','source':'age'}]}, {'drop':['id']}, {'map':[{'target':'name','source':'id'}]}]):
        v=child(f,'id'+str(i));prepare(v)
        migrate(v,{'version':1,'tables':[dict(name='users',**entry)]},expected=3,code='MIGRATION_ERROR')
        check(not v.target.exists(),'invalid ID mapping before creation')


def copy_01(f):
    prepare(f)
    out,_=migrate(f)
    check((f.target/'table_users.tbl').read_bytes()==f.before['table_users.tbl'],'copied file byte identity')
    check(out[0]['tables'][0]['action']=='copied','identity action')
    v=child(f,'swap');prepare(v)
    multi(v)
    out,_=migrate(v,{'version':1,'tables':[{'name':'users','map':[{'target':'name','source':'team'},{'target':'team','source':'name'}]}]})
    check(out[0]['tables']==[{'name':'Other','action':'copied','row_count':'0'},{'name':'users','action':'converted','row_count':'2'}],'mixed actions sorted target spellings')
    check((v.target/'table_other.tbl').read_bytes()==v.before['table_other.tbl'],'mixed copied file byte identity')
    check(rows(v)==[['1',None,'-9223372036854775808','owned'],['2','x','9223372036854775807',None]],'same schema swap values')


def type_02(f):
    valid=[' \t\r\n+00042\n\r\t ','-0','0000','-9223372036854775808','+9223372036854775807',None]
    sql='INSERT INTO users(id,name) VALUES'+','.join('('+str(i+1)+','+('NULL' if value is None else "'"+value+"'")+')' for i,value in enumerate(valid))+';'
    prepare(f,[('id','INT64'),('name','INT64'),('age','INT64'),('team','TEXT')],sql)
    migrate(f)
    check([r[1] for r in rows(f)]==['42','0','0','-9223372036854775808','9223372036854775807',None],'exact integer grammar endpoints and NULL')


def path_01(f):
    p=subprocess.run([f.unit_exe],cwd=f.work,capture_output=True,timeout=30)
    check(p.returncode==0 and b'm8_output_path_roots' in p.stdout+p.stderr,'actual output resolver root preservation '+str(p.stdout+p.stderr))
    prepare(f)
    for i in range(3):
        v=child(f,'paths'+str(i));prepare(v);v.target=[v.old,v.old/'inside',v.work][i]
        migrate(v,expected=3,code='IO_ERROR')
    alias=short_path(f.old)
    v=child(f,'alias');prepare(v);v.old=Path(alias);v.before=f.before;v.target=Path(alias)/'nested'
    migrate(v,expected=3,code='IO_ERROR')
    check(not v.target.exists(),'actual 8.3 alias overlap rejected')


def short_path(path):
    import ctypes
    output=ctypes.create_unicode_buffer(32768)
    count=ctypes.windll.kernel32.GetShortPathNameW(str(path),output,len(output))
    check(0<count<len(output) and output.value.lower()!=str(path).lower(),'actual existing 8.3 directory alias')
    return output.value


def lease_01(f):
    import queue
    import threading
    import time
    prepare(f)
    proc=subprocess.Popen(f.args()+['--stdin'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    lines=queue.Queue();threading.Thread(target=lambda:lines.put(proc.stdout.readline()),daemon=True).start()
    try:
        proc.stdin.write(b'VALUES(1);');proc.stdin.flush()
        check(json.loads(lines.get(timeout=8))['type']=='query_result','source lease holder ready')
        migrate(f,expected=3,code='DATABASE_BUSY')
        original=f.old;f.old=Path(short_path(f.old))
        migrate(f,expected=3,code='DATABASE_BUSY')
        f.old=original
        check(not f.target.exists(),'busy source no output')
    finally:
        proc.stdin.close();proc.wait(timeout=8)
    args=[f.fault_exe,'migrate','--source-data-dir',str(f.old),'--schema-dir',str(f.newschema),'--data-dir',str(f.target),'--format','jsonl']
    proc=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=dict(os.environ,MINIDB_TEST_FAULT='manage_hold'))
    try:
        deadline=time.monotonic()+8
        while not (f.target/'.minidb.pending').exists() and proc.poll() is None and time.monotonic()<deadline:time.sleep(.01)
        check((f.target/'.minidb.pending').exists(),'migration generated marker while retaining lease')
        p,out,err=f.invoke(f.args()+['--stdin'],b'VALUES(1);')
        check(p.returncode==3 and not out and err[0]['code']=='DATABASE_BUSY','source lease retained through output generation')
        stdout,stderr=proc.communicate(timeout=8)
        check(proc.returncode==0 and not stderr and json.loads(stdout)['output_ready'],'held migration completes')
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=8)
    check(rows(f)[0][1]=='owned','handles released after completion')


def multi(f):
    import struct
    definition={'version':1,'name':'Other','columns':[{'name':'id','type':'INT64'},{'name':'payload','type':'TEXT'}]}
    encoded=json.dumps(definition,separators=(',',':')).encode()
    raw=b'MINIDBMS'+struct.pack('<IQ',1,len(encoded))+encoded
    (f.old/'table_other.tbl').write_bytes(raw)
    (f.newschema/'Other.json').write_text(json.dumps(definition),encoding='utf-8')
    f.before['table_other.tbl']=raw
    path=f.newschema/'Other.json';f.schemas_before[path]=path.read_bytes()


def failure_variants(f, faults):
    for i,mode in enumerate(faults):
        v=child(f,'fault'+str(i));prepare(v, None if i%2 else [('id','INT64'),('name','TEXT'),('age','TEXT'),('team','TEXT')])
        multi(v)
        migrate(v,fault=mode,expected=4)
        blocked(v)
        v.sql('SELECT id FROM users;')


def failure_01(f):
    failure_variants(f,['manage_output_lease','manage_marker_create','manage_marker_write','manage_marker_flush','manage_marker_close','manage_table_write','manage_table_flush','manage_table_close'])


def failure_02(f):
    failure_variants(f,['manage_validate','manage_residual','manage_guard_cleanup','manage_marker_cleanup'])


def resource_01(f):
    for i,mode in enumerate(['migrate_plan_alloc','migrate_mapping_alloc','migrate_rows_alloc','manage_serialization_alloc']):
        v=child(f,'alloc'+str(i));prepare(v,[('id','INT64'),('name','TEXT'),('age','TEXT'),('team','TEXT')])
        created=i>=2
        migrate(v,{'version':1,'tables':[]},fault=mode,expected=4 if created else 3,code='RESOURCE_ERROR')
        if created:blocked(v)
        else:check(not v.target.exists(),'prepare allocation creates no output')
    p=subprocess.run([f.unit_exe],cwd=f.work,capture_output=True,timeout=30)
    check(p.returncode==0 and b'm8_migration_boundaries' in p.stdout+p.stderr,'native actual size/integer helper boundaries '+str(p.stdout+p.stderr))


def cli_01(f):
    prepare(f)
    migrate(f,fault='manage_stdout',expected=7,code='IO_ERROR')
    check(rows(f)[0][1]=='owned','stdout failure preserves ready output')
    for i,mode in enumerate(['manage_table_write','migrate_rows_alloc']):
        v=child(f,'production'+str(i));prepare(v,[('id','INT64'),('name','TEXT'),('age','TEXT'),('team','TEXT')]);v.fault_exe=v.exe
        migrate(v,fault=mode)
        check(rows(v)[0][2]=='-9223372036854775808','production ignores test hooks')
    v=child(f,'table');prepare(v)
    p,out,err=v.invoke([v.exe,'migrate','--source-data-dir',str(v.old),'--schema-dir',str(v.newschema),'--data-dir',str(v.target)],parse_stdout=False)
    check(p.returncode==0 and not err and b'operation migrate output_path ' in p.stdout and b'output_ready true' in p.stdout and b'table "users" action copied row_count 2' in p.stdout,'table management output')


def init_01(f):
    for i,mode in enumerate(['manage_output_lease','manage_marker_create','manage_marker_write','manage_marker_flush','manage_marker_close','manage_table_write','manage_table_flush','manage_table_close','manage_serialization_alloc','manage_validate','manage_residual','manage_guard_cleanup','manage_marker_cleanup','manage_stdout']):
        v=child(f,'init'+str(i));v.newschema=v.schema;v.target=v.data
        args=v.args('init');args[0]=v.fault_exe
        p,out,err=v.invoke(args,env=dict(os.environ,MINIDB_TEST_FAULT=mode))
        ready=mode=='manage_stdout'
        check(p.returncode==(7 if ready else 4) and not out and len(err)==1 and err[0]['output_ready']==ready,'init failure phase '+mode+str((p.returncode,out,err)))
        if ready:v.sql('VALUES(1);')
        else:blocked(v)


def regression_01(f):
    prepare(f)
    migrate(f)
    old=f.data;f.data=f.target;f.schema=f.newschema
    f.sql("UPDATE users SET name='new' WHERE id=1;DELETE FROM users WHERE id=2;INSERT INTO users(id,name) VALUES(3,'next');")
    out,_=f.sql('SELECT name,COUNT(*) AS n FROM users GROUP BY name UNION VALUES(NULL,0) ORDER BY n,name;')
    check(out[0]['rows']==[[None,'0'],['new','1'],['next','1']],'migrated store supports mutations aggregate UNION restart')
    check({p.name:p.read_bytes() for p in old.glob('*.tbl')}==f.before,'later target mutations preserve source')


if __name__=='__main__':
    p=argparse.ArgumentParser()
    for name in ['exe','fault-exe','unit-exe','case','work']: p.add_argument('--'+name,required=True)
    a=p.parse_args();f=Fixture(a.exe,a.work,a.fault_exe);f.unit_exe=a.unit_exe
    globals()[a.case.removeprefix('M8-').lower().replace('-','_')](f)
