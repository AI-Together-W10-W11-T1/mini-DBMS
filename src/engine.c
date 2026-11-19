#include "internal.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 실행 문맥은 호출 스레드에만 속하고 next_ex 반환 전에 해제한다. */
static thread_local MdRequest *execution_request;
#ifdef MINIDB_TEST_FAULTS
static thread_local MdTestExecutionHook execution_hook;
static thread_local void *execution_hook_context;
static thread_local uint64_t (*execution_clock)(void *);
static thread_local void *execution_clock_context;
void md_test_execution_hook(MdTestExecutionHook hook, void *context) {
    execution_hook = hook;
    execution_hook_context = context;
}
void md_test_execution_clock(uint64_t (*clock)(void *), void *context) {
    execution_clock = clock;
    execution_clock_context = context;
}
#endif
uint64_t md_execution_now(void) {
#ifdef MINIDB_TEST_FAULTS
    if (execution_clock)
        return execution_clock(execution_clock_context);
#endif
    return GetTickCount64();
}
void md_execution_point(const char *point) {
#ifdef MINIDB_TEST_FAULTS
    if (execution_hook)
        execution_hook(point, execution_hook_context);
#else
    (void)point;
#endif
}
static const char *request_check(MdRequest *request) {
    if (request->protected_commit)
        return NULL;
    if (request->cancel_code)
        return request->cancel_code;
    MdCancelReason reason = request->execution.cancel ?
        request->execution.cancel(request->execution.context) : MD_CANCEL_NONE;
    if (reason != MD_CANCEL_NONE) {
        request->cancel_code = "SERVER_STOPPING";
        request->cancel_stage = "shutdown";
    } else if (request->sql_running && request->execution.sql_timeout_ms &&
               md_execution_now() - request->sql_start >= request->execution.sql_timeout_ms) {
        request->cancel_code = "TIMEOUT";
        request->cancel_stage = "sql";
    }
    return request->cancel_code;
}
const char *md_execution_check(void) {
    return execution_request ? request_check(execution_request) : NULL;
}
void md_execution_protect(void) {
    if (execution_request)
        execution_request->protected_commit = true;
}
bool md_gate_enter(MdRequest *request, bool write) {
    MdDb *db = request->db;
    AcquireSRWLockExclusive(&db->gate);
    if (write)
        db->waiting_writers++;
    bool entered = false;
    for (;;) {
        if (request_check(request))
            break;
        if (!db->writer && !db->readers && write) {
            db->writer = true;
            entered = true;
            break;
        }
        if (!write && !db->writer && !db->waiting_writers) {
            db->readers++;
            entered = true;
            break;
        }
        uint64_t budget = request->execution.lock_timeout_ms;
        if (budget && request->lock_wait_ms >= budget) {
            request->cancel_code = "TIMEOUT";
            request->cancel_stage = "lock";
            break;
        }
        DWORD interval = 10;
        if (budget && budget - request->lock_wait_ms < interval)
            interval = (DWORD)(budget - request->lock_wait_ms);
        uint64_t start = md_execution_now();
        md_execution_point("gate-wait");
        SleepConditionVariableSRW(&db->changed, &db->gate, interval, 0);
        md_execution_point("gate-waited");
        uint64_t elapsed = md_execution_now() - start;
        request->lock_wait_ms = elapsed > UINT64_MAX - request->lock_wait_ms ?
            UINT64_MAX : request->lock_wait_ms + elapsed;
        /* 재취득 시점에도 동일 누적 예산을 적용한다. */
        if (budget && request->lock_wait_ms >= budget) {
            request->cancel_code = "TIMEOUT";
            request->cancel_stage = "lock";
            break;
        }
    }
    if (write) {
        db->waiting_writers--;
        WakeAllConditionVariable(&db->changed);
    }
    ReleaseSRWLockExclusive(&db->gate);
    return entered;
}
void md_gate_leave(MdDb *db, bool write) {
    AcquireSRWLockExclusive(&db->gate);
    if (write)
        db->writer = false;
    else
        db->readers--;
    WakeAllConditionVariable(&db->changed);
    ReleaseSRWLockExclusive(&db->gate);
}

char *md_copy(const char *s, size_t n) {
    if (n == SIZE_MAX)
        return NULL;
    char *p = malloc(n + 1);
    if (p) {
        for (size_t offset = 0; offset < n;) {
            if (md_execution_check()) {
                free(p);
                return NULL;
            }
            size_t length = n - offset > 4096 ? 4096 : n - offset;
            memcpy(p + offset, s + offset, length);
            offset += length;
        }
        p[n] = 0;
    }
    return p;
}
bool md_equal(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return false;
    }
    return *a == *b;
}
void md_diag(MdDiagnostic *d, const char *code, const char *phase, const char *source,
             const char *path, const char *message) {
    memset(d, 0, sizeof *d);
    snprintf(d->code, sizeof d->code, "%s", code);
    snprintf(d->phase, sizeof d->phase, "%s", phase);
    snprintf(d->message, sizeof d->message, "%s", message);
    if (source)
        snprintf(d->source, sizeof d->source, "%s",
                 source); /* 잘라낸 경로로 다른 파일을 가리키지 않는다. */
    if (path && strlen(path) < sizeof d->path)
        strcpy(d->path, path);
}
bool md_utf8(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n;) {
        if (md_execution_check())
            return false;
        unsigned c = p[i++];
        if (c < 128)
            continue;
        unsigned count, value, min;
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            value = c & 31;
            min = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            value = c & 15;
            min = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            value = c & 7;
            min = 0x10000;
        } else
            return false;
        if (count > n - i)
            return false;
        while (count--) {
            c = p[i++];
            if ((c & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (c & 63);
        }
        if (value < min || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}
wchar_t *md_wide(const char *s) {
    if (!s)
        return NULL;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (!n)
        return NULL;
    wchar_t *p = malloc((size_t)n * sizeof *p);
    if (p && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, p, n)) {
        free(p);
        p = NULL;
    }
    return p;
}
char *md_path(const char *s) {
    wchar_t *w = md_wide(s);
    if (!w)
        return NULL;
    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    wchar_t *full = malloc(((size_t)n + 1) * sizeof *full);
    if (!full || !n || !GetFullPathNameW(w, n + 1, full, NULL)) {
        free(w);
        free(full);
        return NULL;
    }
    free(w);
    HANDLE h = CreateFileW(full, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD k = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED);
        wchar_t *real = malloc(((size_t)k + 1) * sizeof *real);
        if (real && k && GetFinalPathNameByHandleW(h, real, k + 1, FILE_NAME_NORMALIZED)) {
            free(full);
            full = real;
        } else
            free(real);
        CloseHandle(h);
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, full, -1, NULL, 0, NULL, NULL);
    char *out = malloc((size_t)len);
    if (out)
        WideCharToMultiByte(CP_UTF8, 0, full, -1, out, len, NULL, NULL);
    free(full);
    return out;
}
char *md_join(const char *a, const char *b) {
    size_t x = strlen(a), y = strlen(b);
    if (x > SIZE_MAX - y - 2)
        return NULL;
    char *p = malloc(x + y + 2);
    if (p) {
        memcpy(p, a, x);
        p[x] = '\\';
        memcpy(p + x + 1, b, y + 1);
    }
    return p;
}
bool md_read_file(const char *path, unsigned char **out, size_t *len, MdDiagnostic *d) {
    *out = NULL;
    *len = 0;
    wchar_t *w = md_wide(path);
    HANDLE h = w ? CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL)
                 : INVALID_HANDLE_VALUE;
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        md_diag(d, "IO_ERROR", "prepare", NULL, path, "Cannot open file");
        return false;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || (uint64_t)sz.QuadPart > SIZE_MAX - 1) {
        CloseHandle(h);
        md_diag(d, "IO_ERROR", "prepare", NULL, path, "Invalid file length");
        return false;
    }
    size_t n = (size_t)sz.QuadPart;
    unsigned char *p = malloc(n + 1);
    if (!p) {
        CloseHandle(h);
        md_diag(d, "RESOURCE_ERROR", "prepare", NULL, path, "File allocation failed");
        return false;
    }
    size_t pos = 0;
    while (pos < n) {
        DWORD got = 0, take = (DWORD)((n - pos) > 0x100000 ? 0x100000 : n - pos);
        if (!ReadFile(h, p + pos, take, &got, NULL) || !got) {
            free(p);
            CloseHandle(h);
            md_diag(d, "IO_ERROR", "prepare", NULL, path, "File read failed");
            return false;
        }
        pos += got;
    }
    CloseHandle(h);
    p[n] = 0;
    *out = p;
    *len = n;
    return true;
}
bool md_write_all(HANDLE h, const void *raw, size_t len) {
    const unsigned char *p = raw;
    while (len) {
        DWORD wrote = 0, n = (DWORD)(len > 0x100000 ? 0x100000 : len);
        if (!WriteFile(h, p, n, &wrote, NULL) || !wrote)
            return false;
        p += wrote;
        len -= wrote;
    }
    return true;
}
bool md_fault(const char *mode) {
#ifdef MINIDB_TEST_FAULTS
    const char *v = getenv("MINIDB_TEST_FAULT");
    return v && !strcmp(v, mode);
#else
    (void)mode;
    return false;
#endif
}
void md_values_free(MdValue *v, size_t n) {
    if (!v)
        return;
    for (size_t i = 0; i < n; i++)
        if (v[i].type == MD_TEXT)
            free(v[i].as.text.ptr);
    free(v);
}
bool md_value_copy(MdValue *out, const MdValue *in) {
    *out = *in;
    if (in->type == MD_TEXT) {
        out->as.text.ptr = md_copy(in->as.text.ptr, in->as.text.len);
        return out->as.text.ptr != NULL;
    }
    return true;
}
bool md_open(const char *schema, const char *data, MdDb **out, MdDiagnostic *d) {
    *out = NULL;
    MdDb *db = calloc(1, sizeof *db);
    if (!db) {
        md_diag(d, "RESOURCE_ERROR", "prepare", NULL, NULL, "DB allocation failed");
        return false;
    }
    db->lease = INVALID_HANDLE_VALUE;
    db->data_dir = md_path(data);
    if (!db->data_dir) {
        md_diag(d, "IO_ERROR", "prepare", "data", data, "Invalid data path");
        md_close(db);
        return false;
    }
    if (!md_catalog_load(schema, db, d) || !md_storage_open(db, d)) {
        md_close(db);
        return false;
    }
    *out = db;
    return true;
}
void md_close(MdDb *db) {
    if (!db)
        return;
    if (db->lease != INVALID_HANDLE_VALUE)
        CloseHandle(db->lease);
    md_catalog_free(db->tables, db->count);
    free(db->data_dir);
    free(db);
}
bool md_request_open(MdDb *db, MdInput input, MdRequest **out, MdDiagnostic *d) {
    *out = NULL;
    bool healthy = false;
    if (db) {
        MdRequest health = {.db = db};
        md_gate_enter(&health, false);
        healthy = db->health == MD_HEALTHY;
        md_gate_leave(db, false);
    }
    if (!db || !input.read || !healthy) {
        md_diag(d, "RECOVERY_REQUIRED", "execute", NULL, NULL, "DB is stopped or input invalid");
        return false;
    }
    return md_request_create(db, input, out, d);
}
bool md_request_create(MdDb *db, MdInput input, MdRequest **out, MdDiagnostic *d) {
    *out = NULL;
    MdRequest *r = calloc(1, sizeof *r);
    if (!r) {
        md_diag(d, "RESOURCE_ERROR", "execute", NULL, NULL, "Request allocation failed");
        return false;
    }
    r->db = db;
    r->input = input;
    r->line = r->column = 1;
    *out = r;
    return true;
}
MdNext md_request_next(MdRequest *r, MdEvent *e) {
    return md_request_next_ex(r, NULL, e);
}
MdNext md_request_next_ex(MdRequest *r, const MdExecutionOptions *options, MdEvent *e) {
    memset(e, 0, sizeof *e);
    if (!r || r->stopped)
        return MD_NEXT_EOF;
    r->execution = options ? *options : (MdExecutionOptions){0};
    r->lock_wait_ms = 0;
    r->sql_running = r->protected_commit = false;
    r->cancel_code = r->cancel_stage = NULL;
    execution_request = r;
    MdNext next = MD_NEXT_STOP;
    if (r->execution.lock_timeout_ms > INT64_MAX || r->execution.sql_timeout_ms > INT64_MAX) {
        md_diag(&e->diagnostic, "ARGUMENT_ERROR", "arguments", NULL, NULL, "Execution interval out of range");
        e->has_diagnostic = e->stop = r->stopped = true;
        goto done;
    }
    if (!md_gate_enter(r, false))
        goto cancelled;
    /* 열린 요청도 다른 요청의 저장 실패 이후에는 입력을 읽거나 SQL을 시작하지 않는다. */
    if (r->db->health != MD_HEALTHY) {
        e->has_diagnostic = e->stop = r->stopped = true;
        e->health = r->db->health;
        md_diag(&e->diagnostic, "RECOVERY_REQUIRED", "execute", NULL, NULL, "DB is stopped");
        md_gate_leave(r->db, false);
        goto done;
    }
    md_gate_leave(r->db, false);
    next = md_sql_next(r, e);
    goto done;
cancelled:
    md_diag(&e->diagnostic, r->cancel_code, "execute", NULL, NULL, "Execution cancelled");
    e->has_diagnostic = e->stop = r->stopped = true;
done:
    execution_request = NULL;
    r->execution = (MdExecutionOptions){0};
    return next;
}
void md_request_close(MdRequest *r) { free(r); }
bool md_operate(const MdOperationOptions *o, MdEvent *e) {
    memset(e, 0, sizeof *e);
    MdMigration migration = {0};
    MdDb *target = NULL, *source = NULL;
    bool ok = false;
    if (!o || !o->schema_dir || !o->data_dir ||
        (o->operation != MD_INIT && o->operation != MD_MIGRATE) ||
        (o->operation == MD_MIGRATE && !o->source_data_dir)) {
        md_diag(&e->diagnostic, "ARGUMENT_ERROR", "arguments", "arguments", NULL, "Invalid management options");
        goto done;
    }
    /* plan 구조 오류는 파일/스키마 접근 오류보다 먼저 보고한다. */
    if (o->operation == MD_MIGRATE && !md_plan_load(o->plan_path, &migration, &e->diagnostic))
        goto done;
    target = calloc(1, sizeof *target);
    if (!target) goto resource;
    target->lease = INVALID_HANDLE_VALUE;
    target->data_dir = md_path(o->data_dir);
    if (!target->data_dir) goto resource;
    if (!md_catalog_load(o->schema_dir, target, &e->diagnostic)) goto done;
    if (o->operation == MD_INIT) {
        ok = md_storage_init(target, e);
        goto done;
    }
    source = calloc(1, sizeof *source);
    if (!source) goto resource;
    source->lease = INVALID_HANDLE_VALUE;
    source->data_dir = md_path(o->source_data_dir);
    if (!source->data_dir) goto resource;
    if (!md_storage_source(source, &e->diagnostic) ||
        !md_mapping_prepare(&migration, source, target, &e->diagnostic))
        goto done;
    ok = md_storage_migrate(source, target, &migration, e);
    goto done;
resource:
    md_diag(&e->diagnostic, "RESOURCE_ERROR", "prepare", "data", NULL, "Management context allocation failed");
done:
    if (!ok) {
        e->has_diagnostic = e->stop = true;
        e->diagnostic.output_ready = MD_NO;
    }
    md_migration_free(&migration);
    md_close(target);
    md_close(source);
    return ok;
}
