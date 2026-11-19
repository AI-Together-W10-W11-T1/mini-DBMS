# M14 서버 성능 측정 계약

2026-11-19 목요일(Asia/Seoul). 요구사항 DBMS-014. 사용자 `a`가 선택한 A 범위와 독립 설계 검토 조건을 정리했다. 실제 구현·검사·측정은 [단일 결과](testing/m14-results.md)에서 구분한다. 결정 기계 원본은 [계약 JSON](../.cache/m14/contract.json), 사전 검토는 [설계 검토 JSON](../.cache/m14/design-review.json)이다.

## 범위와 실행 조건

실제 C SDK로 서버에 지속 연결 부하를 보내 처리량·p50/p95 응답시간·오류율·서버 메모리·직접 큐 대기를 측정한다. 엔진·SDK 공개 함수·wire 의미와 기존 서버 기본값을 유지한다. loopback, 최대 연결32·워커4·대기16을 사용한다. DB gate 잠금 계측·설정 조합 탐색·자동 튜닝·시작 비용 진단·CIM·새 의존성·새 Goal은 범위 밖이다.

기본 전체 실행은 10,000행 × 동시 연결1/4/16/32 × ID 조회/비인덱스 scan_key 조회/혼합95%읽기·5%UPDATE × 반복3의 36개 case다. client당 warmup20회 뒤 measured40회를 수행한다. 수치는 재현 가능한 짧은 실행 기본값이며 명시 옵션으로 변경할 수 있다. 각 case/repeat는 독립 서버와 격리된 fixture DB를 사용한다. CTest의 작은 smoke 실행은 전체36case 측정과 구분한다.

client마다 SDK 객체1개·스레드1개·outstanding 요청1개를 사용하며 한 요청은 SQL1문장이다. 모든 client가 warmup의 이벤트와 DONE을 소비한 뒤 공통 barrier에서 측정을 시작한다. 연결 실패를 숨기거나 자동 재전송하지 않는다.

## 결과 정확성과 실패

성공 이벤트의 문장 번호·열 이름/타입·행수·ID/scan_key/payload 또는 UPDATE affected_rows=1을 검증하며 이벤트 소유권은 한 번 해제한다. 혼합 부하는 각 client의 고유 ID를 읽고 수정한다. 매20회19읽기/1수정을 계획하고, phase/수정 순번이 드러나는 결정적 payload marker를 사용한다. expected payload는 반영이 확인된 수정에 대해서만 바꾼다. warmup 수정은 측정 이전 baseline에 포함한다.

계획/시도/미시도·건너뜀, 성공/확정 실패/전송 실패와 반영 미확인, 계획한95/5와 실제 시도 비율을 구분한다. 성공 이벤트 뒤 전송 실패가 발생해도 이미 확인한 반영 사실을 유지한다. 반영 미확인 UPDATE를 추정하거나 재실행하지 않으며 최종 정합성 미확인으로 남긴다. 종료 후 모든 행의 ID·scan_key·payload·행수를 독립 oracle로 검증한다.

`valid/complete`와 `all_requests_succeeded`를 분리한다. SERVER_BUSY/TIMEOUT도 보존해야 할 측정 결과이며, 모든 시도·trace·최종 DB 상태가 검증됐다면 오류가 포함된 유효한 측정이 가능하다. 표본 누락·불완전 trace·메모리 조회 실패·반영 미확인을 전체 성공으로 표시하지 않는다.

## 시간·큐·메모리 측정

- QPC frequency·원시 ticks·ns를 보존한다. 음수·역행·변환 overflow와 필드 타입 불일치를 거절한다. 같은 호스트의 server/client QPC 기준을 대조한다.
- 요청 응답시간은 `mdc_execute` 직전부터 이벤트와 `MDC_DONE` 소비까지이며 SDK 송수신·재조립·이벤트 검증을 포함한다. 연결·DB open·warmup은 제외한다. 성공 p50/p95는 원시 성공 표본의 nearest-rank로 계산하며 실패 표본과 경과시간도 보존한다.
- 처리량은 공통 측정 시작부터 마지막 measured 요청 완료까지의 실제 경과시간당 정상 완료 요청 수다. client별 시간 합을 분모로 사용하지 않으며 thread join·연결 close는 측정 밖이다.
- 서버 `--queue-metrics PATH`는 기본 비활성인 로컬 계측이다. enqueue부터 worker dequeue까지 별도 QPC로 직접 기록하며 기존 timeout 시계를 바꾸지 않는다. connection-generation·request/job identity, enqueue/dequeue 시각과 SQL event/EOF/error 결과를 보존한다. 공통 측정 창으로 warmup·준비·최종 oracle 작업을 제외하고 SQL/event와 EOF 집계를 구분한다. dequeued sample과 큐 제거·거절을 구분하며 enqueue/dequeue/remove/reject/overflow 회계를 남긴다. dequeue 없는 취소에0대기를 합성하지 않는다.
- 계측은 사전 할당한65,536개 bounded buffer를 사용하고 큐 동기화 경계에서 기록한다. 매 작업 파일 I/O를 피하고 worker join 후 flush한다. 출력 파일은 덮어쓰지 않으며 allocation/QPC/overflow/flush 실패는 trace 불완전으로 처리한다. 큐 대기를 DB gate 대기로 해석하지 않는다.
- 서버 PID의 `GetProcessMemoryInfo/PROCESS_MEMORY_COUNTERS_EX`로 working set/private bytes를 약10ms 주기와 양끝에서 관측한다. 실제 QPC 시각·샘플 수·간격·조회 실패와 관측 최대값을 보존한다. 경계 관측이 측정 창 밖이면 별도로 표시한다. 관측 최대값을 절대 순간 peak나 누수 증명으로 확대하지 않는다.

fresh 프로젝트 `.cache` 목적지에 native 원시 보고서·queue trace·요약·로그·도구/빌드 조건·해시를 남긴다. Python 검증기는 표본 개수·타입·합계·시간 변환·통계를 독립 재계산한다. 큐 작업을 `(generation, request)`별로 묶어 event index 연속성과 SQL/EOF·오류·제거·거절 종료 형태를 검사하고, warmup/실측 단계별 요청 수·성공 EOF·오류 수를 SDK 시도 결과와 교차 대조한다. SQL 성공 뒤 EOF 등록 거절도 오류 요청으로 보존한다. 중도 실패도 원인과 완료 범위를 남긴다. Debug·loopback·짧은 fixed-count 부하·계측 비용·같은 호스트 CPU 경합의 한계를 명시하며 통계적 유의성이나 장시간 steady-state를 주장하지 않는다.

## Given/When/Then

| ID | Given / When | Then |
|---|---|---|
| M14-SDK-01 | 복수 지속 SDK 연결 / warmup barrier 뒤 동시 요청 | 결과·이벤트·DONE·소유 해제와 모든 표본 개수/oracle 일치 |
| M14-QUEUE-01 | 제어한 큐 등록/인수·EOF·제거/거절 / 계측과 측정 창 필터 | 직접 QPC 차이·identity·분류·회계 정확, warmup 제외, 계측 손실은 불완전 |
| M14-STATISTICS-01 | 알려진 ticks/빈도·성공/실패 표본 / 통계와 손상 보고서 검증 | ns·nearest-rank p95·공통 경과 처리량 정확, 타입/overflow/불일치 거절 |
| M14-MIXED-01 | client별 ID·95/5 계획 / warmup·실측 뒤 전체 DB 검증 | 반영 확인/실패/미확인 분리, 최종 marker·행수·ID·scan_key 일치 |
| M14-FAILURE-01 | busy/timeout·fake-peer 단절·서버 중단 / SDK 부하 | 시도·실패·미확인·미시도와 부분 결과 보존, 유효성과 전요청 성공 분리 |
| M14-MEMORY-01 | 실제 서버 PID / 측정 경계와 주기 관측 | 실제 시각·횟수·working set/private bytes 보존, 조회 실패 은폐 없음 |
| M14-OUTPUT-01 | 잘못된 인수·기존 목적지·손상 trace·출력 실패 / 실행·수집 | 기존 파일 보존, 명확한 비성공과 원인 기록 |
| M14-REGRESSION-01 | 계측 비활성/활성 동일 요청 / 실제 원격 smoke·최종 회귀 | 기존 wire·기본값·SQL·종료 의미 보존, 전체198개 누락/skip/실패 없음 |

## 구현·검증 경계

기술 변경은 `tools/bench_m14.c`, `scripts/m14_benchmark.py`, `tests/m14_benchmark.py`, `src/server.c`, `CMakeLists.txt`, `scripts/c_checks.py`의6파일이다. 동시성·소유권·overflow 위험에는 실제 기능 Red를 요구한다. 컴파일/import/소켓 권한 등 환경 실패는 기능 Red가 아니다.

final-once-v1에 따라 작성자는 실제 configure/build와 필요한 부분 검사를 수행한다. 독립 Reviewer가 코드 검토·필요 수정 이후 고정 최종 소스에서 기존190+M14 8개인198개 전체 회귀를 한 번 수행한다. 실패나 후속 수정에 필요한 재검사는 허용한다. 이는 검증 요구이며 실제 통과 선언이 아니다. 실제 측정·검사·실패·한계는 [단일 결과](testing/m14-results.md)에 기록하며, 사용자 최종 확인 뒤 완료 처리한다.
