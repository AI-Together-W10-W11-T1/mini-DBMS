#include "internal.h"
#include "bptree.h"
#include <cmocka.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void reserve_contract(void **state) {
    (void)state;
    MdBptree tree = {0};
    for (int64_t i = 1; i <= 2000; i++) assert_int_equal(md_bt_insert(&tree, i, (size_t)i), MD_BT_OK);
    size_t baseline = md_bt_test_live();
    for (size_t failure = 1;; failure++) {
        MdBtBatch batch = {0};
        md_bt_test_fail_at(failure);
        MdBtResult result = md_bt_prepare(&tree, 2000, &batch);
        if (result == MD_BT_OK) { md_bt_cancel(&batch); break; }
        assert_int_equal(result, MD_BT_NOMEM);
        assert_int_equal(tree.count, 2000);
        assert_int_equal(md_bt_test_live(), baseline);
    }
    md_bt_test_fail_at(0);
    MdBtBatch batch = {0};
    assert_int_equal(md_bt_prepare(&tree, 2000, &batch), MD_BT_OK);
    md_bt_test_fail_at(1);
    for (int64_t i = 2001; i <= 4000; i++) md_bt_apply(&tree, &batch, i, (size_t)i);
    assert_int_equal(md_bt_test_calls(), 0);
    md_bt_cancel(&batch);
    for (int64_t i = 1; i <= 4000; i++) { size_t row = 0; assert_true(md_bt_get(&tree, i, &row)); assert_int_equal(row, i); }
    md_bt_destroy(&tree);
    assert_int_equal(md_bt_test_live(), 0);
    md_bt_test_fail_at(0);
    tree.count = SIZE_MAX;
    assert_int_equal(md_bt_prepare(&tree, 1, &batch), MD_BT_NOMEM);
    tree.count = 0;
}

typedef struct { const char *sql; size_t position; } Reader;
static MdReadStatus read_sql(void *context, unsigned char *buffer, size_t capacity, size_t *length) {
    Reader *reader = context;
    size_t size = strlen(reader->sql) - reader->position;
    if (size > capacity) size = capacity;
    memcpy(buffer, reader->sql + reader->position, size);
    reader->position += size;
    *length = size;
    return size ? MD_READ_DATA : MD_READ_EOF;
}
static MdEvent execute(MdDb *db, const char *sql) {
    Reader reader = {sql, 0};
    MdRequest *request = NULL;
    MdDiagnostic diagnostic = {0};
    assert_true(md_request_open(db, (MdInput){.read = read_sql, .context = &reader}, &request, &diagnostic));
    MdEvent event = {0};
    assert_int_not_equal(md_request_next(request, &event), MD_NEXT_EOF);
    md_request_close(request);
    return event;
}
static void mapping(MdTable *table) {
    assert_int_equal(table->index.count, table->row_count);
    for (size_t i = 0; i < table->row_count; i++) {
        size_t row = SIZE_MAX;
        assert_true(md_bt_get(&table->index, table->rows[i * table->count + table->id].as.integer, &row));
        assert_int_equal(row, i);
    }
}
static void integration(const char *schema, const char *data, const char *kind) {
    MdDb *db = NULL;
    MdDiagnostic diagnostic = {0};
    md_bt_test_fail_at(0);
    if (!strcmp(kind, "startup")) {
        for (size_t failure = 1;; failure++) {
            md_bt_test_fail_at(failure);
            if (md_open(schema, data, &db, &diagnostic)) { md_close(db); db = NULL; break; }
            assert_string_equal(diagnostic.code, "RESOURCE_ERROR");
            assert_null(db);
            assert_int_equal(md_bt_test_live(), 0);
        }
        md_bt_test_fail_at(0);
    }
    assert_true(md_open(schema, data, &db, &diagnostic));
    MdTable *table = &db->tables[0];
    mapping(table);
    if (!strcmp(kind, "lookup")) {
        const char *queries[] = {"SELECT u.id FROM users AS u WHERE 3=u.id;", "SELECT id FROM users WHERE id=3 AND age IS NULL;",
            "SELECT id FROM users WHERE id=999;", "SELECT id FROM users WHERE id=1 OR id=9;"};
        for (size_t i = 0; i < 4; i++) {
            md_test_index_reset();
            MdEvent event = execute(db, queries[i]);
            assert_true(event.has_result);
            assert_int_equal(md_test_index_lookups(), i == 3 ? 0 : 1);
            assert_int_equal(md_test_scan_rows(), i == 3 ? 3 : i == 2 ? 0 : 1);
            md_event_destroy(&event);
        }
        const char *invalid[] = {"SELECT id FROM users WHERE id=999 AND age='invalid';",
            "SELECT id FROM users WHERE id=999 ORDER BY missing;",
            "SELECT id FROM users WHERE id=999 OFFSET 1 ROWS;",
            "SELECT id FROM users WHERE id=999 UNION SELECT missing FROM users;",
            "SELECT id FROM users WHERE id=999 UNION VALUES('wrong');",
            "SELECT id FROM users WHERE id=999 UNION SELECT id FROM users ORDER BY missing;"};
        const char *codes[] = {"TYPE_MISMATCH", "INVALID_ORDER_BY", "INVALID_PAGINATION", "UNKNOWN_COLUMN", "TYPE_MISMATCH", "INVALID_ORDER_BY"};
        for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
            md_test_index_reset();
            MdEvent event = execute(db, invalid[i]);
            assert_string_equal(event.diagnostic.code, codes[i]);
            assert_int_equal(md_test_index_lookups(), 0);
            assert_int_equal(md_test_scan_rows(), 0);
            md_event_destroy(&event);
        }
    } else if (!strcmp(kind, "update")) {
        MdBtNode *root = table->index.root;
        MdEvent event = execute(db, "UPDATE users SET name='again' WHERE id=2;");
        assert_true(event.has_result);
        assert_ptr_equal(table->index.root, root);
        mapping(table);
        md_event_destroy(&event);
        _putenv_s("MINIDB_TEST_FAULT", "replace_temp_write");
        event = execute(db, "UPDATE users SET name='fail' WHERE id=2;");
        assert_false(event.has_result);
        assert_ptr_equal(table->index.root, root);
        mapping(table);
        md_event_destroy(&event);
        _putenv_s("MINIDB_TEST_FAULT", "");
    } else if (!strcmp(kind, "delete")) {
        MdBtNode *root = table->index.root;
        size_t live = md_bt_test_live();
        for (size_t failure = 1;; failure++) {
            md_bt_test_fail_at(failure);
            MdEvent event = execute(db, "DELETE FROM users WHERE id=2;");
            if (event.has_result) { md_event_destroy(&event); break; }
            assert_string_equal(event.diagnostic.code, "RESOURCE_ERROR");
            assert_ptr_equal(table->index.root, root);
            assert_int_equal(table->row_count, 3);
            assert_int_equal(md_bt_test_live(), live);
            mapping(table);
            md_event_destroy(&event);
        }
        mapping(table);
        assert_int_equal(table->row_count, 2);
        md_bt_test_fail_at(0);
        _putenv_s("MINIDB_TEST_FAULT", "replace_backup_cleanup");
        MdEvent event = execute(db, "DELETE FROM users WHERE id=1;");
        assert_true(event.has_result);
        assert_true(event.stop);
        assert_int_equal(table->row_count, 1);
        mapping(table);
        md_event_destroy(&event);
        _putenv_s("MINIDB_TEST_FAULT", "");
    } else if (!strcmp(kind, "storage")) {
        MdBtNode *root = table->index.root;
        size_t live = md_bt_test_live();
        for (size_t failure = 1; failure <= 2; failure++) {
            md_bt_test_fail_at(failure);
            MdEvent event = execute(db, "INSERT INTO users(name) VALUES('a'),('b');");
            assert_false(event.has_result);
            assert_string_equal(event.diagnostic.code, "RESOURCE_ERROR");
            assert_ptr_equal(table->index.root, root);
            assert_int_equal(table->row_count, 2);
            assert_int_equal(md_bt_test_live(), live);
            mapping(table);
            md_event_destroy(&event);
        }
        md_bt_test_fail_at(0);
        _putenv_s("MINIDB_TEST_FAULT", "append_write");
        MdEvent event = execute(db, "INSERT INTO users(name) VALUES('a'),('b');");
        assert_false(event.has_result);
        assert_ptr_equal(table->index.root, root);
        assert_int_equal(table->row_count, 2);
        mapping(table);
        md_event_destroy(&event);
        _putenv_s("MINIDB_TEST_FAULT", "cleanup");
        event = execute(db, "INSERT INTO users(name) VALUES('a'),('b');");
        assert_true(event.has_result);
        assert_true(event.stop);
        assert_int_equal(event.result.affected_rows, 2);
        assert_int_equal(table->row_count, 4);
        mapping(table);
        md_event_destroy(&event);
        _putenv_s("MINIDB_TEST_FAULT", "");
    }
    md_bt_test_fail_at(0);
    md_close(db);
    assert_int_equal(md_bt_test_live(), 0);
}

int main(int argc, char **argv) {
    if (argc == 4) { integration(argv[1], argv[2], argv[3]); return 0; }
    const struct CMUnitTest tests[] = {cmocka_unit_test(reserve_contract)};
    return cmocka_run_group_tests(tests, NULL, NULL);
}
