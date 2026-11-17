# mini-DBMS Wiki

compact 후에는 [HANDOFF](../../HANDOFF.md)를 먼저 읽는다. 최신 실행 상태는 [M1 결과](../testing/m1-results.md), [DBMS-001](../testing/records/DBMS-001.md), [작업 기록](../work-log.md)에 있다. 아래 사실은 2026-11-17 화요일(Asia/Seoul)에 실제 원문·검사 영수증과 대조했다.

## 현재 검증한 사실

- Windows C23 정적 엔진과 얇은 CLI의 M1을 구현했다. init→단일 INSERT→기본 SELECT→바이너리 파일→별도 프로세스 재시작 조회, 파일/명시적 stdin·table/JSONL이 동작한다. 사용자 최종 확인은 대기 중이며 전체 Phase1 완료가 아니다. [구현·불변식](../implementation-m1.md), [실제 결과](../testing/m1-results.md), [README](../../README.md)가 출처다.
- 최초 실제 Red는 빌드 성공 뒤 CLI 15개 runtime assertion 실패였고 최종 Implementer/독립 Reviewer2 회귀는 16/16 PASS다. 추가 SQL/스키마/저장36·실제 stdout5도 PASS했다. P1-SQL-001 전체를 확인했으나 나머지203개 전체 사례 통과로 확대하지 않는다. [검증 기록](../testing/m1-results.md)이 명령·범위를 보존한다.
- INSERT 반영 경계는 append 전체 쓰기+FlushFileBuffers다. 그 전 실패는 길이 복원/flush/확인을 하며 미해결 상태는 표식을 보존해 재시작을 막는다. 반영 후 cleanup/stdout 실패는 성공/건수를 유지하고 후속 SQL을 중단한다. 실제 storage/cli와 실패 검사를 대조했다. [저장 계약](../storage-contract.md), [CLI 계약](../cli-contract.md), [M1 결과](../testing/m1-results.md)가 근거다.
- 해결한 실패 M1-QA-001: help/version 초기 출력 분기에도 실제 stdout write 오류 처리가 필요했다. 공통 plain_output과 실제 read-only handle 회귀로 수정했다. M1 반려2회(Planner 반환형식·CLI결함)는 해결됐고 다음 세 번째 관문은 사용자 보고다. [DBMS-001](../testing/records/DBMS-001.md)이 출처다.
- 고정 Clang/LLDB/CMake/Ninja/cmocka 설치와 yyjson 반입·실제 빌드/범위가 정해진 probe를 확인했다. cmocka/yyjson hash는 공식 HTTPS 원본의 로컬 계산이며 서명 인증이 아니다. [도구 문서](../toolchain-proposal.md), [라이선스](../third-party-licenses.md), tools lock/실제 라이선스가 출처다.
- 하네스는 design의 실제 문서 검사와 implementation의 실제 Red/build/독립QA를 구분한다. 승인한 docs 및 README/HANDOFF 지식 경계를 적용했다. 운영 모의65/61개 검사와 실제 native M1 run은 별도 증거다. [하네스](../workflow/agent-harness.md)가 출처다.

## 유지할 계약과 후속 범위

- [SQL](../sql-contract.md), [스키마](../schema-contract.md), [저장](../storage-contract.md), [CLI](../cli-contract.md) 계약이 의미의 원본이다. 파일/명시적 stdin·기본 표/JSONL·필수 경로를 다시 미정으로 돌리지 않는다. [각 Phase 요구사항](../requirements.md)을 순서대로 구현한다.
- 현재 시작 시 행 적재·이차 시간 ID 중복 검사·선형 SELECT를 사용한다. 대용량 성능·전원 차단 복구·모든 손상 탐지는 보장하지 않는다. 추가 SQL/migrate는 후속 Phase1이고 B+트리/자동ID/100만 건은 Phase2, API/Thread Pool은 Phase3에서 상세 설계한다. [남은 작업](../pending-decisions.md)을 따른다.
- Master만 사용자와 소통하고 자식간 직접 통신/재위임·파일 수정/검토 충돌을 막는다. Node/TS/npm·새 MCP 제외, 추가 도입 보류를 유지한다. 사용자 Goal 유지와 도구 usageLimited는 구분한다. [AGENTS](../../AGENTS.md), [HANDOFF](../../HANDOFF.md), [설정 상태](../setup-status.md)가 출처다.

Wiki 등록·정적 Graphify 갱신은 DBMS 기능 실행의 대체 증거가 아니다. 원문 대조 후 wiki-review --reviewed로 hash를 등록하고 실제 finish 결과를 별도로 확인한다.
