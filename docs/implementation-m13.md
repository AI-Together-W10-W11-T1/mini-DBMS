# M13 공유 읽기·배타 쓰기·SQL 협력 취소 설계 계약

2026-11-19 목요일(Asia/Seoul). 사용자 all-A 선택을 반영한 설계·인수 명세 초안이다.
현재 승인은 문서 작성·검토까지다. 검토한 구체 구현 범위는 후속 사용자 승인으로 확정한다.
함수명·옵션 구조체·wire 세부는 선택한 동작을 구현할 상세 제안으로 검토한다.

## 1. 범위와 선택 출처

M13은 Windows C23의 DB 단위 공유 읽기·배타 쓰기와 실행 중 협력 취소를 다룬다.
현재 사용자 선택 1A는 대기 writer가 새 reader 진입을 막는 admission이다.
선택 2A는 `--sql-timeout-ms` 기본0을 유지하고 양수로 문장 실행 기한을 활성화한다.
선택 3A는 disconnect/shutdown 감지 뒤 조회·변경 전 준비를 안전 지점에서 중단한다.
저장 보호구간에 들어간 문장은 commit 또는 필요한 rollback·cleanup·publication을 완료한다.
SQL·스키마·저장·로컬 CLI 의미와 문장 원자성·첫 오류 중단·앞선 성공 유지를 승계한다.
[SQL](sql-contract.md)·[스키마](schema-contract.md)·[저장](storage-contract.md)·[CLI](cli-contract.md)를 의미의 원본으로 사용한다.
[M12 설계](implementation-m12.md)와 [M12 결과](testing/m12-results.md)의 framing·SDK·reactor·수명을 재사용한다.

## 2. 실행 입구와 DB gate 제안

`MdDb` 내부에 하나의 reader/writer gate를 두어 health·tables·rows·index를 같은 규칙으로 보호한다.
gate 상태는 active readers·active writer·waiting writers와 동기화 primitive로 구성하는 작은 안을 권고한다.
SELECT·VALUES와 SELECT/VALUES 기반 UNION 전체는 READ, INSERT·UPDATE·DELETE는 WRITE로 분류한다.
READ는 writer가 없고 대기 writer도 없을 때 진입한다. WRITE는 active reader/writer가 모두 없을 때 진입한다.
writer 대기 등록부터 새 reader를 막고 취득·취소·기한 만료 시 대기 계수를 정확히 되돌려 깨운다.
동일 상태 잠금 안에서 등록·입장·해제·깨우기를 처리해 취소와 입장 경합의 계수 누수를 막는다.
strict FIFO·모든 요청의 기아 방지·절대 대기시간은 보장 범위에 포함하지 않는다. 지속 writer 유입 시 reader가 지연될 수 있다.

공개 API는 `md_request_next_ex(request, execution_options, event)` 추가를 권고한다.
execution_options는 문장 lock budget·SQL 기한 설정·관찰 전용 취소 callback/context를 전달하는 제안이다.
기존 `md_request_next`는 실행 제어 비활성 옵션으로 같은 경로를 호출해 기존 직렬 caller의 동작을 보존한다.
서로 다른 request의 같은 DB 병렬 next를 지원하고 같은 request의 동시 next/close는 호출자가 금지한다.
request_open의 health 읽기도 보호하며 DB close는 모든 request·worker 종료 뒤 호출한다.
서버의 바깥 global `engine_lock`은 제거하고 request_open/next_ex/close 수명과 gate 보호를 연결한다.
reactor의 socket 소유권·executing context·세대 검사·worker 완료 handoff는 기존 규칙을 따른다.

## 3. 문장 준비·분류·소유권

next 시작의 짧은 shared health 확인으로 stopped DB에서 입력을 새로 소비하지 않는 기존 계약을 보존한다.
초기 확인 뒤 준비 중 다른 writer가 health를 중단 상태로 바꾸면 최종 gate에서 후속 실행을 차단한다. 이미 소비한 요청 입력의 복원은 보장하지 않는다.
이후 요청 소유 입력 scanner와 기존 Lexer 첫 token으로 문장을 준비·분류한다.
BOM·주석·문자열 세미콜론·빈 문장·원래 위치·첫 오류 중단을 기존 scanner/lexer에서 재사용한다.
이 단계는 DB 메타데이터·행·index를 읽지 않으며 raw split·prefix 비교로 SQL을 별도 해석하지 않는다.
미지원 시작 token·어휘/종료 오류는 부작용 없는 오류로 처리한다.
DML 시작 token의 문법 오류는 WRITE gate 안에서 기존 전체 검증으로 거절한다.
분류한 최종 gate를 취득한 뒤 health를 재검사하고 metadata 의존 의미 검증과 실행을 수행한다.
READ에서 WRITE로 암묵 upgrade하지 않으며 검증 중 사용하는 요청 복제·scratch는 request/call 소유로 유지한다.

READ gate는 scan·JOIN·집계·정렬·UNION과 최종 결과의 소유 복사 완료까지 유지한다.
SELECT scratch가 빌린 table TEXT·index 참조는 gate 해제 전에 모두 소유 결과로 복사하거나 폐기한다.
WRITE gate는 staging부터 저장 변경·rollback·cleanup·health 확정·메모리/index publication까지 유지한다.
완성 MdEvent는 DB/request 종료 뒤에도 유효하며 JSON 직렬화·socket 송신은 gate 해제 뒤 수행한다.
부분 조회 결과는 취소 시 폐기하고 임시 메모리·이벤트는 각 소유자가 한 번 해제한다.
기존 조사에서 production read-query의 mutable global race는 확인되지 않았다. 정적 조사 범위의 관찰이다.
`MINIDB_TEST_FAULTS` SQL 계수와 `MD_BT_TEST` 할당/live 계수는 조건부 시험 상태다.
병렬 시험에서 해당 계수를 동기화하거나 요청 소유로 바꾸고 기존 집계·fault 의미를 보존한다.

## 4. 세 기한과 안전 지점

queue 기한은 각 등록부터 worker 인수까지이며 후속 문장 재등록은 새 queue 구간이다.
lock 기한은 문장별 하나의 budget으로 실제 gate 대기 시간을 계수하는 안을 권고한다.
준비 전 health gate와 최종 실행 gate가 같은 budget을 공유해 재취득마다 허용 대기를 연장하지 않는다.
scanner/classify 시간은 lock 대기 시간에서 제외하고 준비 전·후 사용한 대기 시간을 차감한다.
SQL 기한은 최종 문장 gate 취득 뒤 의미 검증·실행 시작에서 단조 시계로 시작한다.
request-local 준비·분류는 SQL 실행 기한 구간에 포함하지 않는다.
기본0은 비활성이며 양수는 협력 기한이다. 단위 변환·누적 대기·시각 계산의 overflow를 검사한다.
각 활성 기한은 진행에 따라 갱신하지 않는다. 준비 중에도 disconnect/shutdown 취소를 주기적으로 확인한다.
scanner의 긴 token·주석·문자열 순회는 준비/실행 구간에 맞는 실행 제어를 전달받는다.
최종 gate 이후 lexer·정적 검증·긴 VALUES 파싱에는 SQL deadline 확인을 적용한다.
scan·JOIN 내부곱·긴 predicate/TEXT 비교·집계 group 탐색·projection/TEXT 복사에 안전 지점을 둔다.
heap build/sift/extract·UNION merge/sort/dedup·페이지 결과 정리도 긴 내부 루프까지 확인한다.
INSERT 중복/타입 검사·예약과 UPDATE/DELETE staged rows·DELETE 대체 index 준비를 확인한다.
저장 bytes 직렬화·예약 등 변경 전 긴 준비에도 확인하고 실패 시 원본·health를 보존한다.
단일 OS I/O·할당 호출의 즉시 중단이나 정확한 wall-clock 반환 시각을 약속하지 않는다.

## 5. 저장 경계·종료·확정 상태

첫 marker 영속 변경 직전에 마지막 취소를 확인하고 이후를 저장 보호구간으로 둔다.
보호구간 전 취소는 변경을 시작하지 않고 staging을 정리하며 committed=false를 유지한다.
marker_write 시작부터 필요한 rollback·commit·cleanup·메모리/index publication 완료까지 취소를 유예한다.
INSERT의 성공 write+flush와 UPDATE/DELETE의 성공 ReplaceFileW가 기존 반영 경계다.
보호구간 중 기한 초과·disconnect/shutdown에도 해당 문장의 정합성 처리를 끝낸다.
반영 성공을 timeout의 committed=false로 바꾸지 않으며 성공·affected_rows·cleanup 진단·health를 보존한다.
실제 저장 오류가 발생하면 기존 rollback 확정/실패/미확인 상태와 cleanup5·recovery6 우선순위를 따른다.
반영 확정 이후 감지한 취소는 후속 SQL을 중단하며 앞선 성공과 현재 확정 성공을 유지한다.
disconnect는 기존 Interlocked flag를 worker 수명 내 callback으로 관찰한다.
shutdown은 신규 접수·새 SQL 등록을 멈추고 조회/변경 전 준비를 안전 지점에서 취소한다.
보호구간 정리와 worker 완료 뒤 request/context/event를 해제하고 join 뒤 DB를 닫는다.
닫힌 연결의 stale 완료는 세대로 거절하고 소유 결과를 한 번 해제한다.

## 6. 오류·SDK 호환 상세 제안

실행 SQL deadline은 기존 TIMEOUT과 operational exit8, `stage=sql` 확장을 권고한다.
lock timeout은 stage=lock, queue timeout은 stage=queue이며 SQL 실행 기한과 구분한다.
shutdown은 SERVER_STOPPING/stage=shutdown을 유지하고 닫힌 연결에는 전송 완료를 전제하지 않는다.
SQL deadline으로 중단한 조회는 부분 성공 결과 없이 진단과 후속 중단을 전달한다.
반영 성공 문장의 취소는 확정 성공을 보존한 채 요청 중단 상태를 전달하도록 event/F 처리를 함께 검토한다.
신규 SDK의 strict stage whitelist·server encoder·SDK decoder·종료 코드 처리를 함께 갱신한다.
구 SDK는 미지원 sql stage를 protocol failure로 거절하고 이미 확정한 이벤트를 보존한다.
현재 `mp_event_decode`의 stage 허용 목록에는 sql이 없으며 실패하면 부분 이벤트를 해제한다. `mdc_next`는 이를 PROTOCOL_ERROR·MDC_ERROR로 반환하고 연결을 폐기한다.
구 SDK가 확인하지 못한 후속 반영은 미확인으로 남기며 자동 재전송하거나 committed=false를 만들지 않는다.
기존 stage와 정상 응답은 호환 회귀로 검사한다. v1의 모든 조합이 완전 상호호환된다고 주장하지 않는다.
새 네트워크 cancel 명령·client cancel API·ACK는 추가 범위로 채택하지 않는다.

## 7. 미래 Given / When / Then

아래 14개는 구현 승인 뒤 실제 검사할 명세다. 이번 설계 run의 실제 runtime 검사는 0회다.

| ID | Given | When | Then |
|---|---|---|---|
| M13-READ-01 | 서로 다른 SELECT/VALUES/UNION 요청 | gate 안 READ A를 barrier로 고정하고 B 진입 | active readers>1의 사건 기록과 serial oracle 결과 일치 |
| M13-RW-01 | TEXT/index를 빌린 reader와 DML | INSERT/UPDATE/DELETE를 경합 | reader/writer 중첩0·소유 복사 완료·원자 publication |
| M13-WW-01 | 두 DML 요청 | WRITE A 보유 중 B 진입 시도 | writer 중첩0·행/index/파일 일치 |
| M13-ADMIT-01 | active reader A·waiting writer B | 후속 reader C 등록 후 A 해제 | C는 B 대기 동안 진입하지 않음·B 해제 뒤 진행 |
| M13-WAIT-01 | health/실행 gate의 대기자 | 취소·기한 경계·재취득 | 계수 복원·깨우기·한 문장 lock budget 유지 |
| M13-CLASSIFY-01 | BOM/주석/문자열/빈 문장·혼합 SQL | prepare/classify·전체 검증 | READ/WRITE·위치·첫 오류·앞선 성공 일치 |
| M13-HEALTH-01 | 초기 health 확인 때 이미 stopped 또는 준비 중 writer가 stopped로 변경 | 다음 next 또는 최종 gate 취득 | 초기 stopped면 새 입력 차단·준비 중 변경이면 이미 소비한 입력을 유지하고 후속 SQL 실행 차단 |
| M13-TIME-01 | 기본0·양수·overflow 설정과 제어 시계 | queue/prepare/lock/실행 각각 지연 | 0 비활성·기한 시작점/단위/상한 정확·준비 취소 가능 |
| M13-LOOP-01 | 긴 lexer/scan/JOIN/TEXT/group/sort/UNION/VALUES | 각 내부 루프 clock/flag 주입 | 안전 지점 중단·부분 결과 폐기·정확한 해제 |
| M13-PREP-01 | DML staging/index/직렬화 준비 | marker 전 취소·기한 | committed=false·파일/행/index/health 원본 보존 |
| M13-COMMIT-01 | marker 전후·flush/replace 전후·cleanup fault | 기한/종료 주입 | 정합성 완료·확정 건수/rollback/health·6>5 우선 보존 |
| M13-LIFE-01 | 실행 중 disconnect/shutdown | 취소·stale 완료·join 경합 | 보호구간 완료·후속 중단·조기/이중 해제0·join 뒤 close |
| M13-WIRE-01 | 신규/구 strict SDK와 SQL timeout·반영 완료 | event/F 전송 또는 중간 단절 | sql stage 처리·구 SDK 안전 failure·확정/미확인 상태 정확 |
| M13-REGRESSION-01 | 고정 소스·기존 serial caller/CLI·조건부 계수 | 작성자/독립 QA 새 configure/build/전체 CTest | 기존176+M13 추가 회귀·기존 결과/수명/계수 의미 보존 |

## 8. 구현 순서·후보 파일·문서 마감

구현 승인 뒤 concurrency·ownership·overflow·storage 상태의 실제 기능 Red→최소 Green→리팩터링을 수행한다.
barrier·사건 순서·결정적 clock/fault와 결과 oracle로 증명하고 단순 경과시간을 읽기 중첩 증거로 쓰지 않는다.
후보 수정은 include/minidb.h, src/internal.h, src/engine.c, src/sql.c, src/storage.c, src/server.c, src/protocol.c다.
SDK 연동 후보는 src/client.c이며 시험 계수 감사 결과에 따라 src/bptree.c를 포함한다.
추가 검사 후보는 tests/unit_m13.c·tests/m13_remote.py이며 CMakeLists.txt·scripts/c_checks.py에 연결한다.
기존 M12 기대값의 M13 변경 대상은 tests/unit_m12.c·tests/m12_remote.py와 함께 정확 목록으로 검토한다.
현재 문서 산출물은 이 문서·HANDOFF.md·docs/pending-decisions.md·docs/wiki/index.md·docs/wiki/sources.json이다.
원문 계약은 별도로 수정하지 않으며 Knowledge 담당이 공개 문서 원문 대조·등록·docs 검사와 JSON 근거를 남긴다.
검토·사용자 구현 승인 뒤 실제 검사하며 최종 사용자 확인 뒤 완료 처리한다.
성능 측정·초기값 조정은 기능/M13 완료 뒤, 100만 행 startup 진단은 전체 구현 마감 뒤 후보로 유지한다.
MVCC·WAL·새 의존성·추가 유료 API와 Phase 1 전체204 감사는 이번 범위에 포함하지 않는다.

현재 소스의 health 확인·서버 직렬 잠금·저장 반영 경계·strict SDK 동작은 [엔진](../src/engine.c)·[서버](../src/server.c)·[저장](../src/storage.c)·[프로토콜](../src/protocol.c)·[클라이언트](../src/client.c)와 대조했다.
선택·초안 근거는 [계약 JSON](../.cache/m13-design/contract.json)·[Planner JSON](../.cache/m13-design/planner-result.json), 문서 마감 근거는 [Knowledge JSON](../.cache/m13-design/knowledge-result.json)에 둔다. 독립 검토·문서 검사의 실제 근거는 [설계 실행 원장](../.cache/agent-harness/m13-design-20261118/)을 따른다.
