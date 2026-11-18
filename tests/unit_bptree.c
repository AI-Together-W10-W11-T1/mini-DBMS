#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bptree.h"

#ifdef MD_BT_RUNTIME
/* 실패 주입 매크로 없이 engine에 들어간 실제 제품 오브젝트를 직접 실행한다. */
static void production_runtime(void **state) {
    (void)state;
    MdBptree tree = {0};
    for (int64_t i = 20000; i > 0; i--) {
        assert_int_equal(md_bt_insert(&tree, i, (size_t)i - 1), MD_BT_OK);
    }
    assert_int_equal(tree.count, 20000);
    assert_false(tree.root->leaf);
    assert_false(tree.root->children[0]->leaf);
    assert_false(tree.root->children[0]->children[0]->leaf);
    MdBtCursor cursor;
    md_bt_lower_bound(&tree, INT64_MIN, &cursor);
    for (int64_t i = 1; i <= 20000; i++) {
        int64_t key;
        size_t value;
        assert_true(md_bt_get(&tree, i, &value));
        assert_int_equal(value, (size_t)i - 1);
        assert_true(md_bt_next(&cursor, &key, &value));
        assert_int_equal(key, i);
        assert_int_equal(value, (size_t)i - 1);
    }
    int64_t key = -1;
    size_t value = SIZE_MAX;
    assert_false(md_bt_next(&cursor, &key, &value));
    assert_int_equal(key, -1);
    assert_true(value == SIZE_MAX);
    assert_int_equal(md_bt_insert(&tree, 1, 99), MD_BT_DUPLICATE);
    assert_int_equal(md_bt_insert(&tree, 0, 99), MD_BT_INVALID);
    md_bt_destroy(&tree);
    assert_null(tree.root);
    assert_int_equal(tree.count, 0);
    md_bt_destroy(&tree);
}

int main(void) {
    const struct CMUnitTest tests[] = {cmocka_unit_test(production_runtime)};
    return cmocka_run_group_tests(tests, NULL, NULL);
}
#else

/* 구현의 탐색 함수를 재사용하지 않는 구조/연결 불변식 검사다. */
typedef struct {
    const MdBtNode *previous;
    size_t depth;
    size_t count;
    int64_t last;
} Audit;

static int64_t inspect(const MdBtNode *node, bool root, size_t depth, Audit *audit) {
    assert_non_null(node);
    assert_true(depth < 64);
    assert_true(node->count > 0 && node->count <= MD_BT_KEYS);
    if (!root) {
        size_t minimum = node->leaf ? (MD_BT_KEYS + 1) / 2 : MD_BT_CHILDREN / 2 - 1;
        assert_true(node->count >= minimum);
    }
    for (size_t i = 1; i < node->count; i++) assert_true(node->keys[i - 1] < node->keys[i]);
    if (node->leaf) {
        if (audit->previous) {
            assert_ptr_equal(audit->previous->next, node);
            assert_true(audit->last < node->keys[0]);
            assert_int_equal(audit->depth, depth);
        } else audit->depth = depth;
        audit->previous = node;
        audit->last = node->keys[node->count - 1];
        audit->count += node->count;
        assert_true(node->keys[0] > 0);
        return node->keys[0];
    }
    int64_t minimum = inspect(node->children[0], false, depth + 1, audit);
    for (size_t i = 1; i <= node->count; i++) {
        int64_t right = inspect(node->children[i], false, depth + 1, audit);
        assert_int_equal(node->keys[i - 1], right);
    }
    return minimum;
}

static void valid(const MdBptree *tree) {
    Audit audit = {0};
    if (tree->count == 0) { assert_null(tree->root); return; }
    (void)inspect(tree->root, true, 0, &audit);
    assert_int_equal(audit.count, tree->count);
    assert_null(audit.previous->next);
}

static size_t row(int64_t key) { return (size_t)key * 7; }
static void add(MdBptree *tree, int64_t key) {
    assert_int_equal(md_bt_insert(tree, key, row(key)), MD_BT_OK);
}

static void sequential(const MdBptree *tree, size_t count) {
    valid(tree);
    assert_int_equal(tree->count, count);
    MdBtCursor cursor;
    md_bt_lower_bound(tree, INT64_MIN, &cursor);
    for (size_t i = 1; i <= count; i++) {
        int64_t key = -99;
        size_t value = SIZE_MAX;
        assert_true(md_bt_get(tree, (int64_t)i, &value));
        assert_int_equal(value, row((int64_t)i));
        assert_true(md_bt_next(&cursor, &key, &value));
        assert_int_equal(key, i);
        assert_int_equal(value, row((int64_t)i));
    }
    int64_t key = -99;
    size_t value = 123;
    assert_false(md_bt_next(&cursor, &key, &value));
    assert_int_equal(key, -99);
    assert_int_equal(value, 123);
    assert_false(md_bt_get(tree, (int64_t)count + 1, &value));
    assert_int_equal(value, 123);
}

static void empty(void **state) {
    (void)state;
    MdBptree tree = {0};
    sequential(&tree, 0);
    md_bt_destroy(&tree);
    md_bt_destroy(&tree);
    valid(&tree);
    md_bt_init(&tree);
    assert_int_equal(md_bt_test_live(), 0);
}

static void insert(void **state) {
    (void)state;
    MdBptree tree = {0};
    assert_int_equal(md_bt_insert(&tree, 1, 0), MD_BT_OK);
    size_t value = 99;
    assert_true(md_bt_get(&tree, 1, &value));
    assert_int_equal(value, 0);
    for (int64_t i = 2; i <= 100; i++) add(&tree, i);
    valid(&tree);
    assert_int_equal(tree.count, 100);
    for (int64_t i = 2; i <= 100; i++) {
        assert_true(md_bt_get(&tree, i, &value));
        assert_int_equal(value, row(i));
    }
    md_bt_destroy(&tree);
}

static uint32_t random_step(uint32_t *seed) {
    *seed = *seed * UINT32_C(1664525) + UINT32_C(1013904223);
    return *seed;
}

static void order(void **state) {
    (void)state;
    int64_t keys[1200];
    for (int mode = 0; mode < 3; mode++) {
        MdBptree tree = {0};
        for (size_t i = 0; i < 1200; i++) keys[i] = mode == 1 ? 1200 - (int64_t)i : (int64_t)i + 1;
        if (mode == 2) {
            uint32_t seed = 20261118;
            for (size_t i = 1199; i > 0; i--) {
                size_t j = random_step(&seed) % (i + 1);
                int64_t swap = keys[i]; keys[i] = keys[j]; keys[j] = swap;
            }
        }
        for (size_t i = 0; i < 1200; i++) { add(&tree, keys[i]); valid(&tree); }
        sequential(&tree, 1200);
        md_bt_destroy(&tree);
    }
}

static void duplicate(void **state) {
    (void)state;
    MdBptree tree = {0};
    for (int64_t i = 1; i <= 1000; i++) add(&tree, i);
    MdBtCursor cursor;
    md_bt_lower_bound(&tree, 1, &cursor);
    for (int64_t i = 1; i <= 1000; i++) {
        md_bt_test_fail_at(1);
        assert_int_equal(md_bt_insert(&tree, i, SIZE_MAX), MD_BT_DUPLICATE);
        assert_int_equal(md_bt_test_calls(), 0);
        valid(&tree);
    }
    md_bt_test_fail_at(0);
    int64_t key; size_t value;
    assert_true(md_bt_next(&cursor, &key, &value));
    assert_int_equal(key, 1);
    assert_int_equal(value, row(1));
    sequential(&tree, 1000);
    md_bt_destroy(&tree);
}

static void split_leaf(void **state) {
    (void)state;
    MdBptree tree = {0};
    for (int64_t i = 1; i <= MD_BT_KEYS; i++) add(&tree, i);
    assert_true(tree.root->leaf);
    add(&tree, MD_BT_KEYS + 1);
    assert_false(tree.root->leaf);
    assert_int_equal(tree.root->count, 1);
    assert_ptr_equal(tree.root->children[0]->next, tree.root->children[1]);
    sequential(&tree, MD_BT_KEYS + 1);
    md_bt_destroy(&tree);
}

static size_t height(const MdBptree *tree) {
    size_t result = 0;
    const MdBtNode *node = tree->root;
    while (node) { result++; node = node->leaf ? NULL : node->children[0]; }
    return result;
}

static void split_internal(void **state) {
    (void)state;
    MdBptree tree = {0};
    size_t count = 0;
    while (height(&tree) < 4) {
        assert_true(count < 20000);
        add(&tree, (int64_t)++count);
        valid(&tree);
    }
    sequential(&tree, count);
    md_bt_destroy(&tree);
}

static void range(void **state) {
    (void)state;
    MdBptree tree = {0};
    for (int64_t i = 2; i <= 200; i += 2) add(&tree, i);
    for (int64_t request = -1; request <= 203; request++) {
        MdBtCursor cursor;
        md_bt_lower_bound(&tree, request, &cursor);
        int64_t first = request < 2 ? 2 : request + request % 2;
        for (int64_t expected = first; expected <= 200; expected += 2) {
            int64_t key; size_t value;
            assert_true(md_bt_next(&cursor, &key, &value));
            assert_int_equal(key, expected);
            assert_int_equal(value, row(expected));
        }
        int64_t key = -8; size_t value = 19;
        assert_false(md_bt_next(&cursor, &key, &value));
        assert_false(md_bt_next(&cursor, &key, &value));
        assert_int_equal(key, -8); assert_int_equal(value, 19);
    }
    valid(&tree);
    md_bt_destroy(&tree);
}

static void boundary(void **state) {
    (void)state;
    MdBptree tree = {0};
    assert_int_equal(md_bt_insert(&tree, 1, 0), MD_BT_OK);
    assert_int_equal(md_bt_insert(&tree, INT64_MAX, SIZE_MAX), MD_BT_OK);
    assert_int_equal(md_bt_insert(&tree, 0, 5), MD_BT_INVALID);
    assert_int_equal(md_bt_insert(&tree, -1, 5), MD_BT_INVALID);
    assert_int_equal(md_bt_insert(&tree, INT64_MIN, 5), MD_BT_INVALID);
    size_t value = 5;
    assert_false(md_bt_get(&tree, 0, &value)); assert_int_equal(value, 5);
    assert_false(md_bt_get(&tree, INT64_MIN, &value)); assert_int_equal(value, 5);
    assert_true(md_bt_get(&tree, INT64_MAX, &value)); assert_true(value == SIZE_MAX);
    MdBtCursor cursor;
    md_bt_lower_bound(&tree, INT64_MIN, &cursor);
    int64_t key;
    assert_true(md_bt_next(&cursor, &key, &value)); assert_int_equal(key, 1); assert_int_equal(value, 0);
    assert_true(md_bt_next(&cursor, &key, &value)); assert_int_equal(key, INT64_MAX); assert_true(value == SIZE_MAX);
    md_bt_lower_bound(&tree, INT64_MAX, &cursor);
    assert_true(md_bt_next(&cursor, &key, &value)); assert_int_equal(key, INT64_MAX);
    valid(&tree); assert_int_equal(tree.count, 2);
    tree.count = SIZE_MAX;
    assert_int_equal(md_bt_insert(&tree, 2, 5), MD_BT_NOMEM);
    assert_true(tree.count == SIZE_MAX);
    tree.count = 2;
    valid(&tree);
    md_bt_destroy(&tree);
}

/* 오름차순 다음 삽입의 꽉 찬 경로로 실제 준비 할당 수를 예측한다. */
static size_t next_allocations(const MdBptree *tree) {
    if (!tree->root) return 1;
    const MdBtNode *path[64]; size_t depth = 0;
    const MdBtNode *node = tree->root;
    while (!node->leaf) { path[depth++] = node; node = node->children[node->count]; }
    if (node->count < MD_BT_KEYS) return 0;
    size_t count = 1;
    while (depth > 0) {
        node = path[--depth];
        if (node->count < MD_BT_KEYS) return count;
        count++;
    }
    return count + 1;
}

static void fail_each(MdBptree *tree, size_t allocations) {
    size_t count = tree->count;
    size_t live = md_bt_test_live();
    for (size_t fail = 1; fail <= allocations; fail++) {
        MdBtCursor cursor;
        md_bt_lower_bound(tree, INT64_MIN, &cursor);
        md_bt_test_fail_at(fail);
        assert_int_equal(md_bt_insert(tree, (int64_t)count + 1, row((int64_t)count + 1)), MD_BT_NOMEM);
        assert_int_equal(md_bt_test_calls(), fail);
        assert_int_equal(md_bt_test_live(), live);
        sequential(tree, count);
        if (count) {
            int64_t key; size_t value;
            assert_true(md_bt_next(&cursor, &key, &value));
            assert_int_equal(key, 1); assert_int_equal(value, row(1));
        }
    }
    md_bt_test_fail_at(0);
    add(tree, (int64_t)count + 1);
    assert_int_equal(md_bt_test_calls(), allocations);
    printf("insert previous=%zu allocations=%zu failed each position and retried\n", count, allocations);
    sequential(tree, count + 1);
}

static void failure_basic(void **state) {
    (void)state;
    MdBptree tree = {0};
    fail_each(&tree, 1);
    md_bt_test_fail_at(1);
    add(&tree, 2);
    assert_int_equal(md_bt_test_calls(), 0);
    sequential(&tree, 2);
    md_bt_test_fail_at(0);
    md_bt_destroy(&tree);
    assert_int_equal(md_bt_test_live(), 0);
}

static void failure_splits(void **state) {
    (void)state;
    MdBptree tree = {0};
    bool covered[5] = {false};
    while (!covered[4]) {
        size_t allocations = next_allocations(&tree);
        assert_true(tree.count < 20000);
        if (allocations > 0 && allocations <= 4 && !covered[allocations] && tree.count) {
            fail_each(&tree, allocations);
            covered[allocations] = true;
        } else add(&tree, (int64_t)tree.count + 1);
    }
    for (size_t i = 1; i <= 4; i++) assert_true(covered[i]);
    md_bt_destroy(&tree);
    assert_int_equal(md_bt_test_live(), 0);
}

static void ownership(void **state) {
    (void)state;
    MdBptree first = {0}, second = {0};
    size_t source[100];
    for (size_t i = 0; i < 100; i++) {
        source[i] = i;
        assert_int_equal(md_bt_insert(&first, (int64_t)i + 1, source[i]), MD_BT_OK);
        assert_int_equal(md_bt_insert(&second, (int64_t)i + 1, SIZE_MAX - i), MD_BT_OK);
        source[i] = 789;
        size_t value = SIZE_MAX;
        assert_true(md_bt_get(&first, (int64_t)i + 1, &value));
        assert_int_equal(value, i);
    }
    md_bt_destroy(&first); md_bt_destroy(&first);
    for (size_t i = 0; i < 100; i++) {
        size_t value;
        assert_int_equal(source[i], 789);
        assert_true(md_bt_get(&second, (int64_t)i + 1, &value)); assert_true(value == SIZE_MAX - i);
    }
    add(&first, 1); sequential(&first, 1);
    md_bt_destroy(&second); md_bt_destroy(&first);
    assert_int_equal(md_bt_test_live(), 0);
}

static void permutation(int64_t *keys, size_t at) {
    if (at == 6) {
        MdBptree tree = {0};
        bool seen[7] = {false};
        for (size_t i = 0; i < 6; i++) {
            add(&tree, keys[i]); seen[keys[i]] = true; valid(&tree);
            for (int64_t k = 1; k <= 6; k++) {
                size_t value = 123;
                assert_int_equal(md_bt_get(&tree, k, &value), seen[k]);
                assert_int_equal(value, seen[k] ? row(k) : 123);
            }
            assert_int_equal(md_bt_insert(&tree, keys[i], 99), MD_BT_DUPLICATE);
            valid(&tree);
        }
        sequential(&tree, 6); md_bt_destroy(&tree); return;
    }
    for (size_t i = at; i < 6; i++) {
        int64_t swap = keys[at]; keys[at] = keys[i]; keys[i] = swap;
        permutation(keys, at + 1);
        swap = keys[at]; keys[at] = keys[i]; keys[i] = swap;
    }
}

static void invariant(void **state) {
    (void)state;
    int64_t keys[6] = {1, 2, 3, 4, 5, 6};
    permutation(keys, 0);
    order(state);
    failure_splits(state);
    assert_int_equal(md_bt_test_live(), 0);
}

int main(int argc, char **argv) {
    const char *names[] = {"M9-EMPTY-01", "M9-INSERT-01", "M9-ORDER-01", "M9-DUPLICATE-01",
        "M9-SPLIT-01", "M9-SPLIT-02", "M9-RANGE-01", "M9-BOUNDARY-01", "M9-FAILURE-01",
        "M9-FAILURE-02", "M9-OWNERSHIP-01", "M9-INVARIANT-01"};
    const CMUnitTestFunction functions[] = {empty, insert, order, duplicate, split_leaf, split_internal,
        range, boundary, failure_basic, failure_splits, ownership, invariant};
    if (argc < 2) return 2;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(argv[1], names[i]) != 0) continue;
        struct CMUnitTest test = {names[i], functions[i], NULL, NULL, NULL};
        printf("B+tree capacity children=%d leaf=%d\n", MD_BT_CHILDREN, MD_BT_KEYS);
        int result = _cmocka_run_group_tests(names[i], &test, 1, NULL, NULL);
        if (argc >= 3) {
            const char *arguments[] = {argv[2], argv[1], NULL};
            intptr_t peer = _spawnv(_P_WAIT, argv[2], arguments);
            if (peer != 0) result = 1;
        }
        if (argc == 4) {
            const char *arguments[] = {argv[3], NULL};
            intptr_t runtime = _spawnv(_P_WAIT, argv[3], arguments);
            if (runtime != 0) result = 1;
        }
        return result;
    }
    return 2;
}
#endif
