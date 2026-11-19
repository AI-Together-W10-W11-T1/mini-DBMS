# M12 서버·C SDK·원격 CLI 설계 계약

2026-11-18 수요일(Asia/Seoul). 사용자 최종 all-A와 초기값 선택을 반영한 문서 설계다. 이번 승인은 계약·GWT 문서화이며 C 구현 승인은 후속 사용자 확인으로 받는다. 이름·wire·함수 서명은 선택한 동작을 구현하는 검토 가능한 상세 제안이다.

## 1. 범위와 기존 계약

M12는 Windows C23 서버 executable, 헤더+정적 C SDK, SDK 사용 CLI, 네트워크 reactor+고정 SQL worker 풀을 다룬다. 길이 프레임 TCP+JSON, 복수 SQL, 지속 연결당 요청 하나, 문장별 배타 엔진 실행을 적용한다. M13에서 공유 읽기·배타 쓰기와 SQL 협력 취소를 설계·구현한다.

[SQL](sql-contract.md)·[스키마](schema-contract.md)·[저장](storage-contract.md)·[로컬 CLI](cli-contract.md)의 의미를 재사용한다. 문장 단위 원자성·첫 오류 중단·앞선 성공 유지와 DB 디렉터리 단일 프로세스 사용권을 보존한다. init/migrate는 기존 관리 CLI로 수행한다. ACK는 생략하며 원격 출력 실패의 관찰 시점은 §5를 따른다.

package registry·타언어 SDK·ODBC·ISO 정식 호환·TLS/인증 도입은 보류한다. Phase 1 전체204개 감사는 사용자 지시로 생략·미실행을 유지한다.

## 2. 실행 입구와 최소 SDK 제안

`minidbd --schema-dir S --data-dir D --port P [--bind IPv4] [자원/시간 옵션]`은 DB open·전수 준비가 성공한 뒤 listen한다. port는 필수이며 1~65535다. bind 기본은 `127.0.0.1`, 명시 IPv4를 허용한다. 준비 실패에 요청을 실행하지 않는다.

`minidb-remote --host IPv4 --port P (--file SQL | --stdin) [--format table|jsonl] [SDK 시간/상한 옵션]`은 SDK만 호출한다. 시작 시 UTF-8 SQL 전체를 요청 상한까지 읽고 제출하므로 stdin은 EOF 뒤 전송된다. 로컬 CLI의 스트리밍 stdin 동작은 유지한다.

공개 헤더는 `minidb_client.h`와 기존 `minidb.h`를 사용한다. SDK의 `MdEvent`는 재조립·검증 완료 후 소유권을 호출자에게 넘기며 `md_event_destroy`로 해제한다. DB 엔진 구현을 SDK에 연결하지 않도록 결과 해제 책임을 공통 결과 소스로 옮긴다.

```c
typedef struct MdClient MdClient;
typedef enum { MDC_EVENT, MDC_DONE, MDC_ERROR } MdClientNext;
bool mdc_connect(const MdClientOptions *options, MdClient **out, MdClientError *error);
bool mdc_execute(MdClient *client, const unsigned char *sql, size_t length, MdClientError *error);
MdClientNext mdc_next(MdClient *client, MdEvent *event, MdClientError *error);
void mdc_close(MdClient *client);
```

Options는 IPv4·port·connect/receive/send 기한·request/frame/event/total 상한을 담는다. 오류는 고정 크기 code/message, 마지막 완성 이벤트의 문장 번호, 미확인 후속 실행 가능 여부를 담는다. SQL 진단은 `MdEvent`에 유지하며 전송 오류와 구분한다. 하나의 연결은 한 호출자만 사용한다.

`execute`는 idle에서만 허용한다. `next`의 DONE 소비 후 재사용한다. SQL 오류도 정상 done까지 소비하면 연결을 재사용할 수 있다. 중간 close·프로토콜 오류·불완전 프레임·전송 실패는 연결을 폐기한다. 이미 반환한 이벤트는 연결 종료 뒤에도 유효하다. 재연결은 새 connect로 하며 SQL 자동 재전송은 하지 않는다.

## 3. wire v1과 바이트 chunk 제안

각 프레임은 `kind 1 byte + payload_length 4 byte unsigned big-endian + payload`다. 길이는 payload 바이트만 세며 NUL·BOM·줄 종료 문자를 덧붙이지 않는다. payload 수신 전 kind/길이/현재 상태/상한을 검증한다. 부분 header·payload, 합쳐진 프레임, 부분 송신을 처리한다.

| kind | payload와 순서 |
|---|---|
| Q | UTF-8 JSON `{"version":1,"type":"execute","sql":"..."}`. 한 요청에 하나 |
| B | JSON `{"version":1,"type":"begin","event_index":"1","bytes":"N"}` |
| D | 완성 이벤트 JSON의 연속 원시 바이트 조각. B와 E 사이에만 허용 |
| E | JSON `{"version":1,"type":"end","event_index":"1"}` |
| F | JSON `{"version":1,"type":"done","status":"ok|error|stopped","events":"N","exit_code":0}` |

Q/B/E/F는 정확한 필수 필드와 버전·타입을 검사하며 중복·미지 필드를 거절한다. B/E의 인덱스는 요청별 1부터 연속이다. D 길이 합은 B.bytes와 정확히 일치해야 한다. D는 UTF-8 문자·JSON token 중간에서도 나눌 수 있다. SDK는 E까지 전체를 합쳐 엄격한 UTF-8·JSON·타입·수치 범위·행 크기를 검증한 뒤 이벤트를 반환한다.

이벤트 JSON은 `{"version":1,"result":null|성공객체,"diagnostic":null|진단객체,"stop":bool,"health":"healthy|stopped_cleanup|recovery_required"}`다. 성공객체·진단객체는 기존 CLI JSONL 필드를 재사용하며 INT64·문장번호·건수는 십진 문자열이다. SQL operation은 run이다. 성공+cleanup 진단을 한 이벤트에 함께 담아 반영 상태를 보존한다.

한 이벤트 JSON을 전부 준비하고 상한을 확인한 뒤 B를 보낸다. byte chunk는 행 단위 결과 포맷을 새로 만들지 않는다. SDK가 불완전한 결과를 성공 이벤트로 반환하지 않는다. 기본 D 목표는 `min(64 KiB, frame_bytes)`이며 하나의 TEXT도 임의 바이트 chunk로 분할된다.

## 4. reactor·작업 큐·동기화

단일 reactor가 accept/receive/send/기한/연결 상태를 소유한다. Winsock select 기반으로 시작하며 설정 연결 수+listener+worker 완료 깨우기 socket의 fd 집합 용량을 시작 시 확인한다. 비차단 socket을 쓰며 reactor에서 SQL을 실행하지 않는다. worker 완료 queue와 깨우기 socket으로 유휴 대기를 깨운다.

완전한 Q를 검증한 요청만 SQL queue에 들어간다. 큐 항목은 요청당 다음 문장 실행 준비를 나타내며 요청은 같은 큐에 중복 등록되지 않는다. pending은 이 대기 항목 수이며 실행 중·송신 중 항목은 제외한다. FIFO로 worker가 가져가고 한 번에 `md_request_next` 한 번을 실행한다.

서버 잠금이 request_open과 각 next의 엔진 접근을 직렬화한다. 다른 요청 사이에 문장 끼어들기를 허용한다. `md_request_next`에서 매 호출 시작 시 DB health를 확인하도록 보강한다. 같은 DB 직렬 호출 계약은 유지하며 공개 health helper는 추가하지 않는다. 요청 open 후 타 요청이 DB를 중단 상태로 바꿔도 후속 SQL을 차단한다.

완성 이벤트는 worker→완료 queue→reactor로 단일 소유권 이전한다. JSON 준비와 socket 대기는 엔진 잠금 밖이다. 이벤트 E 전송이 끝나면 후속 문장을 다시 큐에 넣는다. ACK 없이 전송 완료만 확인한다. EOF를 next로 확인하면 F를 보내고 idle로 돌아간다.

각 연결에는 결과 이벤트 하나만 대기시킨다. 완료 queue 항목은 실행 중 worker당 하나로 제한하고 worker는 완료 인수 전 같은 slot에 다시 쓰지 않는다. 닫힌 연결도 실행 job 완료까지 요청 context를 보존한다. reactor만 socket을 닫으며 stale 완료는 세대 식별자로 거절하고 이벤트를 한 번 해제한다.

큐/lock 기한 초과·연결 종료·종료 요청은 아직 실행되지 않은 항목을 취소한다. worker는 엔진 잠금 취득 뒤 취소/health를 다시 확인한다. 큐 포화는 현재 문장을 시작하지 않고 작은 오류 이벤트+F로 요청을 중단한다. 앞선 성공 이벤트는 유지한다.

Ctrl+C handler는 종료 flag와 깨우기만 수행한다. reactor는 신규 접수·새 SQL 등록을 중단하고 idle/수신/대기 요청을 정리한다. 현재 실행 SQL은 저장 정합성 처리를 마친 뒤 후속 문장을 중단하고 가능한 결과+F를 송신 기한 안에 보낸다. worker join·이벤트 해제 뒤 DB를 닫는다. SQL 실행 중 강제 스레드 종료는 사용하지 않는다.

## 5. 실패·출력·반영 상태

연결 종료·송신 실패를 감지한 뒤 후속 SQL을 실행하지 않는다. 감지 전 DML은 반영될 수 있으며 socket 전송 완료는 원격 stdout 성공 확인을 뜻하지 않는다. remote CLI 출력 실패는 연결을 닫고 로컬 진단을 남기지만 서버가 이미 시작한 SQL의 중단을 보장하지 않는다.

SDK는 받은 완성 이벤트의 확정 committed/affected_rows/cleanup 정보를 유지한다. Q 제출을 시작한 뒤 확정 완료를 받지 못하면 후속 반영 가능성을 미확인으로 표시한다. 확인하지 못한 문장에 committed=false를 만들거나 SQL을 재전송하지 않는다. 요청이 서버에 도착하지 않았다고 확정할 수 있는 connect 실패만 미제출로 표시한다.

운영 오류 code는 PROTOCOL_ERROR, SERVER_BUSY, LIMIT_EXCEEDED, TIMEOUT, SERVER_STOPPING을 사용하고 stage를 protocol/queue/lock/receive/send/shutdown으로 구분한다. 엔진 SQL·저장 진단은 기존 코드를 보존한다. 예기치 못한 I/O·할당 실패는 IO_ERROR·RESOURCE_ERROR로 구분한다.

종료 코드 세부 제안은 F.exit_code에 기존 의미 0/4/5/6을 재사용하고 서버 운영 오류와 remote 자체 전송 실패에 8, remote 출력 실패에 7을 사용한다. 수신해 확정한 상태의 우선순위는 6>5>8>7>4>3>2>0이며 각 진단을 보존한다. 서버 준비 실패는 3, 인자 오류는 2다. F가 없으면 전체 성공으로 처리하지 않는다.

응답 event/total 상한 초과·직렬화 자원 실패는 현재 결과 전송을 시작하기 전에 작은 실패 이벤트로 바꾸고 후속 SQL을 중단한다. DML 반영 확정이면 성공·건수와 cleanup 진단을 유지하면서 별도 전달 실패를 알린다. 긴 메시지는 고정 fallback에서 축약할 수 있지만 반영·복원·건강 상태를 누락하지 않는다. fallback도 전달 불가하면 연결을 닫고 SDK의 반영 미확인 규칙을 적용한다.

## 6. 설정·카운팅·초기값

모든 옵션은 각각 설정한다. 자원 값은 십진 정수 바이트/개수, 시간은 십진 정수 ms다. 음수·기호·overflow·중복 옵션·미지 옵션은 인자 오류다. 시간 0은 비활성이고 SQL timeout은 M12에서 0만 허용한다. 각 enabled 기한은 단조 시계의 전체 구간 기한이며 바이트 도착마다 연장하지 않는다.

| 서버 옵션 | 기본값 | 구간/계수 |
|---|---:|---|
| --max-connections | 32 | accept된 열린 client socket 수 |
| --workers | 4 | SQL worker 수 |
| --pending-jobs | 16 | FIFO 대기 SQL 항목 수 |
| --request-bytes | 1048576 | Q payload 전체 JSON 바이트 |
| --frame-bytes | 1048576 | 프레임 하나의 payload 바이트 |
| --event-bytes | 67108864 | B/E로 감싼 전체 이벤트 JSON 바이트 |
| --response-bytes | 268435456 | 요청별 모든 B/D/E/F payload 바이트의 합 |
| --queue-timeout-ms | 30000 | 등록부터 worker 인수까지, 재등록마다 새 구간 |
| --lock-timeout-ms | 30000 | worker 인수 후 엔진 잠금 취득까지 |
| --receive-timeout-ms | 30000 | Q 첫 header 바이트부터 전체 Q 완성까지 |
| --send-timeout-ms | 30000 | B 시작부터 해당 E 완료까지, F는 별도 구간 |
| --sql-timeout-ms | 0 | M13 연계. M12 비제로는 인자 오류 |
| --idle-timeout-ms | 60000 | accept 또는 F 완료부터 다음 Q 첫 바이트까지 |

SDK connect 기본 5000 ms, receive-frame 기본 30000 ms, send-request 기본 30000 ms다. SDK의 frame/event/response 상한은 같은 기본값과 개별 옵션을 제공하며 서버와 독립적으로 더 작은 제한을 적용할 수 있다. SDK receive는 첫 바이트를 기다리는 시간을 포함해 next의 다음 프레임 대기 구간을 제한하므로 장시간 SQL에서는 사용자가 늘리거나 0으로 비활성화한다.

request_bytes<=frame_bytes<=UINT32_MAX, frame_bytes>=4096, event_bytes>=4096, response_bytes>=16384를 시작 시 검사한다. 작은 제어·실패 이벤트+F를 위해 response 한도 안에 16 KiB를 예약하고 정상 이벤트는 예약을 침범하지 않는다. B/D/E 전체 payload 예산을 overflow 없이 사전 계산한다. event·total 상한은 함께 적용하며 서로 큰 값의 설정을 금지할 필요는 없다.

worker/pending과 connection 개수에 임의 대소 제약을 두지 않는다. 배열·합산·타이머 단위 변환은 SIZE_MAX/정수 범위를 검사하고 할당 실패를 진단한다. 이 값들은 reactor·유한 큐·한 이벤트 수명에 맞춘 초기 운영값이며 측정에 근거한 최적값은 아니다.

chunk는 송신 frame을 제한한다. SQL 엔진 전체 결과·이벤트 직렬화·SDK 재조립은 별도 메모리를 사용하므로 event cap이 프로세스 전체 메모리 상한을 뜻하지 않는다. 기능·M13 완성 뒤 동시 연결·처리량·p95·메모리·큐 대기 측정으로 조정한다.

## 7. 핵심 Given / When / Then

아래는 미래 구현 검증 명세이며 이번 설계 run의 실제 DBMS 검사 결과가 아니다.

| ID | Given | When | Then |
|---|---|---|---|
| M12-WIRE-01 | 정상 Q/응답 | 모든 header 경계·합쳐진 frames·부분 send | 정확한 요청·이벤트 하나로 복원 |
| M12-WIRE-02 | 길이/버전/종류/필드 오류·중간 EOF | 수신 | SQL 미실행 또는 후속 중단·연결 폐기 |
| M12-CHUNK-01 | 긴 TEXT·최대 INT64·NULL·UTF-8/escape | token 중간 D 분할 | 전체 JSON 재조립 후 원값·메타데이터 일치 |
| M12-CHUNK-02 | bytes 불일치·중복 index·E/F 누락 | SDK next | 불완전 이벤트 미반환·전송/프로토콜 오류 |
| M12-SQL-01 | 성공 DML→오류→DML | 복수 SQL | 첫 성공 유지·마지막 미실행·오류+F |
| M12-SDK-01 | 지속 연결 | DONE 전 execute·DONE 후 execute·중간 close | 순서 오류/재사용/중단의 계약 준수 |
| M12-POOL-01 | 유휴/느린 연결과 동시 요청 | pool 실행 | reactor 유휴 대기와 SQL job 분리·엔진 중첩0 |
| M12-QUEUE-01 | worker/queue 포화 | 새 요청·문장 재등록 | 상한 준수·현재 미시작 SQL 중단·앞선 반영 보존 |
| M12-HEALTH-01 | 열린 요청 A·중단 상태를 만든 B | A 다음 문장 | health 재확인·후속 SQL 미실행 |
| M12-OWN-01 | job 실행 중 연결 close | stale 완료·종료 경합 | 조기 해제/이중 해제/다른 연결 오배달 없음 |
| M12-DISCONNECT-01 | DML 직전/반영 직후/송신 중 끊김 | 종료 감지 | 감지 후 후속 중단·확정/미확인 상태 정확 |
| M12-STATE-01 | cleanup 실패·rollback 실패 | 원격 실행 | 성공+진단/건강 상태·종료 우선순위 보존 |
| M12-LIMIT-01 | request/frame/event/total 경계·overflow | 경계-1/동일/+1·설정 오류 | 정확 계수·사전 거절·DML 확정 보존 |
| M12-CONFIG-01 | 독립 자원/시간·0·잘못된 값 | 서버/SDK 시작 | 기본값/단위·관계검사·SQL 비제로 거절 |
| M12-TIME-01 | stalled queue/lock/partial frame/send/idle | 각 기한 경계 | 해당 stage timeout·실행 중 SQL 강제 종료 없음 |
| M12-SHUTDOWN-01 | idle·수신·큐·실행·송신 연결 | Ctrl+C | idle 깨우기·대기 취소·현재 SQL 정합성·join 후 close |
| M12-CLI-01 | 실제 server+SDK CLI·표/JSONL·stdout 실패 | 파일/stdin 실행 | 기존 결과 의미·원격 ACK 생략·오류 상태 유지 |
| M12-REGRESSION-01 | 고정 소스 | 새 configure/build·전체 CTest | 기존158+추가검사·작성자/독립 QA 각각 통과 |

## 8. 구현 순서와 검토 부록

사용자 구현 승인 후 프레임/SDK 실패·캡 경계의 실제 Red→작은 Green, reactor/job 수명·health·저장 상태의 실제 Red→Green, 정상 입구 통합·리팩터링·전체 회귀 순으로 진행한다. concurrency/ownership/overflow/storage 상태/bugfix는 고위험 실제 Red 대상이며 환경/import/컴파일 실패를 기능 Red로 삼지 않는다.

후보 파일은 CMakeLists.txt, scripts/c_checks.py, include/minidb_client.h, src/server.c, src/client.c, src/remote_cli.c, src/protocol.c, src/protocol.h, src/format.c, src/format.h, src/result.c, src/engine.c, src/cli.c, tests/unit_m12.c, tests/m12_remote.py다. 기존 정적 엔진·yyjson을 재사용하고 format/result 공통 코드로 JSON·표·해제를 공유한다. 각 추가 파일은 승인 시 정확 목록으로 고정한다.

이번 설계의 입력·근거·원문 초안은 [Planner JSON](../.cache/m12-design/planner-result.json), 원문 대조·문서 마감 근거는 [Knowledge JSON](../.cache/m12-design/knowledge-result.json)에 둔다. 독립 review/qa와 docs 검사의 실제 근거는 [설계 실행 원장](../.cache/agent-harness/m12-design-20261118/)의 결과 JSON을 따른다.

M13은 SQL read-path 공유 상태·락 분류·협력 취소의 연계만 남긴다. 100만 Debug DB open 118.711853초의 원인·비정상 여부는 미진단이며 전체 구현 마감 뒤 개선 후보 판단으로 보류한다. 기존 [M11 결과](testing/m11-results.md)의 실행 조건·한계를 유지한다.
