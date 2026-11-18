#include "bptree.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static_assert(MD_BT_CHILDREN == 4 || MD_BT_CHILDREN == 32);
/* 리프 깊이 d>=1이면 최소 분기 2와 리프 최소 2개로 키가 2^(d+1)개 필요하다.
 * 양수 INT64 고유 키는 최대 2^63-1개이므로 유효 트리 높이는 64 미만이다.
 * 경로 배열은 64단계를 허용하고, 탐색 시에도 경계를 검사한다. */
#define MD_BT_HEIGHT 64

#ifdef MD_BT_TEST
static size_t allocation_calls;
static size_t allocation_failure;
static size_t live_nodes;
void md_bt_test_fail_at(size_t allocation) {
    allocation_calls = 0;
    allocation_failure = allocation;
}
size_t md_bt_test_calls(void) { return allocation_calls; }
size_t md_bt_test_live(void) { return live_nodes; }
#endif

static MdBtNode *new_node(void) {
#ifdef MD_BT_TEST
    allocation_calls++;
    if (allocation_failure && allocation_calls == allocation_failure) return NULL;
#endif
    MdBtNode *node = calloc(1, sizeof(*node));
#ifdef MD_BT_TEST
    if (node) live_nodes++;
#endif
    return node;
}

static void free_node(MdBtNode *node) {
#ifdef MD_BT_TEST
    live_nodes--;
#endif
    free(node);
}

void md_bt_init(MdBptree *tree) { *tree = (MdBptree){0}; }

static void destroy_node(MdBtNode *node) {
    if (!node->leaf) {
        for (size_t i = 0; i <= node->count; i++) destroy_node(node->children[i]);
    }
    free_node(node);
}

void md_bt_destroy(MdBptree *tree) {
    if (tree->root) destroy_node(tree->root);
    md_bt_init(tree);
}

static size_t leaf_position(const MdBtNode *node, int64_t key) {
    size_t low = 0, high = node->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (node->keys[middle] < key) low = middle + 1;
        else high = middle;
    }
    return low;
}

static size_t child_position(const MdBtNode *node, int64_t key) {
    size_t low = 0, high = node->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (node->keys[middle] <= key) low = middle + 1;
        else high = middle;
    }
    return low;
}

static const MdBtNode *find_leaf(const MdBptree *tree, int64_t key) {
    const MdBtNode *node = tree->root;
    while (node && !node->leaf) node = node->children[child_position(node, key)];
    return node;
}

bool md_bt_get(const MdBptree *tree, int64_t key, size_t *value) {
    const MdBtNode *leaf = find_leaf(tree, key);
    if (!leaf) return false;
    size_t position = leaf_position(leaf, key);
    if (position == leaf->count || leaf->keys[position] != key) return false;
    *value = leaf->values[position];
    return true;
}

void md_bt_lower_bound(const MdBptree *tree, int64_t key, MdBtCursor *cursor) {
    const MdBtNode *leaf = find_leaf(tree, key);
    size_t position = leaf ? leaf_position(leaf, key) : 0;
    if (leaf && position == leaf->count) { leaf = leaf->next; position = 0; }
    *cursor = (MdBtCursor){leaf, position};
}

bool md_bt_next(MdBtCursor *cursor, int64_t *key, size_t *value) {
    if (!cursor->leaf) return false;
    *key = cursor->leaf->keys[cursor->index];
    *value = cursor->leaf->values[cursor->index];
    cursor->index++;
    if (cursor->index == cursor->leaf->count) {
        cursor->leaf = cursor->leaf->next;
        cursor->index = 0;
    }
    return true;
}

static void insert_leaf(MdBtNode *leaf, size_t position, int64_t key, size_t value) {
    size_t remaining = leaf->count - position;
    memmove(leaf->keys + position + 1, leaf->keys + position, remaining * sizeof(*leaf->keys));
    memmove(leaf->values + position + 1, leaf->values + position, remaining * sizeof(*leaf->values));
    leaf->keys[position] = key;
    leaf->values[position] = value;
    leaf->count++;
}

static void split_leaf(MdBtNode *leaf, MdBtNode *right, size_t position, int64_t key, size_t value) {
    int64_t keys[MD_BT_KEYS + 1];
    size_t values[MD_BT_KEYS + 1];
    for (size_t i = 0; i < MD_BT_KEYS + 1; i++) {
        if (i == position) { keys[i] = key; values[i] = value; }
        else {
            size_t source = i < position ? i : i - 1;
            keys[i] = leaf->keys[source]; values[i] = leaf->values[source];
        }
    }
    leaf->count = (MD_BT_KEYS + 1) / 2;
    right->leaf = true;
    right->count = MD_BT_KEYS + 1 - leaf->count;
    memcpy(leaf->keys, keys, leaf->count * sizeof(*keys));
    memcpy(leaf->values, values, leaf->count * sizeof(*values));
    memcpy(right->keys, keys + leaf->count, right->count * sizeof(*keys));
    memcpy(right->values, values + leaf->count, right->count * sizeof(*values));
    right->next = leaf->next;
    leaf->next = right;
}

static int64_t subtree_minimum(const MdBtNode *node) {
    while (!node->leaf) node = node->children[0];
    return node->keys[0];
}

static void set_children(MdBtNode *node, MdBtNode *const *children, size_t count) {
    node->count = count - 1;
    memcpy(node->children, children, count * sizeof(*children));
    /* 구분 키는 항상 오른쪽 서브트리의 최소 키를 복사한다. */
    for (size_t i = 1; i < count; i++) node->keys[i - 1] = subtree_minimum(children[i]);
}

static MdBtResult insert(MdBptree *tree, int64_t key, size_t value, MdBtBatch *batch) {
    if (key <= 0) return MD_BT_INVALID;
    MdBtNode *path[MD_BT_HEIGHT];
    size_t positions[MD_BT_HEIGHT];
    size_t depth = 0;
    MdBtNode *leaf = tree->root;
    while (leaf && !leaf->leaf) {
        if (depth == MD_BT_HEIGHT) return MD_BT_NOMEM;
        path[depth] = leaf;
        positions[depth] = child_position(leaf, key);
        leaf = leaf->children[positions[depth++]];
    }
    size_t position = leaf ? leaf_position(leaf, key) : 0;
    if (leaf && position < leaf->count && leaf->keys[position] == key) return MD_BT_DUPLICATE;
    if (tree->count == SIZE_MAX) return MD_BT_NOMEM;
    if (leaf && leaf->count < MD_BT_KEYS) {
        insert_leaf(leaf, position, key, value);
        tree->count++;
        return MD_BT_OK;
    }

    size_t needed = 1;
    if (leaf) {
        size_t level = depth;
        while (level > 0 && path[level - 1]->count == MD_BT_KEYS) { needed++; level--; }
        if (level == 0) needed++;
    }
    MdBtNode *prepared[MD_BT_HEIGHT + 2];
    /* 모든 실패 가능한 할당을 먼저 끝낸다. 공개된 노드는 아직 변경하지 않는다. */
    for (size_t i = 0; i < needed; i++) {
        if (batch) {
            assert(batch->nodes);
            prepared[i] = batch->nodes;
            batch->nodes = prepared[i]->next;
            batch->count--;
            memset(prepared[i], 0, sizeof *prepared[i]);
        } else prepared[i] = new_node();
        if (!prepared[i]) {
            while (i > 0) free_node(prepared[--i]);
            return MD_BT_NOMEM;
        }
    }
    if (!leaf) {
        leaf = prepared[0];
        leaf->leaf = true;
        insert_leaf(leaf, 0, key, value);
        tree->root = leaf;
        tree->count++;
        return MD_BT_OK;
    }

    size_t used = 0;
    MdBtNode *right = prepared[used++];
    split_leaf(leaf, right, position, key, value);
    /* 이후는 여유 한 칸을 가진 지역 배열과 예약 노드만 사용하므로 실패하지 않는다. */
    while (depth > 0) {
        depth--;
        MdBtNode *parent = path[depth];
        size_t slot = positions[depth] + 1;
        MdBtNode *children[MD_BT_CHILDREN + 1];
        size_t count = parent->count + 2;
        for (size_t i = 0; i < count; i++) {
            children[i] = i == slot ? right : parent->children[i < slot ? i : i - 1];
        }
        if (count <= MD_BT_CHILDREN) {
            set_children(parent, children, count);
            tree->count++;
            return MD_BT_OK;
        }
        right = prepared[used++];
        size_t left_count = count / 2;
        set_children(parent, children, left_count);
        set_children(right, children + left_count, count - left_count);
    }
    MdBtNode *root = prepared[used];
    MdBtNode *children[] = {tree->root, right};
    set_children(root, children, 2);
    tree->root = root;
    tree->count++;
    return MD_BT_OK;
}

MdBtResult md_bt_insert(MdBptree *tree, int64_t key, size_t value) {
    return insert(tree, key, value, NULL);
}

void md_bt_cancel(MdBtBatch *batch) {
    while (batch->nodes) {
        MdBtNode *node = batch->nodes;
        batch->nodes = node->next;
        free_node(node);
    }
    *batch = (MdBtBatch){0};
}

MdBtResult md_bt_prepare(const MdBptree *tree, size_t count, MdBtBatch *batch) {
    *batch = (MdBtBatch){0};
    if (!count || count > SIZE_MAX - tree->count) return MD_BT_NOMEM;
    size_t total = tree->count + count;
    size_t height = 1;
    for (size_t n = total; n > 1; n /= 2) height++;
    if (height >= MD_BT_HEIGHT || count > SIZE_MAX / (height + 1)) return MD_BT_NOMEM;
    size_t needed = count * (height + 1);
    /* 각 삽입의 전 경로 분할 상한과 최종 트리 전체 노드 상한 중 작은 값.
     * 루트를 제외한 리프/내부 노드의 최소 점유를 사용하므로 대량 배치도 제한된다. */
    size_t leaves = total / ((MD_BT_KEYS + 1) / 2);
    if (!leaves) leaves = 1;
    size_t internal = 1 + (leaves - 1) / (MD_BT_CHILDREN / 2 - 1);
    if (leaves <= SIZE_MAX - internal && needed > leaves + internal) needed = leaves + internal;
    for (size_t i = 0; i < needed; i++) {
        MdBtNode *node = new_node();
        if (!node) { md_bt_cancel(batch); return MD_BT_NOMEM; }
        node->next = batch->nodes;
        batch->nodes = node;
        batch->count++;
    }
    batch->remaining = count;
    return MD_BT_OK;
}

void md_bt_apply(MdBptree *tree, MdBtBatch *batch, int64_t key, size_t value) {
    assert(batch->remaining);
    MdBtResult result = insert(tree, key, value, batch);
    assert(result == MD_BT_OK);
    (void)result;
    batch->remaining--;
}
