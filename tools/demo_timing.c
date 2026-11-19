#define WIN32_LEAN_AND_MEAN
#include "minidb.h"
#include "format.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static char *utf8(const wchar_t *wide) {
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                                     NULL, 0, NULL, NULL);
    char *value = length ? malloc((size_t)length) : NULL;
    if (value && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                                     value, length, NULL, NULL)) {
        free(value);
        return NULL;
    }
    return value;
}

static MdReadStatus read_sql(void *context, unsigned char *buffer, size_t capacity,
                             size_t *length) {
    DWORD got = 0;
    *length = 0;
    if (!ReadFile(*(HANDLE *)context, buffer, (DWORD)capacity, &got, NULL))
        return MD_READ_ERROR;
    *length = got;
    return got ? MD_READ_DATA : MD_READ_EOF;
}

int wmain(int argc, wchar_t **argv) {
    if (argc != 4) {
        fputs("Usage: demo_timing SCHEMA_DIR DATA_DIR SQL_FILE\n", stderr);
        return 2;
    }
    char *schema = utf8(argv[1]), *data = utf8(argv[2]);
    if (!schema || !data) {
        free(schema);
        free(data);
        return 3;
    }
    MdDb *db = NULL;
    MdRequest *request = NULL;
    MdDiagnostic diagnostic = {0};
    MdEvent event = {0}, tail = {0};
    HANDLE input = INVALID_HANDLE_VALUE;
    LARGE_INTEGER frequency, begin, ready, end;
    int code = 3;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&begin)) goto done;

    /* 준비는 SQL 파일 열기와 실제 DB open이다. 추정값을 사후 차감하지 않는다. */
    input = CreateFileW(argv[3], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (input == INVALID_HANDLE_VALUE) {
        fputs("Cannot open demo SQL file\n", stderr);
        goto done;
    }
    if (!md_open(schema, data, &db, &diagnostic)) {
        mf_diagnostic(&diagnostic, "run", true);
        goto done;
    }
    if (!QueryPerformanceCounter(&ready) || begin.QuadPart < 0 ||
        ready.QuadPart < begin.QuadPart) goto done;

    /* 같은 열린 DB에서 요청 생성·SQL 파싱/실행·EOF·요청 정리만 측정한다.
     * INSERT의 검증/저장/flush/인덱스 반영은 SQL 실행 비용에 남긴다. */
    if (!md_request_open(db, (MdInput){read_sql, &input}, &request, &diagnostic)) {
        mf_diagnostic(&diagnostic, "run", true);
        code = 4;
        goto done;
    }
    MdNext next = md_request_next(request, &event);
    MdNext last = next == MD_NEXT_RESULT && !event.has_diagnostic && !event.stop
        ? md_request_next(request, &tail) : MD_NEXT_STOP;
    md_request_close(request);
    request = NULL;
    if (!QueryPerformanceCounter(&end) || end.QuadPart < ready.QuadPart) goto done;
    if (event.has_diagnostic || tail.has_diagnostic) {
        MdDiagnostic *error = event.has_diagnostic ? &event.diagnostic : &tail.diagnostic;
        mf_diagnostic(error, "run", true);
        code = mf_diagnostic_exit(error, false);
        goto done;
    }
    if (next != MD_NEXT_RESULT || !event.has_result || event.stop ||
        last != MD_NEXT_EOF || tail.has_result || tail.stop) {
        fputs("Demo timing requires one successful SQL statement\n", stderr);
        code = 4;
        goto done;
    }

    /* 결과는 이벤트 소유다. DB 닫기·JSON 직렬화·출력은 SQL 측정 밖에 둔다. */
    md_close(db);
    db = NULL;
    int64_t preparation = ready.QuadPart - begin.QuadPart;
    int64_t execution = end.QuadPart - ready.QuadPart;
    Buffer rendered;
    mf_begin(&rendered);
    if (!mf_render(&event.result, true, &rendered)) {
        mf_end(&rendered);
        goto done;
    }
    char timing[256];
    int count = snprintf(timing, sizeof timing,
        "{\"preparation_ms\":%.6f,\"sql_ms\":%.6f,\"frequency\":%" PRId64
        ",\"preparation_ticks\":%" PRId64 ",\"sql_ticks\":%" PRId64 ",\"event\":",
        (double)preparation * 1000.0 / (double)frequency.QuadPart,
        (double)execution * 1000.0 / (double)frequency.QuadPart,
        (int64_t)frequency.QuadPart, preparation, execution);
    /* 프레임을 만들지 않고 기존 JSON 결과를 그대로 감싼다. */
    bool written = count > 0 && (size_t)count < sizeof timing &&
        mf_stream_write(STD_OUTPUT_HANDLE, timing, (size_t)count) &&
        mf_stream_write(STD_OUTPUT_HANDLE, rendered.p, rendered.n) &&
        mf_stream_write(STD_OUTPUT_HANDLE, "}\n", 2);
    mf_end(&rendered);
    code = written ? 0 : 7;
done:
    md_request_close(request);
    md_close(db);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    md_event_destroy(&event);
    md_event_destroy(&tail);
    free(schema);
    free(data);
    if (code == 3) fputs("Demo preparation or clock measurement failed\n", stderr);
    return code;
}
