#include "internal.h"
#include "minidb.h"
#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void event_destroy_is_idempotent(void **state) {
    (void)state;
    MdEvent event = {0};
    md_event_destroy(&event);
    md_event_destroy(&event);
    assert_false(event.has_result);
    md_close(NULL);
    md_request_close(NULL);
}
typedef struct {
    const unsigned char *raw;
    size_t len, pos, chunk;
    bool fail;
} Reader;
static MdReadStatus read_slice(void *context, unsigned char *buffer, size_t cap, size_t *length) {
    Reader *r = context;
    *length = 0;
    if (r->pos == r->len)
        return r->fail ? MD_READ_ERROR : MD_READ_EOF;
    size_t n = r->len - r->pos;
    if (n > r->chunk)
        n = r->chunk;
    if (n > cap)
        n = cap;
    memcpy(buffer, r->raw + r->pos, n);
    r->pos += n;
    *length = n;
    return MD_READ_DATA;
}
typedef struct {
    char *root, *schema, *data;
    MdDb *db;
} Fixture;
static void make_directory(const char *path) {
    wchar_t *w = md_wide(path);
    assert_non_null(w);
    assert_true(CreateDirectoryW(w, NULL));
    free(w);
}
static void delete_file(const char *path) {
    wchar_t *w = md_wide(path);
    assert_non_null(w);
    (void)DeleteFileW(w);
    free(w);
}
static void delete_directory(const char *path) {
    wchar_t *w = md_wide(path);
    assert_non_null(w);
    assert_true(RemoveDirectoryW(w));
    free(w);
}
static Fixture fixture(void) {
    Fixture f = {0};
    char unique[80];
    snprintf(unique, sizeof unique, "unit-%lu-%llu", GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    f.root = md_path(unique);
    assert_non_null(f.root);
    make_directory(f.root);
    f.schema = md_join(f.root, "schema");
    f.data = md_join(f.root, "data");
    assert_non_null(f.schema);
    assert_non_null(f.data);
    make_directory(f.schema);
    char *path = md_join(f.schema, "users.json");
    wchar_t *w = md_wide(path);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    assert_true(h != INVALID_HANDLE_VALUE);
    const char *json = "{\"version\":1,\"name\":\"users\",\"columns\":[{\"name\":\"id\",\"type\":"
                       "\"INT64\"},{\"name\":\"name\",\"type\":\"TEXT\"}]}";
    assert_true(md_write_all(h, json, strlen(json)));
    assert_true(CloseHandle(h));
    free(path);
    free(w);
    MdEvent event = {0};
    MdOperationOptions opts = {.operation = MD_INIT, .schema_dir = f.schema, .data_dir = f.data};
    assert_true(md_operate(&opts, &event));
    md_event_destroy(&event);
    MdDiagnostic d;
    assert_true(md_open(f.schema, f.data, &f.db, &d));
    return f;
}
static void fixture_close(Fixture *f) {
    md_close(f->db);
    f->db = NULL;
    const char *files[] = {"table_users.tbl", ".minidb.lock", ".minidb.pending"};
    for (size_t i = 0; i < 3; i++) {
        char *p = md_join(f->data, files[i]);
        delete_file(p);
        free(p);
    }
    delete_directory(f->data);
    char *p = md_join(f->schema, "users.json");
    delete_file(p);
    free(p);
    delete_directory(f->schema);
    delete_directory(f->root);
    free(f->root);
    free(f->schema);
    free(f->data);
}
static void byte_boundaries_and_owned_result(void **state) {
    (void)state;
    Fixture f = fixture();
    /* callback는 매번 정확히 1바이트: BOM·한글·CRLF·escape 경계가 결정적이다. */
    const unsigned char sql[] =
        "\xef\xbb\xbf;/*x*/INSERT INTO users(id,name) VALUES(1,'\xed\x95\x9c\r\nO''Brien');SELECT "
        "name FROM users WHERE id=1;";
    Reader reader = {sql, sizeof sql - 1, 0, 1, false};
    MdRequest *request;
    MdDiagnostic d;
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    MdEvent event = {0};
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    assert_true(event.has_result);
    assert_int_equal(event.result.statement_index, 1);
    assert_true(reader.pos < reader.len);
    md_event_destroy(&event);
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    assert_int_equal(event.result.row_count, 1);
    md_request_close(request);
    md_close(f.db);
    f.db = NULL;
    const char expected[] = "\xed\x95\x9c\r\nO'Brien";
    assert_int_equal(event.result.values[0].as.text.len, sizeof expected - 1);
    assert_memory_equal(event.result.values[0].as.text.ptr, expected, sizeof expected - 1);
    md_event_destroy(&event);
    fixture_close(&f);
}
static void readahead_and_input_failure(void **state) {
    (void)state;
    Fixture f = fixture();
    const unsigned char sql[] =
        "INSERT INTO users(id) VALUES(1);INSERT INTO users(id,name) VALUES(2,'\xff');";
    Reader reader = {sql, sizeof sql - 1, 0, sizeof sql, false};
    MdRequest *request;
    MdDiagnostic d;
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    MdEvent event = {0};
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    assert_true(event.has_result);
    md_event_destroy(&event);
    assert_int_equal(md_request_next(request, &event), MD_NEXT_STOP);
    assert_string_equal(event.diagnostic.code, "LEXICAL_ERROR");
    assert_int_equal(event.diagnostic.statement_index, 2);
    md_event_destroy(&event);
    md_request_close(request);
    const unsigned char second[] = "INSERT INTO users(id) VALUES(3);SELECT";
    reader = (Reader){second, sizeof second - 1, 0, 2, true};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_event_destroy(&event);
    assert_int_equal(md_request_next(request, &event), MD_NEXT_STOP);
    assert_string_equal(event.diagnostic.code, "IO_ERROR");
    assert_false(event.diagnostic.has_position);
    md_event_destroy(&event);
    md_request_close(request);
    fixture_close(&f);
}
int main(void) {
    const struct CMUnitTest tests[] = {cmocka_unit_test(event_destroy_is_idempotent),
                                       cmocka_unit_test(byte_boundaries_and_owned_result),
                                       cmocka_unit_test(readahead_and_input_failure)};
    return cmocka_run_group_tests(tests, NULL, NULL);
}
