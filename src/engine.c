#include "internal.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *md_copy(const char *s, size_t n) {
    if (n == SIZE_MAX)
        return NULL;
    char *p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
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
    if (!db || !input.read || db->health != MD_HEALTHY) {
        md_diag(d, "RECOVERY_REQUIRED", "execute", NULL, NULL, "DB is stopped or input invalid");
        return false;
    }
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
    memset(e, 0, sizeof *e);
    if (!r || r->stopped)
        return MD_NEXT_EOF;
    return md_sql_next(r, e);
}
void md_request_close(MdRequest *r) { free(r); }
bool md_operate(const MdOperationOptions *o, MdEvent *e) {
    memset(e, 0, sizeof *e);
    if (o->operation != MD_INIT) {
        e->has_diagnostic = e->stop = true;
        md_diag(&e->diagnostic, "UNSUPPORTED_FEATURE", "prepare", NULL, NULL,
                "migrate is a future Phase 1 increment");
        return false;
    }
    MdDb *db = calloc(1, sizeof *db);
    if (!db) {
        e->has_diagnostic = true;
        md_diag(&e->diagnostic, "RESOURCE_ERROR", "prepare", NULL, NULL, "DB allocation failed");
        return false;
    }
    db->lease = INVALID_HANDLE_VALUE;
    db->data_dir = md_path(o->data_dir);
    bool ok = db->data_dir && md_catalog_load(o->schema_dir, db, &e->diagnostic);
    if (ok)
        ok = md_storage_init(db, e);
    else if (!e->diagnostic.code[0])
        md_diag(&e->diagnostic, "IO_ERROR", "prepare", "data", o->data_dir, "Invalid output path");
    if (!ok) {
        e->has_diagnostic = e->stop = true;
        e->diagnostic.output_ready = MD_NO;
    }
    md_close(db);
    return ok;
}
void md_event_destroy(MdEvent *e) {
    if (!e)
        return;
    if (e->result.columns)
        for (size_t i = 0; i < e->result.column_count; i++)
            free(e->result.columns[i].name);
    md_values_free(e->result.values, e->result.row_count * e->result.column_count);
    if (e->result.tables)
        for (size_t i = 0; i < e->result.table_count; i++) {
            free(e->result.tables[i].name);
            free(e->result.tables[i].action);
        }
    free(e->result.columns);
    free(e->result.tables);
    free(e->result.output_path);
    memset(e, 0, sizeof *e);
}
