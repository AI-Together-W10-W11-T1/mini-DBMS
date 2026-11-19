#include "internal.h"
#include "protocol.h"
#include "minidb_client.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *cursor;
    unsigned reads;
    MdDb *stop_db;
    uint64_t *clock;
    MdRequest *blocker;
} Input;

typedef struct {
    MdDb *db;
    MdRequest *request;
    Input input;
    MdExecutionOptions options;
    MdEvent event;
    MdNext next;
    const char *hold_point, *cancel_point;
    HANDLE entered, release;
    volatile LONG cancel, reached;
    LONG cancel_after;
    uint64_t clock, advance;
    LONG *sequence;
    LONG order;
    bool budget_probe;
    unsigned wait_signalled, wait_completed;
} Job;

static MdReadStatus read_sql(void *context, unsigned char *buffer, size_t capacity, size_t *length) {
    Input *input = context;
    input->reads++;
    if (input->clock)
        *input->clock += 1000;
    if (input->blocker) {
        assert(md_gate_enter(input->blocker, true));
        input->blocker = NULL;
    }
    if (input->stop_db) {
        MdRequest writer = {.db = input->stop_db};
        assert(md_gate_enter(&writer, true));
        input->stop_db->health = MD_RECOVERY_REQUIRED;
        md_gate_leave(input->stop_db, true);
        input->stop_db = NULL;
    }
    *length = strlen(input->cursor);
    if (*length > capacity)
        *length = capacity;
    memcpy(buffer, input->cursor, *length);
    input->cursor += *length;
    return *length ? MD_READ_DATA : MD_READ_EOF;
}

static MdCancelReason cancelled(void *context) {
    Job *job = context;
    return InterlockedCompareExchange(&job->cancel, 0, 0) ? MD_CANCEL_SHUTDOWN : MD_CANCEL_NONE;
}
static uint64_t test_clock(void *context) { return ((Job *)context)->clock; }
static void observe(const char *point, void *context) {
    Job *job = context;
    if (job->budget_probe) {
        unsigned phase = job->input.reads ? 2 : 1;
        if (!strcmp(point, "gate-wait") && !(job->wait_signalled & phase)) {
            job->wait_signalled |= phase;
            assert(SetEvent(phase == 1 ? job->entered : job->release));
        }
        /* gate 상태 잠금 안: 실제 소유자 해제 후 깨어난 대기만 시계에 반영한다. */
        if (!strcmp(point, "gate-waited") && !job->db->writer &&
            !(job->wait_completed & phase)) {
            job->wait_completed |= phase;
            job->clock += phase == 1 ? 4 : 6;
        }
    }
    if (job->cancel_point && !strcmp(point, job->cancel_point)) {
        LONG reached = InterlockedIncrement(&job->reached);
        if (reached >= (job->cancel_after ? job->cancel_after : 1))
            InterlockedExchange(&job->cancel, 1);
    }
    if (!strcmp(point, "gate-read") || !strcmp(point, "gate-write")) {
        if (job->sequence)
            job->order = InterlockedIncrement(job->sequence);
        job->clock += job->advance;
    }
    if (job->hold_point && !strcmp(point, job->hold_point)) {
        assert(SetEvent(job->entered));
        assert(WaitForSingleObject(job->release, 5000) == WAIT_OBJECT_0);
    }
}
static void job_open(Job *job, MdDb *db, const char *sql) {
    *job = (Job){.db = db, .input = {.cursor = sql}, .clock = 100};
    MdDiagnostic diagnostic;
    assert(md_request_open(db, (MdInput){read_sql, &job->input}, &job->request, &diagnostic));
    job->options = (MdExecutionOptions){.cancel = cancelled, .context = job};
}
static DWORD WINAPI run_job(void *context) {
    Job *job = context;
    md_test_execution_hook(observe, job);
    md_test_execution_clock(test_clock, job);
    job->next = md_request_next_ex(job->request, &job->options, &job->event);
    md_test_execution_hook(NULL, NULL);
    md_test_execution_clock(NULL, NULL);
    return 0;
}
static void job_close(Job *job) {
    md_event_destroy(&job->event);
    md_request_close(job->request);
    if (job->entered) CloseHandle(job->entered);
    if (job->release) CloseHandle(job->release);
}
static HANDLE start_job(Job *job, const char *hold) {
    if (hold) {
        job->hold_point = hold;
        job->entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        job->release = CreateEventW(NULL, TRUE, FALSE, NULL);
        assert(job->entered && job->release);
    }
    HANDLE thread = CreateThread(NULL, 0, run_job, job, 0, NULL);
    assert(thread);
    return thread;
}
static void join_job(HANDLE thread) {
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    CloseHandle(thread);
}
static void wait_entered(Job *job) { assert(WaitForSingleObject(job->entered, 5000) == WAIT_OBJECT_0); }
static void wait_writers(MdDb *db, size_t count) {
    uint64_t start = GetTickCount64();
    for (;;) {
        AcquireSRWLockShared(&db->gate);
        bool ready = db->waiting_writers == count;
        ReleaseSRWLockShared(&db->gate);
        if (ready) return;
        assert(GetTickCount64() - start < 5000);
        Sleep(1);
    }
}
static void execute(MdDb *db, const char *sql, MdEvent *event) {
    Job job;
    job_open(&job, db, sql);
    run_job(&job);
    if (job.next != MD_NEXT_RESULT)
        fprintf(stderr, "SQL failed %s %s\n", sql, job.event.diagnostic.code);
    assert(job.next == MD_NEXT_RESULT && !job.event.has_diagnostic);
    *event = job.event;
    memset(&job.event, 0, sizeof job.event);
    job_close(&job);
}
static void equal_result(const MdResult *left, const MdResult *right) {
    assert(left->kind == right->kind && left->row_count == right->row_count &&
           left->column_count == right->column_count && left->affected_rows == right->affected_rows);
    for (size_t i = 0; i < left->row_count * left->column_count; i++) {
        assert(left->values[i].type == right->values[i].type);
        if (left->values[i].type == MD_INT64)
            assert(left->values[i].as.integer == right->values[i].as.integer);
        if (left->values[i].type == MD_TEXT) {
            assert(left->values[i].as.text.len == right->values[i].as.text.len);
            assert(!memcmp(left->values[i].as.text.ptr, right->values[i].as.text.ptr,
                           left->values[i].as.text.len));
        }
    }
}
static bool index_consistent(MdDb *db) {
    MdTable *table = &db->tables[0];
    if (table->index.count != table->row_count)
        return false;
    for (size_t row = 0; row < table->row_count; row++) {
        size_t found;
        if (!md_bt_get(&table->index, table->rows[row * table->count + table->id].as.integer, &found) || found != row)
            return false;
    }
    return true;
}
static void read_overlap(MdDb *db) {
    const char *queries[] = {"SELECT name FROM users ORDER BY name;", "VALUES (42);",
                            "VALUES (42) UNION VALUES (43);"};
    for (size_t i = 0; i < 3; i++) {
        MdEvent oracle;
        execute(db, queries[i], &oracle);
        Job first, second;
        job_open(&first, db, queries[i]);
        job_open(&second, db, queries[i]);
        HANDLE a = start_job(&first, "gate-read");
        wait_entered(&first);
        HANDLE b = start_job(&second, "gate-read");
        wait_entered(&second);
        AcquireSRWLockShared(&db->gate);
        assert(db->readers == 2 && !db->writer);
        ReleaseSRWLockShared(&db->gate);
        SetEvent(first.release);
        SetEvent(second.release);
        join_job(a); join_job(b);
        assert(first.next == MD_NEXT_RESULT && second.next == MD_NEXT_RESULT);
        equal_result(&first.event.result, &oracle.result);
        equal_result(&second.event.result, &oracle.result);
        md_event_destroy(&oracle);
        job_close(&first); job_close(&second);
    }
}
static void exclusion(MdDb *db, bool writer_first) {
    Job first, second;
    job_open(&first, db, writer_first ? "INSERT INTO users(id,name) VALUES(9,'nine');" :
                                      "SELECT name FROM users ORDER BY name;");
    job_open(&second, db, writer_first ? "UPDATE users SET name='changed' WHERE id=1;" :
                                       "DELETE FROM users WHERE id=2;");
    HANDLE a = start_job(&first, writer_first ? "gate-write" : "gate-read");
    wait_entered(&first);
    HANDLE b = start_job(&second, "gate-write");
    if (!writer_first) wait_writers(db, 1);
    assert(WaitForSingleObject(second.entered, 30) == WAIT_TIMEOUT);
    SetEvent(first.release);
    wait_entered(&second);
    AcquireSRWLockShared(&db->gate);
    assert(db->writer && db->readers == 0);
    ReleaseSRWLockShared(&db->gate);
    SetEvent(second.release);
    join_job(a); join_job(b);
    assert(first.next == MD_NEXT_RESULT && second.next == MD_NEXT_RESULT);
    if (!writer_first) {
        /* 빌린 TEXT는 READ 해제 전에 복제되어 DELETE 뒤에도 유효하다. */
        assert(first.event.result.row_count == 4);
        assert(first.event.result.values[1].as.text.len == 3);
    }
    assert(index_consistent(db));
    job_close(&first); job_close(&second);
}
static void admission(MdDb *db) {
    Job reader, writer, later;
    LONG sequence = 0;
    job_open(&reader, db, "VALUES (1);");
    job_open(&writer, db, "INSERT INTO users(id) VALUES(9);");
    job_open(&later, db, "VALUES (3);");
    reader.sequence = writer.sequence = later.sequence = &sequence;
    HANDLE a = start_job(&reader, "gate-read");
    wait_entered(&reader);
    HANDLE b = start_job(&writer, "gate-write");
    wait_writers(db, 1);
    HANDLE c = start_job(&later, "gate-read");
    assert(WaitForSingleObject(later.entered, 30) == WAIT_TIMEOUT);
    SetEvent(reader.release);
    wait_entered(&writer);
    assert(WaitForSingleObject(later.entered, 30) == WAIT_TIMEOUT);
    SetEvent(writer.release);
    wait_entered(&later);
    SetEvent(later.release);
    join_job(a); join_job(b); join_job(c);
    assert(reader.order == 1 && writer.order == 2 && later.order == 3);
    assert(db->waiting_writers == 0 && db->readers == 0 && !db->writer);
    job_close(&reader); job_close(&writer); job_close(&later);
}
static void waiting(MdDb *db) {
    Job active, waiting, following;
    job_open(&active, db, "VALUES (1);");
    job_open(&waiting, db, "INSERT INTO users(id) VALUES(9);");
    job_open(&following, db, "VALUES (3);");
    HANDLE a = start_job(&active, "gate-read");
    wait_entered(&active);
    HANDLE b = start_job(&waiting, NULL);
    wait_writers(db, 1);
    InterlockedExchange(&waiting.cancel, 1);
    join_job(b);
    assert(waiting.next == MD_NEXT_STOP && db->waiting_writers == 0);
    HANDLE c = start_job(&following, "gate-read");
    wait_entered(&following);
    assert(db->readers == 2);
    SetEvent(active.release); SetEvent(following.release);
    join_job(a); join_job(c);
    job_close(&active); job_close(&waiting); job_close(&following);
    for (uint64_t budget = 10; budget <= 11; budget++) {
        Job probe;
        job_open(&probe, db, "INSERT INTO users(id) VALUES(9);");
        probe.budget_probe = true;
        probe.options.lock_timeout_ms = budget;
        probe.input.clock = &probe.clock;
        MdRequest owner = {.db = db};
        probe.input.blocker = &owner;
        probe.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        probe.release = CreateEventW(NULL, TRUE, FALSE, NULL);
        assert(probe.entered && probe.release && md_gate_enter(&owner, true));
        HANDLE thread = start_job(&probe, NULL);
        wait_entered(&probe); /* next_ex의 준비 전 health gate 대기 */
        md_gate_leave(db, true);
        assert(WaitForSingleObject(probe.release, 5000) == WAIT_OBJECT_0);
        wait_writers(db, 1); /* 입력 준비 뒤 실제 최종 WRITE gate 대기 */
        md_gate_leave(db, true);
        join_job(thread);
        assert(probe.wait_completed == 3 && probe.request->lock_wait_ms == 10);
        assert(probe.clock >= 1110); /* 준비의 1000은 lock budget에 들어가지 않는다. */
        if (budget == 10) {
            assert(probe.next == MD_NEXT_STOP && !probe.event.has_result);
            assert(!strcmp(probe.event.diagnostic.code, "TIMEOUT"));
            assert(!strcmp(probe.request->cancel_stage, "lock"));
        } else {
            assert(probe.next == MD_NEXT_RESULT && probe.event.result.affected_rows == 1);
        }
        assert(!db->waiting_writers && !db->readers && !db->writer);
        printf("two-gate budget=%llu waits=4+6 accumulated=%llu prepare_clock=%llu outcome=%s counters=0\n",
               (unsigned long long)budget, (unsigned long long)probe.request->lock_wait_ms,
               (unsigned long long)probe.clock, budget == 10 ? "lock-timeout" : "success");
        job_close(&probe);
        MdEvent next;
        execute(db, budget == 10 ? "VALUES(99);" : "DELETE FROM users WHERE id=9;", &next);
        md_event_destroy(&next);
    }
}
static void classification(MdDb *db) {
    Job job;
    job_open(&job, db, "\xef\xbb\xbf/*c;*/;--empty\nVALUES ('a;b');INSERT INTO users(id) VALUES(9);SELECT id FROM users WHERE id=9;BAD;VALUES(99);");
    assert(md_request_next(job.request, &job.event) == MD_NEXT_RESULT);
    assert(job.event.result.values[0].as.text.len == 3);
    md_event_destroy(&job.event);
    assert(md_request_next(job.request, &job.event) == MD_NEXT_RESULT);
    assert(job.event.result.affected_rows == 1);
    md_event_destroy(&job.event);
    assert(md_request_next(job.request, &job.event) == MD_NEXT_RESULT);
    assert(job.event.result.values[0].as.integer == 9);
    md_event_destroy(&job.event);
    assert(md_request_next(job.request, &job.event) == MD_NEXT_STOP);
    assert(job.event.diagnostic.statement_index == 4 && job.event.diagnostic.has_position);
    assert(md_request_next(job.request, &(MdEvent){0}) == MD_NEXT_EOF);
    job_close(&job);
}
static void health(MdDb *db) {
    Job initial, preparing;
    job_open(&initial, db, "VALUES(1);");
    job_open(&preparing, db, "VALUES(2);");
    db->health = MD_RECOVERY_REQUIRED;
    run_job(&initial);
    assert(initial.next == MD_NEXT_STOP && initial.input.reads == 0);
    db->health = MD_HEALTHY;
    preparing.input.stop_db = db;
    run_job(&preparing);
    assert(preparing.next == MD_NEXT_STOP && preparing.input.reads > 0);
    assert(!preparing.event.has_result && preparing.event.health == MD_RECOVERY_REQUIRED);
    job_close(&initial); job_close(&preparing);
    db->health = MD_HEALTHY;
}
static void timing(MdDb *db) {
    Job initial;
    job_open(&initial, db, "VALUES(42);");
    initial.cancel = 1;
    run_job(&initial);
    assert(initial.next == MD_NEXT_STOP && initial.input.reads == 0);
    assert(!strcmp(initial.event.diagnostic.code, "SERVER_STOPPING"));
    job_close(&initial);
    for (unsigned mode = 0; mode < 4; mode++) {
        Job job;
        job_open(&job, db, "VALUES(42);");
        job.options.sql_timeout_ms = mode == 0 ? 0 : mode == 3 ? UINT64_MAX : 10;
        job.advance = mode == 2 ? 10 : 0;
        job.input.clock = &job.clock;
        run_job(&job);
        if (mode < 2) assert(job.next == MD_NEXT_RESULT);
        else {
            assert(job.next == MD_NEXT_STOP && !job.event.has_result);
            assert(!strcmp(job.event.diagnostic.code, mode == 3 ? "ARGUMENT_ERROR" : "TIMEOUT"));
        }
        job_close(&job);
    }
}
static void loops(MdDb *db) {
    const struct { const char *point, *sql; } cases[] = {
        {"lexer", "VALUES('long token');"},
        {"scan", "SELECT name FROM users;"},
        {"join", "SELECT a.name FROM users AS a INNER JOIN users AS b ON a.id=b.id;"},
        {"text-compare", "SELECT name FROM users WHERE name='one';"},
        {"group", "SELECT name,COUNT(*) FROM users GROUP BY name;"},
        {"projection", "SELECT name FROM users;"},
        {"sort", "SELECT name FROM users ORDER BY name;"},
        {"union", "VALUES(1) UNION VALUES(2);"},
        {"staging", "UPDATE users SET name='new' WHERE id=1;"},
        {"serialize-text", "INSERT INTO users(id,name) VALUES(9,'nine');"},
        {"page-release", "SELECT name AS a,name AS b FROM users FETCH FIRST 0 ROWS ONLY;"},
        {"page-move", "SELECT name AS a,name AS b FROM users ORDER BY a OFFSET 1 ROWS FETCH FIRST 2 ROWS ONLY;"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        if (!strncmp(cases[i].point, "page-", 5)) {
            Job control;
            job_open(&control, db, cases[i].sql);
            run_job(&control);
            assert(control.next == MD_NEXT_RESULT && !control.event.has_diagnostic);
            assert(control.event.result.row_count == (!strcmp(cases[i].point, "page-release") ? 0 : 2));
            md_event_destroy(&control.event);
            assert(md_request_next(control.request, &control.event) == MD_NEXT_EOF);
            job_close(&control);
        }
        Job job;
        job_open(&job, db, cases[i].sql);
        job.cancel_point = cases[i].point;
        if (!strncmp(cases[i].point, "page-", 5))
            job.cancel_after = 3; /* 실제 셀 정리/이동이 진행된 뒤 취소한다. */
        run_job(&job);
        if (!job.reached || job.next != MD_NEXT_STOP || job.event.has_result)
            fprintf(stderr, "loop %s reached=%ld next=%d code=%s\n", cases[i].point,
                    job.reached, job.next, job.event.diagnostic.code);
        assert(job.reached > 0 && job.next == MD_NEXT_STOP && !job.event.has_result);
        assert(!strcmp(job.event.diagnostic.code, "SERVER_STOPPING"));
        assert(db->health == MD_HEALTHY && !db->readers && !db->writer);
        if (!strncmp(cases[i].point, "page-", 5))
            printf("%s progress=%ld cancelled partial_result=0 gate_released=1\n",
                   cases[i].point, job.reached);
        job_close(&job);
    }
}
static void commit_boundaries(MdDb *db, bool preparation) {
    const char *commands[] = {"INSERT INTO users(id,name) VALUES(9,'nine');",
                              "UPDATE users SET name='changed' WHERE id=1;",
                              "DELETE FROM users WHERE id=1;"};
    const char *points[] = {"before-marker", "after-marker", "before-flush", "after-flush",
                            "before-replace", "after-replace"};
    size_t command_count = preparation ? 3 : 1;
    for (size_t command = 0; command < command_count; command++) {
        size_t begin = preparation ? 0 : 1, end = preparation ? 1 : 4;
        for (size_t point = begin; point < end; point++) {
            Job job;
            job_open(&job, db, commands[command]);
            job.cancel_point = points[point];
            run_job(&job);
            assert(job.reached > 0);
            if (preparation) {
                assert(job.next == MD_NEXT_STOP && !job.event.has_result);
                assert(job.event.diagnostic.committed == MD_NO && db->tables[0].row_count == 4);
            } else {
                assert(job.next == MD_NEXT_RESULT && job.event.has_result && job.event.stop);
                assert(job.event.diagnostic.committed == MD_YES && job.event.result.affected_rows == 1);
                MdEvent cleanup;
                execute(db, "DELETE FROM users WHERE id=9;", &cleanup);
                md_event_destroy(&cleanup);
            }
            assert(db->health == MD_HEALTHY && index_consistent(db));
            job_close(&job);
        }
    }
    if (!preparation) {
        for (size_t command = 1; command < 3; command++) {
            Job job;
            job_open(&job, db, commands[command]);
            job.cancel_point = points[command == 1 ? 4 : 5];
            run_job(&job);
            assert(job.reached && job.next == MD_NEXT_RESULT && job.event.stop);
            assert(job.event.diagnostic.committed == MD_YES && job.event.diagnostic.affected_rows == 1);
            job_close(&job);
        }
    }
}
static int sdk_timeout(const char *port, bool saved_success, bool normal) {
    MdClientOptions options;
    mdc_options_default(&options);
    options.port = (uint16_t)atoi(port);
    MdClient *client = NULL;
    MdClientError error;
    assert(mdc_connect(&options, &client, &error));
    const char *sql = "VALUES(42);";
    assert(mdc_execute(client, (const unsigned char *)sql, strlen(sql), &error));
    MdEvent event;
    MdEvent saved = {0};
    if (saved_success) {
        assert(mdc_next(client, &saved, &error) == MDC_EVENT);
        assert(saved.result.values[0].as.integer == 42);
    }
    MdClientNext next = mdc_next(client, &event, &error);
    if (normal) {
        assert(next == MDC_EVENT && event.has_result && event.result.values[0].as.integer == 42);
        assert(mdc_next(client, &(MdEvent){0}, &error) == MDC_DONE && error.exit_code == 0);
        md_event_destroy(&event);
        mdc_close(client);
        return 0;
    }
#ifdef MINIDB_LEGACY_STAGE
    assert(next == MDC_ERROR && !strcmp(error.code, "PROTOCOL_ERROR") && error.execution_unconfirmed);
    assert(!event.has_result && !event.has_diagnostic);
    if (saved_success)
        assert(error.last_statement_index == 1);
    assert(!mdc_execute(client, (const unsigned char *)sql, strlen(sql), &error));
#else
    assert(next == MDC_EVENT && event.has_diagnostic && !event.has_result);
    assert(!strcmp(event.diagnostic.code, "TIMEOUT") && !strcmp(error.stage, "sql"));
    assert(mdc_next(client, &(MdEvent){0}, &error) == MDC_DONE && error.exit_code == 8);
#endif
    md_event_destroy(&event);
    mdc_close(client);
    if (saved_success)
        assert(saved.result.values[0].as.integer == 42);
    md_event_destroy(&saved);
    return 0;
}
static MdDb *database(const char *directory) {
    assert(CreateDirectoryA(directory, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    char schema[2048], data[2048], path[2048];
    snprintf(schema, sizeof schema, "%s/s", directory);
    snprintf(data, sizeof data, "%s/d", directory);
    assert(CreateDirectoryA(schema, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    snprintf(path, sizeof path, "%s/users.json", schema);
    FILE *file = fopen(path, "wb");
    assert(file);
    const char *json = "{\"version\":1,\"name\":\"users\",\"columns\":[{\"name\":\"id\",\"type\":\"INT64\"},{\"name\":\"name\",\"type\":\"TEXT\"}]}";
    assert(fwrite(json, 1, strlen(json), file) == strlen(json));
    assert(fclose(file) == 0);
    MdEvent event;
    assert(md_operate(&(MdOperationOptions){.operation = MD_INIT, .schema_dir = schema, .data_dir = data}, &event));
    md_event_destroy(&event);
    MdDb *db = NULL;
    MdDiagnostic diagnostic;
    assert(md_open(schema, data, &db, &diagnostic));
    execute(db, "INSERT INTO users(id,name) VALUES(1,'one'),(2,'two'),(3,'three'),(4,'four');", &event);
    md_event_destroy(&event);
    return db;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    if (!strcmp(argv[1], "--sdk-timeout"))
        return sdk_timeout(argv[2], false, false);
    if (!strcmp(argv[1], "--sdk-after-success"))
        return sdk_timeout(argv[2], true, false);
    if (!strcmp(argv[1], "--sdk-normal"))
        return sdk_timeout(argv[2], false, true);
    assert(CreateDirectoryA(argv[2], NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    char directory[2048];
    snprintf(directory, sizeof directory, "%s/%lu-%llu", argv[2], GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    MdDb *db = database(directory);
    const char *name = argv[1];
    if (!strcmp(name, "M13-READ-01")) read_overlap(db);
    else if (!strcmp(name, "M13-RW-01")) exclusion(db, false);
    else if (!strcmp(name, "M13-WW-01")) exclusion(db, true);
    else if (!strcmp(name, "M13-ADMIT-01")) admission(db);
    else if (!strcmp(name, "M13-WAIT-01")) waiting(db);
    else if (!strcmp(name, "M13-CLASSIFY-01")) classification(db);
    else if (!strcmp(name, "M13-HEALTH-01")) health(db);
    else if (!strcmp(name, "M13-TIME-01")) timing(db);
    else if (!strcmp(name, "M13-LOOP-01")) loops(db);
    else if (!strcmp(name, "M13-PREP-01")) commit_boundaries(db, true);
    else if (!strcmp(name, "M13-COMMIT-01")) {
        commit_boundaries(db, false);
        const char *faults[] = {"append_write", "rollback", "cleanup"};
        const MdHealth health[] = {MD_HEALTHY, MD_RECOVERY_REQUIRED, MD_STOPPED_CLEANUP};
        for (size_t i = 0; i < 3; i++) {
            char path[2048];
            snprintf(path, sizeof path, "%s/f%zu", directory, i);
            MdDb *fault_db = database(path);
            Job job;
            job_open(&job, fault_db, "INSERT INTO users(id) VALUES(9);");
            job.cancel_point = "after-marker";
            assert(!_putenv_s("MINIDB_TEST_FAULT", faults[i]));
            run_job(&job);
            assert(!_putenv_s("MINIDB_TEST_FAULT", ""));
            assert(job.reached && job.event.has_diagnostic && job.event.stop);
            assert(job.event.health == health[i]);
            assert(!strcmp(job.event.diagnostic.code, i == 1 ? "RECOVERY_REQUIRED" : "IO_ERROR"));
            if (i == 2) {
                assert(job.event.has_result && job.event.diagnostic.committed == MD_YES);
                assert(job.event.diagnostic.affected_rows == 1);
            } else {
                assert(!job.event.has_result);
                assert(job.event.diagnostic.rollback == (i == 1 ? MD_ROLLBACK_FAILED : MD_ROLLBACK_CONFIRMED));
            }
            job_close(&job);
            md_close(fault_db);
        }
    }
    else assert(!"Unknown M13 test");
    md_close(db);
    printf("%s passed\n", name);
    return 0;
}
