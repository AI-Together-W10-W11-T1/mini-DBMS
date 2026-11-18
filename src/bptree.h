#ifndef MD_BPTREE_H
#define MD_BPTREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef MD_BT_CHILDREN
#define MD_BT_CHILDREN 32
#endif
#define MD_BT_KEYS (MD_BT_CHILDREN - 1)

typedef struct MdBtNode MdBtNode;
struct MdBtNode {
    bool leaf;
    size_t count;
    int64_t keys[MD_BT_KEYS];
    size_t values[MD_BT_KEYS];
    MdBtNode *children[MD_BT_CHILDREN];
    MdBtNode *next;
};

/* 트리는 노드만 소유하며 행 번호를 복사한다. 0 초기화도 빈 트리다.
 * init은 새 객체에 사용한다. 사용 중인 객체의 초기화는 destroy로 한다. */
typedef struct { MdBtNode *root; size_t count; } MdBptree;
typedef struct { const MdBtNode *leaf; size_t index; } MdBtCursor;
#define MD_BT_BATCH_AVAILABLE 1
typedef struct { MdBtNode *nodes; size_t count, remaining; } MdBtBatch;
typedef enum { MD_BT_OK, MD_BT_DUPLICATE, MD_BT_NOMEM, MD_BT_INVALID } MdBtResult;

void md_bt_init(MdBptree *tree);
void md_bt_destroy(MdBptree *tree);
/* 성공한 변경과 destroy는 기존 커서를 무효화한다. 실패와 중복은 커서까지 보존한다.
 * 키는 양수 int64_t이며, 값은 0과 SIZE_MAX를 포함한 모든 size_t다.
 * 할당 실패 또는 표현 가능한 개수/경로 상한은 MD_BT_NOMEM을 반환한다. */
MdBtResult md_bt_insert(MdBptree *tree, int64_t key, size_t value);
/* 준비 성공 뒤 트리를 다른 경로로 변경하지 않으며, 양수 고유 키만 apply한다.
 * cancel은 미사용 예약 노드만 해제한다. apply는 할당·실패 없이 한 키를 반영한다. */
MdBtResult md_bt_prepare(const MdBptree *tree, size_t count, MdBtBatch *batch);
void md_bt_apply(MdBptree *tree, MdBtBatch *batch, int64_t key, size_t value);
void md_bt_cancel(MdBtBatch *batch);
/* 찾지 못하면 출력값을 보존한다. lower_bound는 임의의 int64_t를 받는다. */
bool md_bt_get(const MdBptree *tree, int64_t key, size_t *value);
void md_bt_lower_bound(const MdBptree *tree, int64_t key, MdBtCursor *cursor);
bool md_bt_next(MdBtCursor *cursor, int64_t *key, size_t *value);

#ifdef MD_BT_TEST
void md_bt_test_fail_at(size_t allocation);
size_t md_bt_test_calls(void);
size_t md_bt_test_live(void);
#endif
#endif
