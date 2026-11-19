#ifndef MD_PROTOCOL_H
#define MD_PROTOCOL_H
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "minidb_client.h"
#include "yyjson.h"
#define MP_RESERVE 16384u
#define MP_FRAME_DEFAULT 1048576u
#define MP_EVENT_DEFAULT 67108864u
#define MP_TOTAL_DEFAULT 268435456u
/* 네트워크 byte 수는 payload만 세고, 합산 전 뺄셈으로 overflow를 검사한다. */
bool mp_add(size_t a, size_t b, size_t limit, size_t *out);
bool mp_decimal(const char *text, uint64_t *value);
bool mp_limits(size_t request, size_t frame, size_t event, size_t total);
uint64_t mp_now(void);
bool mp_expired(uint64_t start, uint64_t timeout);
void mp_header(unsigned char header[5], char kind, uint32_t length);
uint32_t mp_length(const unsigned char header[5]);
bool mp_fields(yyjson_val *object, const char *const *keys, size_t count);
bool mp_u64(yyjson_val *value, uint64_t *out);
bool mp_string(yyjson_val *value, const char *expected);
yyjson_doc *mp_json(const char *data, size_t length);
bool mp_request(const char *data, size_t length, char **sql, size_t *sql_length);
char *mp_event_encode(const MdEvent *event, const char *stage, size_t *length);
bool mp_event_decode(const char *data, size_t length, MdEvent *event, char stage[16]);
int mp_event_exit(const MdEvent *event, const char *stage);
void mp_error_event(MdEvent *event, const char *code, const char *stage);
char *mp_copy(const char *data, size_t length);
bool mp_socket_nonblocking(SOCKET socket);
#endif
