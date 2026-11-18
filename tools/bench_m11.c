#define WIN32_LEAN_AND_MEAN
#include "minidb.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

/* 중간 곱은 128비트로 보존한 뒤 UINT64 범위를 검사한다. */
__extension__ typedef unsigned __int128 WideUnsigned;
typedef struct { const char *text; size_t length, offset; } Input;
typedef struct {
    int round, target_index, pair_order;
    bool indexed, hit;
    int64_t target, ticks;
    uint64_t ns;
} Sample;

static bool nanoseconds(int64_t ticks, int64_t frequency, uint64_t *out) {
    if (ticks < 0 || frequency <= 0) return false;
    WideUnsigned result = (WideUnsigned)(uint64_t)ticks * 1000000000 / (uint64_t)frequency;
    if (result > UINT64_MAX) return false;
    *out = (uint64_t)result;
    return true;
}

static bool elapsed(LARGE_INTEGER start, LARGE_INTEGER end, int64_t frequency,
                    int64_t *ticks, uint64_t *ns) {
    if (start.QuadPart < 0 || end.QuadPart < start.QuadPart) return false;
    *ticks = end.QuadPart - start.QuadPart;
    return nanoseconds(*ticks, frequency, ns);
}

static MdReadStatus read_sql(void *context, unsigned char *buffer, size_t capacity, size_t *length) {
    Input *input = context;
    size_t remaining = input->length - input->offset;
    *length = remaining < capacity ? remaining : capacity;
    memcpy(buffer, input->text + input->offset, *length);
    input->offset += *length;
    return *length ? MD_READ_DATA : MD_READ_EOF;
}

static bool oracle(const MdEvent *event, int64_t target, bool hit) {
    const MdResult *result = &event->result;
    if (!event->has_result || event->has_diagnostic || event->stop ||
        event->health != MD_HEALTHY || result->kind != MD_QUERY_RESULT || result->statement_index != 1 ||
        result->column_count != 3 || result->row_count != (size_t)hit || !result->columns)
        return false;
    const char *names[] = {"id", "scan_key", "payload"};
    const MdType types[] = {MD_INT64, MD_INT64, MD_TEXT};
    for (size_t i = 0; i < 3; i++) {
        if (!result->columns[i].name || strcmp(result->columns[i].name, names[i]) ||
            result->columns[i].type != types[i]) return false;
    }
    if (!hit) return true;
    if (!result->values || result->values[0].type != MD_INT64 ||
        result->values[1].type != MD_INT64 || result->values[2].type != MD_TEXT ||
        result->values[0].as.integer != target || result->values[1].as.integer != target)
        return false;
    char expected[64];
    int count = snprintf(expected, sizeof expected, "row-%010" PRId64, target);
    return count > 0 && result->values[2].as.text.ptr &&
           result->values[2].as.text.len == (size_t)count &&
           !memcmp(result->values[2].as.text.ptr, expected, (size_t)count);
}

static bool oracle_selftest(void) {
    MdColumn columns[] = {{"id", MD_INT64}, {"scan_key", MD_INT64}, {"payload", MD_TEXT}};
    MdValue values[] = {{.type=MD_INT64, .as.integer=7}, {.type=MD_INT64, .as.integer=7},
                        {.type=MD_TEXT, .as.text={"row-0000000007", 14}}};
    MdEvent event = {.has_result=true, .result={.kind=MD_QUERY_RESULT, .statement_index=1,
        .columns=columns, .column_count=3, .row_count=1, .values=values}};
    if (!oracle(&event, 7, true) || oracle(&event, 8, true) || oracle(&event, 7, false)) return false;
    values[1].as.integer = 8;
    if (oracle(&event, 7, true)) return false;
    values[1].as.integer = 7;
    values[2].as.text.ptr = "row-0000000008";
    if (oracle(&event, 7, true)) return false;
    values[2].as.text.ptr = "row-0000000007";
    columns[0].name = "wrong";
    if (oracle(&event, 7, true)) return false;
    columns[0].name = "id";
    columns[2].type = MD_INT64;
    if (oracle(&event, 7, true)) return false;
    columns[2].type = MD_TEXT;
    event.has_diagnostic = true;
    if (oracle(&event, 7, true)) return false;
    event.has_diagnostic = false;
    event.result.row_count = 0;
    return oracle(&event, 100, false) && !oracle(&event, 7, true);
}

static bool query(MdDb *db, Sample *sample, int64_t frequency) {
    char sql[256];
    snprintf(sql, sizeof sql, "SELECT id,scan_key,payload FROM bench_rows WHERE %s=%" PRId64 ";",
             sample->indexed ? "id" : "scan_key", sample->target);
    Input input = {sql, strlen(sql), 0};
    MdRequest *request = NULL;
    MdDiagnostic diagnostic = {0};
    MdEvent event = {0}, eof = {0};
    LARGE_INTEGER start, end;
    if (!QueryPerformanceCounter(&start)) return false;
    bool opened = md_request_open(db, (MdInput){read_sql, &input}, &request, &diagnostic);
    MdNext next = opened ? md_request_next(request, &event) : MD_NEXT_STOP;
    MdNext tail = next == MD_NEXT_RESULT ? md_request_next(request, &eof) : MD_NEXT_STOP;
    md_request_close(request);
    bool timed = QueryPerformanceCounter(&end) && elapsed(start, end, frequency, &sample->ticks, &sample->ns);
    /* 오라클과 이벤트 정리는 QPC 조회 구간 밖이다. EOF까지 읽은 결과만 인정한다. */
    bool valid = opened && next == MD_NEXT_RESULT && tail == MD_NEXT_EOF &&
                 !eof.has_result && !eof.has_diagnostic && oracle(&event, sample->target, sample->hit);
    md_event_destroy(&event);
    md_event_destroy(&eof);
    return timed && valid;
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static void print_statistics(const Sample *samples) {
    const char *methods[] = {"id", "scan_key"};
    const char *suffixes[] = {"", "_hit", "_miss"};
    printf("\"statistics\":{");
    for (int method = 0; method < 2; method++) {
        for (int group = 0; group < 3; group++) {
            uint64_t sorted[300];
            size_t count = 0;
            for (size_t i = 0; i < 600; i++) {
                if (samples[i].indexed != (method == 0)) continue;
                if (group && samples[i].hit != (group == 1)) continue;
                sorted[count++] = samples[i].ns;
            }
            qsort(sorted, count, sizeof *sorted, compare_u64);
            printf("%s\"%s%s\":{\"count\":%zu,\"p50_ns\":%" PRIu64 ",\"p95_ns\":%" PRIu64 "}",
                   method || group ? "," : "", methods[method], suffixes[group], count,
                   sorted[(count * 50 + 99) / 100 - 1], sorted[(count * 95 + 99) / 100 - 1]);
        }
    }
    printf("}");
}

static char *utf8(const wchar_t *text) {
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    if (!length) return NULL;
    char *result = malloc((size_t)length);
    if (result && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result, length, NULL, NULL)) {
        free(result);
        return NULL;
    }
    return result;
}

static bool integer(const wchar_t *text, int64_t *out) {
    if (!text || *text < L'0' || *text > L'9') return false;
    wchar_t *end;
    errno = 0;
    unsigned long long value = wcstoull(text, &end, 10);
    if (errno || *end || value > INT64_MAX) return false;
    *out = (int64_t)value;
    return true;
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--selftest")) {
        int64_t ticks;
        uint64_t ns;
        bool valid = oracle_selftest();
        bool timer_valid = !elapsed((LARGE_INTEGER){.QuadPart=2}, (LARGE_INTEGER){.QuadPart=1}, 1, &ticks, &ns) &&
            !elapsed((LARGE_INTEGER){.QuadPart=-1}, (LARGE_INTEGER){.QuadPart=1}, 1, &ticks, &ns) &&
            elapsed((LARGE_INTEGER){.QuadPart=1}, (LARGE_INTEGER){.QuadPart=2}, 1, &ticks, &ns) &&
            ticks == 1 && ns == 1000000000;
        printf("{\"oracle_selftest\":%s,\"timer_selftest\":%s}\n", valid ? "true" : "false", timer_valid ? "true" : "false");
        return valid && timer_valid && fflush(stdout) == 0 ? 0 : 1;
    }
    if (argc == 4 && !wcscmp(argv[1], L"--convert")) {
        int64_t ticks, frequency;
        uint64_t ns;
        if (!integer(argv[2], &ticks) || !integer(argv[3], &frequency) || !nanoseconds(ticks, frequency, &ns)) {
            fputs("invalid timer conversion\n", stderr);
            return 2;
        }
        printf("{\"ns\":%" PRIu64 "}\n", ns);
        return fflush(stdout) == 0 ? 0 : 1;
    }
    if (argc != 7 || wcscmp(argv[1], L"--schema-dir") || wcscmp(argv[3], L"--data-dir") ||
        wcscmp(argv[5], L"--rows")) {
        fputs("expected --schema-dir PATH --data-dir PATH --rows N\n", stderr);
        return 2;
    }
    int64_t rows;
    if (!integer(argv[6], &rows) || rows < 80 || rows > 1000000) {
        fputs("rows must be between 80 and 1000000\n", stderr);
        return 2;
    }
    char *schema = utf8(argv[2]), *data = utf8(argv[4]);
    MdDb *db = NULL;
    MdDiagnostic diagnostic = {0};
    LARGE_INTEGER frequency, start, end;
    int64_t open_ticks;
    uint64_t open_ns;
    if (!schema || !data || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&start)) {
        free(schema); free(data);
        fputs("path or QPC preparation failed\n", stderr);
        return 1;
    }
    bool opened = md_open(schema, data, &db, &diagnostic);
    free(schema); free(data);
    if (!QueryPerformanceCounter(&end) || !opened ||
        !elapsed(start, end, frequency.QuadPart, &open_ticks, &open_ns)) {
        md_close(db);
        fprintf(stderr, "DB open/timer failed %s\n", diagnostic.code);
        return 1;
    }
    Sample samples[600];
    size_t count = 0;
    for (int round = 0; round < 4; round++) {
        for (int target = 0; target < 100; target++) {
            int64_t value = target < 80 ? 1 + (((target * 37 + 17) % 80) * (rows - 1)) / 79 : rows + target - 79;
            for (int order = 0; order < 2; order++) {
                Sample sample = {.round=round, .target_index=target, .pair_order=order,
                    .indexed=((round * 100 + target + order) % 2 == 0), .hit=target < 80, .target=value};
                if (!query(db, &sample, frequency.QuadPart)) {
                    md_close(db);
                    fprintf(stderr, "query/oracle/timer failed round=%d target=%d order=%d\n", round, target, order);
                    return 1;
                }
                if (round) samples[count++] = sample;
            }
        }
    }
    md_close(db);
    printf("{\"rows\":%" PRId64 ",\"seed\":17,\"targets\":100,\"hits\":80,\"misses\":20,"
           "\"warmup_rounds\":1,\"measured_rounds\":3,\"projection\":\"id,scan_key,payload\","
           "\"db_open_count\":1,\"timing_boundary\":\"request_open_through_result_and_eof\","
           "\"frequency\":%" PRId64 ",\"open_ticks\":%" PRId64 ",\"open_ns\":%" PRIu64 ",\"samples\":[",
           rows, frequency.QuadPart, open_ticks, open_ns);
    for (size_t i = 0; i < count; i++) {
        const Sample *sample = &samples[i];
        printf("%s{\"round\":%d,\"target_index\":%d,\"pair_order\":%d,\"method\":\"%s\","
               "\"target\":%" PRId64 ",\"hit\":%s,\"ticks\":%" PRId64 ",\"ns\":%" PRIu64 "}",
               i ? "," : "", sample->round, sample->target_index, sample->pair_order,
               sample->indexed ? "id" : "scan_key", sample->target, sample->hit ? "true" : "false",
               sample->ticks, sample->ns);
    }
    printf("],");
    print_statistics(samples);
    puts("}");
    return ferror(stdout) || fflush(stdout) ? 1 : 0;
}
