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
static void m2_owned_values_and_aliases(void **state) {
    (void)state;
    Fixture f = fixture();
    const unsigned char sql[] =
        "INSERT INTO users(id,name) VALUES(1,'A'),(2,'B');"
        "SELECT u.name AS chosen FROM users AS u WHERE (u.id>=1 AND u.id<>2) OR NULL=1;"
        "VALUES(NULL,'O''Brien'),(3,NULL);";
    Reader reader = {sql, sizeof sql - 1, 0, 1, false};
    MdRequest *request;
    MdDiagnostic d;
    MdEvent event = {0}, selected = {0};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    assert_int_equal(event.result.affected_rows, 2);
    md_event_destroy(&event);
    assert_int_equal(md_request_next(request, &selected), MD_NEXT_RESULT);
    assert_int_equal(selected.result.row_count, 1);
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_request_close(request);
    md_close(f.db);
    f.db = NULL;
    assert_string_equal(selected.result.columns[0].name, "chosen");
    assert_string_equal(selected.result.values[0].as.text.ptr, "A");
    assert_int_equal(event.result.columns[0].type, MD_INT64);
    assert_int_equal(event.result.values[0].type, MD_NULL);
    assert_string_equal(event.result.values[1].as.text.ptr, "O'Brien");
    assert_int_equal(event.result.values[2].as.integer, 3);
    md_event_destroy(&selected);
    md_event_destroy(&event);
    md_event_destroy(&event);
    fixture_close(&f);
}
static void m3_chunked_mutation_and_owned_result(void **state) {
    (void)state;
    Fixture f = fixture();
    const unsigned char sql[] = "INSERT INTO users(id,name) VALUES(1,'old'),(2,'delete');"
        "UPDATE users SET name='new' WHERE id=1;SELECT name FROM users WHERE id=1;"
        "DELETE FROM users WHERE id=2;SELECT id FROM users;";
    Reader reader = {sql, sizeof sql - 1, 0, 1, false};
    MdRequest *request;
    MdDiagnostic d;
    MdEvent event = {0}, selected = {0};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_event_destroy(&event);
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    assert_string_equal(event.result.command, "UPDATE");
    assert_int_equal(event.result.affected_rows, 1);
    md_event_destroy(&event);
    assert_int_equal(md_request_next(request, &selected), MD_NEXT_RESULT);
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    assert_string_equal(event.result.command, "DELETE");
    assert_int_equal(event.result.affected_rows, 1);
    md_event_destroy(&event);
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    assert_int_equal(event.result.row_count, 1);
    assert_int_equal(event.result.values[0].as.integer, 1);
    md_event_destroy(&event);
    assert_int_equal(md_request_next(request, &event), MD_NEXT_EOF);
    md_request_close(request);
    md_close(f.db);
    f.db = NULL;
    assert_string_equal(selected.result.values[0].as.text.ptr, "new");
    md_event_destroy(&selected);
    fixture_close(&f);
}
static void m4_sorted_paged_owned_result(void **state) {
    (void)state;
    Fixture f = fixture();
    const unsigned char sql[] = "INSERT INTO users(id,name) VALUES(3,'C'),(1,'A'),(2,'B');"
        "SELECT id,name FROM users ORDER BY id OFFSET 1 ROWS FETCH FIRST 1 ROWS ONLY;"
        "VALUES(NULL,'z'),(2,'b'),(1,'a') ORDER BY column1 NULLS FIRST OFFSET 1 ROWS;";
    Reader reader = {sql, sizeof sql - 1, 0, 1, false};
    MdRequest *request;
    MdDiagnostic d;
    MdEvent event = {0}, selected = {0};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_event_destroy(&event);
    assert_int_equal(md_request_next(request, &selected), MD_NEXT_RESULT);
    assert_int_equal(selected.result.row_count, 1);
    assert_int_equal(selected.result.values[0].as.integer, 2);
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_request_close(request);
    md_close(f.db);
    f.db = NULL;
    assert_string_equal(selected.result.values[1].as.text.ptr, "B");
    assert_int_equal(event.result.row_count, 2);
    assert_int_equal(event.result.values[0].as.integer, 1);
    assert_string_equal(event.result.values[1].as.text.ptr, "a");
    assert_string_equal(event.result.values[3].as.text.ptr, "b");
    md_event_destroy(&selected);
    md_event_destroy(&event);
    md_event_destroy(&event);
    fixture_close(&f);
}
static void m5_join_owned_text_and_null(void **state) {
    (void)state;
    Fixture f = fixture();
    const unsigned char sql[] = "INSERT INTO users(id,name) VALUES(1,'A'),(2,NULL);"
        "SELECT a.name,b.name FROM users AS a INNER JOIN users AS b ON a.id=b.id ORDER BY a.name;";
    Reader reader = {sql, sizeof sql - 1, 0, 1, false};
    MdRequest *request;
    MdDiagnostic d;
    MdEvent event = {0};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_event_destroy(&event);
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    assert_int_equal(event.result.row_count, 2);
    md_request_close(request);
    md_close(f.db);
    f.db = NULL;
    assert_string_equal(event.result.columns[0].name, "a.name");
    assert_string_equal(event.result.values[0].as.text.ptr, "A");
    assert_string_equal(event.result.values[1].as.text.ptr, "A");
    assert_ptr_not_equal(event.result.values[0].as.text.ptr, event.result.values[1].as.text.ptr);
    assert_int_equal(event.result.values[2].type, MD_NULL);
    assert_int_equal(event.result.values[3].type, MD_NULL);
    md_event_destroy(&event);
    md_event_destroy(&event);
    fixture_close(&f);
}
static void m5_join_checked_width(void **state) {
    (void)state;
    Fixture f = fixture();
    const unsigned char sql[] = "SELECT * FROM users AS a INNER JOIN users AS b ON a.id=b.id;";
    Reader reader = {sql, sizeof sql - 1, 0, 1, false};
    MdRequest *request;
    MdDiagnostic d;
    MdEvent event = {0};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    size_t count = f.db->tables[0].count;
    f.db->tables[0].count = SIZE_MAX;
    MdNext result = md_request_next(request, &event);
    f.db->tables[0].count = count;
    assert_int_equal(result, MD_NEXT_STOP);
    assert_string_equal(event.diagnostic.code, "RESOURCE_ERROR");
    assert_false(event.has_result);
    md_event_destroy(&event);
    md_request_close(request);
    fixture_close(&f);
}
#ifdef MINIDB_TEST_FAULTS
bool md_test_join_result_size(size_t, size_t, size_t *);
static void m5_join_result_size_boundaries(void **state) {
    (void)state;
    size_t bytes = 0, cells = SIZE_MAX / sizeof(MdValue);
    assert_true(md_test_join_result_size(0, 1, &bytes));
    assert_int_equal(bytes, sizeof(MdValue));
    assert_true(md_test_join_result_size(cells - 1, 1, &bytes));
    assert_int_equal(bytes, cells * sizeof(MdValue));
    assert_false(md_test_join_result_size(cells, 1, &bytes));
    assert_false(md_test_join_result_size(SIZE_MAX, 1, &bytes));
    assert_false(md_test_join_result_size(0, cells + 1, &bytes));
    assert_false(md_test_join_result_size(1, cells, &bytes));
    assert_false(md_test_join_result_size(0, 0, &bytes));
}
static void m5_partial_copy_preserves_catalog(void **state) {
    (void)state;
    Fixture f = fixture();
    const unsigned char sql[] = "INSERT INTO users(id,name) VALUES(1,'owned');"
        "SELECT a.name,b.name FROM users AS a INNER JOIN users AS b ON a.id=b.id;";
    Reader reader = {sql, sizeof sql - 1, 0, 1, false};
    MdRequest *request;
    MdDiagnostic d;
    MdEvent event = {0};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_event_destroy(&event);
    assert_int_equal(_putenv("MINIDB_TEST_FAULT=join_copy_after_one"), 0);
    MdNext next = md_request_next(request, &event);
    assert_int_equal(_putenv("MINIDB_TEST_FAULT="), 0);
    assert_int_equal(next, MD_NEXT_STOP);
    assert_string_equal(event.diagnostic.code, "RESOURCE_ERROR");
    assert_false(event.has_result);
    assert_null(event.result.values);
    assert_null(event.result.columns);
    assert_string_equal(f.db->tables[0].rows[1].as.text.ptr, "owned");
    md_event_destroy(&event);
    md_request_close(request);
    /* 새 요청은 실패했던 SELECT만 다시 실행한다. */
    const unsigned char retry[] = "SELECT a.name,b.name FROM users AS a INNER JOIN users AS b ON a.id=b.id;";
    reader = (Reader){retry, sizeof retry - 1, 0, 1, false};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_request_close(request);
    md_close(f.db);
    f.db = NULL;
    assert_string_equal(event.result.values[0].as.text.ptr, "owned");
    assert_string_equal(event.result.values[1].as.text.ptr, "owned");
    md_event_destroy(&event);
    fixture_close(&f);
}
#endif
#ifdef MINIDB_TEST_FAULTS
static void m7_union_failure_cleanup(void **state) {
    (void)state;
    const char *faults[] = {"union_merge", "union_sort_keys", "union_after_merge"};
    Fixture f = fixture();
    const unsigned char sql[] = "VALUES('owned'),('owned'),(NULL) UNION VALUES('owned'),('other');";
    MdRequest *request;
    MdDiagnostic d;
    MdEvent event = {0};
    for (size_t i = 0; i < sizeof faults / sizeof *faults; i++) {
        Reader reader = {sql, sizeof sql - 1, 0, 1, false};
        assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
        assert_int_equal(_putenv_s("MINIDB_TEST_FAULT", faults[i]), 0);
        MdNext next = md_request_next(request, &event);
        assert_int_equal(_putenv_s("MINIDB_TEST_FAULT", ""), 0);
        assert_int_equal(next, MD_NEXT_STOP);
        assert_string_equal(event.diagnostic.code, "RESOURCE_ERROR");
        assert_false(event.has_result);
        assert_null(event.result.values);
        assert_null(event.result.columns);
        md_event_destroy(&event);
        md_event_destroy(&event);
        md_request_close(request);
        reader = (Reader){sql, sizeof sql - 1, 0, 1, false};
        assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
        assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
        assert_int_equal(event.result.row_count, 3);
        md_event_destroy(&event);
        md_request_close(request);
    }
    fixture_close(&f);
}

static void m7_union_size_boundaries(void **state) {
    (void)state;
    size_t bytes = 7, cells = SIZE_MAX / sizeof(MdValue);
    assert_true(md_test_union_size(0, 0, 1, &bytes));
    assert_int_equal(bytes, 0);
    assert_false(md_test_union_size(0, 0, 0, &bytes));
    assert_true(md_test_union_size(cells - 1, 1, 1, &bytes));
    assert_int_equal(bytes, cells * sizeof(MdValue));
    assert_false(md_test_union_size(cells, 1, 1, &bytes));
    assert_false(md_test_union_size(SIZE_MAX, 1, 1, &bytes));
    assert_false(md_test_union_size(1, 1, SIZE_MAX, &bytes));
    assert_false(md_test_union_size(0, 1, cells + 1, &bytes));
    assert_true(md_test_union_size(1, 1, 2, &bytes));
    assert_int_equal(bytes, 4 * sizeof(MdValue));
}

static void m6_count_and_size_boundaries(void **state) {
    (void)state;
    int64_t count = INT64_MAX - 1;
    assert_true(md_test_count_increment(&count));
    assert_int_equal(count, INT64_MAX);
    assert_false(md_test_count_increment(&count));
    assert_int_equal(count, INT64_MAX);
    size_t bytes = 0, cells = SIZE_MAX / sizeof(MdValue);
    assert_true(md_test_aggregate_size(cells - 1, 1, &bytes));
    assert_int_equal(bytes, cells * sizeof(MdValue));
    assert_false(md_test_aggregate_size(cells, 1, &bytes));
    assert_false(md_test_aggregate_size(SIZE_MAX, 1, &bytes));
    assert_false(md_test_aggregate_size(0, cells + 1, &bytes));
    assert_false(md_test_aggregate_size(0, 0, &bytes));
}

static void m6_partial_copy_preserves_catalog(void **state) {
    (void)state;
    const char *faults[] = {"aggregate_key_after_one", "aggregate_output_after_one"};
    Fixture f = fixture();
    const unsigned char insert[] = "INSERT INTO users(id,name) VALUES(1,'owned');";
    Reader reader = {insert, sizeof insert - 1, 0, 1, false};
    MdRequest *request;
    MdDiagnostic d;
    MdEvent event = {0};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_event_destroy(&event);
    md_request_close(request);
    const unsigned char sql[] = "SELECT name,id,COUNT(*) FROM users GROUP BY name,id;";
    for (size_t i = 0; i < 2; i++) {
        reader = (Reader){sql, sizeof sql - 1, 0, 1, false};
        assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
        assert_int_equal(_putenv_s("MINIDB_TEST_FAULT", faults[i]), 0);
        MdNext next = md_request_next(request, &event);
        assert_int_equal(_putenv_s("MINIDB_TEST_FAULT", ""), 0);
        assert_int_equal(next, MD_NEXT_STOP);
        assert_string_equal(event.diagnostic.code, "RESOURCE_ERROR");
        assert_false(event.has_result);
        assert_null(event.result.values);
        assert_null(event.result.columns);
        assert_string_equal(f.db->tables[0].rows[1].as.text.ptr, "owned");
        md_event_destroy(&event);
        md_request_close(request);
        reader = (Reader){sql, sizeof sql - 1, 0, 1, false};
        assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
        assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
        assert_string_equal(event.result.values[0].as.text.ptr, "owned");
        md_event_destroy(&event);
        md_request_close(request);
    }
    fixture_close(&f);
}
#endif
static void m6_group_owned_text_and_null(void **state) {
    (void)state;
    Fixture f = fixture();
    const unsigned char sql[] = "INSERT INTO users(id,name) VALUES(1,'owned'),(2,'owned'),(3,NULL);"
        "SELECT name,COUNT(*) AS n FROM users GROUP BY name ORDER BY name;";
    Reader reader = {sql, sizeof sql - 1, 0, 1, false};
    MdRequest *request;
    MdDiagnostic d;
    MdEvent event = {0};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_event_destroy(&event);
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_request_close(request);
    md_close(f.db);
    f.db = NULL;
    assert_int_equal(event.result.row_count, 2);
    assert_string_equal(event.result.values[0].as.text.ptr, "owned");
    assert_int_equal(event.result.values[1].as.integer, 2);
    assert_int_equal(event.result.values[2].type, MD_NULL);
    md_event_destroy(&event);
    fixture_close(&f);
}
static void m7_union_owned_text_and_null(void **state) {
    (void)state;
    Fixture f = fixture();
    const unsigned char sql[] = "INSERT INTO users(id,name) VALUES(1,'owned'),(2,'owned');"
        "SELECT name FROM users UNION ALL VALUES('owned'),(NULL) UNION VALUES('owned') "
        "UNION ALL VALUES('owned') ORDER BY name OFFSET 0 ROWS FETCH FIRST 3 ROWS ONLY;";
    Reader reader = {sql, sizeof sql - 1, 0, 1, false};
    MdRequest *request;
    MdDiagnostic d;
    MdEvent event = {0};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_event_destroy(&event);
    assert_int_equal(md_request_next(request, &event), MD_NEXT_RESULT);
    md_request_close(request);
    md_close(f.db);
    f.db = NULL;
    assert_int_equal(event.result.row_count, 3);
    assert_string_equal(event.result.values[0].as.text.ptr, "owned");
    assert_string_equal(event.result.values[1].as.text.ptr, "owned");
    assert_ptr_not_equal(event.result.values[0].as.text.ptr, event.result.values[1].as.text.ptr);
    assert_int_equal(event.result.values[2].type, MD_NULL);
    md_event_destroy(&event);
    md_event_destroy(&event);
    fixture_close(&f);
}
static void m8_migration_owned_events_and_text(void **state) {
    (void)state;
    Fixture f = fixture();
    const unsigned char sql[] = "INSERT INTO users(id,name) VALUES(1,'owned'),(2,NULL);";
    Reader reader = {sql, sizeof sql - 1, 0, 1, false};
    MdRequest *request;
    MdDiagnostic d;
    MdEvent insert = {0}, first = {0}, second = {0}, failed = {0}, query = {0};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    assert_int_equal(md_request_next(request, &insert), MD_NEXT_RESULT);
    md_event_destroy(&insert);
    md_request_close(request);
    md_close(f.db);
    f.db = NULL;
    char *one = md_join(f.root, "migrate-one"), *two = md_join(f.root, "migrate-two");
    MdOperationOptions opts = {.operation = MD_MIGRATE, .schema_dir = f.schema,
                              .source_data_dir = f.data, .data_dir = one};
    assert_true(md_operate(&opts, &first));
    opts.data_dir = two;
    assert_true(md_operate(&opts, &second));
    assert_ptr_not_equal(first.result.tables[0].name, second.result.tables[0].name);
    opts.data_dir = one;
    assert_false(md_operate(&opts, &failed));
    md_event_destroy(&second);
    md_event_destroy(&failed);
    assert_string_equal(first.result.tables[0].name, "users");
    assert_string_equal(first.result.tables[0].action, "copied");
    assert_int_equal(first.result.tables[0].row_count, 2);
    assert_true(md_open(f.schema, one, &f.db, &d));
    const unsigned char select[] = "SELECT id,name FROM users ORDER BY id;";
    reader = (Reader){select, sizeof select - 1, 0, 1, false};
    assert_true(md_request_open(f.db, (MdInput){read_slice, &reader}, &request, &d));
    assert_int_equal(md_request_next(request, &query), MD_NEXT_RESULT);
    md_request_close(request);
    md_close(f.db);
    f.db = NULL;
    md_event_destroy(&first);
    assert_string_equal(query.result.values[1].as.text.ptr, "owned");
    assert_int_equal(query.result.values[3].type, MD_NULL);
    md_event_destroy(&query);
    md_event_destroy(&first);
    for (size_t i = 0; i < 2; i++) {
        const char *dir = i ? two : one;
        char *path = md_join(dir, "table_users.tbl");
        delete_file(path); free(path);
        path = md_join(dir, ".minidb.lock");
        delete_file(path); free(path);
        delete_directory(dir);
    }
    free(one); free(two);
    fixture_close(&f);
}
#ifdef MINIDB_TEST_FAULTS
static void m8_output_path_roots(void **state) {
    (void)state;
    const char *leaf = "m8-root-path-test-DO-NOT-CREATE";
    char *root = md_path("C:\\");
    assert_non_null(root);
    size_t size = strlen(root) + strlen(leaf) + 1;
    char *expected = malloc(size);
    assert_non_null(expected);
    snprintf(expected, size, "%s%s", root, leaf);
    const char *inputs[] = {"C:\\m8-root-path-test-DO-NOT-CREATE",
                            "\\\\?\\C:\\m8-root-path-test-DO-NOT-CREATE"};
    for (size_t i = 0; i < sizeof inputs / sizeof *inputs; i++) {
        /* 실제 resolver의 읽기 전용 실행이다. 루트에 디렉터리/파일을 만들지 않는다. */
        char *resolved = md_test_output_resolve(inputs[i]);
        assert_non_null(resolved);
        assert_string_equal(resolved, expected);
        free(resolved);
    }
    free(root);
    free(expected);
    const char *paths[] = {"C:\\new", "\\\\?\\C:\\new", "\\\\server\\share\\new",
                           "\\\\?\\UNC\\server\\share\\new", "C:\\directory\\new"};
    const char *parents[] = {"C:\\", "\\\\?\\C:\\", "\\\\server\\share\\",
                             "\\\\?\\UNC\\server\\share\\", "C:\\directory\\"};
    for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) {
        /* UNC 부모 분리는 순수 문자열 검사로 서버 접속 없이 실행한다. */
        char *parent = md_test_output_parent(paths[i]);
        assert_non_null(parent);
        assert_string_equal(parent, parents[i]);
        free(parent);
    }
}
static void m8_migration_boundaries(void **state) {
    (void)state;
    int64_t value;
    assert_true(md_test_migration_integer("-9223372036854775808", 20, &value));
    assert_int_equal(value, INT64_MIN);
    assert_true(md_test_migration_integer("+9223372036854775807", 20, &value));
    assert_int_equal(value, INT64_MAX);
    const char *invalid[] = {"9223372036854775808", "-9223372036854775809", "", " ", "+", "1 2", "1e2", "0x1", "1.0", "\v1", "1\f"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
        assert_false(md_test_migration_integer(invalid[i], strlen(invalid[i]), &value));
    assert_false(md_test_migration_integer("1\0tail", 6, &value));
    size_t bytes = SIZE_MAX;
    assert_true(md_test_migration_size(0, 1, &bytes));
    assert_int_equal(bytes, 0);
    assert_false(md_test_migration_size(1, 0, &bytes));
    size_t limit = SIZE_MAX / 3 / sizeof(MdValue);
    assert_true(md_test_migration_size(limit, 3, &bytes));
    assert_int_equal(bytes, limit * 3 * sizeof(MdValue));
    assert_false(md_test_migration_size(limit + 1, 3, &bytes));
    assert_false(md_test_migration_size(SIZE_MAX, SIZE_MAX, &bytes));
}
#endif
int main(void) {
    const struct CMUnitTest tests[] = {cmocka_unit_test(event_destroy_is_idempotent),
                                       cmocka_unit_test(byte_boundaries_and_owned_result),
                                       cmocka_unit_test(readahead_and_input_failure),
                                       cmocka_unit_test(m2_owned_values_and_aliases),
                                       cmocka_unit_test(m3_chunked_mutation_and_owned_result),
                                       cmocka_unit_test(m4_sorted_paged_owned_result),
                                       cmocka_unit_test(m5_join_owned_text_and_null),
                                       cmocka_unit_test(m5_join_checked_width),
                                       cmocka_unit_test(m6_group_owned_text_and_null),
                                       cmocka_unit_test(m7_union_owned_text_and_null),
                                       cmocka_unit_test(m8_migration_owned_events_and_text),
#ifdef MINIDB_TEST_FAULTS
                                       cmocka_unit_test(m8_output_path_roots),
                                       cmocka_unit_test(m8_migration_boundaries),
                                       cmocka_unit_test(m5_join_result_size_boundaries),
                                       cmocka_unit_test(m5_partial_copy_preserves_catalog),
                                       cmocka_unit_test(m6_count_and_size_boundaries),
                                       cmocka_unit_test(m7_union_size_boundaries),
                                       cmocka_unit_test(m7_union_failure_cleanup),
                                       cmocka_unit_test(m6_partial_copy_preserves_catalog),
#endif
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
