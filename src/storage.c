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
    bool replacing = !strcmp(operation, "UPDATE") || !strcmp(operation, "DELETE");
    bool management = !strcmp(operation, "init") || !strcmp(operation, "migrate");
    HANDLE h = ((replacing && md_fault("replace_marker_create")) ||
                (management && md_fault("manage_marker_create"))) ? INVALID_HANDLE_VALUE :
        file_open(p, GENERIC_WRITE, 0, CREATE_NEW);
    if (h == INVALID_HANDLE_VALUE) {
        md_diag(d, "IO_ERROR", "execute", "data", p, "Cannot create new pending marker");
        return false;
    }
    char line[128];
    int n =
        snprintf(line, sizeof line, "MINIDB-PENDING 1\noperation %s\noriginal_length %llu\ntarget ",
                 operation, (unsigned long long)original);
    bool ok = !(replacing && md_fault("replace_marker_write")) &&
              !(management && md_fault("manage_marker_write")) &&
              n > 0 && md_write_all(h, line, (size_t)n) &&
              md_write_all(h, target, strlen(target)) && md_write_all(h, "\n", 1) &&
              !(replacing && md_fault("replace_marker_flush")) &&
              !(management && md_fault("manage_marker_flush")) && FlushFileBuffers(h);
    if (!CloseHandle(h))
        ok = false;
    if ((replacing && md_fault("replace_marker_close")) ||
        (management && md_fault("manage_marker_close")))
        ok = false;
    if (!ok)
        md_diag(d, "IO_ERROR", "execute", "data", p,
                "Pending marker write or flush failed; original untouched");
    return ok;
}
static bool residuals_check(MdDb *db, MdDiagnostic *d, bool publishing) {
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
        if ((pending || temp) && !(publishing && (pending || !_wcsicmp(n, L".minidb.output.tmp")))) {
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
        MdBtResult indexed = md_fault("index_startup_alloc") ? MD_BT_NOMEM :
            md_bt_insert(&t->index, row[t->id].as.integer, t->row_count - 1);
        if (indexed == MD_BT_NOMEM) goto resource;
        if (indexed != MD_BT_OK) goto format;
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
static bool residuals(MdDb *db, MdDiagnostic *d) {
    return residuals_check(db, d, false);
}
bool md_storage_open(MdDb *db, MdDiagnostic *d) {
    if (!lease_open(db, d) || !residuals(db, d))
        return false;
    for (size_t i = 0; i < db->count; i++)
        if (!load_table(db, &db->tables[i], d))
            return false;
    return true;
}
/* main/temp/backup 검증은 오류 코드만 보고 파일 상태를 추측하지 않기 위한 것이다. */
static int file_state(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? -1 : 1;
    return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
}

static bool file_equals(const char *path, const unsigned char *expected, size_t length) {
    unsigned char *bytes = NULL;
    size_t size = 0;
    MdDiagnostic diagnostic;
    bool ok = md_read_file(path, &bytes, &size, &diagnostic) && size == length &&
              !memcmp(bytes, expected, size);
    free(bytes);
    return ok;
}

static unsigned char *replacement_bytes(MdTable *t, MdValue *rows, size_t row_count,
                                        const unsigned char *original, size_t size, size_t *length) {
    if (size < 20)
        return NULL;
    uint64_t schema_size = le_read(original + 12, 8);
    if (schema_size > size - 20)
        return NULL;
    size_t header = 20 + (size_t)schema_size, total = header;
    for (size_t r = 0; r < row_count; r++) {
        size_t frame = t->count;
        for (size_t c = 0; c < t->count; c++) {
            MdValue *v = rows + r * t->count + c;
            if (v->type == MD_NULL)
                continue;
            size_t text = v->type == MD_TEXT ? v->as.text.len : 0;
            if (text > SIZE_MAX - 8 || frame > SIZE_MAX - 8 - text)
                return NULL;
            frame += 8 + text;
        }
        if (frame > SIZE_MAX - 8 || total > SIZE_MAX - 8 - frame)
            return NULL;
        total += 8 + frame;
    }
    if (total > INT64_MAX)
        return NULL;
    unsigned char *bytes = malloc(total);
    if (!bytes)
        return NULL;
    memcpy(bytes, original, header);
    size_t pos = header;
    for (size_t r = 0; r < row_count; r++) {
        size_t frame = pos;
        pos += 8;
        for (size_t c = 0; c < t->count; c++) {
            MdValue *v = rows + r * t->count + c;
            bytes[pos++] = v->type != MD_NULL;
            if (v->type == MD_INT64) {
                uint64_t bits;
                memcpy(&bits, &v->as.integer, 8);
                le_write(bytes + pos, bits, 8);
                pos += 8;
            } else if (v->type == MD_TEXT) {
                le_write(bytes + pos, v->as.text.len, 8);
                pos += 8;
                memcpy(bytes + pos, v->as.text.ptr, v->as.text.len);
                pos += v->as.text.len;
            }
        }
        le_write(bytes + frame, pos - frame - 8, 8);
    }
    *length = total;
    return bytes;
}

static bool replace_files(const wchar_t *main, const wchar_t *temp, const wchar_t *backup) {
#ifdef MINIDB_TEST_FAULTS
    /* 성공은 실제 ReplaceFileW다. 희귀 오류는 문서화된 실제 파일 배치를 만들어 주입한다. */
    if (md_fault("replace_1177") || md_fault("replace_restore") ||
        md_fault("replace_restore_check") || md_fault("replace_unexpected")) {
        if (!MoveFileExW(main, backup, 0))
            return false;
        if (md_fault("replace_unexpected")) {
            HANDLE h = CreateFileW(main, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                md_write_all(h, "unexpected-main", 15);
                CloseHandle(h);
            }
        }
        SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
        return false;
    }
    if (md_fault("replace_1175") || md_fault("replace_state") || md_fault("replace_content")) {
        SetLastError(ERROR_UNABLE_TO_REMOVE_REPLACED);
        return false;
    }
    if (md_fault("replace_1176") || md_fault("replace_other")) {
        SetLastError(md_fault("replace_1176") ? ERROR_UNABLE_TO_MOVE_REPLACEMENT : ERROR_ACCESS_DENIED);
        return false;
    }
#endif
    return ReplaceFileW(main, temp, backup, 0, NULL, NULL) != 0;
}

bool md_storage_replace(MdDb *db, MdTable *t, MdValue **rows, size_t row_count,
                        const char *command, uint64_t affected, MdEvent *e) {
    MdDiagnostic *d = &e->diagnostic;
    char *main = table_path(db, t), *temp = NULL, *backup = NULL;
    char *pending = md_join(db->data_dir, ".minidb.pending");
    wchar_t *wm = NULL, *wt = NULL, *wb = NULL, *wp = NULL;
    unsigned char *original = NULL, *bytes = NULL;
    size_t original_size = 0, size = 0;
    bool marker = false, temporary = false, committed = false, restored = false, restore_failed = false;
    MdBptree replacement = {0};
    bool replacing_index = !strcmp(command, "DELETE");
    if (replacing_index) {
        if (md_fault("index_delete_alloc")) goto resource;
        for (size_t i = 0; i < row_count; i++)
            if (md_bt_insert(&replacement, (*rows)[i * t->count + t->id].as.integer, i) != MD_BT_OK)
                goto resource;
    }
    if (!main || !pending)
        goto resource;
    size_t path_length = strlen(main);
    if (path_length > SIZE_MAX - 5)
        goto resource;
    temp = malloc(path_length + 5);
    backup = malloc(path_length + 5);
    if (!temp || !backup)
        goto resource;
    memcpy(temp, main, path_length);
    memcpy(temp + path_length, ".tmp", 5);
    memcpy(backup, main, path_length);
    memcpy(backup + path_length, ".bak", 5);
    wm = md_wide(main); wt = md_wide(temp); wb = md_wide(backup); wp = md_wide(pending);
    if (!wm || !wt || !wb || !wp)
        goto resource;
    if (file_state(wm) != 1 || file_state(wt) != 0 || file_state(wb) != 0 || file_state(wp) != 0)
        goto unresolved;
    if (!md_read_file(main, &original, &original_size, d)) {
        /* 공용 읽기 도우미의 준비 문맥을 현재 SQL 실행/데이터 경계로 보정한다. */
        strcpy(d->phase, "execute");
        strcpy(d->source, "data");
        goto done;
    }
    bytes = replacement_bytes(t, *rows, row_count, original, original_size, &size);
    if (!bytes || md_fault("replace_resource"))
        goto resource;
    /* 성공 결과/새 메모리/직렬화/경로는 원본 변경 전에 모두 준비되어 있다. */
    e->result.kind = MD_STATEMENT_OK;
    strcpy(e->result.command, command);
    e->result.affected_rows = affected;
    if (!marker_write(pending, command, main, original_size, d)) {
        int state = file_state(wp);
        if (state < 0)
            goto unresolved;
        marker = state == 1;
        goto before_replace;
    }
    marker = true;
    HANDLE h = md_fault("replace_temp_create") ? INVALID_HANDLE_VALUE :
        CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        md_diag(d, "IO_ERROR", "execute", "data", temp, "Cannot create replacement temporary file");
        goto before_replace;
    }
    temporary = true;
    bool written;
    if (md_fault("replace_temp_write") || md_fault("replace_pre_cleanup")) {
        (void)md_write_all(h, bytes, size / 2);
        written = false;
    } else
        written = md_write_all(h, bytes, size) && !md_fault("replace_temp_flush") && FlushFileBuffers(h);
    bool closed = CloseHandle(h) != 0;
    if (!written || !closed || md_fault("replace_temp_close")) {
        md_diag(d, "IO_ERROR", "execute", "data", temp, "Replacement write, flush or handle close failed");
        if (!closed)
            goto unresolved;
        goto before_replace;
    }
    if (!replace_files(wm, wt, wb)) {
        DWORD error = GetLastError();
        if (md_fault("replace_state") || md_fault("replace_content"))
            goto unresolved;
        if (error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
            if (file_state(wm) != 0 || file_state(wb) != 1 || file_state(wt) != 1 ||
                !file_equals(backup, original, original_size) || !file_equals(temp, bytes, size))
                goto unresolved;
            if (md_fault("replace_restore") || !MoveFileExW(wb, wm, 0)) {
                restore_failed = true;
                goto unresolved;
            }
            restored = true;
            if (md_fault("replace_restore_check"))
                goto unresolved;
        }
        if (file_state(wm) != 1 || file_state(wb) != 0 || file_state(wt) != 1 ||
            !file_equals(main, original, original_size) || !file_equals(temp, bytes, size))
            goto unresolved;
        md_diag(d, "IO_ERROR", "execute", "data", main, "Replacement failed; original state verified");
        goto before_replace;
    }
    committed = true;
    temporary = false;
    /* 성공 반환이 반영 경계다. 할당 없이 staged 소유권을 단 한 번 live table로 옮긴다. */
    md_values_free(t->rows, t->row_count * t->count);
    t->rows = *rows;
    *rows = NULL;
    t->row_count = row_count;
    if (replacing_index) {
        md_bt_destroy(&t->index);
        t->index = replacement;
        replacement = (MdBptree){0};
    }
    e->has_result = true;
    if (md_fault("replace_backup_cleanup") || md_fault("cleanup") || md_fault("stdout_cleanup") ||
        !DeleteFileW(wb) || md_fault("replace_marker_cleanup") || !DeleteFileW(wp)) {
        md_diag(d, "IO_ERROR", "cleanup", "data", main, "Committed replacement cleanup failed");
        d->committed = MD_YES;
        d->has_affected_rows = true;
        d->affected_rows = affected;
        e->has_diagnostic = e->stop = true;
        db->health = MD_STOPPED_CLEANUP;
        e->health = db->health;
    }
    goto done;

before_replace:
    /* 자체 생성한 잔여물만 정리한다. 원본 내용 확인 실패는 자동 삭제/복원하지 않는다. */
    if (!file_equals(main, original, original_size) ||
        (temporary && (md_fault("replace_pre_cleanup") || !DeleteFileW(wt))) ||
        (marker && !DeleteFileW(wp)))
        goto unresolved;
    d->committed = MD_NO;
    d->rollback = restored ? MD_ROLLBACK_CONFIRMED : MD_ROLLBACK_NOT_NEEDED;
    goto done;
resource:
    md_diag(d, "RESOURCE_ERROR", "execute", "data", main, "Replacement staging allocation failed");
    d->committed = MD_NO;
    goto done;
unresolved:
    md_diag(d, "RECOVERY_REQUIRED", "rollback", "data", main,
            "Replacement state or restoration unresolved; all artifacts preserved");
    d->committed = MD_UNKNOWN;
    d->rollback = restore_failed ? MD_ROLLBACK_FAILED : MD_ROLLBACK_UNKNOWN;
    db->health = MD_RECOVERY_REQUIRED;
    e->health = db->health;
    e->has_diagnostic = e->stop = true;
done:
    if (d->code[0]) {
        strcpy(d->command, command);
        if (strcmp(d->code, "RECOVERY_REQUIRED") && !committed)
            d->committed = MD_NO;
    }
    free(main); free(temp); free(backup); free(pending);
    free(wm); free(wt); free(wb); free(wp);
    free(original); free(bytes);
    md_bt_destroy(&replacement);
    return committed;
}

bool md_storage_insert(MdDb *db, MdTable *t, MdValue *rows, size_t row_count, MdEvent *e) {
    MdDiagnostic *d = &e->diagnostic;
    MdBtBatch batch = {0};
    size_t total = 0;
    if (!row_count || row_count > SIZE_MAX - t->row_count ||
        t->count > SIZE_MAX / (t->row_count + row_count) / sizeof *rows)
        goto resource;
    if (md_fault("index_insert_alloc") || md_bt_prepare(&t->index, row_count, &batch) != MD_BT_OK)
        goto resource;
    /* 각 레코드 프레임은 유지하고 문장 전체 바이트를 변경 전에 준비한다. */
    for (size_t row = 0; row < row_count; row++) {
        size_t len = t->count;
        for (size_t i = 0; i < t->count; i++) {
            MdValue *value = &rows[row * t->count + i];
            if (value->type == MD_NULL)
                continue;
            size_t add = value->type == MD_TEXT ? value->as.text.len : 0;
            if (add > SIZE_MAX - 8 || len > SIZE_MAX - 8 - add)
                goto resource;
            len += 8 + add;
        }
        if (len > SIZE_MAX - 8 || total > SIZE_MAX - 8 - len)
            goto resource;
        total += 8 + len;
    }
    unsigned char *bytes = malloc(total);
    if (!bytes)
        goto resource;
    size_t pos = 0;
    for (size_t row = 0; row < row_count; row++) {
        size_t frame = pos;
        pos += 8;
        for (size_t i = 0; i < t->count; i++) {
            MdValue *value = &rows[row * t->count + i];
            bytes[pos++] = value->type != MD_NULL;
            if (value->type == MD_INT64) {
                uint64_t bits;
                memcpy(&bits, &value->as.integer, 8);
                le_write(bytes + pos, bits, 8);
                pos += 8;
            } else if (value->type == MD_TEXT) {
                le_write(bytes + pos, value->as.text.len, 8);
                pos += 8;
                memcpy(bytes + pos, value->as.text.ptr, value->as.text.len);
                pos += value->as.text.len;
            }
        }
        le_write(bytes + frame, pos - frame - 8, 8);
    }
    MdValue *next = realloc(t->rows, (t->row_count + row_count) * t->count * sizeof *next);
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
    if (total > (uint64_t)INT64_MAX ||
        (uint64_t)original.QuadPart > (uint64_t)INT64_MAX - total) {
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
    e->result.affected_rows = row_count;
    bool ok = SetFilePointerEx(h, original, NULL, FILE_BEGIN) != 0;
    if (ok && (md_fault("append_write") || md_fault("rollback"))) {
        (void)md_write_all(h, bytes, total / 2);
        ok = false;
    } else if (ok)
        ok = md_write_all(h, bytes, total) && FlushFileBuffers(h);
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
    for (size_t i = 0; i < row_count; i++)
        md_bt_apply(&t->index, &batch, rows[i * t->count + t->id].as.integer, t->row_count + i);
    memcpy(t->rows + t->row_count * t->count, rows, row_count * t->count * sizeof *rows);
    memset(rows, 0, row_count * t->count * sizeof *rows);
    t->row_count += row_count;
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
        d->affected_rows = row_count;
        e->has_diagnostic = e->stop = true;
        db->health = MD_STOPPED_CLEANUP;
        e->health = db->health;
    }
    free(bytes);
    free(path);
    free(pending);
    md_bt_cancel(&batch);
    return true;
early:
    free(bytes);
    free(path);
    free(pending);
    md_bt_cancel(&batch);
    return false;
resource:
    md_bt_cancel(&batch);
    md_diag(d, "RESOURCE_ERROR", "execute", "data", NULL, "INSERT staging allocation failed");
    d->committed = MD_NO;
    return false;
}

static int stored_table_compare(const void *a, const void *b) {
    return _stricmp(((const MdTable *)a)->name, ((const MdTable *)b)->name);
}
bool md_storage_source(MdDb *db, MdDiagnostic *d) {
    if (!lease_open(db, d) || !residuals(db, d))
        return false;
    char *pattern = md_join(db->data_dir, "*");
    wchar_t *wide = pattern ? md_wide(pattern) : NULL;
    WIN32_FIND_DATAW item;
    HANDLE find = wide ? FindFirstFileW(wide, &item) : INVALID_HANDLE_VALUE;
    free(wide); free(pattern);
    if (find == INVALID_HANDLE_VALUE) {
        md_diag(d, "IO_ERROR", "prepare", "data", db->data_dir, "Cannot enumerate source tables");
        return false;
    }
    bool ok = true;
    do {
        size_t length = wcslen(item.cFileName);
        if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || length < 4 ||
            _wcsicmp(item.cFileName + length - 4, L".tbl"))
            continue;
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, item.cFileName, -1, NULL, 0, NULL, NULL);
        char *name = size ? malloc((size_t)size) : NULL;
        char *path = NULL;
        unsigned char *raw = NULL;
        size_t bytes = 0;
        MdTable *table = NULL;
        if (!name || db->count >= SIZE_MAX / sizeof *table) {
            md_diag(d, "RESOURCE_ERROR", "prepare", "data", db->data_dir, "Source catalog allocation failed");
            ok = false;
        } else {
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, item.cFileName, -1, name, size, NULL, NULL);
            path = md_join(db->data_dir, name);
            MdTable *next = realloc(db->tables, (db->count + 1) * sizeof *next);
            if (!path || !next) {
                if (next) db->tables = next;
                md_diag(d, "RESOURCE_ERROR", "prepare", "data", db->data_dir, "Source catalog allocation failed");
                ok = false;
            } else {
                db->tables = next;
                table = &next[db->count++];
                memset(table, 0, sizeof *table);
                ok = md_read_file(path, &raw, &bytes, d);
                if (!ok) strcpy(d->source, "data");
                if (ok) {
                    size_t name_size = strlen(name);
                    bool framing = bytes >= 20 && !memcmp(raw, "MINIDBMS", 8) &&
                                   le_read(raw + 8, 4) == 1 && le_read(raw + 12, 8) <= bytes - 20;
                    uint64_t schema = framing ? le_read(raw + 12, 8) : 0;
                    bool filename = name_size > 10 && !_strnicmp(name, "table_", 6);
                    if (filename) name[name_size - 4] = 0;
                    if (!framing || !filename ||
                        (schema >= 3 && !memcmp(raw + 20, "\xef\xbb\xbf", 3)) ||
                        !md_schema_parse(raw + 20, (size_t)schema, name + 6, table, d)) {
                        if (strcmp(d->code, "RESOURCE_ERROR"))
                            md_diag(d, "STORAGE_FORMAT_ERROR", "prepare", "data", path, "Invalid source header or table filename");
                        ok = false;
                    }
                }
            }
        }
        if (ok) {
            for (size_t i = 0; i + 1 < db->count; i++)
                if (md_equal(db->tables[i].name, table->name)) {
                    md_diag(d, "STORAGE_FORMAT_ERROR", "prepare", "data", path, "Duplicate source table");
                    ok = false;
                }
        }
        free(name); free(path); free(raw);
        if (!ok) break;
    } while (FindNextFileW(find, &item));
    DWORD last = GetLastError();
    FindClose(find);
    if (ok && last != ERROR_NO_MORE_FILES) {
        md_diag(d, "IO_ERROR", "prepare", "data", db->data_dir, "Source enumeration failed");
        ok = false;
    }
    if (ok && !db->count) {
        md_diag(d, "STORAGE_FORMAT_ERROR", "prepare", "data", db->data_dir, "Source has no table files");
        ok = false;
    }
    if (!ok) return false;
    qsort(db->tables, db->count, sizeof *db->tables, stored_table_compare);
    /* 헤더 발견 뒤에도 모든 행을 기존 저장 검증기로 전수 검사한다. */
    for (size_t i = 0; i < db->count; i++)
        if (!load_table(db, &db->tables[i], d))
            return false;
    return true;
}
static bool migration_integer(const char *text, size_t length, int64_t *out) {
    size_t first = 0, end = length;
    while (first < end && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r' || text[first] == '\n'))
        first++;
    while (end > first && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
        end--;
    bool negative = first < end && text[first] == '-';
    if (first < end && (text[first] == '-' || text[first] == '+')) first++;
    if (first == end) return false;
    uint64_t value = 0, limit = negative ? (uint64_t)INT64_MAX + 1 : INT64_MAX;
    for (size_t i = first; i < end; i++) {
        unsigned digit = (unsigned char)text[i] - '0';
        if (digit > 9 || value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    /* 최솟값의 절댓값은 signed로 표현할 수 없으므로 별도 경계를 유지한다. */
    *out = negative ? (value == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)value) : (int64_t)value;
    return true;
}
static bool migration_size(size_t rows, size_t columns, size_t *bytes) {
    if (!columns || rows > SIZE_MAX / columns / sizeof(MdValue)) return false;
    *bytes = rows * columns * sizeof(MdValue);
    return true;
}
#ifdef MINIDB_TEST_FAULTS
bool md_test_migration_integer(const char *s, size_t n, int64_t *out) { return migration_integer(s, n, out); }
bool md_test_migration_size(size_t r, size_t c, size_t *out) { return migration_size(r, c, out); }
#endif
static bool converted_rows(const MdTable *source, MdTable *target, const size_t *map, MdDiagnostic *d) {
    size_t bytes;
    if (!migration_size(source->row_count, target->count, &bytes) || md_fault("migrate_rows_alloc"))
        goto resource;
    target->rows = calloc(bytes ? bytes : 1, 1);
    if (!target->rows) goto resource;
    target->row_count = source->row_count;
    for (size_t row = 0; row < source->row_count; row++) {
        for (size_t column = 0; column < target->count; column++) {
            size_t from = map[column];
            MdValue *out = &target->rows[row * target->count + column];
            if (from == SIZE_MAX) continue;
            const MdValue *in = &source->rows[row * source->count + from];
            if (in->type == MD_NULL) continue;
            if (in->type == target->fields[column].type) {
                if (!md_value_copy(out, in)) goto resource;
            } else if (in->type == MD_INT64) {
                char decimal[32];
                int n = snprintf(decimal, sizeof decimal, "%lld", (long long)in->as.integer);
                out->type = MD_TEXT;
                out->as.text.ptr = md_copy(decimal, (size_t)n);
                out->as.text.len = (size_t)n;
                if (!out->as.text.ptr) goto resource;
            } else {
                if (!migration_integer(in->as.text.ptr, in->as.text.len, &out->as.integer)) {
                    md_diag(d, "MIGRATION_ERROR", "execute", "data", NULL, "TEXT is outside the exact INT64 conversion grammar or range");
                    return false;
                }
                out->type = MD_INT64;
            }
        }
    }
    return true;
resource:
    md_diag(d, "RESOURCE_ERROR", "execute", "data", NULL, "Converted rows allocation or size failed");
    return false;
}
static unsigned char *table_bytes(MdTable *table, size_t *size) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *schema = doc ? md_schema_json(doc, table) : NULL;
    if (!schema) {
        if (doc) yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, schema);
    size_t length = 0;
    char *json = yyjson_mut_write(doc, 0, &length);
    yyjson_mut_doc_free(doc);
    if (!json || length > SIZE_MAX - 20) { free(json); return NULL; }
    unsigned char *header = malloc(length + 20);
    if (!header) { free(json); return NULL; }
    memcpy(header, "MINIDBMS", 8);
    le_write(header + 8, 1, 4);
    le_write(header + 12, length, 8);
    memcpy(header + 20, json, length);
    free(json);
    unsigned char *bytes = md_fault("manage_serialization_alloc") ? NULL :
        replacement_bytes(table, table->rows, table->row_count, header, length + 20, size);
    free(header);
    return bytes;
}
static char *output_parent(const char *path) {
    const char *slash = strrchr(path, '\\');
    /* 부모의 마지막 구분자도 보존한다. C:\\를 C:로 바꾸면 드라이브 현재 경로가 된다. */
    return slash ? md_copy(path, (size_t)(slash - path) + 1) : NULL;
}
static bool output_path(MdDb *db, const MdDb *source, MdDiagnostic *d) {
    /* 신규 경로는 기존 부모의 최종 경로를 기준으로 정규화하여 junction/별칭도 비교한다. */
    size_t path_length = strlen(db->data_dir);
    while (path_length > 3 && db->data_dir[path_length - 1] == '\\')
        db->data_dir[--path_length] = 0;
    char *slash = strrchr(db->data_dir, '\\');
    if (!slash || !slash[1]) goto invalid;
    char *parent = output_parent(db->data_dir);
    char *actual = parent ? md_path(parent) : NULL;
    /* 실제 부모 해석이 끝난 뒤 join이 넣을 구분자와 중복되는 끝 구분자만 제거한다. */
    if (actual) {
        size_t length = strlen(actual);
        if (length && actual[length - 1] == '\\') actual[length - 1] = 0;
    }
    char *canonical = actual ? md_join(actual, slash + 1) : NULL;
    free(parent); free(actual);
    if (!canonical) goto resource;
    free(db->data_dir);
    db->data_dir = canonical;
    wchar_t *wide = md_wide(canonical);
    if (!wide) goto resource;
    DWORD attributes = GetFileAttributesW(wide);
    free(wide);
    if (attributes != INVALID_FILE_ATTRIBUTES) goto invalid;
    if (source) {
        size_t a = strlen(source->data_dir), b = strlen(db->data_dir), n = a < b ? a : b;
        if (!_strnicmp(source->data_dir, db->data_dir, n) &&
            (a == b || (a < b ? db->data_dir[a] : source->data_dir[b]) == '\\'))
            goto invalid;
    }
    return true;
invalid:
    md_diag(d, "IO_ERROR", "prepare", "data", db->data_dir, "Output must be a new nonoverlapping directory");
    return false;
resource:
    md_diag(d, "RESOURCE_ERROR", "prepare", "data", db->data_dir, "Canonical output path allocation failed");
    return false;
}
#ifdef MINIDB_TEST_FAULTS
char *md_test_output_parent(const char *path) {
    return output_parent(path);
}
char *md_test_output_resolve(const char *path) {
    MdDb db = {.data_dir = md_path(path), .lease = INVALID_HANDLE_VALUE};
    MdDiagnostic diagnostic = {0};
    if (!db.data_dir) return NULL;
    if (!output_path(&db, NULL, &diagnostic)) {
        free(db.data_dir);
        return NULL;
    }
    return db.data_dir;
}
#endif
static bool generate_output(MdDb *db, MdDb *source, const MdMigration *mapping, MdEvent *event) {
    MdDiagnostic *d = &event->diagnostic;
    bool created = false, ok = false;
    char *pending = NULL, *guard = NULL;
    wchar_t *wide = NULL, *guard_wide = NULL;
    const char *operation = source ? "migrate" : "init";
    if (!output_path(db, source, d)) return false;
    pending = md_join(db->data_dir, ".minidb.pending");
    guard = md_join(db->data_dir, ".minidb.output.tmp");
    wide = md_wide(db->data_dir);
    guard_wide = guard ? md_wide(guard) : NULL;
    event->result.kind = MD_OPERATION_OK;
    strcpy(event->result.command, operation);
    event->result.output_path = md_copy(db->data_dir, strlen(db->data_dir));
    event->result.table_count = db->count;
    event->result.tables = calloc(db->count, sizeof *event->result.tables);
    if (!pending || !guard || !wide || !guard_wide || !event->result.output_path || !event->result.tables)
        goto resource;
    for (size_t i = 0; i < db->count; i++) {
        MdTableResult *result = &event->result.tables[i];
        const char *action = !source ? "created" : mapping->copied[i] ? "copied" : "converted";
        result->name = md_copy(db->tables[i].name, strlen(db->tables[i].name));
        result->action = md_copy(action, strlen(action));
        result->row_count = source ? source->tables[i].row_count : 0;
        if (!result->name || !result->action) goto resource;
    }
    if (!CreateDirectoryW(wide, NULL)) {
        md_diag(d, "IO_ERROR", "prepare", "data", db->data_dir, "Cannot create new output directory");
        goto done;
    }
    created = true;
    /* 표식 파일 생성 자체가 실패해도 guard가 재시작을 차단한다. 값 변환은 이 경계 뒤에만 한다. */
    if (!CreateDirectoryW(guard_wide, NULL)) {
        (void)RemoveDirectoryW(wide);
        md_diag(d, "IO_ERROR", "execute", "data", guard, "Cannot protect incomplete output");
        goto done;
    }
    if (md_fault("manage_output_lease") || !lease_open(db, d)) {
        if (!d->code[0]) md_diag(d, "IO_ERROR", "execute", "data", db->data_dir, "Output lease failed");
        goto done;
    }
    if (!marker_write(pending, operation, db->data_dir, 0, d)) goto done;
    if (md_fault("manage_hold")) Sleep(1500);
    for (size_t i = 0; i < db->count; i++) {
        MdTable *target = &db->tables[i];
        unsigned char *bytes = NULL;
        size_t size = 0;
        char *path = table_path(db, target);
        if (!path) goto resource;
        if (source && mapping->copied[i]) {
            char *old = table_path(source, &source->tables[i]);
            if (!old) { free(path); goto resource; }
            bool read = md_read_file(old, &bytes, &size, d);
            free(old);
            if (!read) {
                strcpy(d->source, "data");
                free(path);
                goto done;
            }
        } else {
            if (source && !converted_rows(&source->tables[i], target, mapping->columns[i], d)) {
                free(path); goto done;
            }
            bytes = table_bytes(target, &size);
            if (!bytes) { free(path); goto resource; }
        }
        HANDLE file = file_open(path, GENERIC_WRITE, 0, CREATE_NEW);
        bool late = i + 1 == db->count;
        bool written = file != INVALID_HANDLE_VALUE &&
            !(late && md_fault("manage_table_write")) && md_write_all(file, bytes, size) &&
            !(late && md_fault("manage_table_flush")) && FlushFileBuffers(file);
        if (file != INVALID_HANDLE_VALUE && !CloseHandle(file)) written = false;
        if (late && md_fault("manage_table_close")) written = false;
        free(bytes);
        md_values_free(target->rows, target->row_count * target->count);
        target->rows = NULL;
        target->row_count = 0;
        if (!written) {
            md_diag(d, "IO_ERROR", "execute", "data", path, "Generated table write, flush or close failed");
            free(path); goto done;
        }
        free(path);
    }
    for (size_t i = 0; i < db->count; i++) {
        if (md_fault("manage_validate") || !load_table(db, &db->tables[i], d)) {
            if (!d->code[0]) md_diag(d, "STORAGE_FORMAT_ERROR", "execute", "data", db->data_dir, "Final output validation failed");
            goto done;
        }
        if (db->tables[i].row_count != event->result.tables[i].row_count) {
            md_diag(d, "STORAGE_FORMAT_ERROR", "execute", "data", db->data_dir, "Generated row count differs");
            goto done;
        }
    }
    if (md_fault("manage_residual") || !residuals_check(db, d, true)) {
        if (!d->code[0]) md_diag(d, "IO_ERROR", "cleanup", "data", db->data_dir, "Final residual check failed");
        goto done;
    }
    if (md_fault("manage_guard_cleanup") || !RemoveDirectoryW(guard_wide) ||
        md_fault("manage_marker_cleanup") || !remove_file(pending)) {
        md_diag(d, "IO_ERROR", "cleanup", "data", db->data_dir, "Output publication cleanup failed");
        goto done;
    }
    /* 전체 검증과 표식의 마지막 삭제가 끝난 뒤에만 단일 성공 이벤트를 공개한다. */
    event->has_result = true;
    ok = true;
    goto done;
resource:
    md_diag(d, "RESOURCE_ERROR", created ? "execute" : "prepare", "data", db->data_dir, "Output staging allocation failed");
done:
    if (!ok) {
        d->output_ready = MD_NO;
        if (created && !strcmp(d->phase, "prepare")) strcpy(d->phase, "execute");
    }
    free(pending); free(guard); free(wide); free(guard_wide);
    return ok;
}
bool md_storage_init(MdDb *db, MdEvent *event) {
    return generate_output(db, NULL, NULL, event);
}
bool md_storage_migrate(MdDb *source, MdDb *target, const MdMigration *mapping, MdEvent *event) {
    return generate_output(target, source, mapping, event);
}
