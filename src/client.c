#include "protocol.h"
#include "format.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct MdClient {
    SOCKET socket;
    MdClientOptions options;
    bool active, broken, submitted, last_stop;
    int confirmed_code;
    uint64_t events, last_statement;
    size_t response;
};
void mdc_options_default(MdClientOptions *o) {
    *o = (MdClientOptions){"127.0.0.1",0,5000,30000,30000,MP_FRAME_DEFAULT,MP_FRAME_DEFAULT,MP_EVENT_DEFAULT,MP_TOTAL_DEFAULT};
}
static bool error(MdClient *c, MdClientError *e, const char *code, const char *message, bool fatal) {
    if (e) {
        memset(e,0,sizeof *e);
        snprintf(e->code,sizeof e->code,"%s",code);
        snprintf(e->message,sizeof e->message,"%s",message);
        e->last_statement_index = c ? c->last_statement : 0;
        e->execution_unconfirmed = c && c->submitted;
        e->exit_code = 8;
    }
    if (fatal && c) { c->broken = true; shutdown(c->socket,SD_BOTH); }
    return false;
}
static bool wait_socket(MdClient *c, bool writing, uint64_t start, uint64_t timeout, MdClientError *e) {
    for (;;) {
        if (mp_expired(start,timeout)) return error(c,e,"TIMEOUT","Socket interval deadline exceeded",true);
        fd_set set, failures; FD_ZERO(&set); FD_SET(c->socket,&set);
        FD_ZERO(&failures); FD_SET(c->socket,&failures);
        uint64_t remaining = timeout ? timeout - (mp_now() - start) : 1000;
        if (remaining > 1000) remaining = 1000;
        struct timeval tv = {(long)(remaining / 1000),(long)(remaining % 1000 * 1000)};
        int rc = select(0,writing ? NULL : &set,writing ? &set : NULL,&failures,&tv);
        if (rc > 0) return true;
        if (rc == SOCKET_ERROR) return error(c,e,"IO_ERROR","Socket wait failed",true);
    }
}
static bool transfer(MdClient *c, void *data, size_t length, bool writing, uint64_t start, uint64_t timeout, MdClientError *e) {
    char *cursor = data;
    while (length) {
        if (!wait_socket(c,writing,start,timeout,e)) return false;
        int take = (int)(length > 65536 ? 65536 : length);
        int n = writing ? send(c->socket,cursor,take,0) : recv(c->socket,cursor,take,0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (n <= 0) return error(c,e,"IO_ERROR",writing ? "Request send failed" : "Incomplete response or missing done",true);
        cursor += n; length -= (size_t)n;
    }
    return true;
}
bool mdc_connect(const MdClientOptions *o, MdClient **out, MdClientError *e) {
    if (out) *out = NULL;
    if (e) memset(e,0,sizeof *e);
    struct in_addr parsed;
    if (!o || !out || !o->host || !o->port || InetPtonA(AF_INET,o->host,&parsed)!=1 ||
        !mp_limits(o->request_bytes,o->frame_bytes,o->event_bytes,o->response_bytes))
        return error(NULL,e,"ARGUMENT_ERROR","Invalid client options",false);
    WSADATA ws;
    if (WSAStartup(MAKEWORD(2,2),&ws)) return error(NULL,e,"IO_ERROR","Winsock initialization failed",false);
    MdClient *c = calloc(1,sizeof *c);
    if (!c) { WSACleanup(); return error(NULL,e,"RESOURCE_ERROR","Client allocation failed",false); }
    c->options = *o;
    c->socket = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    struct sockaddr_in address = {.sin_family=AF_INET,.sin_port=htons(o->port)};
    bool ok = c->socket != INVALID_SOCKET && InetPtonA(AF_INET,o->host,&address.sin_addr) == 1 && mp_socket_nonblocking(c->socket);
    uint64_t start = mp_now();
    if (ok && connect(c->socket,(struct sockaddr *)&address,sizeof address) == SOCKET_ERROR) {
        int code = WSAGetLastError();
        ok = code == WSAEWOULDBLOCK && wait_socket(c,true,start,o->connect_timeout_ms,e);
        int socket_error = 0, size = sizeof socket_error;
        if (ok) ok = !getsockopt(c->socket,SOL_SOCKET,SO_ERROR,(char *)&socket_error,&size) && !socket_error;
    }
    if (!ok) {
        if (!e || !e->code[0]) error(c,e,"IO_ERROR","Connect failed",false);
        mdc_close(c); return false;
    }
    *out = c;
    return true;
}
bool mdc_execute(MdClient *c, const unsigned char *sql, size_t length, MdClientError *e) {
    if (e) memset(e,0,sizeof *e);
    if (!c || c->broken || c->active) return error(c,e,"STATE_ERROR","Execute requires an idle live connection",false);
    if ((!sql && length) || length > c->options.request_bytes) return error(c,e,"LIMIT_EXCEEDED","SQL exceeds request limit",false);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return error(c,e,"RESOURCE_ERROR","Request allocation failed",false);
    yyjson_mut_val *root = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc,root);
    bool ok = root && yyjson_mut_obj_add_uint(doc,root,"version",1) && yyjson_mut_obj_add_str(doc,root,"type","execute") &&
        yyjson_mut_obj_add_strncpy(doc,root,"sql",sql ? (const char *)sql : "",length);
    size_t n = 0;
    char *json = ok ? yyjson_mut_write(doc,0,&n) : NULL;
    yyjson_mut_doc_free(doc);
    if (!json) return error(c,e,"RESOURCE_ERROR","Request serialization failed",false);
    if (n > c->options.request_bytes || n > c->options.frame_bytes) { free(json); return error(c,e,"LIMIT_EXCEEDED","Q payload exceeds request limit",false); }
    unsigned char header[5]; mp_header(header,'Q',(uint32_t)n);
    c->submitted = c->active = true; c->events = c->last_statement = 0; c->response = 0; c->last_stop = false; c->confirmed_code = 0;
    uint64_t start = mp_now();
    ok = transfer(c,header,5,true,start,c->options.send_timeout_ms,e) && transfer(c,json,n,true,start,c->options.send_timeout_ms,e);
    free(json); return ok;
}
static bool receive_frame(MdClient *c, char *kind, char **payload, size_t *length, MdClientError *e) {
    unsigned char header[5];
    uint64_t start = mp_now();
    *payload = NULL;
    if (!transfer(c,header,5,false,start,c->options.receive_timeout_ms,e)) return false;
    *kind = (char)header[0]; *length = mp_length(header);
    if ((*kind != 'B' && *kind != 'D' && *kind != 'E' && *kind != 'F') ||
        *length > c->options.frame_bytes ||
        !mp_add(c->response,*length,c->options.response_bytes,&c->response))
        return error(c,e,"PROTOCOL_ERROR","Invalid response frame or response limit",true);
    *payload = malloc(*length + 1);
    if (!*payload) return error(c,e,"RESOURCE_ERROR","Frame allocation failed",true);
    bool ok = transfer(c,*payload,*length,false,start,c->options.receive_timeout_ms,e);
    (*payload)[*length] = 0;
    if (!ok) { free(*payload); *payload = NULL; }
    return ok;
}
MdClientNext mdc_next(MdClient *c, MdEvent *event, MdClientError *e) {
    if (e) memset(e,0,sizeof *e);
    if (event) memset(event,0,sizeof *event);
    if (!c || !event || !c->active || c->broken) {
        error(c,e,"STATE_ERROR","Next requires an active live request",false);
        return MDC_ERROR;
    }
    char kind, *payload = NULL;
    size_t length;
    if (!receive_frame(c,&kind,&payload,&length,e)) return MDC_ERROR;
    yyjson_doc *doc = mp_json(payload,length);
    yyjson_val *root = yyjson_doc_get_root(doc);
    bool version = yyjson_is_uint(yyjson_obj_get(root,"version")) && yyjson_get_uint(yyjson_obj_get(root,"version")) == 1;
    if (kind == 'F') {
        const char *keys[] = {"version","type","status","events","exit_code"};
        uint64_t events;
        yyjson_val *status = yyjson_obj_get(root,"status"), *exit = yyjson_obj_get(root,"exit_code");
        bool ok = version && mp_fields(root,keys,5) && mp_string(yyjson_obj_get(root,"type"),"done") &&
            (mp_string(status,"ok") || mp_string(status,"error") || mp_string(status,"stopped")) &&
            mp_u64(yyjson_obj_get(root,"events"),&events) && events == c->events && yyjson_is_uint(exit);
        uint64_t code = yyjson_get_uint(exit);
        ok = ok && (code == 0 || code == 4 || code == 5 || code == 6 || code == 8) &&
            (mp_string(status,"ok") == (code == 0)) && code == (uint64_t)c->confirmed_code;
        yyjson_doc_free(doc); free(payload);
        if (!ok) { error(c,e,"PROTOCOL_ERROR","Invalid request done",true); return MDC_ERROR; }
        c->active = c->submitted = false;
        if (e) { e->exit_code = (int)code; e->last_statement_index = c->last_statement; }
        return MDC_DONE;
    }
    const char *keys[] = {"version","type","event_index","bytes"};
    uint64_t index = 0, bytes = 0;
    bool ok = kind == 'B' && !c->last_stop && version && mp_fields(root,keys,4) && mp_string(yyjson_obj_get(root,"type"),"begin") &&
        mp_u64(yyjson_obj_get(root,"event_index"),&index) && c->events != UINT64_MAX && index == c->events + 1 &&
        mp_u64(yyjson_obj_get(root,"bytes"),&bytes) && bytes > 0 && bytes <= c->options.event_bytes && bytes < SIZE_MAX;
    yyjson_doc_free(doc); free(payload);
    if (!ok) { error(c,e,"PROTOCOL_ERROR","Invalid event begin or event limit",true); return MDC_ERROR; }
    char *json = malloc((size_t)bytes + 1);
    if (!json) { error(c,e,"RESOURCE_ERROR","Event allocation failed",true); return MDC_ERROR; }
    size_t used = 0;
    for (;;) {
        if (!receive_frame(c,&kind,&payload,&length,e)) { free(json); return MDC_ERROR; }
        if (kind == 'D') {
            if (!length || length > (size_t)bytes - used) { free(payload); break; }
            memcpy(json + used,payload,length); used += length; free(payload);
            continue;
        }
        if (kind == 'E' && used == bytes) {
            doc = mp_json(payload,length); root = yyjson_doc_get_root(doc);
            const char *end_keys[] = {"version","type","event_index"};
            uint64_t end_index;
            ok = mp_fields(root,end_keys,3) && yyjson_is_uint(yyjson_obj_get(root,"version")) &&
                yyjson_get_uint(yyjson_obj_get(root,"version")) == 1 && mp_string(yyjson_obj_get(root,"type"),"end") &&
                mp_u64(yyjson_obj_get(root,"event_index"),&end_index) && end_index == index;
            yyjson_doc_free(doc); free(payload); json[used] = 0;
            char stage_name[16];
            /* strict decoder가 sql 실행 기한도 검증한다. 모르는 stage의 부분 이벤트는
             * 폐기하고 이미 반환한 이벤트/미확인 반영 상태를 그대로 보존한다. */
            if (!ok || !mp_event_decode(json,used,event,stage_name)) break;
            c->confirmed_code=mf_combine(c->confirmed_code,mp_event_exit(event,stage_name[0] ? stage_name : NULL));
            if (e) snprintf(e->stage,sizeof e->stage,"%s",stage_name);
            c->last_stop=event->stop;
            free(json); c->events++;
            if (event->has_result) c->last_statement = event->result.statement_index;
            if (event->has_diagnostic && event->diagnostic.has_statement) c->last_statement = event->diagnostic.statement_index;
            return MDC_EVENT;
        }
        free(payload); break;
    }
    free(json); error(c,e,"PROTOCOL_ERROR","Invalid event chunks or typed event",true);
    return MDC_ERROR;
}
void mdc_close(MdClient *c) {
    if (!c) return;
    if (c->socket != INVALID_SOCKET) { shutdown(c->socket,SD_BOTH); closesocket(c->socket); }
    free(c); WSACleanup();
}
