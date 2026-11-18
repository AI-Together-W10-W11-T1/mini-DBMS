#define WIN32_LEAN_AND_MEAN
#include "minidb.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
static char *md_copy(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}

typedef struct {
    char small[4096];
    char *p;
    size_t n, cap;
    bool failed;
} Buffer;
static void begin(Buffer *b) {
    memset(b, 0, sizeof *b);
    b->p = b->small;
    b->cap = sizeof b->small;
}
static void end(Buffer *b) {
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
static bool stream_write(DWORD id, const char *p, size_t n) {
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
static void diagnostic(const MdDiagnostic *d, const char *operation, bool json) {
    Buffer b;
    begin(&b);
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
    if (!b.failed)
        (void)stream_write(STD_ERROR_HANDLE, b.p, b.n);
    end(&b);
#undef KEY
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
static bool render(const MdResult *r, bool json, Buffer *b) {
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
static int diagnostic_exit(const MdDiagnostic *d, bool preparing) {
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
static int combine(int a, int b) {
    static const int rank[] = {0, 0, 1, 2, 3, 5, 6, 4};
    return rank[a] >= rank[b] ? a : b;
}
static bool fault(const char *mode) {
#ifdef MINIDB_TEST_FAULTS
    const char *s = getenv("MINIDB_TEST_FAULT");
    return s && !strcmp(s, mode);
#else
    (void)mode;
    return false;
#endif
}
static int event_output(MdEvent *e, const char *operation, const char *source, const char *path,
                        bool json, bool preparing) {
    int code = 0;
    if (e->has_diagnostic) {
        if (!e->diagnostic.source[0] && source)
            snprintf(e->diagnostic.source, sizeof e->diagnostic.source, "%s", source);
        if (!e->diagnostic.path[0] && path && strlen(path) < sizeof e->diagnostic.path)
            strcpy(e->diagnostic.path, path);
        diagnostic(&e->diagnostic, operation, json);
        code = diagnostic_exit(&e->diagnostic, preparing);
    }
    if (e->has_result) {
        Buffer b;
        begin(&b);
        bool rendered = render(&e->result, json, &b);
        bool injected =
            (e->result.kind == MD_STATEMENT_OK && (fault("stdout") || fault("stdout_cleanup"))) ||
            (e->result.kind == MD_OPERATION_OK && fault("manage_stdout"));
        bool ok = rendered && !injected && stream_write(STD_OUTPUT_HANDLE, b.p, b.n);
        if (!ok) {
            MdDiagnostic d = {0};
            strcpy(d.code, rendered ? "IO_ERROR" : "RESOURCE_ERROR");
            strcpy(d.phase, "output");
            strcpy(d.source, "stdout");
            strcpy(d.message,
                   rendered ? "stdout write failed" : "Result serialization allocation failed");
            if (e->result.kind == MD_STATEMENT_OK) {
                d.committed = MD_YES;
                strcpy(d.command, e->result.command);
                d.has_affected_rows = true;
                d.affected_rows = e->result.affected_rows;
            }
            if (e->result.kind == MD_OPERATION_OK)
                d.output_ready = MD_YES;
            else {
                d.has_statement = true;
                d.statement_index = e->result.statement_index;
            }
            diagnostic(&d, operation, json);
            code = combine(code, 7);
            e->stop = true;
        }
        end(&b);
    }
    return code;
}
static MdReadStatus input_read(void *context, unsigned char *buffer, size_t cap, size_t *length) {
    HANDLE h = *(HANDLE *)context;
    DWORD got = 0;
    *length = 0;
    if (!ReadFile(h, buffer, (DWORD)cap, &got, NULL)) {
        if (GetLastError() == ERROR_BROKEN_PIPE)
            return MD_READ_EOF;
        return MD_READ_ERROR;
    }
    if (!got)
        return MD_READ_EOF;
    *length = got;
    return MD_READ_DATA;
}
static int plain_output(const char *text, const char *operation);
static int usage(const char *operation) {
    const char *s =
        "minidb init --schema-dir S --data-dir NEW [--format table|jsonl]\nminidb run --schema-dir "
        "S --data-dir D (--file F | --stdin) [--format table|jsonl]\nminidb migrate "
        "--source-data-dir OLD --schema-dir S --data-dir NEW [--plan P] [--format table|jsonl]\n";
    return plain_output(s, operation);
}
static void setup_diag(MdDiagnostic *d, const char *code, const char *phase, const char *source,
                       const char *path, const char *message) {
    memset(d, 0, sizeof *d);
    snprintf(d->code, sizeof d->code, "%s", code);
    snprintf(d->phase, sizeof d->phase, "%s", phase);
    snprintf(d->message, sizeof d->message, "%s", message);
    if (source)
        snprintf(d->source, sizeof d->source, "%s", source);
    if (path && strlen(path) < sizeof d->path)
        strcpy(d->path, path);
}
static int plain_output(const char *text, const char *operation) {
    if (stream_write(STD_OUTPUT_HANDLE, text, strlen(text)))
        return 0;
    /* help/version도 출력 실패 계약을 따른다. DB를 열지 않았으므로 반영 사실은 미확인이다. */
    MdDiagnostic d;
    setup_diag(&d, "IO_ERROR", "output", "stdout", NULL, "stdout write failed");
    diagnostic(&d, operation, false);
    return 7;
}
static int cli(int argc, char **argv) {
    const char *operation = NULL, *schema = NULL, *data = NULL, *file = NULL, *old = NULL,
               *plan = NULL;
    bool stdin_flag = false, json = false;
    unsigned seen = 0;
    MdDiagnostic d = {0};
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        const char *v = "minidb 0.1.0\n";
        return plain_output(v, NULL);
    }
    if ((argc == 2 && !strcmp(argv[1], "--help")) ||
        (argc == 3 &&
         (!strcmp(argv[1], "init") || !strcmp(argv[1], "run") || !strcmp(argv[1], "migrate")) &&
         !strcmp(argv[2], "--help"))) {
        return usage(argc == 3 ? argv[1] : NULL);
    }
    if (argc > 1 &&
        (!strcmp(argv[1], "run") || !strcmp(argv[1], "init") || !strcmp(argv[1], "migrate")))
        operation = argv[1];
    bool valid = operation != NULL;
    for (int i = 2; i < argc; i++) {
        unsigned bit = 0;
        const char **target = NULL;
        if (!strcmp(argv[i], "--schema-dir")) {
            bit = 1;
            target = &schema;
        } else if (!strcmp(argv[i], "--data-dir")) {
            bit = 2;
            target = &data;
        } else if (!strcmp(argv[i], "--file")) {
            bit = 4;
            target = &file;
        } else if (!strcmp(argv[i], "--stdin")) {
            bit = 8;
            stdin_flag = true;
        } else if (!strcmp(argv[i], "--format")) {
            bit = 16;
        } else if (!strcmp(argv[i], "--source-data-dir")) {
            bit = 32;
            target = &old;
        } else if (!strcmp(argv[i], "--plan")) {
            bit = 64;
            target = &plan;
        } else {
            valid = false;
            continue;
        }
        if (seen & bit)
            valid = false;
        seen |= bit;
        if (bit == 8)
            continue;
        if (i + 1 >= argc || !argv[i + 1][0] || !strncmp(argv[i + 1], "--", 2)) {
            valid = false;
            continue;
        }
        const char *value = argv[++i];
        if (bit == 16) {
            if (!strcmp(value, "jsonl"))
                json = true;
            else if (!strcmp(value, "table"))
                json = false;
            else
                valid = false;
        } else
            *target = value;
    }
    if (!schema || !data)
        valid = false;
    if (operation && !strcmp(operation, "run")) {
        if ((file != NULL) == stdin_flag || old || plan)
            valid = false;
    } else if (operation && !strcmp(operation, "init")) {
        if (file || stdin_flag || old || plan)
            valid = false;
    } else if (operation && !strcmp(operation, "migrate")) {
        if (!old || file || stdin_flag)
            valid = false;
    }
    if (!valid) {
        if (!operation)
            json = false;
        setup_diag(&d, "ARGUMENT_ERROR", "arguments", "arguments", NULL,
                   "Invalid command arguments");
        diagnostic(&d, operation, json);
        return 2;
    }
    MdEvent e = {0};
    if (strcmp(operation, "run")) {
        MdOperationOptions o = {.operation = !strcmp(operation, "init") ? MD_INIT : MD_MIGRATE,
                                .schema_dir = schema,
                                .data_dir = data,
                                .source_data_dir = old,
                                .plan_path = plan};
        (void)md_operate(&o, &e);
        bool preparing = !strcmp(e.diagnostic.phase, "prepare");
        int code = event_output(&e, operation, NULL, NULL, json, preparing);
        md_event_destroy(&e);
        return code;
    }
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    char *input_path = NULL;
    if (file) {
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, file, -1, NULL, 0);
        wchar_t *w = n ? malloc((size_t)n * sizeof *w) : NULL;
        if (w)
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, file, -1, w, n);
        input = w ? CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL)
                  : INVALID_HANDLE_VALUE;
        free(w);
        input_path = md_copy(file, strlen(file));
        if (input == INVALID_HANDLE_VALUE) {
            setup_diag(&d, "IO_ERROR", "prepare", "sql_file", file, "Cannot open SQL file");
            diagnostic(&d, operation, json);
            free(input_path);
            return 3;
        }
    }
    MdDb *db = NULL;
    if (!md_open(schema, data, &db, &d)) {
        diagnostic(&d, operation, json);
        if (file)
            CloseHandle(input);
        free(input_path);
        return 3;
    }
    MdRequest *request = NULL;
    if (!md_request_open(db, (MdInput){input_read, &input}, &request, &d)) {
        diagnostic(&d, operation, json);
        md_close(db);
        if (file)
            CloseHandle(input);
        free(input_path);
        return 4;
    }
    int code = 0;
    for (;;) {
        MdNext next = md_request_next(request, &e);
        if (next == MD_NEXT_EOF)
            break;
        code = combine(code, event_output(&e, operation, file ? "sql_file" : "stdin", input_path,
                                          json, false));
        bool stop = e.stop;
        md_event_destroy(&e);
        if (stop)
            break;
    }
    md_request_close(request);
    md_close(db);
    if (file)
        CloseHandle(input);
    free(input_path);
    return code;
}
int wmain(int argc, wchar_t **wide) {
    char **argv = calloc((size_t)argc, sizeof *argv);
    if (!argv)
        return 3;
    bool ok = true;
    for (int i = 0; i < argc; i++) {
        int n =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL);
        argv[i] = n ? malloc((size_t)n) : NULL;
        if (!argv[i]) {
            ok = false;
            break;
        }
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, argv[i], n, NULL, NULL);
    }
    int code = ok ? cli(argc, argv) : 3;
    for (int i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
    return code;
}
