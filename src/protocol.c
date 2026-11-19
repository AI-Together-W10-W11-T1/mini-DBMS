#include "protocol.h"
#include "format.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool mp_add(size_t a, size_t b, size_t limit, size_t *out) {
    if (a > limit || b > limit - a) return false;
    *out = a + b;
    return true;
}
bool mp_decimal(const char *s, uint64_t *out) {
    if (!s || !*s) return false;
    uint64_t value = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || value > (UINT64_MAX - (unsigned)(*s - '0')) / 10) return false;
        value = value * 10 + (unsigned)(*s - '0');
    }
    *out = value;
    return true;
}
bool mp_limits(size_t request, size_t frame, size_t event, size_t total) {
    return request > 0 && request <= frame && frame >= 4096 && frame <= UINT32_MAX &&
           event >= 4096 && total >= MP_RESERVE;
}
uint64_t mp_now(void) { return GetTickCount64(); }
bool mp_expired(uint64_t start, uint64_t timeout) { return timeout && mp_now() - start >= timeout; }
void mp_header(unsigned char h[5], char kind, uint32_t n) {
    h[0] = (unsigned char)kind;
    for (int i = 4; i >= 1; i--) { h[i] = (unsigned char)n; n >>= 8; }
}
uint32_t mp_length(const unsigned char h[5]) {
    return ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
}
bool mp_fields(yyjson_val *object, const char *const *keys, size_t count) {
    if (!yyjson_is_obj(object) || yyjson_obj_size(object) != count) return false;
    for (size_t i = 0; i < count; i++) {
        size_t matches = 0;
        yyjson_obj_iter it = yyjson_obj_iter_with(object);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&it))) if (mp_string(key, keys[i])) matches++;
        if (matches != 1) return false;
    }
    return true;
}
bool mp_string(yyjson_val *v, const char *s) {
    return yyjson_is_str(v) && yyjson_get_len(v) == strlen(s) && !memcmp(yyjson_get_str(v), s, strlen(s));
}
bool mp_u64(yyjson_val *value, uint64_t *out) {
    if (!yyjson_is_str(value))
        return false;
    const char *text = yyjson_get_str(value);
    size_t length = yyjson_get_len(value);
    if (strlen(text) != length || (length > 1 && text[0] == '0'))
        return false;
    /* wire 숫자의 표기는 CLI 옵션의 십진 파싱과 별도로 엄격히 검사한다. */
    return mp_decimal(text, out);
}
yyjson_doc *mp_json(const char *data, size_t length) {
    return yyjson_read(data, length, 0);
}
char *mp_copy(const char *p, size_t n) {
    if (n == SIZE_MAX) return NULL;
    char *out = malloc(n + 1);
    if (out) { memcpy(out, p, n); out[n] = 0; }
    return out;
}
bool mp_socket_nonblocking(SOCKET s) { u_long yes = 1; return ioctlsocket(s, FIONBIO, &yes) == 0; }
bool mp_request(const char *data, size_t length, char **sql, size_t *sql_length) {
    const char *keys[] = {"version", "type", "sql"};
    *sql = NULL;
    yyjson_doc *doc = mp_json(data, length);
    yyjson_val *root = yyjson_doc_get_root(doc), *value = yyjson_obj_get(root, "sql");
    bool ok = mp_fields(root, keys, 3) && yyjson_is_uint(yyjson_obj_get(root,"version")) &&
              yyjson_get_uint(yyjson_obj_get(root,"version")) == 1 &&
              mp_string(yyjson_obj_get(root,"type"),"execute") && yyjson_is_str(value);
    if (ok) {
        *sql_length = yyjson_get_len(value);
        *sql = mp_copy(yyjson_get_str(value), *sql_length);
        ok = *sql != NULL;
    }
    yyjson_doc_free(doc);
    return ok;
}
static const char *health_name(MdHealth h) {
    return h == MD_RECOVERY_REQUIRED ? "recovery_required" : h == MD_STOPPED_CLEANUP ? "stopped_cleanup" : "healthy";
}
char *mp_event_encode(const MdEvent *event, const char *stage, size_t *length) {
    Buffer result, diagnostic;
    mf_begin(&result); mf_begin(&diagnostic);
    if (event->has_result) mf_render(&event->result, true, &result);
    if (event->has_diagnostic) mf_diagnostic_render(&event->diagnostic, "run", true, &diagnostic);
    char *out = NULL;
    if (!result.failed && !diagnostic.failed) {
        const char *r = event->has_result ? result.p : "null";
        const char *d = event->has_diagnostic ? diagnostic.p : "null";
        size_t n;
        if (mp_add(strlen(r), strlen(d), SIZE_MAX - 512, &n)) {
            out = malloc(n + 512);
            if (out) {
                int used = snprintf(out, n + 512,
                    "{\"version\":1,\"result\":%s,\"diagnostic\":%s,\"stop\":%s,\"health\":\"%s\",\"stage\":%s%s%s}",
                    r, d, event->stop ? "true" : "false", health_name(event->health),
                    stage ? "\"" : "", stage ? stage : "null", stage ? "\"" : "");
                *length = (size_t)used;
            }
        }
    }
    mf_end(&result); mf_end(&diagnostic);
    return out;
}
static bool text_field(yyjson_val *root, const char *key, char *out, size_t capacity, bool nullable) {
    yyjson_val *v = yyjson_obj_get(root,key);
    if (nullable && yyjson_is_null(v)) { out[0] = 0; return true; }
    if (!yyjson_is_str(v) || yyjson_get_len(v) >= capacity || strlen(yyjson_get_str(v)) != yyjson_get_len(v)) return false;
    strcpy(out, yyjson_get_str(v));
    return true;
}
static bool nullable_u64(yyjson_val *root, const char *key, uint64_t *out, bool *present) {
    yyjson_val *v = yyjson_obj_get(root,key);
    *present = !yyjson_is_null(v);
    return !*present || mp_u64(v,out);
}
static bool known_field(yyjson_val *root, const char *key, MdKnown *out) {
    yyjson_val *v = yyjson_obj_get(root,key);
    if (yyjson_is_null(v)) *out = MD_UNKNOWN;
    else if (yyjson_is_bool(v)) *out = yyjson_get_bool(v) ? MD_YES : MD_NO;
    else return false;
    return true;
}
static bool decode_diagnostic(yyjson_val *value, MdDiagnostic *diagnostic) {
    const char *keys[] = {
        "version", "type", "code", "message", "operation", "phase", "source", "path",
        "statement_index", "line", "column", "field_path", "byte_offset", "committed",
        "rollback", "command", "affected_rows", "output_ready"
    };
    yyjson_val *version = yyjson_obj_get(value, "version");
    if (!mp_fields(value, keys, 18) || !yyjson_is_uint(version) || yyjson_get_uint(version) != 1 ||
        !mp_string(yyjson_obj_get(value, "type"), "diagnostic") ||
        !mp_string(yyjson_obj_get(value, "operation"), "run"))
        return false;

    yyjson_val *phase = yyjson_obj_get(value, "phase");
    yyjson_val *source = yyjson_obj_get(value, "source");
    yyjson_val *command = yyjson_obj_get(value, "command");
    if (!mp_string(phase, "arguments") && !mp_string(phase, "prepare") &&
        !mp_string(phase, "input") && !mp_string(phase, "execute") &&
        !mp_string(phase, "rollback") && !mp_string(phase, "cleanup") && !mp_string(phase, "output"))
        return false;
    if (!yyjson_is_null(source) && !mp_string(source, "arguments") &&
        !mp_string(source, "sql_file") && !mp_string(source, "stdin") &&
        !mp_string(source, "schema") && !mp_string(source, "data") &&
        !mp_string(source, "plan") && !mp_string(source, "stdout") && !mp_string(source, "stderr"))
        return false;
    if (!yyjson_is_null(command) && !mp_string(command, "INSERT") &&
        !mp_string(command, "UPDATE") && !mp_string(command, "DELETE"))
        return false;

#define TEXT(field, nullable) \
    if (!text_field(value, #field, diagnostic->field, sizeof diagnostic->field, nullable)) \
        return false
    TEXT(code, false);
    TEXT(message, false);
    TEXT(phase, false);
    TEXT(source, true);
    TEXT(path, true);
    TEXT(field_path, true);
    TEXT(command, true);
#undef TEXT
    bool column_present;
    if (!nullable_u64(value, "statement_index", &diagnostic->statement_index, &diagnostic->has_statement) ||
        !nullable_u64(value, "line", &diagnostic->line, &diagnostic->has_position) ||
        !nullable_u64(value, "column", &diagnostic->column, &column_present) ||
        column_present != diagnostic->has_position ||
        !nullable_u64(value, "byte_offset", &diagnostic->byte_offset, &diagnostic->has_offset) ||
        !nullable_u64(value, "affected_rows", &diagnostic->affected_rows, &diagnostic->has_affected_rows) ||
        !known_field(value, "committed", &diagnostic->committed) ||
        !known_field(value, "output_ready", &diagnostic->output_ready))
        return false;
    /* SQL 위치는 1부터 시작하지만 원본 byte_offset의 0은 유효하다. */
    if ((diagnostic->has_statement && !diagnostic->statement_index) ||
        (diagnostic->has_position && (!diagnostic->line || !diagnostic->column)))
        return false;

    yyjson_val *rollback = yyjson_obj_get(value, "rollback");
    if (mp_string(rollback, "not_needed"))
        diagnostic->rollback = MD_ROLLBACK_NOT_NEEDED;
    else if (mp_string(rollback, "confirmed"))
        diagnostic->rollback = MD_ROLLBACK_CONFIRMED;
    else if (mp_string(rollback, "failed"))
        diagnostic->rollback = MD_ROLLBACK_FAILED;
    else if (mp_string(rollback, "unknown"))
        diagnostic->rollback = MD_ROLLBACK_UNKNOWN;
    else
        return false;
    return true;
}
static bool signed_value(yyjson_val *value, int64_t *out) {
    if (!yyjson_is_str(value))
        return false;
    const char *text = yyjson_get_str(value);
    if (strlen(text) != yyjson_get_len(value) || !*text || *text == '+')
        return false;
    bool negative = *text == '-';
    const char *digits = negative ? text + 1 : text;
    if (digits[0] == '0' && digits[1])
        return false;
    uint64_t magnitude;
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    if (!mp_decimal(digits, &magnitude) || magnitude > limit)
        return false;
    if (!negative)
        *out = (int64_t)magnitude;
    else if (magnitude == (uint64_t)INT64_MAX + 1)
        *out = INT64_MIN;
    else
        *out = -(int64_t)magnitude;
    return true;
}
static bool decode_result(yyjson_val *v, MdResult *r) {
    yyjson_val *type = yyjson_obj_get(v,"type");
    if (!yyjson_is_uint(yyjson_obj_get(v,"version")) || yyjson_get_uint(yyjson_obj_get(v,"version")) != 1 ||
        !mp_u64(yyjson_obj_get(v,"statement_index"),&r->statement_index) || !r->statement_index) return false;
    if (mp_string(type,"statement_ok")) {
        const char *keys[] = {"version","type","statement_index","command","affected_rows","committed"};
        r->kind = MD_STATEMENT_OK;
        return mp_fields(v,keys,6) && text_field(v,"command",r->command,sizeof r->command,false) &&
            (!strcmp(r->command,"INSERT") || !strcmp(r->command,"UPDATE") || !strcmp(r->command,"DELETE")) &&
            mp_u64(yyjson_obj_get(v,"affected_rows"),&r->affected_rows) && yyjson_is_true(yyjson_obj_get(v,"committed"));
    }
    const char *keys[] = {"version","type","statement_index","columns","rows","row_count"};
    if (!mp_string(type,"query_result") || !mp_fields(v,keys,6)) return false;
    r->kind = MD_QUERY_RESULT;
    yyjson_val *columns = yyjson_obj_get(v,"columns"), *rows = yyjson_obj_get(v,"rows");
    uint64_t count;
    if (!yyjson_is_arr(columns) || !yyjson_is_arr(rows) || !mp_u64(yyjson_obj_get(v,"row_count"),&count) || count != yyjson_arr_size(rows)) return false;
    r->column_count = yyjson_arr_size(columns); r->row_count = yyjson_arr_size(rows);
    if (!r->column_count || r->column_count > SIZE_MAX / sizeof(MdColumn) ||
        r->row_count > SIZE_MAX / r->column_count || r->row_count * r->column_count > SIZE_MAX / sizeof(MdValue)) return false;
    r->columns = calloc(r->column_count,sizeof *r->columns);
    r->values = calloc(r->row_count * r->column_count,sizeof *r->values);
    if (!r->columns || (r->row_count && !r->values)) return false;
    for (size_t i = 0; i < r->column_count; i++) {
        yyjson_val *col = yyjson_arr_get(columns,i), *name = yyjson_obj_get(col,"name"), *t = yyjson_obj_get(col,"type");
        const char *column_keys[] = {"name","type"};
        if (!mp_fields(col,column_keys,2) || !yyjson_is_str(name) || strlen(yyjson_get_str(name)) != yyjson_get_len(name)) return false;
        r->columns[i].name = mp_copy(yyjson_get_str(name),yyjson_get_len(name));
        if (!r->columns[i].name) return false;
        if (mp_string(t,"INT64")) r->columns[i].type = MD_INT64;
        else if (mp_string(t,"TEXT")) r->columns[i].type = MD_TEXT;
        else if (mp_string(t,"NULL")) r->columns[i].type = MD_NULL;
        else return false;
    }
    for (size_t row = 0; row < r->row_count; row++) {
        yyjson_val *values = yyjson_arr_get(rows,row);
        if (!yyjson_is_arr(values) || yyjson_arr_size(values) != r->column_count) return false;
        for (size_t col = 0; col < r->column_count; col++) {
            MdValue *value = &r->values[row * r->column_count + col];
            yyjson_val *raw = yyjson_arr_get(values,col);
            if (yyjson_is_null(raw)) continue;
            value->type = r->columns[col].type;
            if (value->type == MD_INT64) { if (!signed_value(raw,&value->as.integer)) return false; }
            else if (value->type == MD_TEXT && yyjson_is_str(raw)) {
                value->as.text.len = yyjson_get_len(raw);
                value->as.text.ptr = mp_copy(yyjson_get_str(raw),value->as.text.len);
                if (!value->as.text.ptr) return false;
            } else return false;
        }
    }
    return true;
}
bool mp_event_decode(const char *data, size_t length, MdEvent *event, char stage_name[16]) {
    stage_name[0]=0;
    memset(event,0,sizeof *event);
    yyjson_doc *doc = mp_json(data,length);
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *keys[] = {"version","result","diagnostic","stop","health","stage"};
    bool ok = mp_fields(root,keys,6) && yyjson_is_uint(yyjson_obj_get(root,"version")) &&
        yyjson_get_uint(yyjson_obj_get(root,"version")) == 1 && yyjson_is_bool(yyjson_obj_get(root,"stop"));
    yyjson_val *health = yyjson_obj_get(root,"health"), *stage = yyjson_obj_get(root,"stage");
    if (mp_string(health,"healthy")) event->health = MD_HEALTHY;
    else if (mp_string(health,"stopped_cleanup")) event->health = MD_STOPPED_CLEANUP;
    else if (mp_string(health,"recovery_required")) event->health = MD_RECOVERY_REQUIRED;
    else ok = false;
    if (!(yyjson_is_null(stage) || mp_string(stage,"protocol") || mp_string(stage,"queue") || mp_string(stage,"lock") ||
#ifndef MINIDB_LEGACY_STAGE
          mp_string(stage,"sql") ||
#endif
          mp_string(stage,"receive") || mp_string(stage,"send") || mp_string(stage,"shutdown"))) ok = false;
    event->stop = yyjson_get_bool(yyjson_obj_get(root,"stop"));
    yyjson_val *result = yyjson_obj_get(root,"result"), *diagnostic = yyjson_obj_get(root,"diagnostic");
    event->has_result = !yyjson_is_null(result); event->has_diagnostic = !yyjson_is_null(diagnostic);
    if (!event->has_result && !event->has_diagnostic) ok = false;
    if (!yyjson_is_null(stage) && !event->has_diagnostic) ok=false;
    if (event->health!=MD_HEALTHY && !event->stop) ok=false;
    if (ok && event->has_result) ok = decode_result(result,&event->result);
    if (ok && event->has_diagnostic) ok = decode_diagnostic(diagnostic,&event->diagnostic);
    if (ok && yyjson_is_str(stage)) snprintf(stage_name,16,"%s",yyjson_get_str(stage));
    yyjson_doc_free(doc);
    if (!ok) md_event_destroy(event);
    return ok;
}
int mp_event_exit(const MdEvent *e, const char *stage) {
    if (!e->has_diagnostic) return 0;
    int code = mf_diagnostic_exit(&e->diagnostic,false);
    return stage && code == 4 ? 8 : code;
}
void mp_error_event(MdEvent *e, const char *code, const char *stage) {
    memset(e,0,sizeof *e);
    e->has_diagnostic = e->stop = true;
    snprintf(e->diagnostic.code,sizeof e->diagnostic.code,"%s",code);
    snprintf(e->diagnostic.phase,sizeof e->diagnostic.phase,"execute");
    snprintf(e->diagnostic.message,sizeof e->diagnostic.message,"Remote %s failure",stage);
}
