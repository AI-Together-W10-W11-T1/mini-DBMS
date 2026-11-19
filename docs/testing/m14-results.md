# M14 구현·측정 결과

2026-11-19 목요일(Asia/Seoul). DBMS-014, run `m14-native-20261119`. 사용자 A 선택과 로컬 TCP 실행 승인에 따라 SDK 부하 도구·직접 큐 계측을 구현했다. 독립 검토 APPROVED·최종 전체198/198 PASS이며 **사용자 완료 승인·정상 하네스 DONE**을 확인했다. 계약·GWT8은 [M14 계약](../implementation-m14.md)을 따른다.

## 변경과 결정

- `tools/bench_m14.c`는 연결별 C SDK 스레드·지속 연결·outstanding1, warmup barrier·QPC 원시 표본·확정 UPDATE marker·Windows 서버 메모리 관측을 수행한다. `scripts/m14_benchmark.py`는 fixture·서버 수명·통계/큐 교차 검증·전체 바이너리 행 oracle·실행 조건/해시를 기록한다.
- `src/server.c`의 선택 `--queue-metrics PATH`는 기본 비활성이다. 기존 queue lock 안에서 고유 job/generation/request·등록/인수/완료·거절/제거를65,536개 사전 할당 buffer에 기록하고 worker join 뒤 덮어쓰기 없이 저장한다. SQL·EOF·오류 대기를 분리하며 DB gate 대기는 측정하지 않는다.
- 기술6파일과 GWT8을 CMake/검사 어댑터에 연결했다. 엔진·SDK 공개 API·wire 의미·서버 기본값(연결32/워커4/대기16)을 유지했고 자동 조정은 하지 않았다. 결과의 `valid/complete`와 전요청 성공 여부를 구분한다.

## 실제 검사·반려·수정

- 최초1-red는 빌드 성공 뒤 loopback WinError10013으로 연결 준비에 실패한 환경 근거이며 기능 Red가 아니다. 권한 대기 취소·ENV_ERROR 인계를 보존하고, 사용자 로컬 TCP 승인 뒤 같은 run을 재개했다.
- [2-red](../../.cache/agent-harness/m14-native-20261119/commands/2-red/receipt.json)는 실제3연결 SQL·DONE 양성 대조 뒤 있어야 할 SQL/EOF6개 큐 표본 누락 assertion 실패다. Master가 원문을 확인하고 기능 Red를 확정했다. 구현 후 작성자 영향12/12가 통과했다.
- 독립 QA는 큐 자체 합계만 맞으면 빈 trace·측정창 밖 trace도 수용하는 검증기 결함 **M14-QA-01**을 재현해 **묶음 반려1회** 했다. 기존 실제36개 선택 자료는 별도 교차 검사에서 일치했으며, 실측 자료 불일치와 검증기 방어 결함을 구분한다. [첫 검토](../../.cache/m14/reviewer-attempt1.json)를 따른다.
- [5-red](../../.cache/agent-harness/m14-native-20261119/commands/5-red/receipt.json)는 성공한 configure/build 뒤 정상 SQL/EOF·초기 busy·SQL 뒤 EOF busy 양성 대조를 통과한 다음 손상 trace 거절 assertion이 실패한 실제 Red다. 요청 그룹·단계·종료 결과와 SDK 시도/성공/오류를 교차 대조하도록 검증기와 시험2파일만 수정했다. 빈 기록·균형을 맞춘 요청 그룹 삭제·측정창 이동을 거절하며 SQL 후 EOF 거절은 보존한다.
- 수정 뒤 최신 작성자 [7-targeted](../../.cache/agent-harness/m14-native-20261119/commands/7-targeted/receipt.json)는 새 configure/build·M14 8/8 PASS다. 작성자 전체 회귀는0회다. 독립 재검토·손상/양성 probe와 기존 자료 재검증은 [독립 수정 검증](../../.cache/m14/reviewer-evidence/after-fix-result.json)에 보존했다.
- 최종 고정 소스에서 VERIFY 새 configure/build·**전체198/198·실패/skip/누락0**을 **정확히1회** 실행했다. [8-regression 영수증](../../.cache/agent-harness/m14-native-20261119/commands/8-regression/receipt.json)·[보고서](../../.cache/agent-harness/m14-native-20261119/commands/8-regression/test-report.json)·[독립 최종 결과](../../.cache/m14/reviewer-result.json)를 따른다.
- 로컬 TCP Red·부분 검사·측정·최종 회귀의 실제 도구 호출자는 Master다. 각 역할이 고정 소스에서 검사를 요청했고, Reviewer는 코드·probe·원시 자료·최종 영수증/해시를 독립 검토했다. Reviewer가 네트워크 검사를 직접 호출했다고 보고하지 않는다. [실행 귀속](../../.cache/m14/execution-notes.json)을 보존한다. 통계 손상 probe는 실제 서버 동시성 회귀를 대신하지 않는다.

## 실제 측정

Debug·loopback·10,000행·연결1/4/16/32·ID/scan_key/혼합95%읽기5%UPDATE, client당 warmup20회 뒤40회·조합별3반복이다. 연결/DB open/warmup은 SQL 측정에서 제외한다. 각 실행은 격리된 DB와 서버를 사용했다.

실제37회는 **원래36회 중35개 유효·1개 무효 + 같은 조건 재측정1개 유효**다. 원래 `scan_key-c4-r2` job402는 enqueue/dequeue가9QPC ticks(0.9µs) 역행해 무효 처리됐고 원본을 보존했다. 원인을 단정하거나0으로 보정하지 않았다. 같은 소스·실행 파일·설정의 재측정으로 선택36개를 구성했다. 최초36회가 모두 통과한 것은 아니다.

선택36개 실측 **19,080시도 = 13,301성공 + SERVER_BUSY5,779**, 오류율30.29%다. warmup의 SERVER_BUSY2,632건은 별도다. 미시도·반영 미확인0이며, 최종36만 행의 전체 필드를 검증했다. 원시값·정확한 교체 경로·반복별 범위·해시는 [측정 manifest](../../.cache/m14/measurement-manifest.json)를 따른다. QA 수정은 검증기만 바꿨으므로 서버/부하를 반복 실행하지 않고 기존37회와 선택36개를 강화된 검증기로 재검사했다. 역사적 실행 해시를 고치지 않고 현재 검증기 해시를 [재검증 manifest](../../.cache/m14/validator-revalidation-manifest.json)에 별도 기록했다.

아래 처리량·p95·큐 p95는 **각3반복 통계의 중앙값**이며, pooled p95가 아니다. p95는 성공 요청 표본, 큐 p95는 SQL 작업의 직접 등록→인수 대기다. 오류/시도는3반복 합계다.

| 부하 | 연결 | 성공 처리량 req/s | 성공 p95 ms | SQL 큐 p95 ms | 오류/시도 합계 |
|---|---:|---:|---:|---:|---:|
| id | 1 | 3583.6 | 0.390 | 0.010 | 0/120 |
| id | 4 | 9894.4 | 0.487 | 0.012 | 0/480 |
| id | 16 | 11027.0 | 1.776 | 0.415 | 0/1920 |
| id | 32 | 5691.2 | 5.238 | 1.847 | 1752/3840 |
| scan_key | 1 | 1210.8 | 0.929 | 0.013 | 0/120 |
| scan_key | 4 | 1703.2 | 3.270 | 0.026 | 0/480 |
| scan_key | 16 | 1670.0 | 13.472 | 5.709 | 0/1920 |
| scan_key | 32 | 1557.9 | 15.558 | 7.314 | 2164/3840 |
| mixed | 1 | 622.1 | 0.726 | 0.016 | 0/120 |
| mixed | 4 | 766.9 | 24.723 | 0.018 | 0/480 |
| mixed | 16 | 866.5 | 78.995 | 41.161 | 0/1920 |
| mixed | 32 | 691.3 | 91.100 | 45.654 | 1863/3840 |

선택36회에서 실행별 관측 최대 working set은6.42~10.41MiB, private bytes는6.55~10.47MiB다. 약10ms 요청 간격과 실제 시각을 보존하며 경계 표본은 구분한다. 측정창 안 표본이 없으면 최대값은 null이며0으로 채우지 않는다. 이번 선택 자료의 null 최대값은0개다.

## 한계·미실행·다음 행동

- 짧은 고정 요청 수·Debug·같은 호스트 loopback 결과다. 계측 비용·클라이언트/서버 CPU 경합을 포함하며 장시간 정상 상태·통계적 유의성·운영 용량을 입증하지 않는다.32접속의 거절을 숨기지 않았고 기본 설정 조정은 수행하지 않았다.
- 메모리는 표본 관측 최대이며 절대 순간 peak·누수 증명이 아니다. 큐 측정은 DB 잠금 대기와 다르다. QPC 역행의 근본 원인·전체 시작 비용 진단·성능 최적화는 미실행이다.
- DB gate 계측·워커/큐 설정 조합(B)·시작 비용 후보·배포/이식은 후속 선택 범위다. 기존 Phase1 전수 감사 생략·전원 손실·전수 fault 미검증을 보존한다.
- 본문 원문 대조·Wiki 일괄 등록·문서 finish와 영향/해시 근거는 [Knowledge JSON](../../.cache/m14/knowledge-result.json)을 따른다. 사용자 최종 완료 확인을 받아 정상 하네스 DONE 처리했다. 완료 상태 변경의 영향 검사·승인 출처는 [완료 JSON](../../.cache/m14/completion-result.json)을 따른다. 새 Goal은 없다.
