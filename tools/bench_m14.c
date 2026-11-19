/* 연결/이벤트는 client 스레드가 독점한다. 워밍업 DONE 이후 공통 barrier를 연다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include "minidb_client.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MEMORY_CAP 65536u
typedef struct { uint64_t begin,end; unsigned client,phase,sequence; bool update,attempted,confirmed,unconfirmed; const char *status; char code[40]; } Sample;
typedef struct { uint64_t tick,working_set,private_bytes; bool ok; } Memory;
typedef struct Run Run;
typedef struct { Run *run; unsigned index; HANDLE thread; char final[80],connect_code[40]; } Client;
struct Run {
    unsigned connections,rows,warmup,requests,port,pid,ready,finished;
    const char *workload,*output;
    Sample *samples; Memory *memory; size_t memory_count;
    Client clients[32]; CRITICAL_SECTION lock; CONDITION_VARIABLE ready_condition,go_condition;
    bool go,abort,valid,memory_overflow; uint64_t frequency,start,end; HANDLE process;
};
static uint64_t now(void) { LARGE_INTEGER v; return QueryPerformanceCounter(&v) && v.QuadPart>=0 ? (uint64_t)v.QuadPart : 0; }
static bool decimal(const char *s,unsigned *out) {
    uint64_t v=0; if (!*s) return false;
    for (;*s;s++) { if (*s<'0'||*s>'9'||v>(UINT32_MAX-(unsigned)(*s-'0'))/10) return false; v=v*10+(unsigned)(*s-'0'); }
    *out=(unsigned)v; return true;
}
static void quoted(FILE *f,const char *s) {
    fputc('"',f); for (const unsigned char *p=(const unsigned char *)s;*p;p++) {
        if (*p=='"'||*p=='\\') fprintf(f,"\\%c",*p); else if (*p<32) fprintf(f,"\\u%04x",*p); else fputc(*p,f);
    } fputc('"',f);
}
static bool exact_result(MdEvent *e,unsigned target,const char *payload) {
    MdResult *r=&e->result;
    return e->has_result && r->kind==MD_QUERY_RESULT && r->statement_index==1 && r->row_count==1 && r->column_count==3 &&
        !strcmp(r->columns[0].name,"id") && r->columns[0].type==MD_INT64 && !strcmp(r->columns[1].name,"scan_key") && r->columns[1].type==MD_INT64 &&
        !strcmp(r->columns[2].name,"payload") && r->columns[2].type==MD_TEXT && r->values[0].type==MD_INT64 && r->values[0].as.integer==target &&
        r->values[1].type==MD_INT64 && r->values[1].as.integer==target && r->values[2].type==MD_TEXT &&
        r->values[2].as.text.len==strlen(payload) && !memcmp(r->values[2].as.text.ptr,payload,strlen(payload));
}
static void execute(Client *c,MdClient **connection,unsigned phase,unsigned sequence) {
    Run *r=c->run; Sample *s=&r->samples[(size_t)c->index*(r->warmup+r->requests)+(phase?r->warmup:0)+sequence];
    *s=(Sample){.client=c->index,.phase=phase,.sequence=sequence,.status="skipped"};
    s->update=!strcmp(r->workload,"mixed") && sequence%20==19;
    if (!*connection) return;
    unsigned target=!strcmp(r->workload,"mixed") ? c->index+1 : (sequence*37+c->index*997)%r->rows+1;
    char initial[80],marker[80],sql[512]; snprintf(initial,sizeof initial,"row-%010u",target);
    snprintf(marker,sizeof marker,"m14-c%u-p%u-n%u",c->index,phase,sequence);
    const char *expected=!strcmp(r->workload,"mixed")?c->final:initial;
    if (s->update) snprintf(sql,sizeof sql,"UPDATE bench_rows SET payload='%s' WHERE id=%u;",marker,target);
    else snprintf(sql,sizeof sql,"SELECT id,scan_key,payload FROM bench_rows WHERE %s=%u;",!strcmp(r->workload,"scan_key")?"scan_key":"id",target);
    MdClientError error={0}; s->begin=now(); s->attempted=true; s->status="transport";
    bool submitted=mdc_execute(*connection,(const unsigned char *)sql,strlen(sql),&error),done=false,failed=false,invalid=false;
    unsigned events=0,results=0;
    if (submitted) for (;;) {
        MdEvent event={0}; MdClientNext next=mdc_next(*connection,&event,&error);
        if (next==MDC_EVENT) {
            events++; if (event.has_result) results++; if (results>1) invalid=true;
            if (event.has_diagnostic) { failed=true; snprintf(s->code,sizeof s->code,"%s",event.diagnostic.code); }
            if (s->update) {
                bool confirmed=event.has_result && event.result.kind==MD_STATEMENT_OK && event.result.statement_index==1 && !strcmp(event.result.command,"UPDATE") && event.result.affected_rows==1;
                confirmed=confirmed || (event.has_diagnostic && event.diagnostic.committed==MD_YES && event.diagnostic.has_affected_rows && event.diagnostic.affected_rows==1);
                if (confirmed) { s->confirmed=true; strcpy(c->final,marker); }
                else if (event.has_result||!event.has_diagnostic) invalid=true;
            } else if (event.has_result && !exact_result(&event,target,expected)) invalid=true;
            else if (!event.has_result&&!event.has_diagnostic) invalid=true;
            md_event_destroy(&event); continue;
        }
        if (next==MDC_DONE) { done=true; if (!events||events>2||(!failed&&(events!=1||error.exit_code||results!=1))) invalid=true; s->status=invalid?"invalid":failed?"error":"ok"; }
        else { snprintf(s->code,sizeof s->code,"%s",error.code); s->unconfirmed=s->update&&!s->confirmed&&error.execution_unconfirmed; }
        break;
    } else { snprintf(s->code,sizeof s->code,"%s",error.code); s->unconfirmed=s->update&&error.execution_unconfirmed; }
    s->end=now(); if (!done) { mdc_close(*connection); *connection=NULL; }
}
static DWORD WINAPI client_main(void *context) {
    Client *c=context; Run *r=c->run; MdClientOptions options; mdc_options_default(&options); options.port=(uint16_t)r->port; options.host="127.0.0.1";
    MdClient *connection=NULL; MdClientError error={0};
    if (!mdc_connect(&options,&connection,&error)) snprintf(c->connect_code,sizeof c->connect_code,"%s",error.code);
    snprintf(c->final,sizeof c->final,"row-%010u",c->index+1);
    for (unsigned i=0;i<r->warmup;i++) execute(c,&connection,0,i);
    EnterCriticalSection(&r->lock); r->ready++; WakeConditionVariable(&r->ready_condition);
    while (!r->go&&!r->abort) SleepConditionVariableCS(&r->go_condition,&r->lock,INFINITE);
    bool abort=r->abort; LeaveCriticalSection(&r->lock);
    if (!abort) for (unsigned i=0;i<r->requests;i++) execute(c,&connection,1,i);
    mdc_close(connection); EnterCriticalSection(&r->lock); r->finished++; WakeConditionVariable(&r->ready_condition); LeaveCriticalSection(&r->lock); return 0;
}
static void observe_memory(Run *r) {
    if (r->memory_count==MEMORY_CAP) { r->memory_overflow=true; return; }
    Memory *s=&r->memory[r->memory_count++]; PROCESS_MEMORY_COUNTERS_EX v={.cb=sizeof v};
    s->ok=GetProcessMemoryInfo(r->process,(PROCESS_MEMORY_COUNTERS *)&v,sizeof v)!=0; s->tick=now();
    if (s->ok) { s->working_set=v.WorkingSetSize; s->private_bytes=v.PrivateUsage; } else r->valid=false;
}
static int run_main(int argc,char **argv) {
    Run run={.connections=1,.rows=10000,.warmup=20,.requests=40,.valid=true}; Run *r=&run;
    const char *names[]={"--connections","--rows","--warmup","--requests","--port","--server-pid","--workload","--output"}; unsigned seen=0;
    for (int i=1;i<argc;i++) {
        unsigned index=0; while (index<8&&strcmp(argv[i],names[index])) index++;
        if (index==8||(seen&(1u<<index))||i+1==argc) return 2;
        seen|=1u<<index; const char *value=argv[++i]; unsigned *numbers[]={&r->connections,&r->rows,&r->warmup,&r->requests,&r->port,&r->pid};
        if (index<6) { if (!decimal(value,numbers[index])) return 2; } else if (index==6) r->workload=value; else r->output=value;
    }
    if ((seen&240)!=240||r->connections<1||r->connections>32||r->rows<80||r->rows>1000000||r->rows<r->connections||!r->requests||r->warmup>10000||r->requests>10000||
        (size_t)r->connections*(r->warmup+r->requests)>262144||!r->port||r->port>65535||!r->pid||
        (strcmp(r->workload,"id")&&strcmp(r->workload,"scan_key")&&strcmp(r->workload,"mixed"))||(!strcmp(r->workload,"mixed")&&(r->warmup%20||r->requests%20))) return 2;
    int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,r->output,-1,NULL,0); wchar_t *wide=length?malloc((size_t)length*sizeof *wide):NULL;
    if (!wide) return 3; MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,r->output,-1,wide,length); FILE *output=_wfopen(wide,L"wbx"); free(wide); if (!output) return 3;
    LARGE_INTEGER frequency; r->valid=QueryPerformanceFrequency(&frequency)&&frequency.QuadPart>0; if (r->valid) r->frequency=(uint64_t)frequency.QuadPart;
    r->samples=calloc((size_t)r->connections*(r->warmup+r->requests),sizeof *r->samples); r->memory=calloc(MEMORY_CAP,sizeof *r->memory);
    r->process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,r->pid);
    InitializeCriticalSection(&r->lock); InitializeConditionVariable(&r->ready_condition); InitializeConditionVariable(&r->go_condition);
    unsigned started=0; if (!r->samples||!r->memory||!r->process||!r->valid) { r->valid=false; goto finish; }
    for (;started<r->connections;started++) { Client *c=&r->clients[started]; c->run=r; c->index=started; c->thread=CreateThread(NULL,0,client_main,c,0,NULL); if (!c->thread) break; }
    if (started!=r->connections) { EnterCriticalSection(&r->lock); r->abort=true; WakeAllConditionVariable(&r->go_condition); LeaveCriticalSection(&r->lock); r->valid=false; }
    else {
        EnterCriticalSection(&r->lock); while (r->ready!=r->connections) SleepConditionVariableCS(&r->ready_condition,&r->lock,INFINITE);
        observe_memory(r); r->start=now(); r->go=true; WakeAllConditionVariable(&r->go_condition); LeaveCriticalSection(&r->lock);
        for (;;) { observe_memory(r); EnterCriticalSection(&r->lock); bool complete=r->finished==r->connections; LeaveCriticalSection(&r->lock); if (complete) break; Sleep(10); }
    }
    for (unsigned i=0;i<started;i++) { WaitForSingleObject(r->clients[i].thread,INFINITE); CloseHandle(r->clients[i].thread); }
    for (unsigned c=0;c<r->connections;c++) for (unsigned i=0;i<r->requests;i++) { Sample *s=&r->samples[(size_t)c*(r->warmup+r->requests)+r->warmup+i]; if (s->attempted&&s->end>r->end) r->end=s->end; }
    observe_memory(r);
finish:
    if (r->memory_overflow||!r->start||r->end<=r->start) r->valid=false;
    fprintf(output,"{\"version\":1,\"valid\":%s,\"frequency\":%" PRIu64 ",\"start\":%" PRIu64 ",\"end\":%" PRIu64 ",\"connections\":%u,\"rows\":%u,\"warmup\":%u,\"requests\":%u,\"workload\":",r->valid?"true":"false",r->frequency,r->start,r->end,r->connections,r->rows,r->warmup,r->requests); quoted(output,r->workload);
    fprintf(output,",\"memory_overflow\":%s,\"clients\":[",r->memory_overflow?"true":"false");
    for (unsigned c=0;c<r->connections;c++) { fprintf(output,"%s{\"client\":%u,\"connect_code\":",c?",":"",c); quoted(output,r->clients[c].connect_code); fputs(",\"final_payload\":",output); quoted(output,r->clients[c].final); fputc('}',output); }
    fputs("],\"samples\":[",output); size_t total=(size_t)r->connections*(r->warmup+r->requests);
    for (size_t i=0;r->samples&&i<total;i++) { Sample *s=&r->samples[i];
        fprintf(output,"%s{\"client\":%u,\"phase\":%u,\"sequence\":%u,\"update\":%s,\"attempted\":%s,\"begin\":%" PRIu64 ",\"end\":%" PRIu64 ",\"status\":",i?",":"",s->client,s->phase,s->sequence,s->update?"true":"false",s->attempted?"true":"false",s->begin,s->end); quoted(output,s->status?s->status:"skipped"); fputs(",\"code\":",output); quoted(output,s->code); fprintf(output,",\"confirmed\":%s,\"unconfirmed\":%s}",s->confirmed?"true":"false",s->unconfirmed?"true":"false");
    }
    fputs("],\"memory\":[",output); for (size_t i=0;i<r->memory_count;i++) { Memory *s=&r->memory[i]; fprintf(output,"%s{\"tick\":%" PRIu64 ",\"ok\":%s,\"working_set\":%" PRIu64 ",\"private_bytes\":%" PRIu64 "}",i?",":"",s->tick,s->ok?"true":"false",s->working_set,s->private_bytes); }
    fputs("]}\n",output); bool written=!ferror(output)&&!fflush(output); if (fclose(output)) written=false;
    if (r->process) CloseHandle(r->process); DeleteCriticalSection(&r->lock); free(r->samples); free(r->memory); return written&&r->valid?0:3;
}
int wmain(int argc,wchar_t **wide) {
    char **argv=calloc((size_t)argc,sizeof *argv); if (!argv) return 3; int result=3;
    for (int i=0;i<argc;i++) { int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,NULL,0,NULL,NULL); argv[i]=n?malloc((size_t)n):NULL; if (!argv[i]) goto done; WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,argv[i],n,NULL,NULL); }
    result=run_main(argc,argv);
done:
    for (int i=0;i<argc;i++) free(argv[i]); free(argv); return result;
}
