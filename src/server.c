/* select의 연결 집합은 시작할 때 검사한다. reactor만 socket과 연결 목록을 소유한다. */
#define FD_SETSIZE 1024
#include "protocol.h"
#include "internal.h"
#include "format.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define METRIC_CAPACITY 65536u
typedef struct {
    uint64_t id, generation, request, event_index, enqueue, dequeue, finish;
    const char *outcome;
} QueueSample;
typedef struct {
    FILE *output;
    QueueSample *samples;
    size_t count, capacity;
    uint64_t frequency, enqueued, dequeued, removed, rejected, overflow;
    bool valid;
} QueueMetrics;

typedef struct Server Server;
typedef struct Connection Connection;
typedef struct {
    size_t connections, workers, pending, request, frame, event, total;
    uint64_t queue_ms, lock_ms, receive_ms, send_ms, sql_ms, idle_ms;
    const char *schema, *data, *bind;
    const char *metrics;
    uint16_t port;
} Options;
typedef struct {
    Server *server;
    HANDLE thread;
    Connection *done;
    MdEvent event;
    MdNext next;
    const char *stage;
} Worker;
enum { RECEIVING, WAITING, SENDING_EVENT, SENDING_DONE, FINISHING, DRAINING };
struct Connection {
    Connection *next;
    SOCKET socket;
    uint64_t generation;
    uint64_t request_sequence;
    size_t metric_index;
    int state, code;
    LONG cancelled;
    int job_state; /* queue_lock 안에서 0/대기/실행을 읽고 쓴다. */
    bool closed, terminal, discard;
    unsigned char header[5];
    size_t header_used, input_used, input_length;
    char *input, *sql;
    size_t sql_length, sql_cursor;
    MdRequest *request;
    uint64_t idle_start, receive_start, queue_start, send_start, events;
    size_t response, output_length, output_used;
    unsigned char *output;
    unsigned char small[MP_RESERVE];
};
struct Server {
    Options options;
    MdDb *db;
    SOCKET listener, wake_read, wake_write;
    CRITICAL_SECTION queue_lock;
    CONDITION_VARIABLE jobs_ready;
    Connection **queue;
    size_t queued, open_count;
    Worker *workers;
    bool workers_stop, shutting_down;
    Connection *connections;
    uint64_t generation;
    QueueMetrics metrics;
};
static volatile LONG stopping;
static SOCKET control_wake = INVALID_SOCKET;
static void wake(SOCKET socket) { if (socket != INVALID_SOCKET) (void)send(socket,"x",1,0); }
static BOOL WINAPI control(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT) return FALSE;
    InterlockedExchange(&stopping,1);
    wake(control_wake);
    return TRUE;
}
static uint64_t metric_now(QueueMetrics *m) {
    LARGE_INTEGER value;
    if (!QueryPerformanceCounter(&value) || value.QuadPart < 0) { m->valid=false; return 0; }
    return (uint64_t)value.QuadPart;
}
static bool metrics_open(QueueMetrics *m, const char *path) {
    if (!path) return true;
    int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,NULL,0);
    wchar_t *wide=length ? malloc((size_t)length*sizeof *wide) : NULL;
    if (!wide) return false;
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,length);
    m->output=_wfopen(wide,L"wbx"); free(wide);
    if (!m->output) return false;
    m->capacity=METRIC_CAPACITY;
#ifdef MINIDB_TEST_FAULTS
    if (getenv("MINIDB_TEST_METRICS_SMALL")) m->capacity=1;
#endif
    m->samples=calloc(m->capacity,sizeof *m->samples);
    LARGE_INTEGER frequency;
    m->valid=m->samples && QueryPerformanceFrequency(&frequency) && frequency.QuadPart>0;
    if (m->valid) m->frequency=(uint64_t)frequency.QuadPart;
    return m->valid;
}
/* sample 슬롯은 queue_lock 안에서 발행한다. worker는 인덱스만 보관하고 join 뒤 한 번 출력한다. */
static size_t metric_add(Server *s, Connection *c, bool admitted) {
    QueueMetrics *m=&s->metrics;
    if (!m->output) return SIZE_MAX;
    if (admitted) m->enqueued++; else m->rejected++;
    if (m->count==m->capacity) { m->overflow++; m->valid=false; return SIZE_MAX; }
    size_t index=m->count++;
    m->samples[index]=(QueueSample){.id=m->enqueued+m->rejected,.generation=c->generation,
        .request=c->request_sequence,.event_index=c->events+1,.enqueue=metric_now(m),.outcome=admitted ? "pending" : "rejected"};
    return index;
}
static bool metrics_close(QueueMetrics *m) {
    if (!m->output) { free(m->samples); return true; }
    bool valid=m->valid && m->enqueued==m->dequeued+m->removed;
    int result=fprintf(m->output,"{\"version\":1,\"valid\":%s,\"frequency\":%llu,\"capacity\":%zu,\"enqueued\":%llu,\"dequeued\":%llu,\"removed\":%llu,\"rejected\":%llu,\"overflow\":%llu,\"samples\":[",
        valid ? "true" : "false",(unsigned long long)m->frequency,m->capacity,
        (unsigned long long)m->enqueued,(unsigned long long)m->dequeued,(unsigned long long)m->removed,
        (unsigned long long)m->rejected,(unsigned long long)m->overflow);
    for (size_t i=0;i<m->count;i++) {
        QueueSample *v=&m->samples[i];
        if (fprintf(m->output,"%s{\"id\":%llu,\"generation\":%llu,\"request\":%llu,\"event_index\":%llu,\"enqueue\":%llu,\"dequeue\":%llu,\"finish\":%llu,\"outcome\":\"%s\"}",i ? "," : "",
            (unsigned long long)v->id,(unsigned long long)v->generation,(unsigned long long)v->request,(unsigned long long)v->event_index,
            (unsigned long long)v->enqueue,(unsigned long long)v->dequeue,(unsigned long long)v->finish,v->outcome)<0) result=-1;
    }
    if (fputs("]}\n",m->output)==EOF || fflush(m->output)) result=-1;
    if (fclose(m->output)) result=-1;
    free(m->samples); m->output=NULL;
    return valid && result>=0;
}
static bool parse_options(int argc, char **argv, Options *o) {
    *o = (Options){32,4,16,MP_FRAME_DEFAULT,MP_FRAME_DEFAULT,MP_EVENT_DEFAULT,MP_TOTAL_DEFAULT,
        30000,30000,30000,30000,0,60000,NULL,NULL,"127.0.0.1",NULL,0};
    const char *names[] = {"--max-connections","--workers","--pending-jobs","--request-bytes","--frame-bytes",
        "--event-bytes","--response-bytes","--queue-timeout-ms","--lock-timeout-ms","--receive-timeout-ms",
        "--send-timeout-ms","--sql-timeout-ms","--idle-timeout-ms","--schema-dir","--data-dir","--bind","--port","--queue-metrics"};
    uint64_t values[] = {o->connections,o->workers,o->pending,o->request,o->frame,o->event,o->total,
        o->queue_ms,o->lock_ms,o->receive_ms,o->send_ms,o->sql_ms,o->idle_ms};
    unsigned seen = 0;
    for (int i = 1; i < argc; i++) {
        size_t index;
        for (index = 0; index < 18 && strcmp(argv[i],names[index]); index++) {}
        if (index == 18 || (seen & (1u << index)) || i + 1 == argc || !argv[i+1][0]) return false;
        seen |= 1u << index;
        const char *value = argv[++i];
        if (index < 13) { if (!mp_decimal(value,&values[index])) return false; }
        else if (index == 13) o->schema = value;
        else if (index == 14) o->data = value;
        else if (index == 15) o->bind = value;
        else if (index == 17) o->metrics = value;
        else { uint64_t port; if (!mp_decimal(value,&port) || !port || port > 65535) return false; o->port = (uint16_t)port; }
    }
    for (size_t i = 0; i < 7; i++) if (values[i] > SIZE_MAX) return false;
    o->connections=(size_t)values[0]; o->workers=(size_t)values[1]; o->pending=(size_t)values[2];
    o->request=(size_t)values[3]; o->frame=(size_t)values[4]; o->event=(size_t)values[5]; o->total=(size_t)values[6];
    o->queue_ms=values[7]; o->lock_ms=values[8]; o->receive_ms=values[9]; o->send_ms=values[10]; o->sql_ms=values[11]; o->idle_ms=values[12];
    struct in_addr address;
    return o->schema && o->data && o->port && InetPtonA(AF_INET,o->bind,&address) == 1 &&
        o->connections > 0 && o->connections <= FD_SETSIZE - 2 && o->workers > 0 && o->pending > 0 &&
        o->workers <= SIZE_MAX / sizeof(Worker) && o->pending <= SIZE_MAX / sizeof(Connection *) && o->sql_ms <= INT64_MAX && o->lock_ms <= INT64_MAX &&
        mp_limits(o->request,o->frame,o->event,o->total);
}
static MdReadStatus sql_read(void *context, unsigned char *buffer, size_t capacity, size_t *length) {
    Connection *c = context;
    *length = c->sql_length - c->sql_cursor;
    if (*length > capacity) *length = capacity;
    memcpy(buffer,c->sql + c->sql_cursor,*length);
    c->sql_cursor += *length;
    return *length ? MD_READ_DATA : MD_READ_EOF;
}
static MdCancelReason execution_cancel(void *context) {
    Connection *connection = context;
    if (InterlockedCompareExchange(&stopping, 0, 0))
        return MD_CANCEL_SHUTDOWN;
    if (InterlockedCompareExchange(&connection->cancelled, 0, 0))
        return MD_CANCEL_DISCONNECT;
    return MD_CANCEL_NONE;
}
#ifdef MINIDB_TEST_FAULTS
static void execution_observe(const char *point, void *context) {
    (void)context;
    const char *selected = getenv("MINIDB_TEST_SERVER_HOLD_POINT");
    bool matches = selected ? !strcmp(point, selected) :
        !strcmp(point, "gate-read") || !strcmp(point, "gate-write");
    const char *hold = getenv("MINIDB_TEST_SERVER_HOLD_MS");
    uint64_t delay;
    if (matches && hold && mp_decimal(hold, &delay) && delay <= 10000)
        Sleep((DWORD)delay);
}
#endif
static DWORD WINAPI worker_main(void *context) {
    Worker *worker = context;
    Server *s = worker->server;
    for (;;) {
        EnterCriticalSection(&s->queue_lock);
        while ((!s->queued || worker->done) && !s->workers_stop)
            SleepConditionVariableCS(&s->jobs_ready,&s->queue_lock,INFINITE);
        if (s->workers_stop) { LeaveCriticalSection(&s->queue_lock); return 0; }
        Connection *c = s->queue[0];
        memmove(s->queue,s->queue+1,(s->queued-1)*sizeof *s->queue); s->queued--;
        c->job_state = 2;
        if (s->metrics.output) {
            s->metrics.dequeued++;
            if (c->metric_index!=SIZE_MAX) s->metrics.samples[c->metric_index].dequeue=metric_now(&s->metrics);
        }
        LeaveCriticalSection(&s->queue_lock);
        MdEvent event = {0};
        MdNext next = MD_NEXT_STOP;
        const char *stage = NULL;
        if (mp_expired(c->queue_start,s->options.queue_ms)) {
            stage = "queue";
            mp_error_event(&event,"TIMEOUT",stage);
        } else if (!c->request && !md_request_create(s->db, (MdInput){sql_read,c},
                                                    &c->request, &event.diagnostic)) {
            event.has_diagnostic = event.stop = true;
        } else {
            MdExecutionOptions options = {.lock_timeout_ms = s->options.lock_ms,
                .sql_timeout_ms = s->options.sql_ms, .cancel = execution_cancel, .context = c};
#ifdef MINIDB_TEST_FAULTS
            md_test_execution_hook(execution_observe, NULL);
#endif
            next = md_request_next_ex(c->request, &options, &event);
#ifdef MINIDB_TEST_FAULTS
            md_test_execution_hook(NULL, NULL);
#endif
            if (event.has_diagnostic) {
                stage = c->request->cancel_stage;
            }
        }
        EnterCriticalSection(&s->queue_lock);
        if (s->metrics.output && c->metric_index!=SIZE_MAX) {
            QueueSample *sample=&s->metrics.samples[c->metric_index];
            sample->finish=metric_now(&s->metrics);
            sample->outcome=next==MD_NEXT_EOF ? "eof" : event.has_diagnostic ? "error" : "sql";
            if (sample->dequeue<sample->enqueue || sample->finish<sample->dequeue) s->metrics.valid=false;
        }
        worker->event=event; worker->next=next; worker->stage=stage; worker->done=c;
        c->job_state=0;
        LeaveCriticalSection(&s->queue_lock);
        wake(s->wake_write);
    }
}
static bool enqueue(Server *s, Connection *c) {
    bool ok;
    EnterCriticalSection(&s->queue_lock);
    ok = !s->shutting_down && !InterlockedCompareExchange(&stopping,0,0) && s->queued < s->options.pending;
    c->metric_index=metric_add(s,c,ok);
    if (ok) {
        c->queue_start=mp_now(); c->job_state=1; c->state=WAITING;
        s->queue[s->queued++]=c;
        WakeConditionVariable(&s->jobs_ready);
    }
    LeaveCriticalSection(&s->queue_lock);
    return ok;
}
static bool remove_queued(Server *s, Connection *c) {
    bool removed=false;
    EnterCriticalSection(&s->queue_lock);
    for (size_t i=0;i<s->queued;i++) if (s->queue[i]==c) {
        memmove(s->queue+i,s->queue+i+1,(s->queued-i-1)*sizeof *s->queue);
        s->queued--; c->job_state=0; removed=true; break;
    }
    if (removed && s->metrics.output) {
        s->metrics.removed++;
        if (c->metric_index!=SIZE_MAX) {
            QueueSample *sample=&s->metrics.samples[c->metric_index];
            sample->finish=metric_now(&s->metrics); sample->outcome="removed";
        }
    }
    LeaveCriticalSection(&s->queue_lock);
    return removed;
}
static void release_output(Connection *c) {
    if (c->output && c->output != c->small) free(c->output);
    c->output=NULL; c->output_length=c->output_used=0;
}
static void close_connection(Server *s, Connection *c) {
    if (c->closed) return;
    c->closed=true;
    InterlockedExchange(&c->cancelled,1);
    shutdown(c->socket,SD_BOTH); closesocket(c->socket); c->socket=INVALID_SOCKET;
    s->open_count--;
    (void)remove_queued(s,c);
    release_output(c);
}
static bool append_frame(unsigned char *output, size_t capacity, size_t *used, char kind, const char *payload, size_t length) {
    size_t end;
    if (!mp_add(*used,5,capacity,&end) || !mp_add(end,length,capacity,&end)) return false;
    mp_header(output+*used,kind,(uint32_t)length);
    memcpy(output+*used+5,payload,length); *used=end;
    return true;
}
static void send_done(Server *s, Connection *c) {
    char json[256];
    int n=snprintf(json,sizeof json,"{\"version\":1,\"type\":\"done\",\"status\":\"%s\",\"events\":\"%llu\",\"exit_code\":%d}",
        c->code ? (c->terminal ? "stopped" : "error") : "ok",(unsigned long long)c->events,c->code);
    release_output(c);
    if (!mp_add(c->response,(size_t)n,s->options.total,&c->response)) { close_connection(s,c); return; }
    c->output=c->small; c->output_length=0;
    append_frame(c->small,sizeof c->small,&c->output_length,'F',json,(size_t)n);
    c->state=SENDING_DONE; c->send_start=mp_now();
}
static int build_event(Server *s, Connection *c, const char *json, size_t length, bool fallback) {
    char begin[256], end[128];
    int bn=snprintf(begin,sizeof begin,"{\"version\":1,\"type\":\"begin\",\"event_index\":\"%llu\",\"bytes\":\"%llu\"}",
        (unsigned long long)(c->events+1),(unsigned long long)length);
    int en=snprintf(end,sizeof end,"{\"version\":1,\"type\":\"end\",\"event_index\":\"%llu\"}",(unsigned long long)(c->events+1));
    size_t payload, total;
    size_t budget=fallback ? s->options.total : s->options.total-MP_RESERVE;
    if (length > s->options.event || !mp_add(length,(size_t)bn+(size_t)en,SIZE_MAX,&payload) ||
        !mp_add(c->response,payload,budget,&total)) return false;
    size_t chunk=s->options.frame < 65536 ? s->options.frame : 65536;
    size_t frames=length/chunk+(length%chunk!=0)+2;
    size_t wire;
    if (frames > SIZE_MAX/5 || !mp_add(payload,frames*5,SIZE_MAX,&wire)) return false;
    unsigned char *output=fallback ? c->small : malloc(wire);
    if (!output) return -1;
    if (fallback && wire > sizeof c->small) return 0;
    size_t used=0;
    bool ok=append_frame(output,wire,&used,'B',begin,(size_t)bn);
    for (size_t offset=0;ok && offset<length;) {
        size_t take=length-offset; if (take>chunk) take=chunk;
        ok=append_frame(output,wire,&used,'D',json+offset,take); offset+=take;
    }
    ok=ok && append_frame(output,wire,&used,'E',end,(size_t)en);
    if (!ok) { if (!fallback) free(output); return false; }
    release_output(c); c->output=output; c->output_length=used; c->response=total;
    c->events++; c->state=SENDING_EVENT; c->send_start=mp_now();
    return true;
}
static void deliver(Server *s, Connection *c, MdEvent *event, const char *stage) {
    c->terminal=event->stop;
    if (stage && (!strcmp(stage,"protocol") || !strcmp(stage,"receive"))) c->discard=true;
    c->code=mf_combine(c->code,mp_event_exit(event,stage));
    size_t length=0;
    char *json=mp_event_encode(event,stage,&length);
    int built=json ? build_event(s,c,json,length,false) : -1;
    if (built==1) { free(json); md_event_destroy(event); return; }
    bool encoded=built==0;
    free(json);
    /* 반영 후 큰 결과/할당 실패도 작은 고정 버퍼에서 반영·복원·health 사실을 유지한다. */
    MdDiagnostic d=event->diagnostic;
    bool committed=event->has_result && event->result.kind==MD_STATEMENT_OK;
    uint64_t statement=event->has_result ? event->result.statement_index : d.statement_index;
    uint64_t affected=committed ? event->result.affected_rows : d.affected_rows;
    if (!event->has_diagnostic) {
        memset(&d,0,sizeof d);
        strcpy(d.code,encoded ? "LIMIT_EXCEEDED" : "RESOURCE_ERROR"); strcpy(d.phase,"execute");
    }
    if (committed) { d.committed=MD_YES; d.has_affected_rows=true; d.affected_rows=affected; }
    char result[256]="null", index[32]="null", affected_text[32]="null";
    if (committed) snprintf(result,sizeof result,"{\"version\":1,\"type\":\"statement_ok\",\"statement_index\":\"%llu\",\"command\":\"%s\",\"affected_rows\":\"%llu\",\"committed\":true}",
        (unsigned long long)statement,event->result.command,(unsigned long long)affected);
    if (statement) snprintf(index,sizeof index,"\"%llu\"",(unsigned long long)statement);
    if (d.has_affected_rows) snprintf(affected_text,sizeof affected_text,"\"%llu\"",(unsigned long long)d.affected_rows);
    const char *rollback=d.rollback==MD_ROLLBACK_FAILED ? "failed" : d.rollback==MD_ROLLBACK_UNKNOWN ? "unknown" : d.rollback==MD_ROLLBACK_CONFIRMED ? "confirmed" : "not_needed";
    char fallback[4096];
    int n=snprintf(fallback,sizeof fallback,
        "{\"version\":1,\"result\":%s,\"diagnostic\":{\"version\":1,\"type\":\"diagnostic\",\"code\":\"%s\",\"message\":\"Response delivery limit or allocation failure\",\"operation\":\"run\",\"phase\":\"%s\",\"source\":null,\"path\":null,\"statement_index\":%s,\"line\":null,\"column\":null,\"field_path\":null,\"byte_offset\":null,\"committed\":%s,\"rollback\":\"%s\",\"command\":null,\"affected_rows\":%s,\"output_ready\":%s},\"stop\":true,\"health\":\"%s\",\"stage\":\"send\"}",
        result,d.code,d.phase,index,d.committed==MD_YES ? "true" : d.committed==MD_NO ? "false" : "null",rollback,affected_text,
        d.output_ready==MD_YES ? "true" : d.output_ready==MD_NO ? "false" : "null",
        event->health==MD_RECOVERY_REQUIRED ? "recovery_required" : event->health==MD_STOPPED_CLEANUP ? "stopped_cleanup" : "healthy");
    c->terminal=true; c->code=mf_combine(c->code,8);
    md_event_destroy(event);
    if (n<0 || (size_t)n>=sizeof fallback || build_event(s,c,fallback,(size_t)n,true)!=1) close_connection(s,c);
}
static void operation_error(Server *s, Connection *c, const char *code, const char *stage) {
    MdEvent event; mp_error_event(&event,code,stage); deliver(s,c,&event,stage);
}
static void submit(Server *s, Connection *c) {
    if (!enqueue(s,c)) operation_error(s,c,s->shutting_down ? "SERVER_STOPPING" : "SERVER_BUSY",s->shutting_down ? "shutdown" : "queue");
}
static void receive_request(Server *s, Connection *c) {
    for (;;) {
        if (c->header_used<5) {
            int n=recv(c->socket,(char *)c->header+c->header_used,(int)(5-c->header_used),0);
            if (n==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK) return;
            if (n<=0) { close_connection(s,c); return; }
            if (!c->header_used) c->receive_start=mp_now();
            c->header_used+=(size_t)n;
            if (c->header_used<5) continue;
            c->input_length=mp_length(c->header);
            if (c->header[0]!='Q' || !c->input_length) {
                operation_error(s,c,"PROTOCOL_ERROR","protocol"); return;
            }
            if (c->input_length>s->options.request || c->input_length>s->options.frame) {
                operation_error(s,c,"LIMIT_EXCEEDED","protocol"); return;
            }
            c->input=malloc(c->input_length+1);
            if (!c->input) { operation_error(s,c,"RESOURCE_ERROR","receive"); return; }
        }
        int take=(int)(c->input_length-c->input_used > INT_MAX ? INT_MAX : c->input_length-c->input_used);
        int n=recv(c->socket,c->input+c->input_used,take,0);
        if (n==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK) return;
        if (n<=0) { close_connection(s,c); return; }
        c->input_used+=(size_t)n;
        if (c->input_used<c->input_length) continue;
        c->input[c->input_length]=0;
        bool ok=mp_request(c->input,c->input_length,&c->sql,&c->sql_length);
        free(c->input); c->input=NULL;
        if (!ok) { operation_error(s,c,"PROTOCOL_ERROR","protocol"); return; }
        c->request_sequence++;
        submit(s,c); return;
    }
}
static void send_output(Server *s, Connection *c) {
    while (c->output_used<c->output_length) {
        size_t remaining=c->output_length-c->output_used;
        int n=send(c->socket,(char *)c->output+c->output_used,(int)(remaining>65536 ? 65536 : remaining),0);
        if (n==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK) return;
        if (n<=0) { close_connection(s,c); return; }
        c->output_used+=(size_t)n;
        if (c->output_used<c->output_length) return;
    }
    int state=c->state; release_output(c);
    if (state==SENDING_EVENT) {
        if (!c->terminal && (s->shutting_down || InterlockedCompareExchange(&stopping,0,0))) {
            operation_error(s,c,"SERVER_STOPPING","shutdown"); return;
        }
        if (c->terminal) send_done(s,c); else submit(s,c);
    } else if (c->discard) {
        /* 거절한 Q의 잔여 payload를 고정 버퍼로 비워 오류 응답을 TCP reset으로 잃지 않는다. */
        shutdown(c->socket,SD_SEND);
        c->state=DRAINING; c->send_start=mp_now();
        if (s->shutting_down) close_connection(s,c);
    } else c->state=FINISHING;
}
static bool cleanup_request(Server *s, Connection *c) {
    EnterCriticalSection(&s->queue_lock);
    bool busy=c->job_state!=0;
    for (size_t i=0;i<s->options.workers;i++) if (s->workers[i].done==c) busy=true;
    LeaveCriticalSection(&s->queue_lock);
    if (busy) return false;
    md_request_close(c->request); c->request=NULL;
    free(c->sql); c->sql=NULL; c->sql_length=c->sql_cursor=0;
    free(c->input); c->input=NULL;
    return true;
}
static void consume_completions(Server *s) {
    for (size_t i=0;i<s->options.workers;i++) {
        Worker *w=&s->workers[i];
        EnterCriticalSection(&s->queue_lock);
        Connection *c=w->done;
        MdEvent event=w->event; MdNext next=w->next; const char *stage=w->stage;
        if (c) { w->done=NULL; memset(&w->event,0,sizeof w->event); WakeAllConditionVariable(&s->jobs_ready); }
        LeaveCriticalSection(&s->queue_lock);
        if (!c) continue;
        if (c->closed) md_event_destroy(&event);
        else if (next==MD_NEXT_EOF) { md_event_destroy(&event); send_done(s,c); }
        else deliver(s,c,&event,stage);
    }
}
static bool setup_wake(Server *s) {
    s->wake_read=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    s->wake_write=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    int length=sizeof address;
    return s->wake_read!=INVALID_SOCKET && s->wake_write!=INVALID_SOCKET &&
        !bind(s->wake_read,(struct sockaddr *)&address,sizeof address) && !getsockname(s->wake_read,(struct sockaddr *)&address,&length) &&
        !connect(s->wake_write,(struct sockaddr *)&address,sizeof address) && mp_socket_nonblocking(s->wake_read) && mp_socket_nonblocking(s->wake_write);
}
static void accept_connections(Server *s) {
    for (;;) {
        SOCKET socket=accept(s->listener,NULL,NULL);
        if (socket==INVALID_SOCKET) return;
        if (s->open_count>=s->options.connections || !mp_socket_nonblocking(socket)) { closesocket(socket); continue; }
#ifdef MINIDB_TEST_FAULTS
        const char *buffer=getenv("MINIDB_TEST_SEND_BUFFER");
        uint64_t size;
        if (buffer && mp_decimal(buffer,&size) && size<=INT_MAX) {
            int bytes=(int)size; setsockopt(socket,SOL_SOCKET,SO_SNDBUF,(char *)&bytes,sizeof bytes);
        }
#endif
        Connection *c=calloc(1,sizeof *c);
        if (!c) { closesocket(socket); continue; }
        c->socket=socket; c->generation=++s->generation; c->idle_start=mp_now();
        c->next=s->connections; s->connections=c; s->open_count++;
    }
}
static int reactor(Server *s) {
    for (;;) {
        if (InterlockedCompareExchange(&stopping,0,0) && !s->shutting_down) {
            s->shutting_down=true;
            closesocket(s->listener); s->listener=INVALID_SOCKET;
            for (Connection *c=s->connections;c;c=c->next) {
                InterlockedExchange(&c->cancelled,1);
                if (c->state==RECEIVING || c->state==FINISHING || c->state==DRAINING) close_connection(s,c);
                else if (remove_queued(s,c)) operation_error(s,c,"SERVER_STOPPING","shutdown");
            }
        }
        consume_completions(s);
        Connection **link=&s->connections;
        while (*link) {
            Connection *c=*link;
            if (c->closed && cleanup_request(s,c)) { *link=c->next; free(c); continue; }
            if (!c->closed && c->state==FINISHING && cleanup_request(s,c)) {
                if (s->shutting_down || c->discard) close_connection(s,c);
                else {
                    c->state=RECEIVING; c->header_used=c->input_used=c->input_length=c->response=0;
                    c->events=0; c->code=0; c->terminal=false; c->idle_start=mp_now();
                }
            }
            if (!c->closed && c->state==RECEIVING) {
                if (!c->header_used && mp_expired(c->idle_start,s->options.idle_ms)) close_connection(s,c);
                else if (c->header_used && mp_expired(c->receive_start,s->options.receive_ms)) operation_error(s,c,"TIMEOUT","receive");
            }
            if (!c->closed && c->state==WAITING && mp_expired(c->queue_start,s->options.queue_ms) && remove_queued(s,c))
                operation_error(s,c,"TIMEOUT","queue");
            if (!c->closed && (c->state==SENDING_EVENT || c->state==SENDING_DONE || c->state==DRAINING) && mp_expired(c->send_start,s->options.send_ms)) close_connection(s,c);
            link=&c->next;
        }
        if (s->shutting_down && !s->connections) return 0;
        fd_set reads,writes; FD_ZERO(&reads); FD_ZERO(&writes);
        FD_SET(s->wake_read,&reads);
        if (s->listener!=INVALID_SOCKET) FD_SET(s->listener,&reads);
        for (Connection *c=s->connections;c;c=c->next) if (!c->closed) {
            if (c->state==DRAINING || (c->state!=FINISHING && !c->discard)) FD_SET(c->socket,&reads);
            if (c->state==SENDING_EVENT || c->state==SENDING_DONE) FD_SET(c->socket,&writes);
        }
        struct timeval tv={0,10000};
        int ready=select(0,&reads,&writes,NULL,&tv);
        if (ready==SOCKET_ERROR) { InterlockedExchange(&stopping,1); continue; }
        if (FD_ISSET(s->wake_read,&reads)) { char bytes[256]; while (recv(s->wake_read,bytes,sizeof bytes,0)>0) {} }
        if (s->listener!=INVALID_SOCKET && FD_ISSET(s->listener,&reads)) accept_connections(s);
        for (Connection *c=s->connections;c;c=c->next) if (!c->closed) {
            if (FD_ISSET(c->socket,&reads)) {
                if (c->state==RECEIVING) receive_request(s,c);
                else if (c->state==DRAINING) {
                    char discard[4096];
                    int n=recv(c->socket,discard,sizeof discard,0);
                    if (n<=0) {
                        if (!n || WSAGetLastError()!=WSAEWOULDBLOCK) close_connection(s,c);
                    } else {
                        c->input_used+=(size_t)n;
                        if (c->input_used>=c->input_length) close_connection(s,c);
                    }
                } else {
                    char byte; int n=recv(c->socket,&byte,1,MSG_PEEK);
                    /* pipelining 검출도 후속 SQL 실행을 막는다. */
                    if (n==0 || n>0 || (n==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK)) close_connection(s,c);
                }
            }
            if (!c->closed && FD_ISSET(c->socket,&writes)) send_output(s,c);
        }
    }
}
static int server_main(int argc, char **argv) {
    Server server={0}; Server *s=&server;
    s->listener=s->wake_read=s->wake_write=INVALID_SOCKET;
    if (!parse_options(argc,argv,&s->options)) { fputs("ARGUMENT_ERROR invalid server options\n",stderr); return 2; }
    MdDiagnostic diagnostic;
    if (!md_open(s->options.schema,s->options.data,&s->db,&diagnostic)) { mf_diagnostic(&diagnostic,"run",true); return 3; }
    WSADATA ws;
    if (WSAStartup(MAKEWORD(2,2),&ws)) { md_close(s->db); return 3; }
    if (!metrics_open(&s->metrics,s->options.metrics)) {
        (void)metrics_close(&s->metrics); WSACleanup(); md_close(s->db);
        fputs("IO_ERROR or RESOURCE_ERROR queue metrics preparation failed\n",stderr); return 3;
    }
    InitializeCriticalSection(&s->queue_lock); InitializeConditionVariable(&s->jobs_ready);
    s->queue=calloc(s->options.pending,sizeof *s->queue); s->workers=calloc(s->options.workers,sizeof *s->workers);
    size_t started=0;
    int result=3;
    if (!s->queue || !s->workers || !setup_wake(s)) goto done;
    for (;started<s->options.workers;started++) {
        s->workers[started].server=s;
        s->workers[started].thread=CreateThread(NULL,0,worker_main,&s->workers[started],0,NULL);
        if (!s->workers[started].thread) goto done;
    }
    s->listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(s->options.port)};
    InetPtonA(AF_INET,s->options.bind,&address.sin_addr);
    if (s->listener==INVALID_SOCKET || bind(s->listener,(struct sockaddr *)&address,sizeof address) ||
        listen(s->listener,SOMAXCONN) || !mp_socket_nonblocking(s->listener)) goto done;
    control_wake=s->wake_write; SetConsoleCtrlHandler(control,TRUE);
    result=reactor(s);
    SetConsoleCtrlHandler(control,FALSE); control_wake=INVALID_SOCKET;
done:
    EnterCriticalSection(&s->queue_lock); s->workers_stop=true; WakeAllConditionVariable(&s->jobs_ready); LeaveCriticalSection(&s->queue_lock);
    for (size_t i=0;i<started;i++) { WaitForSingleObject(s->workers[i].thread,INFINITE); CloseHandle(s->workers[i].thread); }
    if (s->listener!=INVALID_SOCKET) closesocket(s->listener);
    if (s->wake_read!=INVALID_SOCKET) closesocket(s->wake_read);
    if (s->wake_write!=INVALID_SOCKET) closesocket(s->wake_write);
    free(s->workers); free(s->queue);
    DeleteCriticalSection(&s->queue_lock);
    WSACleanup(); md_close(s->db);
    if (!metrics_close(&s->metrics)) result=3;
    if (result) fputs("IO_ERROR or RESOURCE_ERROR server preparation failed\n",stderr);
    return result;
}
int wmain(int argc, wchar_t **wide) {
    char **argv=calloc((size_t)argc,sizeof *argv);
    if (!argv) return 3;
    int result=3;
    for (int i=0;i<argc;i++) {
        int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,NULL,0,NULL,NULL);
        argv[i]=n ? malloc((size_t)n) : NULL;
        if (!argv[i]) goto done;
        WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,argv[i],n,NULL,NULL);
    }
    result=server_main(argc,argv);
done:
    for (int i=0;i<argc;i++) free(argv[i]); free(argv);
    return result;
}
