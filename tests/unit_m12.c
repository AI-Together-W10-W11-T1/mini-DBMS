#include "protocol.h"
#include "internal.h"
#include "minidb_client.h"
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static MdReadStatus read_sql(void *context, unsigned char *buffer, size_t capacity, size_t *length) {
    const char **cursor = context;
    *length = strlen(*cursor);
    if (*length > capacity) *length = capacity;
    memcpy(buffer, *cursor, *length);
    *cursor += *length;
    return *length ? MD_READ_DATA : MD_READ_EOF;
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--sdk-reject")) {
        MdClientOptions options;
        mdc_options_default(&options);
        options.port = (uint16_t)atoi(argv[2]);
        MdClient *client = NULL;
        MdClientError error;
        if (!mdc_connect(&options, &client, &error) ||
            !mdc_execute(client, (const unsigned char *)"VALUES (42);", 12, &error)) {
            fprintf(stderr, "fake-peer setup failed %s %s\n", error.code, error.message);
            mdc_close(client);
            return 2;
        }
        MdEvent event;
        MdClientNext next = mdc_next(client, &event, &error);
        md_event_destroy(&event);
        mdc_close(client);
        if (next != MDC_ERROR || strcmp(error.code, "PROTOCOL_ERROR")) {
            fprintf(stderr, "invalid typed event accepted, next=%d code=%s\n", next, error.code);
            return 1;
        }
        return 0;
    }
    if (argc==3 && !strcmp(argv[1],"--sdk-connect-fail")) {
        MdClientOptions options; mdc_options_default(&options);
        options.port=(uint16_t)atoi(argv[2]); options.connect_timeout_ms=0;
        MdClient *client=NULL; MdClientError error;
        assert(!mdc_connect(&options,&client,&error));
        assert(!client && !error.execution_unconfirmed);
        return 0;
    }
    if (argc==3 && !strcmp(argv[1],"--sdk-send-timeout")) {
        MdClientOptions options; mdc_options_default(&options);
        options.port=(uint16_t)atoi(argv[2]); options.send_timeout_ms=40;
        options.request_bytes=options.frame_bytes=8*1024*1024;
        MdClient *client=NULL; MdClientError error;
        assert(mdc_connect(&options,&client,&error));
        size_t length=4*1024*1024;
        unsigned char *sql=malloc(length); assert(sql); memset(sql,'x',length);
        assert(!mdc_execute(client,sql,length,&error));
        assert(!strcmp(error.code,"TIMEOUT") && error.execution_unconfirmed);
        free(sql); mdc_close(client);
        return 0;
    }
    if (argc == 8 && !strcmp(argv[1], "--sdk-error")) {
        MdClientOptions options; mdc_options_default(&options);
        options.port=(uint16_t)atoi(argv[2]);
        options.receive_timeout_ms=(uint64_t)strtoull(argv[4],NULL,10);
        options.event_bytes=(size_t)strtoull(argv[5],NULL,10);
        options.frame_bytes=(size_t)strtoull(argv[6],NULL,10);
        options.request_bytes=options.frame_bytes;
        options.response_bytes=(size_t)strtoull(argv[7],NULL,10);
        MdClient *client=NULL; MdClientError error;
        assert(mdc_connect(&options,&client,&error));
        assert(mdc_execute(client,(const unsigned char *)"VALUES (42);",12,&error));
        MdEvent saved={0}; size_t events=0;
        for (;;) {
            MdEvent event;
            MdClientNext next=mdc_next(client,&event,&error);
            if (next!=MDC_EVENT) { assert(next==MDC_ERROR); break; }
            events++;
            md_event_destroy(&saved); saved=event;
        }
        assert(events==(size_t)atoi(argv[3]));
        assert(error.execution_unconfirmed);
        assert(error.last_statement_index==(events ? 1u : 0u));
        assert(!mdc_execute(client,(const unsigned char *)"VALUES (42);",12,&error));
        mdc_close(client);
        if (events) assert(saved.result.values[0].as.integer==42);
        md_event_destroy(&saved);
        return 0;
    }
    if ((argc == 3 && !strcmp(argv[1], "--sdk")) || (argc==6 && !strcmp(argv[1],"--sdk-capped"))) {
        MdClientOptions options; mdc_options_default(&options);
        options.port = (uint16_t)atoi(argv[2]);
        if (argc==6) {
            options.event_bytes=(size_t)strtoull(argv[3],NULL,10);
            options.frame_bytes=(size_t)strtoull(argv[4],NULL,10);
            options.request_bytes=options.frame_bytes;
            options.response_bytes=(size_t)strtoull(argv[5],NULL,10);
        }
        MdClient *client = NULL; MdClientError error; MdEvent event;
        bool connected = mdc_connect(&options,&client,&error);
        if (!connected) fprintf(stderr,"connect error %s %s port=%u\n",error.code,error.message,options.port);
        assert(connected);
        assert(mdc_execute(client,(const unsigned char *)"VALUES (42);",12,&error));
        assert(!mdc_execute(client,(const unsigned char *)"VALUES (42);",12,&error));
        assert(mdc_next(client,&event,&error) == MDC_EVENT);
        assert(event.has_result && event.result.row_count == 1);
        assert(event.result.values[0].as.integer == 42);
        assert(mdc_next(client,&(MdEvent){0},&error) == MDC_DONE);
        if (argc==3) {
            MdEvent second;
            assert(mdc_next(client,&second,&error)==MDC_ERROR);
            assert(mdc_execute(client,(const unsigned char *)"VALUES (42);",12,&error));
            assert(mdc_next(client,&second,&error)==MDC_EVENT);
            assert(second.result.values[0].as.integer==42);
            md_event_destroy(&second);
            assert(mdc_next(client,&second,&error)==MDC_DONE);
        }
        mdc_close(client);
        assert(event.result.values[0].as.integer == 42);
        md_event_destroy(&event);
        return 0;
    }
    MdClientOptions invalid_options; mdc_options_default(&invalid_options);
    invalid_options.host="localhost"; invalid_options.port=1;
    MdClient *invalid_client=NULL; MdClientError invalid_error;
    assert(!mdc_connect(&invalid_options,&invalid_client,&invalid_error));
    assert(!strcmp(invalid_error.code,"ARGUMENT_ERROR") && !invalid_client);
    const struct {
        const char *json;
        bool valid;
        uint64_t expected;
    } wire_numbers[] = {
        {"\"0\"", true, 0},
        {"\"18446744073709551615\"", true, UINT64_MAX},
        {"\"00\"", false, 0},
        {"\"01\"", false, 0},
        {"\"18446744073709551616\"", false, 0}
    };
    for (size_t i = 0; i < sizeof wire_numbers / sizeof wire_numbers[0]; i++) {
        yyjson_doc *document = mp_json(wire_numbers[i].json, strlen(wire_numbers[i].json));
        uint64_t parsed = 0;
        assert(mp_u64(yyjson_doc_get_root(document), &parsed) == wire_numbers[i].valid);
        if (wire_numbers[i].valid)
            assert(parsed == wire_numbers[i].expected);
        yyjson_doc_free(document);
    }
    uint64_t option_value;
    assert(mp_decimal("0001", &option_value) && option_value == 1);
    const char *bounds =
        "{\"version\":1,\"result\":{\"version\":1,\"type\":\"query_result\","
        "\"statement_index\":\"18446744073709551615\",\"columns\":[{\"name\":\"lo\",\"type\":\"INT64\"},"
        "{\"name\":\"hi\",\"type\":\"INT64\"}],\"rows\":[[\"-9223372036854775808\",\"9223372036854775807\"]],"
        "\"row_count\":\"1\"},\"diagnostic\":{\"version\":1,\"type\":\"diagnostic\",\"code\":\"IO_ERROR\","
        "\"message\":\"test\",\"operation\":\"run\",\"phase\":\"output\",\"source\":\"stdout\",\"path\":null,"
        "\"statement_index\":\"18446744073709551615\",\"line\":\"1\",\"column\":\"1\",\"field_path\":null,"
        "\"byte_offset\":\"0\",\"committed\":null,\"rollback\":\"not_needed\",\"command\":null,"
        "\"affected_rows\":\"18446744073709551615\",\"output_ready\":null},\"stop\":true,\"health\":\"healthy\",\"stage\":\"send\"}";
    MdEvent boundary_event;
    char stage[16];
    assert(mp_event_decode(bounds, strlen(bounds), &boundary_event, stage));
    assert(boundary_event.result.statement_index == UINT64_MAX);
    assert(boundary_event.result.values[0].as.integer == INT64_MIN);
    assert(boundary_event.result.values[1].as.integer == INT64_MAX);
    assert(boundary_event.diagnostic.affected_rows == UINT64_MAX);
    assert(boundary_event.diagnostic.has_offset && boundary_event.diagnostic.byte_offset == 0);
    assert(!strcmp(stage, "send"));
    md_event_destroy(&boundary_event);
    size_t sum;
    assert(mp_add(8,2,10,&sum) && sum==10);
    assert(!mp_add(SIZE_MAX,1,SIZE_MAX,&sum));
    assert(!mp_add(11,0,10,&sum));
    uint64_t number;
    assert(mp_decimal("18446744073709551615",&number) && number==UINT64_MAX);
    assert(!mp_decimal("18446744073709551616",&number));
    assert(!mp_decimal("+1",&number) && !mp_decimal("-1",&number));
    assert(mp_limits(4096,4096,4096,16384));
    assert(!mp_limits(4097,4096,4096,16384));
    unsigned char header[5]; mp_header(header,'D',UINT32_MAX);
    assert(mp_length(header)==UINT32_MAX);
    MdDb db = {0};
    MdRequest *request = NULL;
    MdDiagnostic diagnostic;
    const char *sql = "VALUES (42);";
    assert(md_request_open(&db, (MdInput){read_sql, &sql}, &request, &diagnostic));
    db.health = MD_RECOVERY_REQUIRED;
    MdEvent event;
    MdNext next = md_request_next(request, &event);
    assert(next == MD_NEXT_STOP);
    assert(event.has_diagnostic && !event.has_result && event.stop);
    assert(event.health == MD_RECOVERY_REQUIRED);
    assert(!strcmp(event.diagnostic.code, "RECOVERY_REQUIRED"));
    assert(sql[0] == 'V');
    md_event_destroy(&event);
    md_request_close(request);
    puts("M12 health checked before input or SQL execution");
    return 0;
}
