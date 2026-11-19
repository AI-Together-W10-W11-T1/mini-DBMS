#include "format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mf_begin(Buffer *b) {
    memset(b, 0, sizeof *b);
    b->p = b->small;
    b->cap = sizeof b->small;
}
void mf_end(Buffer *b) {
    if (b->p != b->small)
        free(b->p);
}
static void putn(Buffer *b, const char *p, size_t n) {
    if (b->failed)
        return;
    if (n > SIZE_MAX - b->n - 1) {
        b->failed = true;
        return;
    }
    size_t needed = b->n + n + 1;
    if (needed > b->cap) {
        size_t cap = b->cap;
        while (cap < needed) {
            if (cap > SIZE_MAX / 2) {
                cap = needed;
                break;
            }
            cap *= 2;
        }
        char *next = malloc(cap);
        if (!next) {
            b->failed = true;
            return;
        }
        memcpy(next, b->p, b->n);
        if (b->p != b->small)
            free(b->p);
        b->p = next;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void put(Buffer *b, const char *p) { putn(b, p, strlen(p)); }
static void number(Buffer *b, uint64_t n, bool quote) {
    char s[32];
    snprintf(s, sizeof s, "%llu", (unsigned long long)n);
    if (quote)
        put(b, "\"");
    put(b, s);
    if (quote)
        put(b, "\"");
}
static void quoted(Buffer *b, const char *p, size_t n, bool table) {
    put(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned c = (unsigned char)p[i];
        if (c == '"')
            put(b, "\\\"");
        else if (c == '\\')
            put(b, "\\\\");
        else if (c == '\n')
            put(b, "\\n");
        else if (c == '\r')
            put(b, "\\r");
        else if (c == '\t')
            put(b, "\\t");
        else if (c < 32 || (table && c == '|')) {
            char x[8];
            snprintf(x, sizeof x, "\\u%04x", c);
            put(b, x);
        } else
            putn(b, p + i, 1);
    }
    put(b, "\"");
}
static void string(Buffer *b, const char *s, bool table) {
    if (!s || !*s)
        put(b, "null");
    else
        quoted(b, s, strlen(s), table);
}
static void nullable_number(Buffer *b, uint64_t n, bool known) {
    if (known)
        number(b, n, true);
    else
        put(b, "null");
}
static void known(Buffer *b, MdKnown k) {
    put(b, k == MD_YES ? "true" : k == MD_NO ? "false" : "null");
}
static const char *type_name(MdType t) {
    return t == MD_INT64 ? "INT64" : t == MD_TEXT ? "TEXT" : "NULL";
}
bool mf_stream_write(DWORD id, const char *p, size_t n) {
    HANDLE h = GetStdHandle(id);
    while (n) {
        DWORD wrote = 0, take = (DWORD)(n > 0x100000 ? 0x100000 : n);
        if (!WriteFile(h, p, take, &wrote, NULL) || !wrote)
            return false;
        p += wrote;
        n -= wrote;
    } /* WriteFile는 CRT 버퍼를 거치지 않는다. 별도 미출력 버퍼가 없다. */
    return true;
}
void mf_diagnostic_render(const MdDiagnostic *d, const char *operation, bool json, Buffer *out) {
    Buffer b;
    mf_begin(&b);
    const char *rollback = d->rollback == MD_ROLLBACK_CONFIRMED ? "confirmed"
                           : d->rollback == MD_ROLLBACK_FAILED  ? "failed"
                           : d->rollback == MD_ROLLBACK_UNKNOWN ? "unknown"
                                                                : "not_needed";
    if (json) {
        put(&b, "{\"version\":1,\"type\":\"diagnostic\",\"code\":");
        string(&b, d->code, false);
        put(&b, ",\"message\":");
        string(&b, d->message, false);
        put(&b, ",\"operation\":");
        string(&b, operation, false);
        put(&b, ",\"phase\":");
        string(&b, d->phase, false);
    } else {
        put(&b, "diagnostic code=");
        put(&b, d->code);
        put(&b, " operation=");
        put(&b, operation ? operation : "null");
        put(&b, " phase=");
        put(&b, d->phase);
        put(&b, " message=");
        string(&b, d->message, true);
    }
#define KEY(k) put(&b, json ? ",\"" k "\":" : " " k "=")
    KEY("source");
    string(&b, d->source, !json);
    KEY("path");
    string(&b, d->path, !json);
    KEY("statement_index");
    nullable_number(&b, d->statement_index, d->has_statement);
    KEY("line");
    nullable_number(&b, d->line, d->has_position);
    KEY("column");
    nullable_number(&b, d->column, d->has_position);
    KEY("field_path");
    string(&b, d->field_path, !json);
    KEY("byte_offset");
    nullable_number(&b, d->byte_offset, d->has_offset);
    KEY("committed");
    known(&b, d->committed);
    KEY("rollback");
    string(&b, rollback, !json);
    KEY("command");
    string(&b, d->command, !json);
    KEY("affected_rows");
    nullable_number(&b, d->affected_rows, d->has_affected_rows);
    KEY("output_ready");
    known(&b, d->output_ready);
    if (json)
        put(&b, "}");
    put(&b, "\n");
    putn(out, b.p, b.n);
    if (b.failed) out->failed = true;
    mf_end(&b);
#undef KEY
}
void mf_diagnostic(const MdDiagnostic *d, const char *operation, bool json) {
    Buffer b; mf_begin(&b);
    mf_diagnostic_render(d, operation, json, &b);
    if (!b.failed) (void)mf_stream_write(STD_ERROR_HANDLE, b.p, b.n);
    mf_end(&b);
}
static void render_value(Buffer *b, const MdValue *v, bool json) {
    if (v->type == MD_NULL)
        put(b, json ? "null" : "NULL");
    else if (v->type == MD_TEXT)
        quoted(b, v->as.text.ptr, v->as.text.len, !json);
    else {
        char s[32];
        snprintf(s, sizeof s, "%lld", (long long)v->as.integer);
        if (json)
            put(b, "\"");
        put(b, s);
        if (json)
            put(b, "\"");
    }
}
bool mf_render(const MdResult *r, bool json, Buffer *b) {
    if (r->kind == MD_STATEMENT_OK) {
        if (json) {
            put(b, "{\"version\":1,\"type\":\"statement_ok\",\"statement_index\":");
            number(b, r->statement_index, true);
            put(b, ",\"command\":");
            string(b, r->command, false);
            put(b, ",\"affected_rows\":");
            number(b, r->affected_rows, true);
            put(b, ",\"committed\":true}\n");
        } else {
            put(b, "statement ");
            number(b, r->statement_index, false);
            put(b, " ");
            put(b, r->command);
            put(b, " affected_rows ");
            number(b, r->affected_rows, false);
            put(b, " committed true\n");
        }
        return !b->failed;
    }
    if (r->kind == MD_QUERY_RESULT) {
        if (json) {
            put(b, "{\"version\":1,\"type\":\"query_result\",\"statement_index\":");
            number(b, r->statement_index, true);
            put(b, ",\"columns\":[");
        } else {
            put(b, "statement ");
            number(b, r->statement_index, false);
            put(b, " query_result\n");
        }
        for (size_t i = 0; i < r->column_count; i++) {
            if (i)
                put(b, json ? "," : "|");
            if (json)
                put(b, "{\"name\":");
            quoted(b, r->columns[i].name, strlen(r->columns[i].name), !json);
            put(b, json ? ",\"type\":" : ":");
            if (json)
                quoted(b, type_name(r->columns[i].type), strlen(type_name(r->columns[i].type)),
                       false);
            else
                put(b, type_name(r->columns[i].type));
            if (json)
                put(b, "}");
        }
        put(b, json ? "],\"rows\":[" : "\n");
        for (size_t i = 0; i < r->row_count; i++) {
            if (json && i)
                put(b, ",");
            if (json)
                put(b, "[");
            for (size_t j = 0; j < r->column_count; j++) {
                if (j)
                    put(b, json ? "," : "|");
                render_value(b, &r->values[i * r->column_count + j], json);
            }
            put(b, json ? "]" : "\n");
        }
        put(b, json ? "],\"row_count\":" : "row_count ");
        number(b, r->row_count, json);
        put(b, json ? "}\n" : "\n");
        return !b->failed;
    }
    if (json) {
        put(b, "{\"version\":1,\"type\":\"operation_ok\",\"operation\":");
        string(b, r->command, false);
        put(b, ",\"output_path\":");
        string(b, r->output_path, false);
        put(b, ",\"tables\":[");
    } else {
        put(b, "operation ");
        put(b, r->command);
        put(b, " output_path ");
        string(b, r->output_path, true);
        put(b, " output_ready true\n");
    }
    for (size_t i = 0; i < r->table_count; i++) {
        MdTableResult *t = &r->tables[i];
        if (json && i)
            put(b, ",");
        put(b, json ? "{\"name\":" : "table ");
        string(b, t->name, !json);
        put(b, json ? ",\"action\":" : " action ");
        if (json)
            string(b, t->action, false);
        else
            put(b, t->action);
        put(b, json ? ",\"row_count\":" : " row_count ");
        number(b, t->row_count, json);
        put(b, json ? "}" : "\n");
    }
    if (json)
        put(b, "],\"output_ready\":true}\n");
    return !b->failed;
}
int mf_diagnostic_exit(const MdDiagnostic *d, bool preparing) {
    if (!strcmp(d->phase, "arguments"))
        return 2;
    if (preparing)
        return 3;
    if (d->output_ready == MD_NO)
        return 4;
    if (d->rollback == MD_ROLLBACK_FAILED || d->rollback == MD_ROLLBACK_UNKNOWN ||
        !strcmp(d->code, "RECOVERY_REQUIRED"))
        return 6;
    if (!strcmp(d->phase, "cleanup") && d->committed == MD_YES)
        return 5;
    if (!strcmp(d->phase, "output"))
        return 7;
    return 4;
}
int mf_combine(int a, int b) {
    static const int rank[] = {0, 0, 1, 2, 3, 6, 7, 4, 5};
    return rank[a] >= rank[b] ? a : b;
}
