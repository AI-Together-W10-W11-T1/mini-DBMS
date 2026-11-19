#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TK_END, TK_WORD, TK_NUMBER, TK_STRING, TK_PUNCT, TK_BAD };
enum { CMP_NONE, CMP_EQ, CMP_NE, CMP_LT, CMP_LE, CMP_GT, CMP_GE };
typedef struct {
    const unsigned char *raw;
    size_t len, pos, start;
    int kind;
    char punct;
    int comparison;
    char *text;
    size_t text_len;
    const char *error;
} Lexer;
static bool space(unsigned c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}
static bool alpha(unsigned c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static bool digit(unsigned c) { return c >= '0' && c <= '9'; }
static void next(Lexer *l) {
    free(l->text);
    l->text = NULL;
    l->text_len = 0;
    l->kind = TK_END;
    l->comparison = CMP_NONE;
    md_execution_point("lexer");
    const char *cancel = md_execution_check();
    if (cancel) {
        l->kind = TK_BAD;
        l->error = cancel;
        return;
    }
    for (;;) {
        if (md_execution_check()) {
            l->kind = TK_BAD;
            l->error = md_execution_check();
            return;
        }
        while (!md_execution_check() && l->pos < l->len && space(l->raw[l->pos]))
            l->pos++;
        if (l->pos + 1 < l->len && l->raw[l->pos] == '-' && l->raw[l->pos + 1] == '-') {
            l->pos += 2;
            while (!md_execution_check() && l->pos < l->len && l->raw[l->pos] != '\r' && l->raw[l->pos] != '\n')
                l->pos++;
            continue;
        }
        if (l->pos + 1 < l->len && l->raw[l->pos] == '/' && l->raw[l->pos + 1] == '*') {
            l->pos += 2;
            while (!md_execution_check() && l->pos + 1 < l->len && !(l->raw[l->pos] == '*' && l->raw[l->pos + 1] == '/'))
                l->pos++;
            if (l->pos + 1 >= l->len) {
                l->kind = TK_BAD;
                l->error = "LEXICAL_ERROR";
                return;
            }
            l->pos += 2;
            continue;
        }
        break;
    }
    l->start = l->pos;
    if (l->pos == l->len)
        return;
    unsigned c = l->raw[l->pos++];
    if (alpha(c)) {
        while (!md_execution_check() && l->pos < l->len && (alpha(l->raw[l->pos]) || digit(l->raw[l->pos])))
            l->pos++;
        l->kind = TK_WORD;
        l->text_len = l->pos - l->start;
        l->text = md_copy((char *)l->raw + l->start, l->text_len);
    } else if (digit(c)) {
        while (!md_execution_check() && l->pos < l->len && digit(l->raw[l->pos]))
            l->pos++;
        l->kind = TK_NUMBER;
        l->text_len = l->pos - l->start;
        l->text = md_copy((char *)l->raw + l->start, l->text_len);
    } else if (c == '\'') {
        char *p = malloc(l->len - l->start);
        if (!p) {
            l->kind = TK_BAD;
            l->error = "RESOURCE_ERROR";
            return;
        }
        size_t n = 0;
        bool closed = false;
        while (!md_execution_check() && l->pos < l->len) {
            c = l->raw[l->pos++];
            if (c == '\'') {
                if (l->pos < l->len && l->raw[l->pos] == '\'') {
                    l->pos++;
                    p[n++] = '\'';
                } else {
                    closed = true;
                    break;
                }
            } else
                p[n++] = (char)c;
        }
        if (!closed) {
            free(p);
            l->kind = TK_BAD;
            l->error = "LEXICAL_ERROR";
            return;
        }
        p[n] = 0;
        l->kind = TK_STRING;
        l->text = p;
        l->text_len = n;
    } else if (strchr("(),;*=.<>+-/", c)) {
        l->kind = TK_PUNCT;
        l->punct = (char)c;
        if (c == '=')
            l->comparison = CMP_EQ;
        else if (c == '<' || c == '>') {
            l->comparison = c == '<' ? CMP_LT : CMP_GT;
            if (l->pos < l->len && l->raw[l->pos] == '=') {
                l->pos++;
                l->comparison = c == '<' ? CMP_LE : CMP_GE;
            } else if (c == '<' && l->pos < l->len && l->raw[l->pos] == '>') {
                l->pos++;
                l->comparison = CMP_NE;
            }
        }
    } else {
        l->kind = TK_BAD;
        l->error = "LEXICAL_ERROR";
        return;
    }
    if ((l->kind == TK_WORD || l->kind == TK_NUMBER) && !l->text) {
        l->kind = TK_BAD;
        l->error = "RESOURCE_ERROR";
    }
}
static bool word(Lexer *l, const char *s) { return l->kind == TK_WORD && md_equal(l->text, s); }
static bool punct(Lexer *l, char c) { return l->kind == TK_PUNCT && l->punct == c; }
static bool take_word(Lexer *l, const char *s) {
    if (!word(l, s))
        return false;
    next(l);
    return true;
}
static bool take_punct(Lexer *l, char c) {
    if (!punct(l, c))
        return false;
    next(l);
    return true;
}
static const char *bad(Lexer *l) { return l->kind == TK_BAD ? l->error : "SYNTAX_ERROR"; }
static bool literal(Lexer *l, MdValue *v, const char **error) {
    memset(v, 0, sizeof *v);
    bool signed_number = punct(l, '-') || punct(l, '+');
    bool negative = punct(l, '-');
    if (signed_number) {
        size_t number_start = l->pos;
        next(l);
        if (l->kind != TK_NUMBER || l->start != number_start) {
            *error = "LEXICAL_ERROR";
            return false;
        }
    }
    if (word(l, "NULL")) {
        next(l);
        return true;
    }
    if (l->kind == TK_STRING) {
        v->type = MD_TEXT;
        v->as.text.ptr = md_copy(l->text, l->text_len);
        v->as.text.len = l->text_len;
        if (!v->as.text.ptr) {
            *error = "RESOURCE_ERROR";
            return false;
        }
        next(l);
        return true;
    }
    if (l->kind == TK_NUMBER) {
        const char *s = l->text;
        uint64_t limit = negative ? UINT64_C(9223372036854775808) : INT64_MAX, value = 0;
        for (; *s; s++) {
            unsigned n = (unsigned)(*s - '0');
            if (value > (limit - n) / 10) {
                *error = "INTEGER_RANGE";
                return false;
            }
            value = value * 10 + n;
        }
        v->type = MD_INT64;
        if (negative && value == UINT64_C(9223372036854775808))
            v->as.integer = INT64_MIN;
        else
            v->as.integer = negative ? -(int64_t)value : (int64_t)value;
        next(l);
        return true;
    }
    *error = bad(l);
    return false;
}
static size_t column(MdTable *t, const char *name) {
    for (size_t i = 0; i < t->count; i++)
        if (md_equal(t->fields[i].name, name))
            return i;
    return SIZE_MAX;
}
static MdTable *table(MdDb *db, const char *name) {
    for (size_t i = 0; i < db->count; i++)
        if (md_equal(db->tables[i].name, name))
            return &db->tables[i];
    return NULL;
}
static const char *finish(Lexer *l) {
    if (!take_punct(l, ';'))
        return bad(l);
    if (l->kind != TK_END)
        return bad(l);
    return NULL;
}
static const char *finish_query(Lexer *l) {
    const char *deferred[] = {"INNER", "JOIN", "GROUP", "HAVING", "ORDER", "OFFSET", "FETCH", "UNION", "INTERSECT", "EXCEPT"};
    for (size_t i = 0; i < sizeof deferred / sizeof *deferred; i++)
        if (word(l, deferred[i]))
            return "UNSUPPORTED_FEATURE";
    return finish(l);
}
typedef struct {
    MdValue *values;
    size_t value_count, row_count, column_count;
} LiteralRows;

static void rows_free(LiteralRows *rows) {
    md_values_free(rows->values, rows->value_count);
    memset(rows, 0, sizeof *rows);
}

static const char *literal_rows(Lexer *l, LiteralRows *rows) {
    const char *error = NULL;
    do {
        if (!take_punct(l, '('))
            return bad(l);
        size_t start = rows->value_count;
        do {
            MdValue value = {0};
            if (!literal(l, &value, &error))
                return error;
            if (rows->value_count == SIZE_MAX / sizeof *rows->values) {
                if (value.type == MD_TEXT)
                    free(value.as.text.ptr);
                return "RESOURCE_ERROR";
            }
            MdValue *more = realloc(rows->values, (rows->value_count + 1) * sizeof *more);
            if (!more) {
                if (value.type == MD_TEXT)
                    free(value.as.text.ptr);
                return "RESOURCE_ERROR";
            }
            rows->values = more;
            rows->values[rows->value_count++] = value;
        } while (take_punct(l, ','));
        if (!take_punct(l, ')'))
            return bad(l);
        size_t width = rows->value_count - start;
        if (rows->row_count && width != rows->column_count)
            return "COLUMN_COUNT_MISMATCH";
        rows->column_count = width;
        rows->row_count++;
    } while (take_punct(l, ','));
    return NULL;
}

static const char *sort_ids(int64_t *ids, size_t count) {
    /* INSERT 중복 검사도 취소 가능한 제자리 힙 정렬을 쓴다. */
    for (size_t start = count / 2, end = count; end > 1;) {
        size_t root;
        if (start)
            root = --start;
        else {
            int64_t value = ids[--end];
            ids[end] = ids[0];
            ids[0] = value;
            root = 0;
        }
        while (root < end / 2) {
            const char *cancel = md_execution_check();
            if (cancel)
                return cancel;
            size_t child = root * 2 + 1;
            if (child + 1 < end && ids[child] < ids[child + 1])
                child++;
            if (ids[root] >= ids[child])
                break;
            int64_t value = ids[root];
            ids[root] = ids[child];
            ids[child] = value;
            root = child;
        }
    }
    return NULL;
}

static const char *insert(MdRequest *r, Lexer *l, MdEvent *e) {
    const char *error = NULL;
    MdTable *t = NULL;
    LiteralRows rows = {0};
    MdValue *mapped = NULL;
    int64_t *ids = NULL;
    size_t *columns = NULL, n = 0, mapped_count = 0;
    next(l);
    if (!take_word(l, "INTO") || l->kind != TK_WORD || !md_identifier(l->text))
        return bad(l);
    t = table(r->db, l->text);
    if (!t)
        return "UNKNOWN_TABLE";
    next(l);
    columns = malloc(t->count * sizeof *columns);
    if (!columns)
        return "RESOURCE_ERROR";
    if (!take_punct(l, '(')) {
        error = bad(l);
        goto done;
    }
    do {
        if (l->kind != TK_WORD || !md_identifier(l->text)) {
            error = bad(l);
            goto done;
        }
        size_t c = column(t, l->text);
        if (c == SIZE_MAX) {
            error = "UNKNOWN_COLUMN";
            goto done;
        }
        for (size_t i = 0; i < n; i++)
            if (columns[i] == c) {
                error = "DUPLICATE_COLUMN";
                goto done;
            }
        if (n == t->count) {
            error = "DUPLICATE_COLUMN";
            goto done;
        }
        columns[n++] = c;
        next(l);
    } while (take_punct(l, ','));
    if (!take_punct(l, ')') || !take_word(l, "VALUES")) {
        error = bad(l);
        goto done;
    }
    error = literal_rows(l, &rows);
    if (!error)
        error = finish(l);
    if (error)
        goto done;
    if (rows.column_count != n) {
        error = "COLUMN_COUNT_MISMATCH";
        goto done;
    }
    bool has_id = false;
    for (size_t i = 0; i < n; i++)
        has_id |= columns[i] == t->id;
    int64_t maximum = 0;
    if (!has_id && t->index.root) {
        const MdBtNode *node = t->index.root;
        while (!node->leaf) node = node->children[node->count];
        maximum = node->keys[node->count - 1];
    }
    if (!has_id && rows.row_count > (uint64_t)(INT64_MAX - maximum)) {
        error = "INTEGER_RANGE";
        goto done;
    }
    if (rows.row_count > SIZE_MAX / t->count / sizeof *mapped) {
        error = "RESOURCE_ERROR";
        goto done;
    }
    mapped_count = rows.row_count * t->count;
    mapped = calloc(mapped_count, sizeof *mapped);
    if (!mapped) {
        error = "RESOURCE_ERROR";
        goto done;
    }
    if (rows.row_count > SIZE_MAX / sizeof *ids || !(ids = malloc(rows.row_count * sizeof *ids))) {
        error = "RESOURCE_ERROR";
        goto done;
    }
    /* 문장 전체의 타입·ID를 검사한 뒤에만 저장 함수를 호출한다. */
    for (size_t row = 0; row < rows.row_count; row++) {
        md_execution_point("staging");
        if ((error = md_execution_check()))
            goto done;
        MdValue *values = mapped + row * t->count;
        for (size_t j = 0; j < n; j++) {
            MdValue *value = &rows.values[row * n + j];
            if (value->type != MD_NULL && value->type != t->fields[columns[j]].type) {
                error = "TYPE_MISMATCH";
                goto done;
            }
            values[columns[j]] = *value;
            memset(value, 0, sizeof *value);
        }
        if (!has_id) values[t->id] = (MdValue){.type = MD_INT64, .as.integer = maximum + (int64_t)row + 1};
        if (values[t->id].type != MD_INT64 || values[t->id].as.integer <= 0) {
            error = "INVALID_ID";
            goto done;
        }
        int64_t id = values[t->id].as.integer;
        size_t existing;
        if (md_bt_get(&t->index, id, &existing)) { error = "DUPLICATE_ID"; goto done; }
        ids[row] = id;
    }
    error = sort_ids(ids, rows.row_count);
    if (error)
        goto done;
    for (size_t i = 1; i < rows.row_count; i++)
        if (ids[i - 1] == ids[i]) { error = "DUPLICATE_ID"; goto done; }
    if (!md_storage_insert(r->db, t, mapped, rows.row_count, e))
        error = e->diagnostic.code;
done:
    free(ids);
    free(columns);
    rows_free(&rows);
    md_values_free(mapped, mapped_count);
    return error;
}

typedef struct {
    char *qualifier, *name;
} ColumnRef;

static void reference_free(ColumnRef *ref) {
    free(ref->qualifier);
    free(ref->name);
}

static const char *reference(Lexer *l, ColumnRef *ref) {
    if (word(l, "COUNT") || word(l, "SUM") || word(l, "AVG") || word(l, "MIN") || word(l, "MAX"))
        return "UNSUPPORTED_FEATURE";
    if (l->kind != TK_WORD || !md_identifier(l->text))
        return bad(l);
    ref->name = md_copy(l->text, l->text_len);
    if (!ref->name)
        return "RESOURCE_ERROR";
    next(l);
    if (take_punct(l, '.')) {
        ref->qualifier = ref->name;
        ref->name = NULL;
        if (l->kind != TK_WORD || !md_identifier(l->text))
            return bad(l);
        ref->name = md_copy(l->text, l->text_len);
        if (!ref->name)
            return "RESOURCE_ERROR";
        next(l);
    }
    return NULL;
}

/* 입력 범위는 요청에만 속한다. 합친 인덱스는 왼쪽 스키마 뒤 오른쪽 스키마다. */
typedef struct {
    MdTable *tables[2];
    char *aliases[2];
    size_t count, width;
} InputScope;

static MdField *scope_field(const InputScope *scope, size_t index) {
    if (index < scope->tables[0]->count)
        return &scope->tables[0]->fields[index];
    return &scope->tables[1]->fields[index - scope->tables[0]->count];
}

static const char *input_name(const InputScope *scope, size_t input) {
    return scope->aliases[input] ? scope->aliases[input] : scope->tables[input]->name;
}

static const char *bind_reference(const InputScope *scope, ColumnRef *ref, size_t *index) {
    size_t offset = 0, matches = 0;
    bool qualifier_found = false;
    for (size_t i = 0; i < scope->count; i++) {
        MdTable *t = scope->tables[i];
        bool visible = !ref->qualifier || md_equal(ref->qualifier, input_name(scope, i));
        qualifier_found |= visible;
        if (visible) {
            size_t c = column(t, ref->name);
            if (c != SIZE_MAX) {
                *index = offset + c;
                matches++;
            }
        }
        offset += t->count;
    }
    if (!qualifier_found)
        return "UNKNOWN_TABLE";
    return matches > 1 ? "AMBIGUOUS_COLUMN" : matches ? NULL : "UNKNOWN_COLUMN";
}

/* COUNT는 입력 컬럼 인덱스로 합친다. SIZE_MAX는 COUNT(*)다. */
typedef struct {
    size_t *keys, key_count, *counts, count_count;
    MdValue *groups;
    size_t group_count, width;
    bool active;
} Aggregate;

/* 필수 피연산자 자리에 도달한 지원 절 키워드는 식의 누락을 뜻한다. */
static bool clause_boundary(Lexer *l) {
    const char *boundaries[] = {"GROUP", "HAVING", "ORDER", "OFFSET", "FETCH", "UNION", "WHERE",
                                "FROM", "INNER", "JOIN", "AND", "OR", "AS", "ON"};
    for (size_t i = 0; i < sizeof boundaries / sizeof *boundaries; i++)
        if (word(l, boundaries[i]))
            return true;
    return false;
}

static const char *count_reference(Lexer *l, ColumnRef *ref, bool *all) {
    if (clause_boundary(l))
        return bad(l);
    if (!take_word(l, "COUNT"))
        return l->kind == TK_WORD || l->kind == TK_NUMBER || l->kind == TK_STRING ?
               "UNSUPPORTED_FEATURE" : bad(l);
    if (!take_punct(l, '('))
        return bad(l);
    *all = take_punct(l, '*');
    const char *error = NULL;
    if (!*all) {
        if (clause_boundary(l))
            return bad(l);
        /* 빠진 인수와 어휘 오류는 원래 진단을 유지하고, 인식 가능한 식만 미지원이다. */
        if (l->kind != TK_WORD)
            return l->kind == TK_NUMBER || l->kind == TK_STRING || punct(l, '+') ||
                   punct(l, '-') || punct(l, '(') ? "UNSUPPORTED_FEATURE" : bad(l);
        if (word(l, "NULL") || word(l, "DISTINCT"))
            return "UNSUPPORTED_FEATURE";
        error = reference(l, ref);
    }
    if (!error && !take_punct(l, ')'))
        error = punct(l, '+') || punct(l, '-') || punct(l, '*') || punct(l, '/') ?
                "UNSUPPORTED_FEATURE" : bad(l);
    return error;
}

static const char *aggregate_count(Aggregate *a, size_t column, size_t *index) {
    a->active = true;
    for (size_t i = 0; i < a->count_count; i++)
        if (a->counts[i] == column) {
            *index = i;
            return NULL;
        }
    if (a->count_count == SIZE_MAX / sizeof *a->counts)
        return "RESOURCE_ERROR";
    size_t *more = realloc(a->counts, (a->count_count + 1) * sizeof *more);
    if (!more)
        return "RESOURCE_ERROR";
    a->counts = more;
    *index = a->count_count;
    a->counts[a->count_count++] = column;
    return NULL;
}

typedef struct {
    bool is_column;
    size_t column;
    MdType type;
    MdValue value;
} Scalar;

typedef enum { P_COMPARE, P_IS_NULL, P_IS_NOT_NULL, P_AND, P_OR, P_GROUP } PredicateKind;
typedef struct {
    PredicateKind kind;
    int comparison;
    Scalar left, right;
} PredicateNode;
typedef struct {
    PredicateNode *nodes;
    size_t count;
} Predicate;

#ifdef MINIDB_TEST_FAULTS
static thread_local size_t index_lookups, scan_rows;
void md_test_index_reset(void) { index_lookups = scan_rows = 0; }
size_t md_test_index_lookups(void) { return index_lookups; }
size_t md_test_scan_rows(void) { return scan_rows; }
#endif

static bool index_candidate(const Predicate *p, size_t id, int64_t *key) {
    /* OR가 하나라도 있으면 보수적으로 전체 스캔한다. 나머지 식은 AND뿐이므로
     * 그 안의 ID 등식은 모든 결과가 반드시 만족할 조건이다. */
    for (size_t i = 0; i < p->count; i++) if (p->nodes[i].kind == P_OR) return false;
    for (size_t i = 0; i < p->count; i++) {
        const PredicateNode *node = &p->nodes[i];
        if (node->kind != P_COMPARE || node->comparison != CMP_EQ) continue;
        const Scalar *column = &node->left, *literal = &node->right;
        if (!column->is_column) { column = &node->right; literal = &node->left; }
        if (column->is_column && column->column == id && !literal->is_column && literal->value.type == MD_INT64) {
            *key = literal->value.as.integer;
            return true;
        }
    }
    return false;
}

static void scalar_free(Scalar *scalar) {
    if (scalar->value.type == MD_TEXT)
        free(scalar->value.as.text.ptr);
}

static void predicate_free(Predicate *p) {
    for (size_t i = 0; i < p->count; i++) {
        scalar_free(&p->nodes[i].left);
        scalar_free(&p->nodes[i].right);
    }
    free(p->nodes);
}

static const char *scalar(Lexer *l, const InputScope *scope, Scalar *s) {
    if (word(l, "NOT"))
        return "UNSUPPORTED_FEATURE";
    if (l->kind == TK_WORD && !word(l, "NULL")) {
        ColumnRef ref = {0};
        const char *error = reference(l, &ref);
        if (!error)
            error = bind_reference(scope, &ref, &s->column);
        reference_free(&ref);
        if (error)
            return error;
        s->is_column = true;
        s->type = scope_field(scope, s->column)->type;
        return NULL;
    }
    const char *error = NULL;
    if (!literal(l, &s->value, &error))
        return error;
    s->type = s->value.type;
    return NULL;
}

static const char *predicate_append(Predicate *p, PredicateNode node) {
    if (p->count == SIZE_MAX / sizeof *p->nodes)
        return "RESOURCE_ERROR";
    PredicateNode *more = realloc(p->nodes, (p->count + 1) * sizeof *more);
    if (!more)
        return "RESOURCE_ERROR";
    p->nodes = more;
    p->nodes[p->count++] = node;
    return NULL;
}

static const char *predicate_atom(Lexer *l, const InputScope *scope, Predicate *p, Aggregate *aggregate) {
    PredicateNode node = {0};
    const char *error = NULL;
    if (aggregate) {
        ColumnRef ref = {0};
        bool all = false;
        size_t column = SIZE_MAX, index = 0;
        error = count_reference(l, &ref, &all);
        if (!error && !all)
            error = bind_reference(scope, &ref, &column);
        if (!error)
            error = aggregate_count(aggregate, column, &index);
        reference_free(&ref);
        if (!error && index > SIZE_MAX - aggregate->key_count)
            error = "RESOURCE_ERROR";
        if (!error)
            node.left = (Scalar){.is_column = true, .column = aggregate->key_count + index, .type = MD_INT64};
    } else
        error = scalar(l, scope, &node.left);
    if (error)
        goto failed;
    if (!aggregate && take_word(l, "IS")) {
        node.kind = take_word(l, "NOT") ? P_IS_NOT_NULL : P_IS_NULL;
        if (!take_word(l, "NULL")) {
            error = bad(l);
            goto failed;
        }
    } else {
        node.kind = P_COMPARE;
        node.comparison = l->comparison;
        if (!node.comparison) {
            error = bad(l);
            goto failed;
        }
        next(l);
        if (aggregate) {
            if (clause_boundary(l))
                error = bad(l);
            else if (l->kind != TK_NUMBER && !punct(l, '+') && !punct(l, '-'))
                error = l->kind == TK_WORD || l->kind == TK_STRING ?
                        "UNSUPPORTED_FEATURE" : bad(l);
            else if (!literal(l, &node.right.value, &error)) {
                if (!error)
                    error = bad(l);
            }
            node.right.type = MD_INT64;
        } else
            error = scalar(l, scope, &node.right);
        if (error)
            goto failed;
        if (node.left.type != MD_NULL && node.right.type != MD_NULL &&
            node.left.type != node.right.type) {
            error = "TYPE_MISMATCH";
            goto failed;
        }
    }
    error = predicate_append(p, node);
    if (!error)
        return NULL;
failed:
    scalar_free(&node.left);
    scalar_free(&node.right);
    return error;
}

static int precedence(PredicateKind kind) { return kind == P_AND ? 2 : kind == P_OR ? 1 : 0; }

static const char *predicate_parse(Lexer *l, const InputScope *scope, Predicate *p, Aggregate *aggregate) {
    PredicateKind *operators = NULL;
    size_t count = 0;
    bool need_atom = true;
    const char *error = NULL;
    /* 후위식과 명시적 괄호 스택으로 깊은 입력도 C 호출 스택에 의존하지 않는다. */
    for (;;) {
        if (need_atom && !punct(l, '(')) {
            error = predicate_atom(l, scope, p, aggregate);
            if (error)
                break;
            need_atom = false;
            continue;
        }
        if (!need_atom && punct(l, ')')) {
            while (count && operators[count - 1] != P_GROUP) {
                error = predicate_append(p, (PredicateNode){.kind = operators[--count]});
                if (error)
                    goto done;
            }
            if (!count) {
                error = "SYNTAX_ERROR";
                break;
            }
            count--;
            next(l);
            continue;
        }
        PredicateKind op;
        if (need_atom && punct(l, '('))
            op = P_GROUP;
        else if (!need_atom && (word(l, "AND") || word(l, "OR")))
            op = word(l, "AND") ? P_AND : P_OR;
        else
            break;
        if (op != P_GROUP) {
            while (count && precedence(operators[count - 1]) >= precedence(op)) {
                error = predicate_append(p, (PredicateNode){.kind = operators[--count]});
                if (error)
                    goto done;
            }
        }
        if (count == SIZE_MAX / sizeof *operators) {
            error = "RESOURCE_ERROR";
            break;
        }
        PredicateKind *more = realloc(operators, (count + 1) * sizeof *more);
        if (!more) {
            error = "RESOURCE_ERROR";
            break;
        }
        operators = more;
        operators[count++] = op;
        next(l);
        need_atom = true;
    }
    if (!error && need_atom)
        error = bad(l);
    while (!error && count) {
        PredicateKind op = operators[--count];
        error = op == P_GROUP ? "SYNTAX_ERROR" : predicate_append(p, (PredicateNode){.kind = op});
    }
done:
    free(operators);
    return error;
}

static const char *predicate(Lexer *l, const InputScope *scope, Predicate *p) {
    return predicate_parse(l, scope, p, NULL);
}

static const MdValue *scalar_value(const Scalar *s, MdValue *row) {
    return s->is_column ? row + s->column : &s->value;
}

static int compare_values(const MdValue *a, const MdValue *b) {
    if (a->type == MD_INT64)
        return (a->as.integer > b->as.integer) - (a->as.integer < b->as.integer);
    size_t n = a->as.text.len < b->as.text.len ? a->as.text.len : b->as.text.len;
    for (size_t offset = 0; offset < n;) {
        md_execution_point("text-compare");
        if (md_execution_check())
            return 0;
        size_t length = n - offset > 4096 ? 4096 : n - offset;
        int compared = memcmp(a->as.text.ptr + offset, b->as.text.ptr + offset, length);
        if (compared)
            return compared;
        offset += length;
    }
    return (a->as.text.len > b->as.text.len) - (a->as.text.len < b->as.text.len);
}

static bool predicate_matches(Predicate *p, MdValue *row, int *stack) {
    size_t count = 0;
    for (size_t i = 0; i < p->count; i++) {
        PredicateNode *node = &p->nodes[i];
        if (md_execution_check())
            return false;
        if (node->kind == P_AND || node->kind == P_OR) {
            int right = stack[--count], left = stack[--count];
            int answer;
            if (node->kind == P_AND) {
                if (left == 0 || right == 0)
                    answer = 0;
                else if (left < 0 || right < 0)
                    answer = -1;
                else
                    answer = 1;
            } else {
                if (left == 1 || right == 1)
                    answer = 1;
                else if (left < 0 || right < 0)
                    answer = -1;
                else
                    answer = 0;
            }
            stack[count++] = answer;
            continue;
        }
        const MdValue *a = scalar_value(&node->left, row);
        if (node->kind == P_IS_NULL || node->kind == P_IS_NOT_NULL) {
            stack[count++] = (a->type == MD_NULL) == (node->kind == P_IS_NULL);
            continue;
        }
        const MdValue *b = scalar_value(&node->right, row);
        if (a->type == MD_NULL || b->type == MD_NULL) {
            stack[count++] = -1;
            continue;
        }
        int cmp = compare_values(a, b), answer = 0;
        switch (node->comparison) {
        case CMP_EQ: answer = cmp == 0; break;
        case CMP_NE: answer = cmp != 0; break;
        case CMP_LT: answer = cmp < 0; break;
        case CMP_LE: answer = cmp <= 0; break;
        case CMP_GT: answer = cmp > 0; break;
        case CMP_GE: answer = cmp >= 0; break;
        }
        stack[count++] = answer;
    }
    return stack[0] == 1;
}

typedef struct {
    char op; /* 0이면 컬럼/리터럴, 나머지는 후위식의 + 또는 -다. */
    Scalar scalar;
} SetNode;
typedef struct {
    SetNode *nodes;
    size_t count, target;
    MdType type;
} Assignment;

static void assignment_free(Assignment *a) {
    for (size_t i = 0; i < a->count; i++)
        scalar_free(&a->nodes[i].scalar);
    free(a->nodes);
}

static const char *set_append(Assignment *a, SetNode node) {
    if (a->count == SIZE_MAX / sizeof *a->nodes)
        return "RESOURCE_ERROR";
    SetNode *more = realloc(a->nodes, (a->count + 1) * sizeof *more);
    if (!more)
        return "RESOURCE_ERROR";
    a->nodes = more;
    a->nodes[a->count++] = node;
    return NULL;
}

static const char *set_expression(Lexer *l, MdTable *t, Assignment *a) {
    char *operators = NULL;
    size_t count = 0;
    bool need_value = true;
    const char *error = NULL;
    /* 괄호 깊이는 힙 스택으로 처리한다. 부호는 리터럴에만 붙고 일반 단항식은 없다. */
    for (;;) {
        if (need_value) {
            if (punct(l, '(')) {
                if (count == SIZE_MAX) {
                    error = "RESOURCE_ERROR";
                    break;
                }
                char *more = realloc(operators, count + 1);
                if (!more) {
                    error = "RESOURCE_ERROR";
                    break;
                }
                operators = more;
                operators[count++] = '(';
                next(l);
                continue;
            }
            SetNode node = {0};
            error = scalar(l, &(InputScope){.tables = {t}, .count = 1}, &node.scalar);
            if (!error)
                error = set_append(a, node);
            if (error) {
                scalar_free(&node.scalar);
                break;
            }
            need_value = false;
        } else if (punct(l, ')')) {
            while (count && operators[count - 1] != '(') {
                error = set_append(a, (SetNode){.op = operators[--count]});
                if (error)
                    goto done;
            }
            if (!count) {
                error = "SYNTAX_ERROR";
                break;
            }
            count--;
            next(l);
        } else if (punct(l, '+') || punct(l, '-')) {
            char op = l->punct;
            while (count && operators[count - 1] != '(') {
                error = set_append(a, (SetNode){.op = operators[--count]});
                if (error)
                    goto done;
            }
            if (count == SIZE_MAX) {
                error = "RESOURCE_ERROR";
                break;
            }
            char *more = realloc(operators, count + 1);
            if (!more) {
                error = "RESOURCE_ERROR";
                break;
            }
            operators = more;
            operators[count++] = op;
            next(l);
            need_value = true;
        } else {
            if (punct(l, '*') || punct(l, '/') || punct(l, '|') || punct(l, '('))
                error = "UNSUPPORTED_FEATURE";
            break;
        }
    }
    if (!error && need_value)
        error = bad(l);
    while (!error && count) {
        char op = operators[--count];
        error = op == '(' ? "SYNTAX_ERROR" : set_append(a, (SetNode){.op = op});
    }
    if (!error) {
        if (a->count > SIZE_MAX / sizeof(MdType)) {
            error = "RESOURCE_ERROR";
            goto done;
        }
        MdType *types = malloc(a->count * sizeof *types);
        if (!types) {
            error = "RESOURCE_ERROR";
            goto done;
        }
        size_t used = 0;
        for (size_t i = 0; i < a->count; i++) {
            if ((error = md_execution_check()))
                break;
            SetNode *node = &a->nodes[i];
            if (!node->op)
                types[used++] = node->scalar.type;
            else {
                MdType right = types[--used], left = types[--used];
                if (left == MD_TEXT || right == MD_TEXT) {
                    error = "TYPE_MISMATCH";
                    break;
                }
                types[used++] = MD_INT64;
            }
        }
        if (!error) {
            a->type = types[0];
            if (a->type != MD_NULL && a->type != t->fields[a->target].type)
                error = "TYPE_MISMATCH";
        }
        free(types);
    }
done:
    free(operators);
    return error;
}

static const char *set_evaluate(Assignment *a, MdValue *row, MdValue *stack, MdValue *out) {
    size_t used = 0;
    for (size_t i = 0; i < a->count; i++) {
        const char *cancel = md_execution_check();
        if (cancel)
            return cancel;
        SetNode *node = &a->nodes[i];
        if (!node->op) {
            stack[used++] = *scalar_value(&node->scalar, row);
            continue;
        }
        MdValue right = stack[--used], left = stack[--used], result = {0};
        if (left.type != MD_NULL && right.type != MD_NULL) {
            int64_t x = left.as.integer, y = right.as.integer;
            /* 경계와 비교한 뒤 연산하므로 signed overflow 자체를 실행하지 않는다. */
            bool overflow = node->op == '+'
                ? ((y > 0 && x > INT64_MAX - y) || (y < 0 && x < INT64_MIN - y))
                : ((y < 0 && x > INT64_MAX + y) || (y > 0 && x < INT64_MIN + y));
            if (overflow)
                return "INTEGER_OVERFLOW";
            result.type = MD_INT64;
            result.as.integer = node->op == '+' ? x + y : x - y;
        }
        stack[used++] = result;
    }
    return md_value_copy(out, &stack[0]) ? NULL : "RESOURCE_ERROR";
}

static const char *mutate(MdRequest *r, Lexer *l, MdEvent *e, bool update) {
    const char *command = update ? "UPDATE" : "DELETE", *error = NULL;
    MdTable *t = NULL;
    Assignment *assignments = NULL;
    size_t count = 0, max_nodes = 0, retained = 0;
    uint64_t matched = 0;
    Predicate p = {0};
    int *predicate_stack = NULL;
    MdValue *value_stack = NULL, *staged = NULL;
    next(l);
    if (!update && !take_word(l, "FROM")) {
        error = bad(l);
        goto done;
    }
    if (l->kind != TK_WORD || !md_identifier(l->text)) {
        error = bad(l);
        goto done;
    }
    t = table(r->db, l->text);
    if (!t) {
        error = "UNKNOWN_TABLE";
        goto done;
    }
    next(l);
    if (word(l, "AS")) {
        error = "UNSUPPORTED_FEATURE";
        goto done;
    }
    if (update) {
        if (!take_word(l, "SET")) {
            error = bad(l);
            goto done;
        }
        do {
            if (l->kind != TK_WORD || !md_identifier(l->text)) {
                error = bad(l);
                goto done;
            }
            size_t target = column(t, l->text);
            if (target == SIZE_MAX) {
                error = "UNKNOWN_COLUMN";
                goto done;
            }
            if (target == t->id) {
                error = "READ_ONLY_COLUMN";
                goto done;
            }
            for (size_t i = 0; i < count; i++)
                if (assignments[i].target == target) {
                    error = "DUPLICATE_COLUMN";
                    goto done;
                }
            next(l);
            if (!take_punct(l, '=')) {
                error = bad(l);
                goto done;
            }
            if (count == SIZE_MAX / sizeof *assignments) {
                error = "RESOURCE_ERROR";
                goto done;
            }
            Assignment *more = realloc(assignments, (count + 1) * sizeof *more);
            if (!more) {
                error = "RESOURCE_ERROR";
                goto done;
            }
            assignments = more;
            Assignment *a = &assignments[count++];
            *a = (Assignment){.target = target};
            error = set_expression(l, t, a);
            if (error)
                goto done;
            if (a->count > max_nodes)
                max_nodes = a->count;
        } while (take_punct(l, ','));
    }
    if (!take_word(l, "WHERE")) {
        error = bad(l);
        goto done;
    }
    error = predicate(l, &(InputScope){.tables = {t}, .count = 1}, &p);
    if (!error)
        error = finish_query(l);
    if (error)
        goto done;
    if (p.count > SIZE_MAX / sizeof *predicate_stack || max_nodes > SIZE_MAX / sizeof *value_stack ||
        (t->row_count && t->count > SIZE_MAX / t->row_count / sizeof *staged)) {
        error = "RESOURCE_ERROR";
        goto done;
    }
    predicate_stack = malloc(p.count * sizeof *predicate_stack);
    value_stack = max_nodes ? malloc(max_nodes * sizeof *value_stack) : NULL;
    size_t cells = t->row_count * t->count;
    staged = cells ? calloc(cells, sizeof *staged) : NULL;
    if (!predicate_stack || (max_nodes && !value_stack) || (cells && !staged)) {
        error = "RESOURCE_ERROR";
        goto done;
    }
    /* 기존 행과 staging은 별도 소유다. 모든 우변은 복사/수정 전의 old row를 본다. */
    for (size_t i = 0; i < t->row_count; i++) {
        md_execution_point("staging");
        if ((error = md_execution_check()))
            goto done;
        MdValue *old = t->rows + i * t->count;
        bool match = predicate_matches(&p, old, predicate_stack);
        if (match)
            matched++;
        if (!update && match)
            continue;
        MdValue *out = staged + retained * t->count;
        retained++;
        for (size_t c = 0; c < t->count; c++)
            if (!md_value_copy(out + c, old + c)) {
                error = "RESOURCE_ERROR";
                goto done;
            }
        if (update && match)
            for (size_t j = 0; j < count; j++) {
                Assignment *a = &assignments[j];
                MdValue value = {0};
                error = set_evaluate(a, old, value_stack, &value);
                if (error) {
                    if (value.type == MD_TEXT)
                        free(value.as.text.ptr);
                    goto done;
                }
                if (out[a->target].type == MD_TEXT)
                    free(out[a->target].as.text.ptr);
                out[a->target] = value;
            }
    }
    if (!matched) {
        e->has_result = true;
        e->result.kind = MD_STATEMENT_OK;
        strcpy(e->result.command, command);
    } else if (!md_storage_replace(r->db, t, &staged, retained, command, matched, e))
        error = e->diagnostic.code;
done:
    for (size_t i = 0; i < count; i++)
        assignment_free(assignments + i);
    free(assignments);
    predicate_free(&p);
    free(predicate_stack);
    free(value_stack);
    md_values_free(staged, t ? t->row_count * t->count : 0);
    return error;
}

typedef struct {
    ColumnRef ref;
    char *alias;
    size_t column, aggregate_index;
    bool is_count, count_all;
} SelectItem;

typedef struct {
    size_t column;
    bool descending, nulls_first;
} SortKey;
typedef struct {
    SortKey *keys;
    size_t count;
    uint64_t offset, fetch;
    bool has_fetch;
} QueryTail;

static const char *page_count(Lexer *l, uint64_t *count) {
    MdValue value = {0};
    const char *error = NULL;
    if (l->kind != TK_NUMBER && !punct(l, '+') && !punct(l, '-'))
        return bad(l);
    if (!literal(l, &value, &error))
        return error && !strcmp(error, "INTEGER_RANGE") ? "INVALID_PAGINATION" : error;
    if (value.as.integer < 0)
        return "INVALID_PAGINATION";
    *count = (uint64_t)value.as.integer;
    return NULL;
}

static const char *order_column(Lexer *l, const MdResult *result, size_t *column_index) {
    if (l->kind == TK_NUMBER || punct(l, '+') || punct(l, '-'))
        return "INVALID_ORDER_BY";
    ColumnRef ref = {0};
    const char *error = reference(l, &ref);
    char *qualified = NULL;
    if (error)
        goto done;
    const char *name = ref.name;
    if (ref.qualifier) {
        size_t a = strlen(ref.qualifier), b = strlen(ref.name);
        if (a > SIZE_MAX - b - 2 || !(qualified = malloc(a + b + 2))) {
            error = "RESOURCE_ERROR";
            goto done;
        }
        memcpy(qualified, ref.qualifier, a);
        qualified[a] = '.';
        memcpy(qualified + a + 1, ref.name, b + 1);
        name = qualified;
    }
    /* 입력 테이블 재검색 없이 최종 출력 이름 하나에만 결합한다. */
    error = "INVALID_ORDER_BY";
    for (size_t i = 0; i < result->column_count; i++)
        if (md_equal(name, result->columns[i].name)) {
            *column_index = i;
            error = NULL;
            break;
        }
done:
    free(qualified);
    reference_free(&ref);
    return error;
}

static const char *query_tail(Lexer *l, const MdResult *result, QueryTail *tail) {
    const char *error;
    bool has_offset = false, has_order = false;
    if (take_word(l, "ORDER")) {
        has_order = true;
        if (!take_word(l, "BY"))
            return bad(l);
        do {
            SortKey key = {0};
            error = order_column(l, result, &key.column);
            if (error)
                return error;
            if (take_word(l, "DESC"))
                key.descending = true;
            else
                take_word(l, "ASC");
            if (take_word(l, "NULLS")) {
                if (take_word(l, "FIRST"))
                    key.nulls_first = true;
                else if (!take_word(l, "LAST"))
                    return bad(l);
            }
            if (tail->count == SIZE_MAX / sizeof *tail->keys)
                return "RESOURCE_ERROR";
            SortKey *more = realloc(tail->keys, (tail->count + 1) * sizeof *more);
            if (!more)
                return "RESOURCE_ERROR";
            tail->keys = more;
            tail->keys[tail->count++] = key;
        } while (take_punct(l, ','));
    }
    if (take_word(l, "OFFSET")) {
        has_offset = true;
        error = page_count(l, &tail->offset);
        if (error)
            return error;
        if (!take_word(l, "ROWS"))
            return bad(l);
    }
    if (take_word(l, "FETCH")) {
        tail->has_fetch = true;
        if (!take_word(l, "FIRST"))
            return bad(l);
        error = page_count(l, &tail->fetch);
        if (error)
            return error;
        if (!take_word(l, "ROWS") || !take_word(l, "ONLY"))
            return bad(l);
    }
    if (word(l, "ORDER") || word(l, "OFFSET") || word(l, "FETCH") ||
        word(l, "GROUP") || word(l, "HAVING") ||
        ((has_order || has_offset || tail->has_fetch) && word(l, "UNION")))
        return "SYNTAX_ERROR";
    error = finish_query(l);
    if (!error && has_offset && !has_order)
        error = "INVALID_PAGINATION";
    return error;
}

static int compare_rows(const MdResult *result, const QueryTail *tail, size_t a, size_t b) {
    for (size_t i = 0; i < tail->count; i++) {
        const SortKey *key = tail->keys + i;
        const MdValue *left = result->values + a * result->column_count + key->column;
        const MdValue *right = result->values + b * result->column_count + key->column;
        if (left->type == MD_NULL || right->type == MD_NULL) {
            if (left->type == right->type)
                continue;
            /* NULL 위치는 DESC 반전과 별개다. */
            return (left->type == MD_NULL) == key->nulls_first ? -1 : 1;
        }
        int compared = compare_values(left, right);
        if (compared) {
            int sign = (compared > 0) - (compared < 0);
            return key->descending ? -sign : sign;
        }
    }
    return 0;
}

static void swap_rows(MdResult *result, size_t a, size_t b) {
    for (size_t i = 0; i < result->column_count; i++) {
        MdValue value = result->values[a * result->column_count + i];
        result->values[a * result->column_count + i] = result->values[b * result->column_count + i];
        result->values[b * result->column_count + i] = value;
    }
}

static void sift_rows(MdResult *result, const QueryTail *tail, size_t root, size_t count) {
    while (root < count / 2) {
        md_execution_point("sort");
        if (md_execution_check())
            return;
        size_t child = root * 2 + 1;
        if (child + 1 < count && compare_rows(result, tail, child, child + 1) < 0)
            child++;
        if (compare_rows(result, tail, root, child) >= 0)
            return;
        swap_rows(result, root, child);
        root = child;
    }
}

static void apply_query_tail(MdResult *result, const QueryTail *tail) {
    /* 요청별 힙 정렬: 전역 comparator 문맥·추가 행 복사·재귀가 없다. */
    if (tail->count && result->row_count > 1) {
        for (size_t root = result->row_count / 2; root > 0 && !md_execution_check();)
            sift_rows(result, tail, --root, result->row_count);
        for (size_t count = result->row_count; count > 1;) {
            if (md_execution_check())
                return;
            swap_rows(result, 0, --count);
            sift_rows(result, tail, 0, count);
        }
    }
    size_t start = tail->offset < result->row_count ? (size_t)tail->offset : result->row_count;
    size_t kept = result->row_count - start;
    if (tail->has_fetch && tail->fetch < kept)
        kept = (size_t)tail->fetch;
    /* offset+fetch는 더하지 않는다. 범위 밖 TEXT만 해제하고 남은 소유권을 옮긴다. */
    for (size_t row = 0; row < result->row_count; row++)
        if (row < start || row - start >= kept)
            for (size_t col = 0; col < result->column_count; col++) {
                if (md_execution_check())
                    return;
                MdValue *value = result->values + row * result->column_count + col;
                if (value->type == MD_TEXT)
                    free(value->as.text.ptr);
                /* 취소 시 원래 row_count로 정리하므로 해제한 셀의 소유권을 비운다. */
                *value = (MdValue){0};
                md_execution_point("page-release");
            }
    if (kept && start)
        for (size_t cell = 0; cell < kept * result->column_count; cell++) {
            if (md_execution_check())
                return;
            MdValue *source = result->values + start * result->column_count + cell;
            result->values[cell] = *source;
            *source = (MdValue){0};
            md_execution_point("page-move");
        }
    result->row_count = kept;
}

static const char *table_input(MdDb *db, Lexer *l, InputScope *scope, size_t index) {
    if (l->kind != TK_WORD || !md_identifier(l->text))
        return bad(l);
    scope->tables[index] = table(db, l->text);
    if (!scope->tables[index])
        return "UNKNOWN_TABLE";
    next(l);
    if (take_word(l, "AS")) {
        if (l->kind != TK_WORD || !md_identifier(l->text))
            return bad(l);
        scope->aliases[index] = md_copy(l->text, l->text_len);
        if (!scope->aliases[index])
            return "RESOURCE_ERROR";
        next(l);
    }
    scope->count++;
    return NULL;
}

static char *output_name(const InputScope *scope, size_t index) {
    const char *field = scope_field(scope, index)->name;
    if (scope->count == 1)
        return md_copy(field, strlen(field));
    size_t input = index < scope->tables[0]->count ? 0 : 1;
    const char *name = input_name(scope, input);
    size_t a = strlen(name), b = strlen(field);
    if (a > SIZE_MAX - b - 2)
        return NULL;
    char *out = malloc(a + b + 2);
    if (out) {
        memcpy(out, name, a);
        out[a] = '.';
        memcpy(out + a + 1, field, b + 1);
    }
    return out;
}

static bool result_size(size_t rows, size_t width, size_t *bytes) {
    /* 증가·셀 수·바이트 수는 연산 전에 검사한다. Cartesian 곱은 계산하지 않는다. */
    if (rows == SIZE_MAX || !width || width > SIZE_MAX / (rows + 1) / sizeof(MdValue))
        return false;
    *bytes = (rows + 1) * width * sizeof(MdValue);
    return true;
}

#ifdef MINIDB_TEST_FAULTS
/* 테스트 전용 경계 관찰점: 실제 행 추가와 동일한 계산 함수를 호출한다. */
bool md_test_join_result_size(size_t rows, size_t width, size_t *bytes) {
    return result_size(rows, width, bytes);
}
#endif

/* 카운터는 검사 뒤 증가하며 실패 시 기존 값이 보존된다. */
static bool count_increment(int64_t *value) {
    if (*value == INT64_MAX)
        return false;
    ++*value;
    return true;
}

#ifdef MINIDB_TEST_FAULTS
bool md_test_count_increment(int64_t *value) { return count_increment(value); }
bool md_test_aggregate_size(size_t rows, size_t width, size_t *bytes) {
    return result_size(rows, width, bytes);
}
#endif

static const char *aggregate_new_group(Aggregate *a, MdValue *row, size_t *index) {
    size_t bytes;
    if (!result_size(a->group_count, a->width, &bytes) || md_fault("aggregate_group"))
        return "RESOURCE_ERROR";
    MdValue *more = realloc(a->groups, bytes);
    if (!more)
        return "RESOURCE_ERROR";
    a->groups = more;
    *index = a->group_count++;
    MdValue *group = more + *index * a->width;
    memset(group, 0, a->width * sizeof *group);
    /* 초기화한 행을 먼저 등록하여 부분 TEXT 복사 실패도 동일하게 회수한다. */
    for (size_t k = 0; k < a->key_count; k++)
        if ((k == 1 && md_fault("aggregate_key_after_one")) ||
            !md_value_copy(group + k, row + a->keys[k]))
            return "RESOURCE_ERROR";
    for (size_t c = 0; c < a->count_count; c++)
        group[a->key_count + c] = (MdValue){.type = MD_INT64,
            .as.integer = md_fault("aggregate_count_overflow") ? INT64_MAX : 0};
    return NULL;
}

static const char *aggregate_row(Aggregate *a, MdValue *row) {
    size_t index;
    for (index = 0; index < a->group_count; index++) {
        md_execution_point("group");
        const char *cancel = md_execution_check();
        if (cancel)
            return cancel;
        MdValue *group = a->groups + index * a->width;
        size_t key;
        for (key = 0; key < a->key_count; key++) {
            if (md_execution_check())
                return md_execution_check();
            const MdValue *left = group + key, *right = row + a->keys[key];
            if (left->type != right->type || (left->type != MD_NULL && compare_values(left, right)))
                break;
        }
        if (key == a->key_count)
            break;
    }
    if (index == a->group_count) {
        const char *error = aggregate_new_group(a, row, &index);
        if (error)
            return error;
    }
    MdValue *group = a->groups + index * a->width;
    for (size_t c = 0; c < a->count_count; c++)
        if ((a->counts[c] == SIZE_MAX || row[a->counts[c]].type != MD_NULL) &&
            !count_increment(&group[a->key_count + c].as.integer))
            return "INTEGER_OVERFLOW";
    return NULL;
}

static const char *project_row(MdResult *result, SelectItem *items, size_t count,
                               MdValue *row, bool aggregate, bool joined) {
    md_execution_point("projection");
    if (md_execution_check())
        return md_execution_check();
    size_t rows = result->row_count, bytes;
    if (!result_size(rows, count, &bytes) || (joined && !aggregate && md_fault("join_row")))
        return "RESOURCE_ERROR";
    MdValue *more = realloc(result->values, bytes);
    if (!more)
        return "RESOURCE_ERROR";
    result->values = more;
    memset(more + rows * count, 0, count * sizeof *more);
    result->row_count++;
    for (size_t c = 0; c < count; c++) {
        size_t index = aggregate ? items[c].aggregate_index : items[c].column;
        if ((c == 1 && ((aggregate && md_fault("aggregate_output_after_one")) ||
                       (joined && !aggregate && md_fault("join_copy_after_one")))) ||
            !md_value_copy(&more[rows * count + c], row + index))
            return "RESOURCE_ERROR";
    }
    return NULL;
}

static const char *select_query(MdRequest *r, Lexer *l, MdEvent *e) {
    const char *error = NULL;
    SelectItem *items = NULL;
    size_t count = 0;
    InputScope scope = {0};
    Predicate filter = {0}, having = {0};
    Aggregate aggregate = {0};
    int *stack = NULL;
    MdValue *scratch = NULL;
    size_t join_left = 0, join_right = 0;
    next(l);
    bool star = take_punct(l, '*');
    if (!star) {
        do {
            if (count == SIZE_MAX / sizeof *items) {
                error = "RESOURCE_ERROR";
                goto done;
            }
            SelectItem *more = realloc(items, (count + 1) * sizeof *more);
            if (!more) {
                error = "RESOURCE_ERROR";
                goto done;
            }
            items = more;
            memset(&items[count], 0, sizeof *items);
            SelectItem *item = &items[count++];
            item->is_count = word(l, "COUNT");
            error = item->is_count ? count_reference(l, &item->ref, &item->count_all) :
                                     reference(l, &item->ref);
            if (error)
                goto done;
            if (take_word(l, "AS")) {
                if (l->kind != TK_WORD || !md_identifier(l->text)) {
                    error = bad(l);
                    goto done;
                }
                item->alias = md_copy(l->text, l->text_len);
                if (!item->alias) {
                    error = "RESOURCE_ERROR";
                    goto done;
                }
                next(l);
            }
        } while (take_punct(l, ','));
    }
    if (!take_word(l, "FROM")) {
        error = bad(l);
        goto done;
    }
    error = table_input(r->db, l, &scope, 0);
    if (error)
        goto done;
    if (take_word(l, "INNER")) {
        if (!take_word(l, "JOIN")) {
            error = bad(l);
            goto done;
        }
        error = table_input(r->db, l, &scope, 1);
        if (error)
            goto done;
        if (md_equal(input_name(&scope, 0), input_name(&scope, 1))) {
            error = "DUPLICATE_TABLE_ALIAS";
            goto done;
        }
        if (scope.tables[0] == scope.tables[1] && (!scope.aliases[0] || !scope.aliases[1])) {
            error = "INVALID_JOIN";
            goto done;
        }
    } else if (word(l, "JOIN") || word(l, "LEFT") || word(l, "RIGHT") || word(l, "FULL") ||
               word(l, "OUTER") || word(l, "CROSS")) {
        error = "UNSUPPORTED_FEATURE";
        goto done;
    }
    scope.width = scope.tables[0]->count;
    if (scope.count == 2) {
        if (scope.width > SIZE_MAX - scope.tables[1]->count || md_fault("join_width")) {
            error = "RESOURCE_ERROR";
            goto done;
        }
        scope.width += scope.tables[1]->count;
    }
    if (scope.width > SIZE_MAX / sizeof *scratch || (star && scope.width > SIZE_MAX / sizeof *items)) {
        error = "RESOURCE_ERROR";
        goto done;
    }
    if (scope.count == 2) {
        if (!take_word(l, "ON")) {
            error = bad(l);
            goto done;
        }
        ColumnRef left = {0}, right = {0};
        error = reference(l, &left);
        if (!error)
            error = bind_reference(&scope, &left, &join_left);
        /* 비교 토큰 누락은 문법 오류이며, 존재하는 비등호 연산자와 구분한다. */
        if (!error && l->comparison == CMP_NONE)
            error = bad(l);
        if (!error && l->comparison != CMP_EQ)
            error = "INVALID_JOIN";
        if (!error) {
            next(l);
            error = reference(l, &right);
        }
        if (!error)
            error = bind_reference(&scope, &right, &join_right);
        reference_free(&left);
        reference_free(&right);
        if (error)
            goto done;
        if ((join_left < scope.tables[0]->count) == (join_right < scope.tables[0]->count)) {
            error = "INVALID_JOIN";
            goto done;
        }
        if (scope_field(&scope, join_left)->type != scope_field(&scope, join_right)->type) {
            error = "TYPE_MISMATCH";
            goto done;
        }
        if (word(l, "AND") || word(l, "OR")) {
            error = "UNSUPPORTED_FEATURE";
            goto done;
        }
    }
    if (star) {
        items = calloc(scope.width, sizeof *items);
        if (!items) {
            error = "RESOURCE_ERROR";
            goto done;
        }
        count = scope.width;
        for (size_t i = 0; i < count; i++)
            items[i].column = i;
    } else {
        for (size_t i = 0; i < count; i++) {
            items[i].column = SIZE_MAX;
            if (!items[i].count_all)
                error = bind_reference(&scope, &items[i].ref, &items[i].column);
            if (!error && items[i].is_count)
                error = aggregate_count(&aggregate, items[i].column, &items[i].aggregate_index);
            if (error)
                goto done;
        }
    }
    if (count > SIZE_MAX / sizeof *e->result.columns) {
        error = "RESOURCE_ERROR";
        goto done;
    }
    e->result.kind = MD_QUERY_RESULT;
    e->result.columns = calloc(count, sizeof *e->result.columns);
    if (!e->result.columns) {
        error = "RESOURCE_ERROR";
        goto done;
    }
    e->result.column_count = count;
    for (size_t i = 0; i < count; i++) {
        e->result.columns[i].type = items[i].is_count ? MD_INT64 : scope_field(&scope, items[i].column)->type;
        char *name;
        if (items[i].alias)
            name = md_copy(items[i].alias, strlen(items[i].alias));
        else if (items[i].is_count) {
            const char *field = items[i].count_all ? "all" : scope_field(&scope, items[i].column)->name;
            size_t length = strlen(field);
            name = length > SIZE_MAX - 7 ? NULL : malloc(length + 7);
            if (name) {
                memcpy(name, "count_", 6);
                memcpy(name + 6, field, length + 1);
            }
        } else
            name = output_name(&scope, items[i].column);
        e->result.columns[i].name = name;
        if (!name) {
            error = "RESOURCE_ERROR";
            goto done;
        }
        for (size_t j = 0; j < i; j++)
            if (md_equal(name, e->result.columns[j].name)) {
                error = "DUPLICATE_OUTPUT_NAME";
                goto done;
            }
    }
    if (take_word(l, "WHERE")) {
        error = predicate(l, &scope, &filter);
        if (error)
            goto done;
    }
    if (take_word(l, "GROUP")) {
        aggregate.active = true;
        if (!take_word(l, "BY")) {
            error = bad(l);
            goto done;
        }
        do {
            ColumnRef ref = {0};
            size_t index = 0;
            if (l->kind == TK_NUMBER || punct(l, '+') || punct(l, '-'))
                error = "UNSUPPORTED_FEATURE";
            else
                error = reference(l, &ref);
            if (!error)
                error = bind_reference(&scope, &ref, &index);
            reference_free(&ref);
            if (error)
                goto done;
            if (aggregate.key_count == SIZE_MAX / sizeof *aggregate.keys) {
                error = "RESOURCE_ERROR";
                goto done;
            }
            size_t *more = realloc(aggregate.keys, (aggregate.key_count + 1) * sizeof *more);
            if (!more) {
                error = "RESOURCE_ERROR";
                goto done;
            }
            aggregate.keys = more;
            aggregate.keys[aggregate.key_count++] = index;
        } while (take_punct(l, ','));
    }
    if (take_word(l, "HAVING")) {
        error = predicate_parse(l, &scope, &having, &aggregate);
        if (error)
            goto done;
    }
    if (aggregate.active) {
        if (aggregate.key_count > SIZE_MAX - aggregate.count_count) {
            error = "RESOURCE_ERROR";
            goto done;
        }
        aggregate.width = aggregate.key_count + aggregate.count_count;
        for (size_t i = 0; i < count; i++) {
            if (items[i].is_count) {
                items[i].aggregate_index += aggregate.key_count;
                continue;
            }
            size_t k;
            for (k = 0; k < aggregate.key_count; k++)
                if (items[i].column == aggregate.keys[k])
                    break;
            if (k == aggregate.key_count) {
                error = "INVALID_GROUPING";
                goto done;
            }
            items[i].aggregate_index = k;
        }
    }
    if (r->validate_only) goto done;
    size_t stack_count = filter.count > having.count ? filter.count : having.count;
    if (stack_count) {
        if (stack_count > SIZE_MAX / sizeof *stack) {
            error = "RESOURCE_ERROR";
            goto done;
        }
        int *more = realloc(stack, stack_count * sizeof *stack);
        if (!more) {
            error = "RESOURCE_ERROR";
            goto done;
        }
        stack = more;
    }
    /* 모든 정적 검증 뒤에만 스캔한다. scratch의 TEXT는 카탈로그에서 빌리며 해제하지 않는다. */
    if (scope.count == 2) {
        scratch = malloc(scope.width * sizeof *scratch);
        if (!scratch) {
            error = "RESOURCE_ERROR";
            goto done;
        }
    }
    MdTable *left = scope.tables[0], *right = scope.tables[1];
    size_t first = 0, end = left->row_count;
    int64_t candidate;
    if (!right && index_candidate(&filter, left->id, &candidate)) {
        bool found = md_bt_get(&left->index, candidate, &first);
#ifdef MINIDB_TEST_FAULTS
        index_lookups++;
        if (md_fault("index_lookup_fail")) { error = "RESOURCE_ERROR"; goto done; }
#endif
        end = found ? first + 1 : 0;
    }
    for (size_t i = first; i < end; i++) {
        md_execution_point("scan");
        if ((error = md_execution_check()))
            goto done;
#ifdef MINIDB_TEST_FAULTS
        scan_rows++;
#endif
        MdValue *row = left->rows + i * left->count;
        if (scratch)
            memcpy(scratch, row, left->count * sizeof *scratch);
        size_t right_count = right ? right->row_count : 1;
        for (size_t j = 0; j < right_count; j++) {
            md_execution_point("join");
            if ((error = md_execution_check()))
                goto done;
            if (scratch) {
                memcpy(scratch + left->count, right->rows + j * right->count,
                       right->count * sizeof *scratch);
                row = scratch;
                const MdValue *a = row + join_left, *b = row + join_right;
                if (a->type == MD_NULL || b->type == MD_NULL || compare_values(a, b))
                    continue;
            }
            if (filter.count && !predicate_matches(&filter, row, stack))
                continue;
            error = aggregate.active ? aggregate_row(&aggregate, row) :
                    project_row(&e->result, items, count, row, false, right != NULL);
            if (error)
                goto done;
        }
    }
    if (aggregate.active) {
        if (!aggregate.key_count && !aggregate.group_count) {
            size_t index;
            error = aggregate_new_group(&aggregate, NULL, &index);
            if (error)
                goto done;
        }
        for (size_t g = 0; g < aggregate.group_count; g++) {
            if ((error = md_execution_check()))
                goto done;
            MdValue *row = aggregate.groups + g * aggregate.width;
            if (having.count && !predicate_matches(&having, row, stack))
                continue;
            error = project_row(&e->result, items, count, row, true, false);
            if (error)
                goto done;
        }
    }
done:
    for (size_t i = 0; i < count; i++) {
        reference_free(&items[i].ref);
        free(items[i].alias);
    }
    free(items);
    free(scope.aliases[0]);
    free(scope.aliases[1]);
    free(scratch);
    free(stack);
    predicate_free(&having);
    md_values_free(aggregate.groups, aggregate.group_count * aggregate.width);
    free(aggregate.keys);
    free(aggregate.counts);
    predicate_free(&filter);
    return error;
}

static const char *values_query(Lexer *l, MdEvent *e, bool validate_only) {
    LiteralRows rows = {0};
    next(l);
    const char *error = literal_rows(l, &rows);
    if (error)
        goto done;
    e->result.kind = MD_QUERY_RESULT;
    e->result.columns = calloc(rows.column_count, sizeof *e->result.columns);
    if (!e->result.columns) {
        error = "RESOURCE_ERROR";
        goto done;
    }
    e->result.column_count = rows.column_count;
    for (size_t column = 0; column < rows.column_count; column++) {
        MdColumn *field = &e->result.columns[column];
        char name[32];
        snprintf(name, sizeof name, "column%zu", column + 1);
        field->name = md_copy(name, strlen(name));
        if (!field->name) {
            error = "RESOURCE_ERROR";
            goto done;
        }
        for (size_t row = 0; row < rows.row_count; row++) {
            if ((error = md_execution_check()))
                goto done;
            MdType type = rows.values[row * rows.column_count + column].type;
            if (type == MD_NULL)
                continue;
            if (field->type != MD_NULL && field->type != type) {
                error = "TYPE_MISMATCH";
                goto done;
            }
            field->type = type;
        }
    }
    if (validate_only) goto done;
    e->result.values = rows.values;
    e->result.row_count = rows.row_count;
    rows.values = NULL;
    rows.value_count = 0;
done:
    rows_free(&rows);
    return error;
}

/* 합계·셀 수·바이트 수는 모두 연산 전에 검사한다. 두 빈 분기도 유효하다. */
static bool union_size(size_t left, size_t right, size_t width, size_t *bytes) {
    if (!width || right > SIZE_MAX - left)
        return false;
    size_t rows = left + right;
    if (rows && width > SIZE_MAX / rows / sizeof(MdValue))
        return false;
    *bytes = rows * width * sizeof(MdValue);
    return true;
}
#ifdef MINIDB_TEST_FAULTS
bool md_test_union_size(size_t left, size_t right, size_t width, size_t *bytes) {
    return union_size(left, right, width, bytes);
}
#endif

static const char *union_merge(MdResult *left, MdResult *right, bool all, bool validate_only) {
    md_execution_point("union");
    if (md_execution_check())
        return md_execution_check();
    if (left->column_count != right->column_count)
        return "COLUMN_COUNT_MISMATCH";
    for (size_t i = 0; i < left->column_count; i++) {
        MdType a = left->columns[i].type, b = right->columns[i].type;
        if (a != MD_NULL && b != MD_NULL && a != b)
            return "TYPE_MISMATCH";
        if (a == MD_NULL)
            left->columns[i].type = b;
    }
    if (validate_only) return NULL;
    size_t bytes;
    if (!union_size(left->row_count, right->row_count, left->column_count, &bytes) ||
        md_fault("union_merge"))
        return "RESOURCE_ERROR";
    if (bytes) {
        MdValue *more = realloc(left->values, bytes);
        if (!more)
            return "RESOURCE_ERROR";
        left->values = more;
    }
    if (right->row_count)
        memcpy(left->values + left->row_count * left->column_count, right->values,
               right->row_count * right->column_count * sizeof *right->values);
    left->row_count += right->row_count;
    /* TEXT를 복사하지 않고 소유권을 옮긴다. 분기 정리는 빈 배열만 회수한다. */
    free(right->values);
    right->values = NULL;
    right->row_count = 0;
    if (md_fault("union_after_merge"))
        return "RESOURCE_ERROR";
    if (all)
        return NULL;
    QueryTail full = {0};
    if (left->column_count > SIZE_MAX / sizeof *full.keys || md_fault("union_sort_keys"))
        return "RESOURCE_ERROR";
    full.keys = calloc(left->column_count, sizeof *full.keys);
    if (!full.keys)
        return "RESOURCE_ERROR";
    full.count = left->column_count;
    for (size_t i = 0; i < full.count; i++)
        full.keys[i].column = i;
    apply_query_tail(left, &full);
    if (md_execution_check()) {
        free(full.keys);
        return md_execution_check();
    }
    size_t kept = 0;
    for (size_t row = 0; row < left->row_count; row++) {
        if (md_execution_check()) {
            free(full.keys);
            return md_execution_check();
        }
        if (kept && !compare_rows(left, &full, kept - 1, row)) {
            for (size_t col = 0; col < left->column_count; col++) {
                MdValue *value = left->values + row * left->column_count + col;
                if (value->type == MD_TEXT)
                    free(value->as.text.ptr);
                memset(value, 0, sizeof *value);
            }
        } else {
            if (kept != row) {
                memcpy(left->values + kept * left->column_count,
                       left->values + row * left->column_count,
                       left->column_count * sizeof *left->values);
                memset(left->values + row * left->column_count, 0,
                       left->column_count * sizeof *left->values);
            }
            kept++;
        }
    }
    left->row_count = kept;
    free(full.keys);
    return NULL;
}

static const char *query_core(MdRequest *r, Lexer *l, MdEvent *event) {
    if (word(l, "SELECT"))
        return select_query(r, l, event);
    if (word(l, "VALUES"))
        return values_query(l, event, r->validate_only);
    if (punct(l, '('))
        return "UNSUPPORTED_FEATURE";
    return bad(l);
}

static const char *query_execute(MdRequest *r, Lexer *l, MdEvent *event) {
    QueryTail tail = {0};
    const char *error = query_core(r, l, event);
    while (!error && take_word(l, "UNION")) {
        bool all = take_word(l, "ALL");
        MdEvent branch = {0};
        error = query_core(r, l, &branch);
        if (!error)
            error = union_merge(&event->result, &branch.result, all, r->validate_only);
        /* 성공·실패 양쪽에서 분기 메타데이터와 잔여 owned 값을 회수한다. */
        md_event_destroy(&branch);
    }
    if (!error)
        error = query_tail(l, &event->result, &tail);
    if (!error) {
        if (!r->validate_only) apply_query_tail(&event->result, &tail);
        event->has_result = true;
    }
    free(tail.keys);
    return error;
}

static const char *query(MdRequest *r, Lexer *l, MdEvent *event) {
    /* ORDER/페이지/뒤 UNION 분기까지 먼저 의미 검사한다. 이 경로는 행·인덱스를
     * 읽거나 집계·정렬·출력을 실행하지 않고 컬럼 메타데이터만 만든다. */
    Lexer validation = *l;
    validation.text = l->text ? md_copy(l->text, l->text_len) : NULL;
    if (l->text && !validation.text) return "RESOURCE_ERROR";
    MdRequest request = *r;
    request.validate_only = true;
    MdEvent checked = {0};
    const char *error = query_execute(&request, &validation, &checked);
    free(validation.text);
    md_event_destroy(&checked);
    return error ? error : query_execute(r, l, event);
}

static int read_byte(MdRequest *r) {
    if (md_execution_check())
        return -1;
    if (r->cursor == r->used) {
        if (r->eof || r->input_error)
            return -1;
        size_t n = 0;
        MdReadStatus status = r->input.read(r->input.context, r->buffer, sizeof r->buffer, &n);
        if (status == MD_READ_ERROR || (status == MD_READ_DATA && (!n || n > sizeof r->buffer))) {
            r->input_error = true;
            return -1;
        }
        if (status == MD_READ_EOF) {
            r->eof = true;
            return -1;
        }
        r->used = n;
        r->cursor = 0;
    }
    unsigned c = r->buffer[r->cursor++];
    r->offset++;
    if (c == '\r') {
        r->line++;
        r->column = 1;
        r->previous_cr = true;
    } else if (c == '\n') {
        if (!r->previous_cr)
            r->line++;
        r->column = 1;
        r->previous_cr = false;
    } else {
        if ((c & 0xc0) != 0x80)
            r->column++;
        r->previous_cr = false;
    }
    return (int)c;
}
MdNext md_sql_next(MdRequest *r, MdEvent *e) {
    for (;;) {
        unsigned char *raw = NULL;
        size_t n = 0, capacity = 0;
        int state = 0, previous = 0;
        bool terminated = false;
        uint64_t line = r->line, col = r->column, offset = r->offset;
        bool previous_cr = r->previous_cr;
        for (;;) {
            int c = read_byte(r);
            if (c < 0)
                break;
            if (n == capacity) {
                size_t k = capacity ? capacity * 2 : 256;
                if (k < capacity) {
                    free(raw);
                    raw = NULL;
                    break;
                }
                unsigned char *more = realloc(raw, k);
                if (!more) {
                    free(raw);
                    raw = NULL;
                    break;
                }
                raw = more;
                capacity = k;
            }
            raw[n++] = (unsigned char)c;
            if (state == 1) {
                if (c == '\'')
                    state = 0;
            } else if (state == 2) {
                if (c == '\r' || c == '\n')
                    state = 0;
            } else if (state == 3) {
                if (previous == '*' && c == '/') {
                    state = 0;
                    c = 0;
                }
            } else {
                if (previous == '-' && c == '-')
                    state = 2;
                else if (previous == '/' && c == '*')
                    state = 3;
                else if (c == '\'')
                    state = 1;
                else if (c == ';') {
                    terminated = true;
                    break;
                }
            }
            previous = c;
        }
        const char *error = NULL;
        if (r->cancel_code)
            error = r->cancel_code;
        if (!raw && n == capacity && !r->eof && !r->input_error)
            error = "RESOURCE_ERROR";
        size_t bom = 0;
        if (!r->started) {
            r->started = true;
            if (raw && n >= 3 && !memcmp(raw, "\xef\xbb\xbf", 3)) {
                bom = 3;
                if (r->line == 1)
                    r->column -= 1;
            }
        }
        if (!error && r->input_error)
            error = "IO_ERROR";
        if (!error &&
            (!md_utf8(raw ? raw + bom : NULL, n - bom) || (raw && memchr(raw + bom, 0, n - bom))))
            error = "LEXICAL_ERROR";
        Lexer l = {.raw = raw ? raw + bom : NULL, .len = n - bom};
        if (raw && (!error || !strcmp(error, "LEXICAL_ERROR")))
            next(&l);
        size_t statement_start = l.start;
        const char *dml = word(&l, "INSERT") ? "INSERT" : word(&l, "UPDATE") ? "UPDATE" :
                          word(&l, "DELETE") ? "DELETE" : NULL;
        bool empty = !error && (l.kind == TK_END || (punct(&l, ';') && n > 0));
        if (empty) {
            free(l.text);
            free(raw);
            if (r->eof)
                return MD_NEXT_EOF;
            continue;
        }
        r->statement++;
        if (!error && (state == 1 || state == 3 || l.kind == TK_BAD))
            error = l.kind == TK_BAD ? l.error : "LEXICAL_ERROR";
        if (!error && !terminated)
            error = "SYNTAX_ERROR";
        bool gate = false;
        bool write = dml != NULL;
        if (!error) {
            gate = md_gate_enter(r, write);
            if (!gate)
                error = r->cancel_code;
            else if (r->db->health != MD_HEALTHY) {
                error = "RECOVERY_REQUIRED";
                e->health = r->db->health;
            } else {
                r->sql_start = md_execution_now();
                r->sql_running = true;
                md_execution_point(write ? "gate-write" : "gate-read");
                error = md_execution_check();
            }
        }
        if (!error) {
            if (word(&l, "INSERT"))
                error = insert(r, &l, e);
            else if (word(&l, "UPDATE") || word(&l, "DELETE"))
                error = mutate(r, &l, e, word(&l, "UPDATE"));
            else if (word(&l, "SELECT"))
                error = query(r, &l, e);
            else if (word(&l, "VALUES"))
                error = query(r, &l, e);
            else
                error = "UNSUPPORTED_FEATURE";
        }
        if (!r->protected_commit && md_execution_check()) {
            error = r->cancel_code;
            md_event_destroy(e);
        }
        if (r->protected_commit) {
            r->protected_commit = false;
            /* 반영 뒤 취소는 확정 성공과 기존 cleanup/recovery 진단을 보존한다. */
            if (md_execution_check()) {
                e->stop = true;
                if (e->has_result && !e->has_diagnostic) {
                    md_diag(&e->diagnostic, r->cancel_code, "execute", NULL, NULL,
                            "Committed statement completed before cancellation");
                    e->diagnostic.committed = MD_YES;
                    e->diagnostic.has_affected_rows = true;
                    e->diagnostic.affected_rows = e->result.affected_rows;
                    snprintf(e->diagnostic.command, sizeof e->diagnostic.command, "%s", e->result.command);
                    e->has_diagnostic = true;
                }
            }
        }
        if (error) {
            char code[40];
            snprintf(code, sizeof code, "%s", error);
            if (!e->diagnostic.code[0]) {
                md_event_destroy(e);
                md_diag(&e->diagnostic, code, !strcmp(code, "IO_ERROR") ? "input" : "execute", NULL,
                        NULL, "SQL statement failed");
                e->diagnostic.committed = dml ? MD_NO : MD_UNKNOWN;
                if (dml)
                    strcpy(e->diagnostic.command, dml);
            }
            e->has_diagnostic = e->stop = true;
            r->stopped = true;
        }
        e->result.statement_index = r->statement;
        if (e->has_diagnostic) {
            e->diagnostic.has_statement = true;
            e->diagnostic.statement_index = r->statement;
            if (!r->input_error) {
                /* 알려진 문장 시작 위치를 보존한다. UTF-8 연속 바이트는 열을 늘리지 않는다. */
                for (size_t i = 0; raw && i < statement_start; i++) {
                    unsigned c = raw[bom + i];
                    if (c == '\r') {
                        line++;
                        col = 1;
                        previous_cr = true;
                    } else if (c == '\n') {
                        if (!previous_cr)
                            line++;
                        col = 1;
                        previous_cr = false;
                    } else {
                        if ((c & 0xc0) != 0x80)
                            col++;
                        previous_cr = false;
                    }
                }
                e->diagnostic.has_position = true;
                e->diagnostic.line = line;
                e->diagnostic.column = col;
                e->diagnostic.has_offset = true;
                e->diagnostic.byte_offset = offset + bom + statement_start;
            }
        }
        free(l.text);
        free(raw);
        if (gate) {
            e->health = r->db->health;
            md_gate_leave(r->db, write);
        }
        if (e->stop)
            r->stopped = true;
        return e->has_result ? MD_NEXT_RESULT : MD_NEXT_STOP;
    }
}
