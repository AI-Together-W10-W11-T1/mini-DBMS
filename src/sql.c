#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TK_END, TK_WORD, TK_NUMBER, TK_STRING, TK_PUNCT, TK_BAD };
typedef struct {
    const unsigned char *raw;
    size_t len, pos, start;
    int kind;
    char punct;
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
    for (;;) {
        while (l->pos < l->len && space(l->raw[l->pos]))
            l->pos++;
        if (l->pos + 1 < l->len && l->raw[l->pos] == '-' && l->raw[l->pos + 1] == '-') {
            l->pos += 2;
            while (l->pos < l->len && l->raw[l->pos] != '\r' && l->raw[l->pos] != '\n')
                l->pos++;
            continue;
        }
        if (l->pos + 1 < l->len && l->raw[l->pos] == '/' && l->raw[l->pos + 1] == '*') {
            l->pos += 2;
            while (l->pos + 1 < l->len && !(l->raw[l->pos] == '*' && l->raw[l->pos + 1] == '/'))
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
        while (l->pos < l->len && (alpha(l->raw[l->pos]) || digit(l->raw[l->pos])))
            l->pos++;
        l->kind = TK_WORD;
        l->text_len = l->pos - l->start;
        l->text = md_copy((char *)l->raw + l->start, l->text_len);
    } else if (digit(c) || ((c == '-' || c == '+') && l->pos < l->len && digit(l->raw[l->pos]))) {
        while (l->pos < l->len && digit(l->raw[l->pos]))
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
        while (l->pos < l->len) {
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
    } else if (strchr("(),;*=.", c)) {
        l->kind = TK_PUNCT;
        l->punct = (char)c;
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
        bool negative = *s == '-';
        if (*s == '-' || *s == '+')
            s++;
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
static const char *insert(MdRequest *r, Lexer *l, MdEvent *e) {
    const char *error = NULL;
    MdTable *t = NULL;
    MdValue *values = NULL;
    size_t *columns = NULL, n = 0;
    next(l);
    if (!take_word(l, "INTO") || l->kind != TK_WORD || !md_identifier(l->text))
        return bad(l);
    t = table(r->db, l->text);
    if (!t)
        return "UNKNOWN_TABLE";
    next(l);
    values = calloc(t->count, sizeof *values);
    columns = malloc(t->count * sizeof *columns);
    if (!values || !columns) {
        error = "RESOURCE_ERROR";
        goto done;
    }
    if (take_punct(l, '(')) {
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
        if (!take_punct(l, ')')) {
            error = bad(l);
            goto done;
        }
    } else {
        error = "SYNTAX_ERROR";
        goto done;
    }
    if (!take_word(l, "VALUES") || !take_punct(l, '(')) {
        error = bad(l);
        goto done;
    }
    size_t got = 0;
    for (;;) {
        MdValue value = {0};
        if (!literal(l, &value, &error))
            goto done;
        if (got >= n) {
            if (value.type == MD_TEXT)
                free(value.as.text.ptr);
            error = "COLUMN_COUNT_MISMATCH";
            goto done;
        }
        values[columns[got++]] = value;
        if (!take_punct(l, ','))
            break;
    }
    if (got != n) {
        error = "COLUMN_COUNT_MISMATCH";
        goto done;
    }
    if (!take_punct(l, ')')) {
        error = bad(l);
        goto done;
    }
    if (punct(l, ',')) {
        error = "UNSUPPORTED_FEATURE";
        goto done;
    }
    error = finish(l);
    if (error)
        goto done;
    bool has_id = false;
    for (size_t i = 0; i < n; i++)
        if (columns[i] == t->id)
            has_id = true;
    if (!has_id) {
        error = "ID_REQUIRED";
        goto done;
    }
    for (size_t i = 0; i < t->count; i++)
        if (values[i].type != MD_NULL && values[i].type != t->fields[i].type) {
            error = "TYPE_MISMATCH";
            goto done;
        }
    if (values[t->id].type != MD_INT64 || values[t->id].as.integer <= 0) {
        error = "INVALID_ID";
        goto done;
    }
    for (size_t i = 0; i < t->row_count; i++)
        if (t->rows[i * t->count + t->id].as.integer == values[t->id].as.integer) {
            error = "DUPLICATE_ID";
            goto done;
        }
    if (!md_storage_insert(r->db, t, values, e)) {
        error = e->diagnostic.code;
    }
done:
    free(columns);
    md_values_free(values, t ? t->count : 0);
    return error;
}
static const char *select_query(MdRequest *r, Lexer *l, MdEvent *e) {
    const char *error = NULL;
    char **names = NULL;
    size_t n = 0, *projection = NULL;
    MdTable *t = NULL;
    MdValue condition = {0};
    size_t where = SIZE_MAX;
    next(l);
    bool star = take_punct(l, '*');
    if (!star) {
        for (;;) {
            if (l->kind != TK_WORD || !md_identifier(l->text)) {
                error = bad(l);
                goto done;
            }
            if (n == SIZE_MAX / sizeof *names) {
                error = "RESOURCE_ERROR";
                goto done;
            }
            char **more = realloc(names, (n + 1) * sizeof *more);
            if (!more) {
                error = "RESOURCE_ERROR";
                goto done;
            }
            names = more;
            names[n] = md_copy(l->text, l->text_len);
            if (!names[n]) {
                error = "RESOURCE_ERROR";
                goto done;
            }
            n++;
            next(l);
            if (!take_punct(l, ','))
                break;
        }
    }
    if (!take_word(l, "FROM") || l->kind != TK_WORD || !md_identifier(l->text)) {
        error = bad(l);
        goto done;
    }
    t = table(r->db, l->text);
    if (!t) {
        error = "UNKNOWN_TABLE";
        goto done;
    }
    next(l);
    size_t count = star ? t->count : n;
    projection = malloc(count * sizeof *projection);
    if (!projection) {
        error = "RESOURCE_ERROR";
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        projection[i] = star ? i : column(t, names[i]);
        if (projection[i] == SIZE_MAX) {
            error = "UNKNOWN_COLUMN";
            goto done;
        }
        for (size_t j = 0; j < i; j++)
            if (projection[j] == projection[i]) {
                error = "DUPLICATE_OUTPUT_NAME";
                goto done;
            }
    }
    if (take_word(l, "WHERE")) {
        if (l->kind != TK_WORD || !md_identifier(l->text)) {
            error = bad(l);
            goto done;
        }
        where = column(t, l->text);
        if (where == SIZE_MAX) {
            error = "UNKNOWN_COLUMN";
            goto done;
        }
        next(l);
        if (!take_punct(l, '=')) {
            error = bad(l);
            goto done;
        }
        if (!literal(l, &condition, &error))
            goto done;
        if (condition.type != MD_NULL && condition.type != t->fields[where].type) {
            error = "TYPE_MISMATCH";
            goto done;
        }
    }
    if (l->kind == TK_WORD) {
        error = "UNSUPPORTED_FEATURE";
        goto done;
    }
    error = finish(l);
    if (error)
        goto done;
    e->result.kind = MD_QUERY_RESULT;
    e->result.columns = calloc(count, sizeof *e->result.columns);
    if (!e->result.columns) {
        error = "RESOURCE_ERROR";
        goto done;
    }
    e->result.column_count = count;
    for (size_t i = 0; i < count; i++) {
        e->result.columns[i].type = t->fields[projection[i]].type;
        e->result.columns[i].name =
            md_copy(t->fields[projection[i]].name, strlen(t->fields[projection[i]].name));
        if (!e->result.columns[i].name) {
            error = "RESOURCE_ERROR";
            goto done;
        }
    }
    for (size_t i = 0; i < t->row_count; i++) {
        MdValue *row = t->rows + i * t->count;
        bool match = true;
        if (where != SIZE_MAX) {
            MdValue *v = &row[where];
            match = v->type != MD_NULL && condition.type != MD_NULL;
            if (match && v->type == MD_INT64)
                match = v->as.integer == condition.as.integer;
            else if (match)
                match = v->as.text.len == condition.as.text.len &&
                        !memcmp(v->as.text.ptr, condition.as.text.ptr, v->as.text.len);
        }
        if (!match)
            continue;
        size_t rows = e->result.row_count;
        if (rows == SIZE_MAX || count > SIZE_MAX / (rows + 1) / sizeof *row) {
            error = "RESOURCE_ERROR";
            goto done;
        }
        MdValue *more = realloc(e->result.values, (rows + 1) * count * sizeof *more);
        if (!more) {
            error = "RESOURCE_ERROR";
            goto done;
        }
        e->result.values = more;
        memset(more + rows * count, 0, count * sizeof *more);
        e->result.row_count++;
        for (size_t j = 0; j < count; j++)
            if (!md_value_copy(&more[rows * count + j], &row[projection[j]])) {
                error = "RESOURCE_ERROR";
                goto done;
            }
    }
    e->has_result = true;
done:
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
    free(projection);
    if (condition.type == MD_TEXT)
        free(condition.as.text.ptr);
    return error;
}
static int read_byte(MdRequest *r) {
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
        bool is_insert = word(&l, "INSERT");
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
        if (!error) {
            if (word(&l, "INSERT"))
                error = insert(r, &l, e);
            else if (word(&l, "SELECT"))
                error = select_query(r, &l, e);
            else
                error = "UNSUPPORTED_FEATURE";
        }
        if (error) {
            char code[40];
            snprintf(code, sizeof code, "%s", error);
            if (!e->diagnostic.code[0]) {
                md_event_destroy(e);
                md_diag(&e->diagnostic, code, !strcmp(code, "IO_ERROR") ? "input" : "execute", NULL,
                        NULL, "SQL statement failed");
                e->diagnostic.committed = is_insert ? MD_NO : MD_UNKNOWN;
                if (is_insert)
                    strcpy(e->diagnostic.command, "INSERT");
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
        if (e->stop)
            r->stopped = true;
        return e->has_result ? MD_NEXT_RESULT : MD_NEXT_STOP;
    }
}
