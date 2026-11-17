#ifndef MD_INTERNAL_H
#define MD_INTERNAL_H
#define WIN32_LEAN_AND_MEAN
#include "minidb.h"
#include "yyjson.h"
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
} MdTable;
struct MdDb {
    MdHealth health;
    MdTable *tables;
    size_t count;
    char *data_dir;
    HANDLE lease;
};
struct MdRequest {
    MdDb *db;
    MdInput input;
    bool stopped, eof, input_error;
    unsigned char buffer[4096];
    size_t used, cursor;
    uint64_t statement, offset, line, column;
    bool previous_cr, started;
};
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
bool md_storage_init(MdDb *, MdEvent *);
bool md_storage_insert(MdDb *, MdTable *, MdValue *, MdEvent *);
MdNext md_sql_next(MdRequest *, MdEvent *);
#endif
