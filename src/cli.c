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

#include "format.h"
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
        mf_diagnostic(&e->diagnostic, operation, json);
        code = mf_diagnostic_exit(&e->diagnostic, preparing);
    }
    if (e->has_result) {
        Buffer b;
        mf_begin(&b);
        bool rendered = mf_render(&e->result, json, &b);
        bool injected =
            (e->result.kind == MD_STATEMENT_OK && (fault("stdout") || fault("stdout_cleanup"))) ||
            (e->result.kind == MD_OPERATION_OK && fault("manage_stdout"));
        bool ok = rendered && !injected && mf_stream_write(STD_OUTPUT_HANDLE, b.p, b.n);
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
            mf_diagnostic(&d, operation, json);
            code = mf_combine(code, 7);
            e->stop = true;
        }
        mf_end(&b);
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
    if (mf_stream_write(STD_OUTPUT_HANDLE, text, strlen(text)))
        return 0;
    /* help/version도 출력 실패 계약을 따른다. DB를 열지 않았으므로 반영 사실은 미확인이다. */
    MdDiagnostic d;
    setup_diag(&d, "IO_ERROR", "output", "stdout", NULL, "stdout write failed");
    mf_diagnostic(&d, operation, false);
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
        mf_diagnostic(&d, operation, json);
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
            mf_diagnostic(&d, operation, json);
            free(input_path);
            return 3;
        }
    }
    MdDb *db = NULL;
    if (!md_open(schema, data, &db, &d)) {
        mf_diagnostic(&d, operation, json);
        if (file)
            CloseHandle(input);
        free(input_path);
        return 3;
    }
    MdRequest *request = NULL;
    if (!md_request_open(db, (MdInput){input_read, &input}, &request, &d)) {
        mf_diagnostic(&d, operation, json);
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
        code = mf_combine(code, event_output(&e, operation, file ? "sql_file" : "stdin", input_path,
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
