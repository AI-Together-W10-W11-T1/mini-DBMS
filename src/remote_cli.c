#include "protocol.h"
#include "format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int local_error(const char *code, const char *phase, const char *message, bool json, int exit_code) {
    MdDiagnostic diagnostic={0};
    snprintf(diagnostic.code,sizeof diagnostic.code,"%s",code);
    snprintf(diagnostic.phase,sizeof diagnostic.phase,"%s",phase);
    snprintf(diagnostic.message,sizeof diagnostic.message,"%s",message);
    mf_diagnostic(&diagnostic,"run",json);
    return exit_code;
}
static int remote_main(int argc, char **argv) {
    MdClientOptions options; mdc_options_default(&options);
    const char *file=NULL; bool stdin_flag=false, json=false, host_seen=false;
    const char *names[]={"--host","--port","--file","--stdin","--format","--connect-timeout-ms","--receive-timeout-ms",
        "--send-timeout-ms","--request-bytes","--frame-bytes","--event-bytes","--response-bytes"};
    unsigned seen=0;
    for (int i=1;i<argc;i++) {
        size_t index; for (index=0;index<12 && strcmp(argv[i],names[index]);index++) {}
        if (index==12 || (seen&(1u<<index))) goto arguments;
        seen|=1u<<index;
        if (index==3) { stdin_flag=true; continue; }
        if (++i==argc || !argv[i][0]) goto arguments;
        const char *value=argv[i]; uint64_t n;
        if (index==0) { options.host=value; host_seen=true; }
        else if (index==2) file=value;
        else if (index==4) { if (!strcmp(value,"jsonl")) json=true; else if (strcmp(value,"table")) goto arguments; }
        else {
            if (!mp_decimal(value,&n)) goto arguments;
            if (index==1) { if (!n || n>65535) goto arguments; options.port=(uint16_t)n; }
            else if (index==5) options.connect_timeout_ms=n;
            else if (index==6) options.receive_timeout_ms=n;
            else if (index==7) options.send_timeout_ms=n;
            else {
                if (n>SIZE_MAX) goto arguments;
                if (index==8) options.request_bytes=(size_t)n;
                if (index==9) options.frame_bytes=(size_t)n;
                if (index==10) options.event_bytes=(size_t)n;
                if (index==11) options.response_bytes=(size_t)n;
            }
        }
    }
    struct in_addr address;
    if (!host_seen || !options.port || InetPtonA(AF_INET,options.host,&address)!=1 || (file!=NULL)==stdin_flag ||
        !mp_limits(options.request_bytes,options.frame_bytes,options.event_bytes,options.response_bytes)) goto arguments;
    HANDLE input=GetStdHandle(STD_INPUT_HANDLE);
    if (file) {
        int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,file,-1,NULL,0);
        wchar_t *wide=n ? malloc((size_t)n*sizeof *wide) : NULL;
        if (wide) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,file,-1,wide,n);
        input=wide ? CreateFileW(wide,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL) : INVALID_HANDLE_VALUE;
        free(wide);
        if (input==INVALID_HANDLE_VALUE) return local_error("IO_ERROR","prepare","Cannot open SQL input",json,3);
    }
    /* stdin도 EOF까지 전체 수집한다. capacity는 요청 상한을 넘기지 않는다. */
    size_t length=0, capacity=options.request_bytes<4096 ? options.request_bytes : 4096;
    unsigned char *sql=malloc(capacity ? capacity : 1);
    int code=0;
    if (!sql) code=local_error("RESOURCE_ERROR","prepare","Input allocation failed",json,3);
    while (!code) {
        unsigned char buffer[4096]; DWORD got=0;
        if (!ReadFile(input,buffer,sizeof buffer,&got,NULL)) {
            if (GetLastError()==ERROR_BROKEN_PIPE) break;
            code=local_error("IO_ERROR","prepare","SQL input read failed",json,3); break;
        }
        if (!got) break;
        size_t needed;
        if (!mp_add(length,got,options.request_bytes,&needed)) { code=local_error("LIMIT_EXCEEDED","prepare","SQL input limit exceeded",json,8); break; }
        if (needed>capacity) {
            size_t grown=capacity;
            while (grown<needed) grown=grown>options.request_bytes/2 ? options.request_bytes : grown*2;
            unsigned char *next=realloc(sql,grown);
            if (!next) { code=local_error("RESOURCE_ERROR","prepare","SQL input allocation failed",json,3); break; }
            sql=next; capacity=grown;
        }
        memcpy(sql+length,buffer,got); length=needed;
    }
    if (file) CloseHandle(input);
    if (code) { free(sql); return code; }
    MdClient *client=NULL; MdClientError error;
    if (!mdc_connect(&options,&client,&error)) {
        free(sql); return local_error(error.code,"prepare",error.message,json,!strcmp(error.code,"ARGUMENT_ERROR") ? 2 : 8);
    }
    if (!mdc_execute(client,sql,length,&error)) code=8;
    free(sql);
    if (!code) for (;;) {
        MdEvent event;
        MdClientNext next=mdc_next(client,&event,&error);
        if (next==MDC_ERROR) { code=mf_combine(code,8); break; }
        if (next==MDC_DONE) { code=mf_combine(code,error.exit_code); break; }
        if (event.has_diagnostic) {
            mf_diagnostic(&event.diagnostic,"run",json);
            code=mf_combine(code,mf_diagnostic_exit(&event.diagnostic,false));
        }
        bool output_ok=true;
        if (event.has_result) {
            Buffer buffer; mf_begin(&buffer);
            output_ok=mf_render(&event.result,json,&buffer) && mf_stream_write(STD_OUTPUT_HANDLE,buffer.p,buffer.n);
            mf_end(&buffer);
        }
        if (!output_ok) {
            MdDiagnostic output={0};
            strcpy(output.code,"IO_ERROR"); strcpy(output.phase,"output");
            strcpy(output.source,"stdout"); strcpy(output.message,"stdout write failed");
            output.has_statement=true; output.statement_index=event.result.statement_index;
            if (event.result.kind==MD_STATEMENT_OK) {
                output.committed=MD_YES; output.has_affected_rows=true;
                output.affected_rows=event.result.affected_rows;
                strcpy(output.command,event.result.command);
            }
            mf_diagnostic(&output,"run",json);
            code=mf_combine(code,7);
            md_event_destroy(&event);
            break;
        }
        md_event_destroy(&event);
    }
    if (error.code[0]) {
        char message[256];
        snprintf(message,sizeof message,"%.160s; last_statement=%llu execution_unconfirmed=%s",error.message,
            (unsigned long long)error.last_statement_index,error.execution_unconfirmed ? "true" : "false");
        code=mf_combine(code,local_error(error.code,"execute",message,json,8));
    }
    mdc_close(client); return code;
arguments:
    return local_error("ARGUMENT_ERROR","arguments","Invalid remote arguments",json,2);
}
int wmain(int argc, wchar_t **wide) {
    char **argv=calloc((size_t)argc,sizeof *argv);
    if (!argv) return 3;
    int code=3;
    for (int i=0;i<argc;i++) {
        int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,NULL,0,NULL,NULL);
        argv[i]=n ? malloc((size_t)n) : NULL;
        if (!argv[i]) goto done;
        WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide[i],-1,argv[i],n,NULL,NULL);
    }
    code=remote_main(argc,argv);
done:
    for (int i=0;i<argc;i++) free(argv[i]); free(argv);
    return code;
}
