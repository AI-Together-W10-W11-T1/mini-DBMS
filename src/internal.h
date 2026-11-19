#ifndef MD_INTERNAL_H
#define MD_INTERNAL_H
#define WIN32_LEAN_AND_MEAN
#include "minidb.h"
#include "yyjson.h"
#include "bptree.h"
#include <windows.h>
typedef struct {
    char *name;
    MdType type;
} MdField;
typedef struct {
    char *name;
    MdField *fields;
    size_t count, id;
    MdValue *rows;
    size_t row_count;
    MdBptree index;
} MdTable;
struct MdDb {
    /* 0 초기화 SRW/CV는 stack fixture와 실제 DB 모두에서 유효하다. */
    SRWLOCK gate;
    CONDITION_VARIABLE changed;
    size_t readers, waiting_writers;
    bool writer;
    MdHealth health;
    MdTable *tables;
    size_t count;
    char *data_dir;
    HANDLE lease;
};
struct MdRequest {
    MdDb *db;
    MdInput input;
    bool stopped, eof, input_error, validate_only;
    unsigned char buffer[4096];
    size_t used, cursor;
    uint64_t statement, offset, line, column;
    bool previous_cr, started;
    MdExecutionOptions execution;
    uint64_t lock_wait_ms, sql_start;
    bool sql_running, protected_commit;
    const char *cancel_code, *cancel_stage;
};
bool md_gate_enter(MdRequest *, bool);
bool md_request_create(MdDb *, MdInput, MdRequest **, MdDiagnostic *);
void md_gate_leave(MdDb *, bool);
const char *md_execution_check(void);
void md_execution_protect(void);
void md_execution_point(const char *);
uint64_t md_execution_now(void);
#ifdef MINIDB_TEST_FAULTS
typedef void (*MdTestExecutionHook)(const char *, void *);
void md_test_execution_hook(MdTestExecutionHook, void *);
void md_test_execution_clock(uint64_t (*)(void *), void *);
#endif
void md_diag(MdDiagnostic *, const char *, const char *, const char *, const char *, const char *);
char *md_copy(const char *, size_t);
bool md_equal(const char *, const char *);
bool md_utf8(const unsigned char *, size_t);
wchar_t *md_wide(const char *);
char *md_path(const char *);
char *md_join(const char *, const char *);
bool md_read_file(const char *, unsigned char **, size_t *, MdDiagnostic *);
bool md_write_all(HANDLE, const void *, size_t);
bool md_fault(const char *);
void md_values_free(MdValue *, size_t);
bool md_value_copy(MdValue *, const MdValue *);
void md_catalog_free(MdTable *, size_t);
bool md_catalog_load(const char *, MdDb *, MdDiagnostic *);
bool md_schema_parse(const unsigned char *, size_t, const char *, MdTable *, MdDiagnostic *);
bool md_schema_same(const MdTable *, const MdTable *);
bool md_identifier(const char *);
yyjson_mut_val *md_schema_json(yyjson_mut_doc *, const MdTable *);
bool md_storage_open(MdDb *, MdDiagnostic *);
typedef struct {
    yyjson_doc *document;
    size_t **columns;
    bool *copied;
    size_t count;
} MdMigration;
bool md_plan_load(const char *, MdMigration *, MdDiagnostic *);
bool md_mapping_prepare(MdMigration *, const MdDb *, const MdDb *, MdDiagnostic *);
void md_migration_free(MdMigration *);
bool md_storage_source(MdDb *, MdDiagnostic *);
bool md_storage_migrate(MdDb *, MdDb *, const MdMigration *, MdEvent *);
bool md_storage_init(MdDb *, MdEvent *);
bool md_storage_insert(MdDb *, MdTable *, MdValue *, size_t, MdEvent *);
bool md_storage_replace(MdDb *, MdTable *, MdValue **, size_t, const char *, uint64_t, MdEvent *);
#ifdef MINIDB_TEST_FAULTS
void md_test_index_reset(void);
size_t md_test_index_lookups(void);
size_t md_test_scan_rows(void);
char *md_test_output_parent(const char *);
char *md_test_output_resolve(const char *);
bool md_test_migration_integer(const char *, size_t, int64_t *);
bool md_test_migration_size(size_t, size_t, size_t *);
bool md_test_union_size(size_t, size_t, size_t, size_t *);
bool md_test_count_increment(int64_t *);
bool md_test_aggregate_size(size_t, size_t, size_t *);
#endif
MdNext md_sql_next(MdRequest *, MdEvent *);
#endif
