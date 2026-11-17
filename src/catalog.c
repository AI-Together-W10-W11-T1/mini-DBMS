#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *const keywords[] = {
#include "keywords.inc"
};
bool md_identifier(const char *s) {
    if (!s || !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_'))
        return false;
    for (const char *p = s + 1; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_'))
            return false;
    for (size_t i = 0; i < sizeof keywords / sizeof *keywords; i++)
        if (md_equal(s, keywords[i]))
            return false;
    return true;
}
void md_catalog_free(MdTable *t, size_t n) {
    if (!t)
        return;
    for (size_t i = 0; i < n; i++) {
        free(t[i].name);
        if (t[i].fields)
            for (size_t j = 0; j < t[i].count; j++)
                free(t[i].fields[j].name);
        free(t[i].fields);
        md_values_free(t[i].rows, t[i].row_count * t[i].count);
    }
    free(t);
}
static bool exact_keys(yyjson_val *v, const char *const *keys, size_t n) {
    if (!yyjson_is_obj(v) || yyjson_obj_size(v) != n)
        return false;
    bool found[3] = {false, false, false};
    size_t i, max;
    yyjson_val *k, *value;
    yyjson_obj_foreach(v, i, max, k, value) {
        (void)value;
        size_t j;
        for (j = 0; j < n; j++)
            if (yyjson_get_len(k) == strlen(keys[j]) &&
                !memcmp(yyjson_get_str(k), keys[j], strlen(keys[j])))
                break;
        if (j == n || found[j])
            return false;
        found[j] = true;
    }
    return true;
}
static bool text_name(yyjson_val *v) {
    return yyjson_is_str(v) && strlen(yyjson_get_str(v)) == yyjson_get_len(v) &&
           md_identifier(yyjson_get_str(v));
}
bool md_schema_parse(const unsigned char *raw, size_t n, const char *stem, MdTable *t,
                     MdDiagnostic *d) {
    memset(t, 0, sizeof *t);
    size_t bom = 0;
    if (n >= 3 && !memcmp(raw, "\xef\xbb\xbf", 3)) {
        raw += 3;
        n -= 3;
        bom = 3;
    }
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts((char *)raw, n, 0, NULL, &err);
    if (!doc) {
        bool allocation = err.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION;
        md_diag(d, allocation ? "RESOURCE_ERROR" : "SCHEMA_ERROR", "prepare", "schema", NULL,
                allocation ? "JSON allocation failed" : "Invalid strict UTF-8 JSON");
        d->has_offset = !allocation;
        d->byte_offset = err.pos + bom;
        return false;
    }
    const char *rootkeys[] = {"version", "name", "columns"}, *colkeys[] = {"name", "type"};
    yyjson_val *root = yyjson_doc_get_root(doc), *name = yyjson_obj_get(root, "name"),
               *cols = yyjson_obj_get(root, "columns"), *version = yyjson_obj_get(root, "version");
    bool ok = exact_keys(root, rootkeys, 3) && yyjson_is_uint(version) &&
              yyjson_get_uint(version) == 1 && text_name(name) && yyjson_is_arr(cols) &&
              yyjson_arr_size(cols) > 0;
    if (ok && stem)
        ok = md_equal(stem, yyjson_get_str(name));
    if (!ok)
        goto invalid;
    t->name = md_copy(yyjson_get_str(name), yyjson_get_len(name));
    t->count = yyjson_arr_size(cols);
    t->id = SIZE_MAX;
    if (t->count > SIZE_MAX / sizeof *t->fields)
        goto resource;
    t->fields = calloc(t->count, sizeof *t->fields);
    if (!t->name || !t->fields)
        goto resource;
    for (size_t i = 0; i < t->count; i++) {
        yyjson_val *c = yyjson_arr_get(cols, i), *cn = yyjson_obj_get(c, "name"),
                   *ty = yyjson_obj_get(c, "type");
        if (!exact_keys(c, colkeys, 2) || !text_name(cn) || !yyjson_is_str(ty))
            goto invalid;
        const char *ts = yyjson_get_str(ty);
        if (yyjson_get_len(ty) == 5 && !memcmp(ts, "INT64", 5))
            t->fields[i].type = MD_INT64;
        else if (yyjson_get_len(ty) == 4 && !memcmp(ts, "TEXT", 4))
            t->fields[i].type = MD_TEXT;
        else
            goto invalid;
        t->fields[i].name = md_copy(yyjson_get_str(cn), yyjson_get_len(cn));
        if (!t->fields[i].name)
            goto resource;
        for (size_t j = 0; j < i; j++)
            if (md_equal(t->fields[i].name, t->fields[j].name))
                goto invalid;
        if (md_equal(t->fields[i].name, "id")) {
            if (t->fields[i].type != MD_INT64)
                goto invalid;
            t->id = i;
        }
    }
    if (t->id == SIZE_MAX)
        goto invalid;
    yyjson_doc_free(doc);
    return true;
resource:
    md_diag(d, "RESOURCE_ERROR", "prepare", "schema", NULL, "Schema allocation failed");
    goto failed;
invalid:
    md_diag(d, "SCHEMA_ERROR", "prepare", "schema", NULL,
            "Invalid schema keys, version, identifier, column or ID definition");
    strcpy(d->field_path, "$");
failed:
    yyjson_doc_free(doc);
    free(t->name);
    if (t->fields) {
        for (size_t i = 0; i < t->count; i++)
            free(t->fields[i].name);
    }
    free(t->fields);
    memset(t, 0, sizeof *t);
    return false;
}
bool md_schema_same(const MdTable *a, const MdTable *b) {
    if (!md_equal(a->name, b->name) || a->count != b->count)
        return false;
    for (size_t i = 0; i < a->count; i++)
        if (a->fields[i].type != b->fields[i].type ||
            !md_equal(a->fields[i].name, b->fields[i].name))
            return false;
    return true;
}
yyjson_mut_val *md_schema_json(yyjson_mut_doc *doc, const MdTable *t) {
    yyjson_mut_val *root = yyjson_mut_obj(doc), *cols = yyjson_mut_arr(doc);
    if (!root || !cols || !yyjson_mut_obj_add_uint(doc, root, "version", 1) ||
        !yyjson_mut_obj_add_strcpy(doc, root, "name", t->name) ||
        !yyjson_mut_obj_add_val(doc, root, "columns", cols))
        return NULL;
    for (size_t i = 0; i < t->count; i++) {
        yyjson_mut_val *c = yyjson_mut_obj(doc);
        if (!c || !yyjson_mut_obj_add_strcpy(doc, c, "name", t->fields[i].name) ||
            !yyjson_mut_obj_add_str(doc, c, "type",
                                    t->fields[i].type == MD_INT64 ? "INT64" : "TEXT") ||
            !yyjson_mut_arr_append(cols, c))
            return NULL;
    }
    return root;
}
static int table_cmp(const void *a, const void *b) {
    const MdTable *x = a, *y = b;
    return _stricmp(x->name, y->name);
}
bool md_catalog_load(const char *dir, MdDb *db, MdDiagnostic *d) {
    char *absolute = md_path(dir), *pattern = absolute ? md_join(absolute, "*") : NULL;
    wchar_t *w = pattern ? md_wide(pattern) : NULL;
    WIN32_FIND_DATAW item;
    HANDLE find = w ? FindFirstFileW(w, &item) : INVALID_HANDLE_VALUE;
    free(pattern);
    free(w);
    if (find == INVALID_HANDLE_VALUE) {
        md_diag(d, "IO_ERROR", "prepare", "schema", absolute ? absolute : dir,
                "Cannot enumerate schema directory");
        free(absolute);
        return false;
    }
    bool ok = true;
    do {
        if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        int len = WideCharToMultiByte(CP_UTF8, 0, item.cFileName, -1, NULL, 0, NULL, NULL);
        char *filename = malloc((size_t)len);
        if (!filename) {
            ok = false;
            md_diag(d, "RESOURCE_ERROR", "prepare", "schema", absolute,
                    "Schema filename allocation failed");
            break;
        }
        WideCharToMultiByte(CP_UTF8, 0, item.cFileName, -1, filename, len, NULL, NULL);
        size_t slen = strlen(filename);
        if (slen < 5 || !md_equal(filename + slen - 5, ".json")) {
            free(filename);
            continue;
        }
        char *path = md_join(absolute, filename);
        filename[slen - 5] = 0;
        unsigned char *raw = NULL;
        size_t n = 0;
        MdTable table = {0};
        if (!path) {
            md_diag(d, "RESOURCE_ERROR", "prepare", "schema", absolute, "Path allocation failed");
            ok = false;
        } else if (!md_read_file(path, &raw, &n, d)) {
            strcpy(d->source, "schema");
            ok = false;
        } else if (!md_schema_parse(raw, n, filename, &table, d)) {
            if (strlen(path) < sizeof d->path)
                strcpy(d->path, path);
            ok = false;
        }
        free(raw);
        free(filename);
        free(path);
        if (!ok)
            break;
        for (size_t i = 0; i < db->count; i++)
            if (md_equal(table.name, db->tables[i].name)) {
                md_diag(d, "SCHEMA_ERROR", "prepare", "schema", absolute, "Duplicate table name");
                ok = false;
                break;
            }
        if (ok && db->count < SIZE_MAX / sizeof *db->tables - 1) {
            MdTable *next = realloc(db->tables, (db->count + 1) * sizeof *next);
            if (next) {
                db->tables = next;
                db->tables[db->count++] = table;
            } else
                ok = false;
        } else
            ok = false;
        if (!ok) {
            free(table.name);
            for (size_t i = 0; i < table.count; i++)
                free(table.fields[i].name);
            free(table.fields);
            if (!d->code[0])
                md_diag(d, "RESOURCE_ERROR", "prepare", "schema", absolute,
                        "Catalog allocation failed");
            break;
        }
    } while (FindNextFileW(find, &item));
    DWORD last = GetLastError();
    FindClose(find);
    if (ok && last != ERROR_NO_MORE_FILES) {
        md_diag(d, "IO_ERROR", "prepare", "schema", absolute, "Schema enumeration failed");
        ok = false;
    }
    if (ok && !db->count) {
        md_diag(d, "SCHEMA_ERROR", "prepare", "schema", absolute,
                "Schema directory has no JSON tables");
        ok = false;
    }
    free(absolute);
    if (ok)
        qsort(db->tables, db->count, sizeof *db->tables, table_cmp);
    return ok;
}
