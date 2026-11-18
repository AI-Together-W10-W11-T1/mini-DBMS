# mini-DBMS Wiki

2026-11-18 수요일(Asia/Seoul). compact 후 [HANDOFF](../../HANDOFF.md)를 먼저 읽는다. 검증한 결정·실패·한계는 아래 원본으로 연결하며 당시 실행 이력을 복사하지 않는다.

## 문서 마감 단일 정책

- 검증한 새 결정은 [작업 흐름의 문서 마감 절](../workflow/agent-workflow.md#문서-마감)에 모았다. 상세 결과1회·짧은 HANDOFF·실행 변경 시 README·검증한 새 지식의 Wiki·본문 확정 후 일괄 등록/finish·영향 검사·JSON 기계 근거 참조를 적용한다. AGENTS와 하네스·개발 도구의 연결 원문을 대조했다.
- 실제 작성자/독립 검증·수동 유지보수 경로·한계와 최종 사용자 완료 승인은 [단일 정책 결과](../testing/document-close-results.md)를 따른다. 기존 역할·독립 회귀·최신성·사용자 확인 관문을 유지한다.

## 현재 M11 대량 생성·비교

- 사용자1A/2A의 결정적 SQL·CLI10000행 배치·공개 엔진 API QPC 비교를 구현하고10k/100k/1m 전 필드를 검증했다. GWT8·작성자/독립158개·자체137행·실제 손상 거절·측정값·한계는 [단일 M11 결과](../testing/m11-results.md)를 따른다. 사용자 완료 승인·정상 하네스 DONE을 확인했다.
- scripts/m11_benchmark.py·tools/bench_m11.c·tests/m11_benchmark.py와 CMakeLists.txt·scripts/c_checks.py, 승인 계약·결과 JSON을 대조했다. Debug·warm SQL 조회·100만 실제 실행1회, 측정 후 Python 숫자/bool 검증 엄격화와 native 해시 보존을 확인했다. 시작 비용 profiling·최적화는 후속 사용자 협의 범위다.

## 보존한 M10 엔진 통합

- 사용자1A/2A의 DELETE 교체 인덱스·UPDATE 재사용·INSERT 배치 예약을 구현·독립 검증했다. 자동 ID·시작/재시작·ID/AND 조회·OR 선형·migrate의 GWT15·실제 Red/Green·작성자/독립150개·독립 CLI26개·실패·한계는 [단일 M10 결과](../testing/m10-results.md)를 따른다. 사용자 완료 승인·정상 하네스 DONE을 확인했다.
- src/bptree.c·h의 예약/적용, internal.h·catalog.c의 테이블 소유권, storage.c의 반영 경계·교체 매핑, sql.c의 후보 재평가·메타데이터 선행 검증과 tests/unit_m10.c·m10_cli.py, 기존 fixture2개 정렬·등록·SQL 계약을 원문 대조했다. 성공 쿼리의 두 번 파싱과 비용 미측정,100만 건 비교 미실행을 결과에 남겼다.

## 보존한 M9 독립 B+ 트리

- 기존최댓값+1·레코드 ID→메모리 행 번호·트리→통합→100만 비교, production 자식32/리프31·시험 자식4/리프3과 첫 범위 A를 선택했다. 실제 Red·GWT12·작성자/독립135개·양쪽 용량 probe·한계는 [단일 M9 결과](../testing/m9-results.md)를 따른다. 구현·독립 QA 완료, 사용자 완료 승인·정상 하네스 DONE 확인다.
- src/bptree.c·h의 오른쪽 서브트리 최소 구분키·분할 사전 할당·실패 보존·노드 소유권·커서 무효화와 tests/unit_bptree.c의 구조/oracle/fault·production 실행, CMakeLists.txt·scripts/c_checks.py의12개 등록을 원문 대조했다. 자동 ID 실제 발급·SQL 연동·삭제·배치 예약·100만 비교는 후속 범위다.
- 최신 사용자 지시로 추가 QA를 중단했고 이미 완료한 독립 증거를 보존한다. 이후 사용자 완료 승인·정상 하네스 DONE을 확인했다. 다음 선택과 Phase 1 감사 생략의 기존 경계는 [결정 원본](../pending-decisions.md#다음-단계)을 따른다.

## 보존한 M8 명시적 변환

- 사용자 A 선택·GWT20·실제 Red·작성자/독립123개·독립 프로세스36개·native 경로12개·반려1 해결·fixture/환경 실패·측정·한계는 [단일 M8 결과](../testing/m8-results.md)를 따른다. 사용자 완료 승인·정상 하네스 DONE을 확인했다. 후속 진행은 현재 M9 결과와 결정 원본을 따른다.
- 사용자가 Phase 1 전체204개 인수 감사를 건너뛰기로 했다. 기존 회귀·감사 미실행·남은 관찰과 선택 출처는 [결정 원본](../pending-decisions.md#다음-단계)에 보존한다.
- src/engine.c의 관리 수명, src/catalog.c의 엄격 plan/일대일 대응, src/storage.c의 원본 헤더/전체 행 검증·정확 정수/크기·복사/변환·사용권·guard/pending 공개·루트 부모 보존, src/cli.c의 단계별 진단을 대조했다. tests/m8_cli.py의 실제 프로세스·독립 decoder와 tests/unit_milestone.c의 production helper/소유 수명·등록을 확인했다.
- M8-QA-001은 루트 부모의 마지막 구분자를 보존하는 최소 수정과 실제 production wrapper Red/Green·독립 재검토로 해결했다. UNC 서버·루트 출력 생성·junction·전수 fault·누수 분석·전원 손실·100만 행은 미검증이다. M7 ORDER 없는 ALL 결과 순서 관찰도 남아 있다.

## 보존한 M7 UNION

- 사용자 A 선택·heap 전체 열 dedup·GWT13·실제 검사·fixture 오류·측정·한계는 [단일 M7 결과](../testing/m7-results.md)를 따른다. 사용자 ‘M7 완료 승인’을 Master가 정상 finish에 반영해 DONE을 확인했다. 다음은 migrate·관리와 전체 인수 회귀 계획 협의다.
- src/sql.c의 좌결합·전체 행 중복 제거·크기 검사·TEXT 소유권, native 수명/실패 시험, tests/m7_cli.py와 M6 기대 정렬·CMake/검사 어댑터를 원문 대조했다. 초기 overflow Red 한계와 비차단 시험 관찰은 결과에 보존한다.

## 보존한 M6 집계

- 사용자 A 선택·선형 그룹·GWT14·작성자/독립90개·oracle30·음성28·추가10·초기 Red 한계는 [단일 M6 결과](../testing/m6-results.md)를 따른다. 구현·독립 QA와 사용자 완료 승인·하네스 DONE을 확인했다. 다음은 UNION 계획 협의다.
- 첫 오류 분류 수정의 token-kind 한계·지원 절 WORD 경계 잔존·반려2회와 Master 원인 재분석·fixture 오류 구분은 결과에 보존한다. 실제 Red와 최종 독립 재검토로 해결했다.
- src/sql.c의 그룹·실제 증가/크기 검사·TEXT 소유권·부분 정리, tests/unit_milestone.c의 경계/수명, tests/m6_cli.py의14개와 CMake/검사 어댑터를 대조했다. 최악 이차 시간·합성 경계·선택 fault·대용량/heap 미검증 한계는 결과를 따른다.

## 보존한 M5 JOIN

- 사용자 A 승인 범위·중첩 루프·이름/출력·소유권·크기 검사·GWT·실제76개 회귀·oracle32·경계19는 [단일 M5 결과](../testing/m5-results.md)를 따른다. 독립 QA·사용자 완료 승인·하네스 DONE을 확인했다.
- ON 필수 비교 토큰 누락을 존재하는 비등호와 구분하지 못한 R1은 실제 Red 뒤 수정·독립 재검토로 해결했다. 반려1회이며 세부 근거와 한계는 결과 원문에 둔다.
- 당시 구현과 검사의 원문 대조·한계는 위 M5 결과에 보존한다.

## 보존한 workflow 유지보수

- workflow 간소화의 완료 승인·정상 CLI 확인·실제 검증·측정 한계는 [단일 slim-v1 결과](../testing/workflow-slim-v1-results.md)를 따른다.
- 명시적 slim-v1·기본 strict 보존·고위험 실제 Red·부분/전체 근거·독립 회귀·기대값 정렬 관문·단일 결과·짧은 출력은 [하네스 원문](../workflow/agent-harness.md), [역할 흐름](../workflow/agent-workflow.md), [AGENTS](../../AGENTS.md)를 따른다.
- 역할별 서명 캐시·매 호출 실제 configure/build/CTest·새 증거·기능 실패/실행 오류 분리·docs JSON 그래프 제외는 [도구 원문](../workflow/dev-tools.md)과 `scripts/c_checks.py`, `scripts/dev_tools.py` 및 해당 Python 시험을 대조했다. 모의 unit과 실제 native 실행은 별도 근거다.
- WF-001 기능 실패 오분류와 WF-002 기존 Python 테스트 경로 누락은 독립 재검토로 해결했다. Windows 중첩 경로 환경 실패의 짧은 layout 수정과 검증 범위는 [결과](../testing/workflow-slim-v1-results.md)를 따른다. byte는 provider token이 아니며 절감률·TDD 인과효과는 미측정이다.

## 보존한 원본과 완료 이력

- M1~M4는 사용자 완료 승인·하네스 DONE, M1 Goal은 complete다. [M1 계획](../implementation-m1.md)·[결과](../testing/m1-results.md)·[DBMS-001](../testing/records/DBMS-001.md), [M2 계획](../implementation-m2.md)·[결과](../testing/m2-results.md)·[DBMS-002](../testing/records/DBMS-002.md)를 따른다.
- [M3 계획](../implementation-m3.md)·[결과](../testing/m3-results.md)·[DBMS-003](../testing/records/DBMS-003.md), [M4 계획](../implementation-m4.md)·[결과](../testing/m4-results.md)·[DBMS-004](../testing/records/DBMS-004.md)에 실제 Red/Green·독립 검사·실패·제한을 보존한다.
- [SQL](../sql-contract.md)·[스키마](../schema-contract.md)·[저장](../storage-contract.md)·[CLI](../cli-contract.md)가 의미의 원본이다. [요구사항](../requirements.md)·[Phase 설계](../phase-design-policy.md)에 따라 후속 범위를 선택한다.
- 설치·고정 도구·서명 미검증 한계는 [도구 제안](../toolchain-proposal.md)·[라이선스](../third-party-licenses.md)·[설정 상태](../setup-status.md)에 보존한다. [README](../../README.md)는 재현 예제, [남은 작업](../pending-decisions.md)·[작업 기록](../work-log.md)은 다음 행동과 역사 원본이다.

기존 출처 목록을 보존하고 M11 결과·도구·시험을 추가 등록했다. 변경 원문과 문서를 대조했다. 문서 마감의 실제 관측·해시는 [M11 Knowledge JSON](../../.cache/m11/knowledge-result.json)을 따른다.
