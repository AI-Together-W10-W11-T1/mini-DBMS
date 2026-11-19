#ifndef MINIDB_H
#define MINIDB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 경로는 UTF-8, 값과 결과의 소유권은 이벤트에 있다.
 * 서로 다른 요청의 병렬 next를 지원한다. 같은 요청의 next/close는 직렬 호출한다.
 * 모든 요청 종료 뒤 DB를 닫는다. */
typedef struct MdDb MdDb;
typedef struct MdRequest MdRequest;
typedef enum { MD_NULL, MD_INT64, MD_TEXT } MdType;
typedef struct {
    char *ptr;
    size_t len;
} MdText;
typedef struct {
    MdType type;
    union {
        int64_t integer;
        MdText text;
    } as;
} MdValue;
typedef struct {
    char *name;
    MdType type;
} MdColumn;
typedef enum { MD_UNKNOWN, MD_NO, MD_YES } MdKnown;
typedef enum {
    MD_ROLLBACK_NOT_NEEDED,
    MD_ROLLBACK_CONFIRMED,
    MD_ROLLBACK_FAILED,
    MD_ROLLBACK_UNKNOWN
} MdRollback;
typedef enum { MD_HEALTHY, MD_STOPPED_CLEANUP, MD_RECOVERY_REQUIRED } MdHealth;
typedef struct {
    char code[40], message[256], phase[24], command[16], path[1024];
    char source[24], field_path[128];
    uint64_t statement_index, line, column, byte_offset;
    bool has_statement, has_position, has_offset;
    MdKnown committed, output_ready;
    MdRollback rollback;
    uint64_t affected_rows;
    bool has_affected_rows;
} MdDiagnostic;
typedef enum { MD_QUERY_RESULT, MD_STATEMENT_OK, MD_OPERATION_OK } MdResultKind;
typedef struct {
    char *name;
    char *action;
    uint64_t row_count;
} MdTableResult;
typedef struct {
    MdResultKind kind;
    uint64_t statement_index, affected_rows;
    char command[16];
    MdColumn *columns;
    size_t column_count, row_count;
    MdValue *values; /* 행 우선 배열: row_count * column_count */
    char *output_path;
    MdTableResult *tables;
    size_t table_count;
} MdResult;
typedef struct {
    bool has_result, has_diagnostic, stop;
    MdResult result;
    /* 고정 크기 진단은 반영 확정 뒤 할당 실패로 성공 상태를 잃지 않는다. */
    MdDiagnostic diagnostic;
    MdHealth health;
} MdEvent;
typedef enum { MD_READ_DATA, MD_READ_EOF, MD_READ_ERROR } MdReadStatus;
typedef MdReadStatus (*MdRead)(void *context, unsigned char *buffer, size_t capacity,
                               size_t *length);
typedef struct {
    MdRead read;
    void *context;
} MdInput;
typedef enum { MD_NEXT_RESULT, MD_NEXT_EOF, MD_NEXT_STOP } MdNext;

typedef enum { MD_CANCEL_NONE, MD_CANCEL_DISCONNECT, MD_CANCEL_SHUTDOWN } MdCancelReason;
typedef MdCancelReason (*MdCancel)(void *context);
typedef struct {
    /* 밀리초 단위 0은 비활성이다. 양수 상한은 INT64_MAX다.
     * lock은 문장 내 gate 대기의 합계, SQL 시계는 최종 gate 뒤 시작한다. */
    uint64_t lock_timeout_ms, sql_timeout_ms;
    /* 관찰 전용 callback은 빠르게 반환하며 엔진을 재진입하지 않는다. */
    MdCancel cancel;
    /* callback과 context는 next_ex 반환까지 유효해야 한다. */
    void *context;
} MdExecutionOptions;
typedef enum { MD_INIT, MD_MIGRATE } MdOperation;
typedef struct {
    MdOperation operation;
    const char *schema_dir, *data_dir, *source_data_dir, *plan_path;
} MdOperationOptions;

bool md_open(const char *schema_dir, const char *data_dir, MdDb **out, MdDiagnostic *diagnostic);
void md_close(MdDb *db);
bool md_request_open(MdDb *db, MdInput input, MdRequest **out, MdDiagnostic *diagnostic);
MdNext md_request_next(MdRequest *request, MdEvent *event);
MdNext md_request_next_ex(MdRequest *request, const MdExecutionOptions *options, MdEvent *event);
void md_request_close(MdRequest *request);
bool md_operate(const MdOperationOptions *options, MdEvent *event);
/* 이벤트는 DB/요청 종료 뒤에도 유효하다. 0 초기화 이벤트도 해제 가능하다. */
void md_event_destroy(MdEvent *event);
#endif
