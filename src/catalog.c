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
        md_bt_destroy(&t[i].index);
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

/* plan은 구조 검증을 먼저 끝내고, 이름과 대응 의미는 원본/대상 카탈로그로 검증한다. */
static bool plan_keys(yyjson_val *object, const char *const *keys, size_t required, size_t total) {
    if (!yyjson_is_obj(object))
        return false;
    bool seen[3] = {false};
    size_t i, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(object, i, count, key, value) {
        (void)value;
        size_t k;
        for (k = 0; k < total; k++)
            if (yyjson_get_len(key) == strlen(keys[k]) &&
                !memcmp(yyjson_get_str(key), keys[k], strlen(keys[k])))
                break;
        if (k == total || seen[k])
            return false;
        seen[k] = true;
    }
    for (size_t i = 0; i < required; i++)
        if (!seen[i])
            return false;
    return true;
}
bool md_plan_load(const char *path, MdMigration *m, MdDiagnostic *d) {
    if (!path)
        return true;
    unsigned char *raw = NULL;
    size_t size = 0, bom = 0;
    if (!md_read_file(path, &raw, &size, d)) {
        strcpy(d->source, "plan");
        return false;
    }
    if (size >= 3 && !memcmp(raw, "\xef\xbb\xbf", 3))
        bom = 3;
    yyjson_read_err error;
    m->document = md_fault("migrate_plan_alloc") ? NULL :
        yyjson_read_opts((char *)raw + bom, size - bom, 0, NULL, &error);
    free(raw);
    if (!m->document) {
        bool allocation = md_fault("migrate_plan_alloc") || error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION;
        md_diag(d, allocation ? "RESOURCE_ERROR" : "ARGUMENT_ERROR",
                allocation ? "prepare" : "arguments", "plan", path, "Cannot parse strict migration plan JSON");
        if (!allocation) {
            d->has_offset = true;
            d->byte_offset = error.pos + bom;
        }
        return false;
    }
    const char *rootkeys[] = {"version", "tables"}, *tablekeys[] = {"name", "map", "drop"},
               *mapkeys[] = {"target", "source"};
    yyjson_val *root = yyjson_doc_get_root(m->document), *version = yyjson_obj_get(root, "version"),
               *tables = yyjson_obj_get(root, "tables");
    if (!plan_keys(root, rootkeys, 2, 2) || !yyjson_is_uint(version) ||
        yyjson_get_uint(version) != 1 || !yyjson_is_arr(tables))
        goto invalid;
    size_t i, count;
    yyjson_val *table;
    yyjson_arr_foreach(tables, i, count, table) {
        yyjson_val *map = yyjson_obj_get(table, "map"), *drop = yyjson_obj_get(table, "drop");
        if (!plan_keys(table, tablekeys, 1, 3) || !yyjson_is_str(yyjson_obj_get(table, "name")) ||
            (map && !yyjson_is_arr(map)) || (drop && !yyjson_is_arr(drop)))
            goto invalid;
        for (size_t j = 0; j < yyjson_arr_size(map); j++) {
            yyjson_val *entry = yyjson_arr_get(map, j);
            if (!plan_keys(entry, mapkeys, 2, 2) || !yyjson_is_str(yyjson_obj_get(entry, "source")) ||
                !yyjson_is_str(yyjson_obj_get(entry, "target")))
                goto invalid;
        }
        for (size_t j = 0; j < yyjson_arr_size(drop); j++)
            if (!yyjson_is_str(yyjson_arr_get(drop, j)))
                goto invalid;
    }
    return true;
invalid:
    md_diag(d, "ARGUMENT_ERROR", "arguments", "plan", path,
            "Invalid plan fields, types, version or decoded duplicate keys");
    strcpy(d->field_path, "$");
    return false;
}
void md_migration_free(MdMigration *m) {
    if (m->columns)
        for (size_t i = 0; i < m->count; i++)
            free(m->columns[i]);
    free(m->columns);
    free(m->copied);
    if (m->document)
        yyjson_doc_free(m->document);
    memset(m, 0, sizeof *m);
}
static size_t field_index(const MdTable *table, const char *name) {
    for (size_t i = 0; i < table->count; i++)
        if (md_equal(table->fields[i].name, name))
            return i;
    return SIZE_MAX;
}
bool md_mapping_prepare(MdMigration *m, const MdDb *source, const MdDb *target, MdDiagnostic *d) {
    yyjson_val *tables = m->document ? yyjson_obj_get(yyjson_doc_get_root(m->document), "tables") : NULL;
    if (source->count != target->count)
        goto semantic;
    /* source와 target은 같은 접은 이름 정렬을 사용한다. */
    for (size_t i = 0; i < target->count; i++)
        if (!md_equal(source->tables[i].name, target->tables[i].name))
            goto semantic;
    for (size_t i = 0; i < yyjson_arr_size(tables); i++) {
        yyjson_val *entry = yyjson_arr_get(tables, i), *name = yyjson_obj_get(entry, "name");
        if (!text_name(name))
            goto semantic;
        bool found = false;
        for (size_t t = 0; t < target->count; t++)
            found |= md_equal(target->tables[t].name, yyjson_get_str(name));
        if (!found)
            goto semantic;
        for (size_t j = 0; j < i; j++)
            if (md_equal(yyjson_get_str(name), yyjson_get_str(yyjson_obj_get(yyjson_arr_get(tables, j), "name"))))
                goto semantic;
    }
    m->count = target->count;
    m->columns = calloc(m->count, sizeof *m->columns);
    m->copied = calloc(m->count, sizeof *m->copied);
    if (!m->columns || !m->copied || md_fault("migrate_mapping_alloc"))
        goto resource;
    for (size_t t = 0; t < target->count; t++) {
        const MdTable *old = &source->tables[t], *next = &target->tables[t];
        yyjson_val *plan = NULL;
        for (size_t i = 0; i < yyjson_arr_size(tables); i++) {
            yyjson_val *entry = yyjson_arr_get(tables, i);
            if (md_equal(next->name, yyjson_get_str(yyjson_obj_get(entry, "name"))))
                plan = entry;
        }
        yyjson_val *map = yyjson_obj_get(plan, "map"), *drop = yyjson_obj_get(plan, "drop");
        if (next->count > SIZE_MAX / sizeof **m->columns)
            goto resource;
        m->columns[t] = malloc(next->count * sizeof **m->columns);
        if (!m->columns[t])
            goto resource;
        for (size_t c = 0; c < next->count; c++)
            m->columns[t][c] = field_index(old, next->fields[c].name);
        for (size_t i = 0; i < yyjson_arr_size(map); i++) {
            yyjson_val *entry = yyjson_arr_get(map, i), *to = yyjson_obj_get(entry, "target"),
                       *from = yyjson_obj_get(entry, "source");
            if (!text_name(to) || !text_name(from))
                goto semantic;
            size_t c = field_index(next, yyjson_get_str(to)), r = field_index(old, yyjson_get_str(from));
            if (c == SIZE_MAX || r == SIZE_MAX)
                goto semantic;
            for (size_t j = 0; j < i; j++)
                if (md_equal(yyjson_get_str(to), yyjson_get_str(yyjson_obj_get(yyjson_arr_get(map, j), "target"))))
                    goto semantic;
            m->columns[t][c] = r;
        }
        if (m->columns[t][next->id] != old->id)
            goto semantic;
        for (size_t c = 0; c < next->count; c++)
            for (size_t j = 0; j < c; j++)
                if (m->columns[t][c] != SIZE_MAX && m->columns[t][c] == m->columns[t][j])
                    goto semantic;
        for (size_t i = 0; i < yyjson_arr_size(drop); i++) {
            yyjson_val *name = yyjson_arr_get(drop, i);
            if (!text_name(name) || field_index(old, yyjson_get_str(name)) == SIZE_MAX)
                goto semantic;
            for (size_t j = 0; j < i; j++)
                if (md_equal(yyjson_get_str(name), yyjson_get_str(yyjson_arr_get(drop, j))))
                    goto semantic;
        }
        for (size_t r = 0; r < old->count; r++) {
            bool used = false, dropped = false;
            for (size_t c = 0; c < next->count; c++)
                used |= m->columns[t][c] == r;
            for (size_t i = 0; i < yyjson_arr_size(drop); i++)
                dropped |= md_equal(old->fields[r].name, yyjson_get_str(yyjson_arr_get(drop, i)));
            if (used == dropped)
                goto semantic;
        }
        m->copied[t] = md_schema_same(old, next);
        for (size_t c = 0; c < next->count; c++)
            m->copied[t] &= m->columns[t][c] == c;
    }
    return true;
semantic:
    md_diag(d, "MIGRATION_ERROR", "prepare", "plan", NULL,
            "Invalid table set, identifier, one-to-one mapping, drop or ID preservation");
    return false;
resource:
    md_diag(d, "RESOURCE_ERROR", "prepare", "plan", NULL, "Mapping allocation failed");
    return false;
}
