# mini-DBMS 인수인계 — compact 후 먼저 읽기

2026-11-17 화요일(Asia/Seoul). 이 문서는 재개용 요약이다. 최신 사용자 지시·앱 권한·현재 승인 범위를 우선하며 과거 승인이나 완료 Goal로 새 범위를 추정하지 않는다.

## 현재 위치

사용자가 승인한 **첫 실행 마일스톤 M1의 구현과 독립 QA를 완료했다. 최종 사용자 확인은 대기 중**이다. 정식 run은 `m1-native-20261117`, 요구사항은 `DBMS-001`이다. Knowledge 문서·Wiki 원문 대조와 실제 finish(21-finish exit 0)를 마쳤다. Master가 결과를 보고한 뒤 사용자 확인을 받아야 하며, 확인 없이 DONE/Goal 완료로 바꾸지 않는다.

- C23 정적 엔진과 얇은 CLI, engine/sql/catalog/storage/cli 다섯 책임을 구현했다. `init → 단일 INSERT → 기본 SELECT → 파일 저장 → 별도 프로세스 재시작 조회`, 파일/명시적 stdin, table/JSONL, 핵심 실패 처리가 동작한다.
- 최종 기술 소스 digest: `6480aeac9ecce378a1a7def6ac46753dbaaec01b8f6ab7a83bd623dac74256a8`.
- 최초 실제 build 성공 뒤 Red는 16개 중 단위 1 PASS·CLI 15 런타임 FAIL이었다. 구현 후 Green/회귀 16개 PASS. 독립 QA가 help/version 실제 stdout 오류 진단 누락을 발견해 반려했고, 해당 실패를 먼저 재현한 뒤 수정했다.
- 최종 Implementer 검사 `16-build`, `17-green`, `18-regression`과 Reviewer2의 새 `19-build`, `20-regression`이 같은 최종 소스에서 PASS했다. Reviewer2는 추가 SQL/스키마/저장 36개와 실제 read-only stdout handle 5개도 PASS했다. CTest 단위 case 하나 안에는 cmocka 함수 3개가 있다.
- 실제 증거와 부분 GWT 대응은 [M1 검증 기록](docs/testing/m1-results.md), [DBMS-001 기록](docs/testing/records/DBMS-001.md)을 따른다. 204개 전체 인수 명세 통과로 확대하지 않는다.
- M1 누적 품질 반려는 **2회**: Planner 반환 형식 1회, 독립 QA의 CLI stdout 처리 1회. 둘 다 해결했고 Master가 두 번째 뒤 원인을 재분석했다. 다음 품질 반려가 세 번째면 자동 재지시를 멈추고 사용자에게 보고한다. 별도 bootstrap 하네스 작업의 해결된 반려 2회와 혼합하지 않는다.
- 사용자는 M1 Goal 유지를 명시했다. 하지만 Master가 확인한 도구 상태는 `usageLimited`(tokensUsed 498033, timeUsedSeconds 1479)이며 active라고 기록하지 않는다. 목표와 작업 재개 승인은 유지되지만 Goal 런타임 재개는 사용자가 제어한다. 새 Goal을 만들지 않는다.

## 바로 확인할 것

1. 이 파일, [AGENTS.md](AGENTS.md), `dev-tools.json`, [Wiki](docs/wiki/index.md), 관련 [요구사항](docs/requirements.md)과 계약을 읽는다. `.private/project-history/approvals.md`는 Master만 확인하고 자식에게 전달하지 않는다.
2. [M1 계획·불변식](docs/implementation-m1.md), [실행 결과](docs/testing/m1-results.md), [현재 설정](docs/setup-status.md), [남은 작업](docs/pending-decisions.md), [작업 기록](docs/work-log.md)을 읽는다. 이전 문서의 미설치·stub·run 미생성 문구는 과거 시점이며 현재 상태는 이 문서와 최신 결과를 우선한다.
3. 실제 하네스 상태는 `.cache/agent-harness/m1-native-20261117/state.json`을 읽고 정상 CLI로만 전이한다. 원장을 직접 수정하지 않는다. Knowledge 문서 수정 후 Wiki/finish가 최신인지 확인한다.

## 빌드·실행

저장소 루트에서 `python scripts/c_checks.py regression`을 실행하면 설치된 고정 도구 경로로 실제 C 빌드와 16개 CTest를 수행한다. 기본 실행 파일은 `.cache/bootstrap/scaffold/manual/manual/regression/build/minidb.exe`다. [README](README.md)에 스키마 JSON·SQL 파일·새 DBdir 예제가 있다. CMake preset만으로 고정 컴파일러가 자동 선택된다고 가정하지 않는다.

현재 독립 QA 실행 파일은 `.cache/agent-harness/m1-native-20261117/artifacts/VERIFY/regression/build/minidb.exe`이고 실제 Master 데모는 `.cache/m1-demo/session-kq5mvz5u/receipts.json` 및 `1.stdout`~`4.stdout`이다. 이 경로는 캐시이므로 배포 아티팩트나 추적된 소스로 간주하지 않는다.

## 도구와 검증 한계

llvm-mingw 20261104(Clang/LLDB 23.1.5), CMake/CTest 4.5.0, Ninja 1.13.2, cmocka 2.0.1, yyjson 0.13.0을 고정·설치했다. 특정 C23 기능·Windows 실행, LLDB smoke, cmocka/CTest 성공/의도적 실패 전파를 독립 확인했다. 하네스 유지보수는 작성자 65개·독립 reviewer 61개 PASS였으며 DBMS 기능 결과와 구분한다.

cmocka·yyjson SHA-256은 공식 HTTPS 원본에서 로컬 계산했다. cmocka detached signature는 암호학적으로 검증하지 않았고 독립 재다운로드도 불가했다. 전체 C23 적합성·모든 라이선스 배포 상황·업스트림 서명 인증으로 확대하지 않는다.

## 미완료와 다음 행동

- **M1 최종 사용자 확인**을 받은 뒤에만 해당 run을 완료한다. 현재 문서 마감 영수증은 정식 run의 finish 결과를 확인한다.
- 후속 Phase 1: 복수 행 INSERT·확장 조건식·VALUES·UPDATE/DELETE·계산·정렬/페이지·JOIN·집계·UNION·migrate. 기존 계약은 유지하고 증분 구현 계획/새 선택이 필요한 부분만 Master가 사용자와 상의한다.
- Phase 2 B+ 트리·자동 ID·100만 건 비교, Phase 3 API/Thread Pool은 아직 구현하지 않았다. 해당 Phase에서 상세 설계한다.
- 현재 시작 시 행 적재와 이차 시간 ID 중복 검사, 선형 SELECT를 사용한다. 대용량 성능·전원 차단 복구·완전 손상 탐지·모든 할당 실패·실제 콘솔 렌더링을 검증했다고 주장하지 않는다.
- Master만 사용자와 소통한다. 자식간 통신·재위임 금지, 승인 파일 경계를 지키고 같은 파일 수정/검증을 겹치지 않는다. source 변경 후 `python scripts/dev_tools.py finish`를 실행하며 Wiki는 원문 대조 후 등록한다.
- 기존 미커밋·미추적 변경을 보존한다. Git 커밋/게시·추가 과금·개인정보 외부 전송은 이번 범위가 아니다. Git 예외는 명령별 safe.directory만 쓴다. Node/TS/npm·새 MCP 제외와 CIM 별도 승인 규칙을 유지한다.
