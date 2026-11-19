#ifndef MINIDB_CLIENT_H
#define MINIDB_CLIENT_H
#include "minidb.h"
typedef struct MdClient MdClient;
typedef struct {
    const char *host;
    uint16_t port;
    uint64_t connect_timeout_ms, receive_timeout_ms, send_timeout_ms;
    size_t request_bytes, frame_bytes, event_bytes, response_bytes;
} MdClientOptions;
/* EVENT의 stage는 원격 운영 구간이다. ERROR는 마지막 완성 문장과 반영 미확인을 담는다. */
typedef struct {
    char code[40], message[256], stage[16];
    uint64_t last_statement_index;
    bool execution_unconfirmed;
    int exit_code;
} MdClientError;
typedef enum { MDC_EVENT, MDC_DONE, MDC_ERROR } MdClientNext;
/* 기본값을 채운 후 필요한 옵션을 개별 수정한다. 시간 0은 비활성이다. */
void mdc_options_default(MdClientOptions *options);
bool mdc_connect(const MdClientOptions *options, MdClient **out, MdClientError *error);
bool mdc_execute(MdClient *client, const unsigned char *sql, size_t length, MdClientError *error);
MdClientNext mdc_next(MdClient *client, MdEvent *event, MdClientError *error);
void mdc_close(MdClient *client);
#endif
