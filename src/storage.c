#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void le_write(unsigned char *p, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)v;
        v >>= 8;
    }
}
static uint64_t le_read(const unsigned char *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static char *table_path(MdDb *db, const MdTable *t) {
    size_t n = strlen(t->name);
    char *name = malloc(n + 11);
    if (!name)
        return NULL;
    memcpy(name, "table_", 6);
    for (size_t i = 0; i < n; i++) {
        char c = t->name[i];
        name[6 + i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    memcpy(name + 6 + n, ".tbl", 5);
    char *path = md_join(db->data_dir, name);
    free(name);
    return path;
}
static bool remove_file(const char *path) {
    wchar_t *w = md_wide(path);
    bool ok = w && DeleteFileW(w);
    free(w);
    return ok;
}
static HANDLE file_open(const char *p, DWORD access, DWORD share, DWORD disposition) {
    wchar_t *w = md_wide(p);
    HANDLE h = w ? CreateFileW(w, access, share, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL)
                 : INVALID_HANDLE_VALUE;
    free(w);
    return h;
}
static bool lease_open(MdDb *db, MdDiagnostic *d) {
    wchar_t *w = md_wide(db->data_dir);
    DWORD attr = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        md_diag(d, "IO_ERROR", "prepare", "data", db->data_dir, "Data directory does not exist");
        return false;
    }
    char *p = md_join(db->data_dir, ".minidb.lock");
    if (!p) {
        md_diag(d, "RESOURCE_ERROR", "prepare", "data", NULL, "Lock path allocation failed");
        return false;
    }
    db->lease = file_open(p, GENERIC_READ | GENERIC_WRITE, 0, OPEN_ALWAYS);
    if (db->lease == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        md_diag(d,
                err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION ? "DATABASE_BUSY"
                                                                              : "IO_ERROR",
                "prepare", "data", p, "Cannot acquire database process lease");
        free(p);
        return false;
    }
    free(p);
    return true;
}
static bool marker_write(const char *p, const char *operation, const char *target,
                         uint64_t original, MdDiagnostic *d) {
    HANDLE h = file_open(p, GENERIC_WRITE, 0, CREATE_NEW);
    if (h == INVALID_HANDLE_VALUE) {
        md_diag(d, "IO_ERROR", "execute", "data", p, "Cannot create new pending marker");
        return false;
    }
    char line[128];
    int n =
        snprintf(line, sizeof line, "MINIDB-PENDING 1\noperation %s\noriginal_length %llu\ntarget ",
                 operation, (unsigned long long)original);
    bool ok = n > 0 && md_write_all(h, line, (size_t)n) &&
              md_write_all(h, target, strlen(target)) && md_write_all(h, "\n", 1) &&
              FlushFileBuffers(h);
    if (!CloseHandle(h))
        ok = false;
    if (!ok)
        md_diag(d, "IO_ERROR", "execute", "data", p,
                "Pending marker write or flush failed; original untouched");
    return ok;
}
static bool residuals(MdDb *db, MdDiagnostic *d) {
    char *pattern = md_join(db->data_dir, "*");
    wchar_t *w = pattern ? md_wide(pattern) : NULL;
    WIN32_FIND_DATAW f;
    HANDLE h = w ? FindFirstFileW(w, &f) : INVALID_HANDLE_VALUE;
    free(w);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        md_diag(d, "IO_ERROR", "prepare", "data", db->data_dir, "Cannot enumerate data directory");
        return false;
    }
    bool ok = true;
    do {
        const wchar_t *n = f.cFileName;
        size_t len = wcslen(n);
        bool pending = !_wcsicmp(n, L".minidb.pending");
        bool temp =
            (len >= 4 && (!_wcsicmp(n + len - 4, L".tmp") || !_wcsicmp(n + len - 4, L".bak"))) &&
            (!_wcsnicmp(n, L"table_", 6) || !_wcsnicmp(n, L".minidb", 7));
        if (pending || temp) {
            md_diag(d, "RECOVERY_REQUIRED", "prepare", "data", db->data_dir,
                    "Unresolved pending marker or engine work file");
            ok = false;
            break;
        }
    } while (FindNextFileW(h, &f));
    DWORD last = GetLastError();
    FindClose(h);
    if (ok && last != ERROR_NO_MORE_FILES) {
        md_diag(d, "IO_ERROR", "prepare", "data", db->data_dir, "Data enumeration failed");
        ok = false;
    }
    return ok;
}
static bool load_table(MdDb *db, MdTable *t, MdDiagnostic *d) {
    char *path = table_path(db, t);
    unsigned char *raw = NULL;
    size_t n = 0;
    if (!path) {
        md_diag(d, "RESOURCE_ERROR", "prepare", "data", NULL, "Table path allocation failed");
        return false;
    }
    if (!md_read_file(path, &raw, &n, d)) {
        strcpy(d->source, "data");
        free(path);
        return false;
    }
    bool ok = false;
    MdTable stored = {0};
    if (n < 20 || memcmp(raw, "MINIDBMS", 8) || le_read(raw + 8, 4) != 1)
        goto format;
    uint64_t schema_len = le_read(raw + 12, 8);
    if (schema_len > n - 20)
        goto format;
    if (schema_len >= 3 && !memcmp(raw + 20, "\xef\xbb\xbf", 3))
        goto format;
    if (!md_schema_parse(raw + 20, (size_t)schema_len, NULL, &stored, d)) {
        if (!strcmp(d->code, "RESOURCE_ERROR"))
            goto end;
        goto format;
    }
    if (!md_schema_same(t, &stored)) {
        md_diag(d, "SCHEMA_MISMATCH", "prepare", "data", path,
                "Stored schema differs from external schema");
        goto end;
    }
    size_t pos = 20 + (size_t)schema_len;
    while (pos < n) {
        if (n - pos < 8)
            goto format;
        uint64_t payload = le_read(raw + pos, 8);
        pos += 8;
        if (payload > n - pos)
            goto format;
        size_t end = pos + (size_t)payload;
        if (t->row_count == SIZE_MAX || t->count > SIZE_MAX / (t->row_count + 1) / sizeof *t->rows)
            goto resource;
        MdValue *next = realloc(t->rows, (t->row_count + 1) * t->count * sizeof *next);
        if (!next)
            goto resource;
        t->rows = next;
        MdValue *row = next + t->row_count * t->count;
        memset(row, 0, t->count * sizeof *row);
        t->row_count++;
        for (size_t i = 0; i < t->count; i++) {
            if (pos >= end)
                goto format;
            unsigned tag = raw[pos++];
            if (tag > 1)
                goto format;
            if (!tag)
                continue;
            row[i].type = t->fields[i].type;
            if (end - pos < 8)
                goto format;
            uint64_t value = le_read(raw + pos, 8);
            pos += 8;
            if (row[i].type == MD_INT64) { /* memcpy는 2의 보수 비트를 부호 있는 값으로 보존한다. */
                memcpy(&row[i].as.integer, &value, 8);
            } else {
                if (value > end - pos || value > SIZE_MAX - 1 || !md_utf8(raw + pos, (size_t)value))
                    goto format;
                row[i].as.text.ptr = md_copy((char *)raw + pos, (size_t)value);
                if (!row[i].as.text.ptr)
                    goto resource;
                row[i].as.text.len = (size_t)value;
                pos += (size_t)value;
            }
        }
        if (pos != end || row[t->id].type != MD_INT64 || row[t->id].as.integer <= 0)
            goto format;
        for (size_t r = 0; r + 1 < t->row_count; r++)
            if (t->rows[r * t->count + t->id].as.integer == row[t->id].as.integer)
                goto format;
    }
    ok = true;
    goto end;
resource:
    md_diag(d, "RESOURCE_ERROR", "prepare", "data", path, "Stored rows allocation failed");
    goto end;
format:
    md_diag(d, "STORAGE_FORMAT_ERROR", "prepare", "data", path,
            "Invalid stored schema, record framing, UTF-8 or ID");
end:
    free(stored.name);
    for (size_t i = 0; i < stored.count; i++)
        free(stored.fields[i].name);
    free(stored.fields);
    free(raw);
    free(path);
    return ok;
}
bool md_storage_open(MdDb *db, MdDiagnostic *d) {
    if (!lease_open(db, d) || !residuals(db, d))
        return false;
    for (size_t i = 0; i < db->count; i++)
        if (!load_table(db, &db->tables[i], d))
            return false;
    return true;
}
bool md_storage_init(MdDb *db, MdEvent *e) {
    MdDiagnostic *d = &e->diagnostic;
    wchar_t *w = md_wide(db->data_dir);
    if (!w) {
        md_diag(d, "RESOURCE_ERROR", "prepare", "data", db->data_dir,
                "Output path allocation failed");
        return false;
    }
    if (GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES) {
        free(w);
        md_diag(d, "IO_ERROR", "prepare", "data", db->data_dir, "Output directory already exists");
        d->output_ready = MD_NO;
        return false;
    }
    /* 성공 이벤트 전체를 생성 전에 준비한다. */
    e->result.kind = MD_OPERATION_OK;
    strcpy(e->result.command, "init");
    e->result.output_path = md_copy(db->data_dir, strlen(db->data_dir));
    e->result.tables = calloc(db->count, sizeof *e->result.tables);
    e->result.table_count = db->count;
    if (!e->result.output_path || !e->result.tables)
        goto resource;
    for (size_t i = 0; i < db->count; i++) {
        e->result.tables[i].name = md_copy(db->tables[i].name, strlen(db->tables[i].name));
        e->result.tables[i].action = md_copy("created", 7);
        if (!e->result.tables[i].name || !e->result.tables[i].action)
            goto resource;
    }
    if (!CreateDirectoryW(w, NULL)) {
        free(w);
        md_diag(d, "IO_ERROR", "prepare", "data", db->data_dir, "Cannot create output directory");
        d->output_ready = MD_NO;
        return false;
    }
    free(w);
    w = NULL;
    if (!lease_open(db, d))
        goto failed;
    char *pending = md_join(db->data_dir, ".minidb.pending");
    if (!pending)
        goto resource;
    if (!marker_write(pending, "init", db->data_dir, 0, d)) {
        free(pending);
        goto failed;
    }
    for (size_t i = 0; i < db->count; i++) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *schema = doc ? md_schema_json(doc, &db->tables[i]) : NULL;
        if (!schema) {
            if (doc)
                yyjson_mut_doc_free(doc);
            free(pending);
            goto resource;
        }
        yyjson_mut_doc_set_root(doc, schema);
        size_t len = 0;
        char *json = yyjson_mut_write(doc, 0, &len);
        yyjson_mut_doc_free(doc);
        char *path = table_path(db, &db->tables[i]);
        if (!json || !path) {
            free(json);
            free(path);
            free(pending);
            goto resource;
        }
        HANDLE h = file_open(path, GENERIC_WRITE, 0, CREATE_NEW);
        unsigned char header[20];
        memcpy(header, "MINIDBMS", 8);
        le_write(header + 8, 1, 4);
        le_write(header + 12, len, 8);
        bool ok = h != INVALID_HANDLE_VALUE && md_write_all(h, header, 20) &&
                  md_write_all(h, json, len) && FlushFileBuffers(h);
        if (h != INVALID_HANDLE_VALUE && !CloseHandle(h))
            ok = false;
        free(json);
        if (!ok) {
            md_diag(d, "IO_ERROR", "execute", "data", path, "Initial table write failed");
            free(path);
            free(pending);
            goto failed;
        }
        free(path);
    }
    for (size_t i = 0; i < db->count; i++)
        if (!load_table(db, &db->tables[i], d)) {
            free(pending);
            goto failed;
        }
    if (!remove_file(pending)) {
        md_diag(d, "IO_ERROR", "cleanup", "data", pending, "Initial pending marker cleanup failed");
        free(pending);
        goto failed;
    }
    free(pending);
    e->has_result = true;
    return true;
resource:
    free(w);
    md_diag(d, "RESOURCE_ERROR", db->lease == INVALID_HANDLE_VALUE ? "prepare" : "execute", "data",
            db->data_dir, "Initialization allocation failed");
failed:
    d->output_ready = MD_NO;
    return false;
}
bool md_storage_insert(MdDb *db, MdTable *t, MdValue *row, MdEvent *e) {
    MdDiagnostic *d = &e->diagnostic;
    size_t len = t->count;
    for (size_t i = 0; i < t->count; i++)
        if (row[i].type != MD_NULL) {
            size_t add = row[i].type == MD_TEXT ? row[i].as.text.len : 0;
            if (add > SIZE_MAX - 8 || len > SIZE_MAX - 8 - add)
                goto resource;
            len += 8 + add;
        }
    if (len > SIZE_MAX - 8)
        goto resource;
    unsigned char *bytes = malloc(len + 8);
    if (!bytes)
        goto resource;
    le_write(bytes, len, 8);
    size_t pos = 8;
    for (size_t i = 0; i < t->count; i++) {
        bytes[pos++] = (row[i].type != MD_NULL);
        if (row[i].type == MD_INT64) {
            uint64_t bits;
            memcpy(&bits, &row[i].as.integer, 8);
            le_write(bytes + pos, bits, 8);
            pos += 8;
        } else if (row[i].type == MD_TEXT) {
            le_write(bytes + pos, row[i].as.text.len, 8);
            pos += 8;
            memcpy(bytes + pos, row[i].as.text.ptr, row[i].as.text.len);
            pos += row[i].as.text.len;
        }
    }
    if (t->row_count == SIZE_MAX || t->count > SIZE_MAX / (t->row_count + 1) / sizeof *row) {
        free(bytes);
        goto resource;
    }
    MdValue *next = realloc(t->rows, (t->row_count + 1) * t->count * sizeof *next);
    if (!next) {
        free(bytes);
        goto resource;
    }
    t->rows = next;
    char *path = table_path(db, t), *pending = md_join(db->data_dir, ".minidb.pending");
    if (!path || !pending) {
        free(path);
        free(pending);
        free(bytes);
        goto resource;
    }
    HANDLE h = file_open(path, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING);
    LARGE_INTEGER original;
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &original) || original.QuadPart < 0) {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        md_diag(d, "IO_ERROR", "execute", "data", path, "Cannot open append target");
        goto early;
    }
    if (len + 8 > (uint64_t)INT64_MAX ||
        (uint64_t)original.QuadPart > (uint64_t)INT64_MAX - (len + 8)) {
        CloseHandle(h);
        md_diag(d, "RESOURCE_ERROR", "execute", "data", path,
                "Append would exceed Windows signed file offset range");
        d->committed = MD_NO;
        goto early;
    }
    if (!marker_write(pending, "INSERT", path, (uint64_t)original.QuadPart, d)) {
        CloseHandle(h);
        goto early;
    }
    /* 성공 결과는 고정 크기이며 append 전 확정 건수까지 준비되어 있다. */
    e->result.kind = MD_STATEMENT_OK;
    strcpy(e->result.command, "INSERT");
    e->result.affected_rows = 1;
    bool ok = SetFilePointerEx(h, original, NULL, FILE_BEGIN) != 0;
    if (ok && (md_fault("append_write") || md_fault("rollback"))) {
        (void)md_write_all(h, bytes, (len + 8) / 2);
        ok = false;
    } else if (ok)
        ok = md_write_all(h, bytes, len + 8) && FlushFileBuffers(h);
    if (!ok) {
        bool restored = !md_fault("rollback") && SetFilePointerEx(h, original, NULL, FILE_BEGIN) &&
                        SetEndOfFile(h) && FlushFileBuffers(h);
        LARGE_INTEGER actual;
        if (restored)
            restored = GetFileSizeEx(h, &actual) && actual.QuadPart == original.QuadPart;
        CloseHandle(h);
        if (restored) {
            md_diag(d, "IO_ERROR", "execute", "data", path,
                    "Append failed; original length restored");
            d->committed = MD_NO;
            d->rollback = MD_ROLLBACK_CONFIRMED;
            if (!remove_file(pending)) {
                md_diag(d, "RECOVERY_REQUIRED", "rollback", "data", pending,
                        "Rollback completed but pending marker could not be removed");
                d->committed = MD_NO;
                d->rollback = MD_ROLLBACK_CONFIRMED;
                db->health = MD_RECOVERY_REQUIRED;
            }
        } else {
            md_diag(d, "RECOVERY_REQUIRED", "rollback", "data", path,
                    "Append rollback failed; files and marker preserved");
            d->rollback = MD_ROLLBACK_FAILED;
            db->health = MD_RECOVERY_REQUIRED;
        }
        strcpy(d->command, "INSERT");
        e->health = db->health;
        goto early;
    }
    /* flush가 반영 경계다. 이후에는 결과를 미반영 실패로 바꾸지 않는다. */
    memcpy(t->rows + t->row_count * t->count, row, t->count * sizeof *row);
    memset(row, 0, t->count * sizeof *row);
    t->row_count++;
    e->has_result = true;
    bool closed = CloseHandle(h) != 0;
    bool cleaned =
        closed && !md_fault("cleanup") && !md_fault("stdout_cleanup") && remove_file(pending);
    if (!cleaned) {
        md_diag(d, "IO_ERROR", "cleanup", "data", pending,
                "Committed INSERT pending marker cleanup failed");
        d->committed = MD_YES;
        strcpy(d->command, "INSERT");
        d->has_affected_rows = true;
        d->affected_rows = 1;
        e->has_diagnostic = e->stop = true;
        db->health = MD_STOPPED_CLEANUP;
        e->health = db->health;
    }
    free(bytes);
    free(path);
    free(pending);
    return true;
early:
    free(bytes);
    free(path);
    free(pending);
    return false;
resource:
    md_diag(d, "RESOURCE_ERROR", "execute", "data", NULL, "INSERT staging allocation failed");
    d->committed = MD_NO;
    return false;
}
