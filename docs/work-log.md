# mini-DBMS 작업 기록


## 2026-11-17 화요일 — 실제 M1 구현·독립 QA (Asia/Seoul, 최신)

- 목적·결정: 승인한 Windows C23 정적 엔진+얇은 CLI의 다섯 책임으로 첫 실행 구간을 구현했다. 기존 SQL/스키마/저장/CLI 계약을 유지하고 init·단일 INSERT·기본 SELECT·파일 저장/별도 프로세스 재시작·입출력·핵심 실패부터 연결했다.
- 이유·핵심: 이벤트가 결과를 소유하고 append flush를 반영 경계로 삼는다. 반영 전 오류는 원래 길이 복원을 확인하고, 반영 후 정리·출력 오류는 성공/건수를 보존해 재실행·잘못된 롤백을 막는다. 입력은 문장별 소비하므로 뒤의 잘못된 바이트가 앞선 반영을 취소하지 않는다.
- 실제 검사: 최초 build PASS·실제 Red 단위1 PASS/CLI15 FAIL → 구현 Green/회귀16 PASS. 독립 QA가 초기 help/version stdout 실패 누락을 찾아 실제 read-only handle Red를 추가한 뒤 좁게 수정했다. 최종 Implementer build/Green/회귀와 독립 Reviewer2 새 build/회귀16개, 추가36개·실제 stdout5개 모두 PASS다. 최종 소스 digest는 6480aeac9ecce378a1a7def6ac46753dbaaec01b8f6ab7a83bd623dac74256a8이다.
- 검토·실패: M1 누적 반려2회는 Planner findings 반환 schema와 QA의 M1-QA-001이다. Master 재분석 후 해결했다. 별도 bootstrap 반려2회 및 운영 모의65/61개 검사와 혼합하지 않는다.
- 문서: README 재현 명령·스키마/SQL 예제, HANDOFF, 설정/대기/도구/라이선스/하네스·Wiki 및 DBMS-001/M1 결과를 실제 원문/영수증과 대조했다. 실제 21-finish exit 0으로 Graphify 갱신·Wiki changed_sources=[]/page_changed=false를 확인했다. vendor 부분 추출·헤더 no-symbol 경고는 한계로 보존한다. 결과 문서화 뒤 재등록/최신 finish를 수행하며 최종 영수증을 따른다.
- 미완료·한계: 전체 204개 GWT 통과가 아니다. P1-SQL-001 전체와 명시한 부분 검사만 대응한다. 나머지 SQL·migrate·Phase2/3, 100만 건 성능·전원 차단 복구는 남아 있다. 시작 행 적재/이차 시간 ID 중복 검사·선형 SELECT를 사용한다.
- 다음 행동: 최종 사용자 확인 후 해당 M1 run을 완료한다. 이후 기존 계약의 후속 Phase1 증분 계획에서 새 선택이 필요한 사항만 사용자와 상의한다. Goal은 명시 유지됐지만 도구 상태는 usageLimited로 확인됐으며 active/complete로 바꾸지 않았다.
- 출처: [M1 실제 결과](testing/m1-results.md), [DBMS-001](testing/records/DBMS-001.md), 실제 src/include/tests와 tools lock, 정식 run commands/1~20 및 Reviewer2 추가 결과, Master의 승인·Goal 상태 요약. 개인 원장/대화 본문은 복제하지 않았다. 커밋·게시·추가 과금은 수행하지 않았다.

아래 항목은 당시 시점의 과거 기록이다. 현재 실행 상태는 위 최신 항목과 M1 결과를 우선한다.

## 2026-11-17 화요일 — 실제 M1 착수 준비·진행 기록 (Asia/Seoul, 최신)

- 목적: 사용자가 승인하고 재개한 실제 첫 마일스톤의 범위·도구 증거·현재 실패와 미실행을 문서화해 정식 DBMS run baseline 생성에 전달한다.
- 결정·이유: Windows x64 C23 엔진과 얇은 CLI를 engine/sql/catalog/storage/cli 책임으로 연결한다. init·단일 INSERT·기본 SELECT·파일/명시적 stdin·table/JSONL·영속/재시작·핵심 실패 방어부터 실제 Red→Green→Refactor→회귀로 진행한다. 나머지 SQL·migrate는 후속 Phase 1 증분이며 전체 상세설계를 미리 고정하지 않는다. [M1 계획](implementation-m1.md)에 승인 범위·소유권 API·시험을 정리했다.
- 실제 도구 증거: llvm-mingw 20261104(Clang/LLDB 23.1.5), CMake/CTest 4.5.0, Ninja 1.13.2, cmocka 2.0.1, yyjson 0.13.0 설치/반입. C23=202311 int64=8 pointer=8 실행, LLDB breakpoint·step·local 42·backtrace·정상 종료, cmocka/CTest 성공 0·의도적 실패 8을 독립 재현했다. 이번 문서 작업이 이를 재실행한 것은 아니다.
- 출처 한계: cmocka·yyjson 해시는 공식 HTTPS 파일의 로컬 계산이다. 업스트림 서명 인증이 아니며 cmocka detached signature는 암호학적으로 검증하지 않았다. 독립 검토의 업스트림 재다운로드는 불가했다. 로컬 아카이브 해시·추출 안전성·vendor 일치는 확인했다.
- 실제 C 증거: 자체 C23/cmocka C99 CMake scaffold 컴파일 성공. 최초 수동 8개 중 단위 1개 PASS·CLI 7개 런타임 기능 FAIL이다. 현재 실행 코드는 stub이며 최종 DB가 아니다. 16개 등록과 실제 build/red/green/regression 4개 명령 연결은 완료했지만 추가 8개·정식 SQL Red·Green은 아직 실행하지 않았다.
- 운영 QA: 하네스 기준선 모의 53개에서 설계 전용 경로와 구현 Knowledge Manager의 루트 README/HANDOFF·승인 구현 지식 문서 explicit whitelist를 추가했다. Red/build/gate는 유지했다. 누적 품질 반려 2회 뒤 REWORK2 해결, 작성자 전체 65 PASS·Reviewer test_agent_harness 61개 독립 PASS 및 APPROVED를 Master가 전달했다. 이는 SQL 기능이나 실제 전체 역할 파이프라인 통과가 아니다.
- 마감 실패·미완료: 작성자 finish 1회는 의도적 checks 설정 변경에 따른 Wiki 원문 해시 stale로 실패했다. 최종 Knowledge Manager가 원문 대조·검토 재등록 후 마감한다. 현재 정식 DBMS run은 없고 사용자 명시 유지 Goal은 활성 상태다. 이번 기록에서는 Wiki 검토 등록·finish를 실행하지 않았다.
- 다음 행동: 네 진행 문서 작성 후 쓰기를 중단한다. 모든 source writer 정지 뒤 Master가 정식 baseline/run을 생성하고 실제 Red→최소 Green→Refactor→16개 회귀·독립 QA를 진행한다. 검증된 새 결정/실패만 Wiki에 기록하고 마감·사용자 최종 확인 뒤 완료 처리한다.
- 출처: Master가 전달한 최신 사용자 승인·재개/검토 결과 요약, [SQL](sql-contract.md)·[스키마](schema-contract.md)·[저장](storage-contract.md)·[CLI](cli-contract.md), [공개 API](../include/minidb.h), [실제 시험](../tests/milestone_cli.py), [검사 어댑터](../scripts/c_checks.py), [도구 lock](../tools/c-toolchain-lock.json), .cache/bootstrap/install-receipt.json 및 toolchain-reviewer/qa-receipt.json. 개인 원장·대화 본문은 읽거나 복제하지 않았다.

아래는 당시 시점의 과거 기록이다. 현재 상태는 이 최신 항목과 [설정 상태](setup-status.md)를 우선한다.


CLI 최종 마감(2026-11-17): 원문 대조 후 `wiki-review --reviewed` 등록과 `python scripts/dev_tools.py finish`의 Graphify 갱신·Wiki 검사를 통과했다. 정적 문서/운영 검사이며 CLI 기능 실행 증거가 아니다.

2026-11-17 화요일(Asia/Seoul) CLI 문서화 검사: GWT 48개는 전부 미실행이다. 독립 검토 지적을 묶어서 반영해 minidb 실행 파일명, table 사람용 stderr와 JSONL 모드 stderr 구분, 단일 역슬래시 pipe escape, 유효한 SQL fixture를 정합화했다. 문서 22개 UTF-8·로컬 링크 163개·CLI 48/저장 48/스키마 41/SQL 67개 ID·JSON 예제 4개·Git 공백 검사를 확인했다. 실제 C 구현·기능 검사는 수행하지 않았다.

현재 CLI 상태는 아래 2026-11-17 CLI 계약 후속 기록을 따른다. 과거 CLI 미선택 언급은 당시 기록으로 보존한다.

## 2026-11-17 화요일 — CLI 계약 문서화 (Asia/Seoul, 최신 후속 기록)

- 목적: 사용자가 선택한 CLI 입력·출력·경로 정책과 최종 문서화 승인 범위를 계약·인수 명세·현재 상태 문서에 연결한다.
- 결정: 파일 또는 명시적 stdin 입력을 지원하고 기본 REPL은 두지 않는다. 기본 표와 선택 JSONL, 필수 스키마·데이터 경로를 채택했다. `init`·`run`·`migrate`, 성공 결과 stdout/진단 stderr, 종료 코드·겹친 실패 우선순위는 [CLI 계약](cli-contract.md), 검증 조건은 [CLI 인수 명세](testing/phase1-cli-acceptance.md)를 따른다. JSONL 정수 값·건수·인덱스는 십진 문자열과 타입 메타데이터로 표현하고 NULL은 JSON null이다.
- 결정 이유: 파일 실행 요구를 유지하며 파이프 입력도 명시적으로 지원한다. 사람이 읽는 기본 출력과 기계가 소비하는 선택 출력을 제공하고 대상 경로를 명시해 실행 대상을 분명히 한다. 기존 SQL·스키마·저장 계약의 의미는 바꾸지 않는다.
- 범위와 미실행: 문서화 작업이다. 실제 C 코드·CLI/SQL/저장 기능 실행·C 빌드·도구 설치·C23 검증·yyjson 반입은 수행한 것으로 기록하지 않는다. 작성한 GWT는 실행 통과가 아니다. 이번 실제 정적 검사·검토·마감 결과는 Master가 별도 기록한다.
- 미완료·다음 행동: 최종 결과는 사용자 확인 대기다. CLI 선택을 다시 묻지 않고 다음 구현 계획에서 최소 C API·모듈 경계·증분 구현 순서, 도구 설치/C23 검증·실제 검사 연결·설계 전용 하네스 경로를 구체화한다. Phase 1 상세 설계 전체가 완료됐다는 뜻은 아니다. 새 Goal이나 전역 설정 변경은 이번 범위에 없다.
- 출처: Master가 전달한 현재 사용자의 CLI 3개 선택과 최종 문서화 계획 승인 요약, [CLI 계약](cli-contract.md), [CLI 인수 명세](testing/phase1-cli-acceptance.md). 개인 대화 본문·개인 승인 원장은 복제하지 않는다.

## 이전 문서화 기록

저장·변환 최종 마감: `wiki-review --reviewed` 등록 및 `python scripts/dev_tools.py finish`의 Graphify 갱신·Wiki 검사를 통과했다(2026-11-17). 이는 정적 문서/운영 검사이며 DBMS 기능 통과가 아니다.

2026-11-17 화요일(Asia/Seoul) 저장·변환 문서화 마감: GWT 48개를 작성했다. 독립 검토에서 동일 스키마의 비항등 컬럼 매핑이 단순 복사로 생략되는 문제를 찾아 계약과 교환 사례를 보완했다. 문서 20개의 UTF-8·로컬 링크 123개·저장 48/스키마 41/SQL 67개 ID 유일성·Git 공백 검사를 확인했다. C 구현·파일 API·SQL·자동 기능 검사는 미실행이다. Wiki·Graphify 마감 결과는 종료 명령으로 별도 확인한다.

아래 기존 기록의 기준일은 2026-11-16 월요일, Asia/Seoul이다. 당시 수행 사실과 다음 선택을 보존하며, 현재 상태는 마지막 날짜의 후속 기록을 함께 읽는다. 대화 길이로 작업 시간을 추정하지 않는다. 현재 상태의 원본은 [SQL 계약](sql-contract.md), [SQL 인수 명세](testing/phase1-sql-acceptance.md), [스키마 계약](schema-contract.md), [스키마 인수 명세](testing/phase1-schema-acceptance.md), [설정 상태](setup-status.md)다.

## 목적

C23·Windows 네이티브 환경에서 미니 DBMS를 단계적으로 구현하기 위한 작업 환경과 Phase 1 SQL 계약을 준비했다. Phase 1의 처리기를 Phase 2 메모리 B+ 트리와 100만 건 이상 비교에 재사용하고, Phase 3에서 API·Thread Pool로 확장한다.

## 수행 내용과 선택 이유

| 작업 | 수행한 내용 | 선택 이유 |
|---|---|---|
| 개발 운영 환경 | RTK·Graphify·Wiki와 하네스 운영 규칙을 정리했다. | 긴 출력 관리, 코드 구조 탐색, 검증된 결정 재사용을 지원한다. |
| 역할·소통 | Master와 Planner / Implementer / Reviewer_QA / Knowledge Manager의 다섯 역할로 정리했다. 독립 작업의 병렬 실행, 읽기/쓰기 충돌 검사, 누적 반려 규칙을 반영했다. | 사용자 선택을 Master가 관리하면서 독립 작업을 겹쳐 진행한다. |
| 환경 방향 | C23·Windows와 cmocka·CTest를 채택하고, 공식 LTS가 없으면 최신 안정판을 고정하는 정책을 정했다. | 언어·실행 환경과 검증 도구를 일관되게 관리한다. 실제 설치·실행 검증과 선택은 구분한다. |
| Phase별 설계 | 전체 상세설계를 한 번에 고정하지 않고 현재 Phase만 설계·선택·구현·검증하기로 했다. | 과도한 모듈 분리를 피하고 후속 요구에 맞춰 조정한다. |
| SQL 계약 | 문법·자료형·NULL·이름 해석·오류·실행 순서·Phase 호환을 문서화했다. | 파서·실행기·테스트가 서로 다른 동작을 기대하지 않도록 기준을 고정한다. |
| 인수 검증 | 성공·실패·경계값을 67개 Given/When/Then 사례로 정리했다. | 구현 전에 통과 조건을 확인하고 기능별 회귀 검사로 연결한다. |

## 확정한 SQL의 주요 결정

- INT64·UTF-8 TEXT·NULL을 지원한다. 문자열은 대소문자 구분 바이트 비교이며, 문자열 내부 실제 CR/LF를 보존한다. 마지막 문장까지 세미콜론이 필수다.
- Phase 1의 id는 직접 입력하는 양의 INT64·고유·필수 값이다. UPDATE로 변경할 수 없으며 삭제한 ID의 직접 재사용은 허용한다. Phase 2는 명시 ID 입력을 유지하고 생략 시 자동 부여를 추가한다.
- INSERT는 컬럼 목록이 필수이고 복수 행을 지원한다. 생략한 비ID 컬럼은 NULL이다. UPDATE·DELETE는 WHERE가 필수다.
- SET의 컬럼 복사·정수 덧뺄셈은 변경 전 행을 기준으로 평가한다. 정상적으로 처리 가능한 실패에서 문장 단위 원자성을 지키며 파일 전체 롤백·전원 차단 복구와 구분한다.
- WHERE의 비교·AND/OR·괄호·NULL 검사, 두 입력 INNER JOIN/self-join, COUNT·GROUP BY·HAVING을 포함한다.
- FETCH FIRST는 ORDER BY 없이 허용하되 반환 행·순서는 보장하지 않는다. OFFSET이 있으면 ORDER BY가 필수다. 페이지 예제에는 고유 키를 마지막 정렬 키로 사용한다.
- 독립 VALUES와 UNION·UNION ALL을 지원한다. UNION 계열은 좌결합이며 타입·컬럼 수를 검사하고 최종 정렬·페이지를 전체 결과에 적용한다.

세부 규칙과 성공·오류 SQL은 [SQL 계약](sql-contract.md)을 따른다. 이 요약으로 계약을 대체하지 않는다.

## 산출물

| 문서 | 용도 |
|---|---|
| [과제 요구사항](requirements.md) | Phase 1~3의 필수 결과물·재사용 관계·품질 요구 |
| [Phase별 설계 원칙](phase-design-policy.md) | 현재 Phase 상세설계와 사용자 선택 절차 |
| [SQL 계약](sql-contract.md) | EBNF, 동작, 고정 오류 코드, 대표 SQL, Phase 호환 |
| [SQL 인수 명세](testing/phase1-sql-acceptance.md) | P1-SQL-001~067의 검증 조건 |
| [설정 상태](setup-status.md) | 설치·선택·실행 검증 상태의 구분 |
| [다음 선택 목록](pending-decisions.md) | 남은 설계·연결 항목 |
| [제3자 라이선스 기록](third-party-licenses.md) | cmocka 채택과 배포 고지 확인 사항 |

## 실제 검증과 미실행 항목

| 구분 | 확인한 사실 | 해석의 한계 |
|---|---|---|
| 과거 운영 환경 검증 | RTK 설치·해시·실행, Graphify 설치·색인·질의, 하네스·도구 단위 검사 53개 통과를 기록했다. | DBMS 엔진과 SQL 기능 통과의 증거가 아니다. 이번 문서 작업에서 이를 새로 실행했다고 기록하지 않는다. |
| SQL 문서 교차 검토 | 계약과 명세를 서브에이전트로 나누어 작성하고 읽기 전용 검토를 수행했다. 오류 판정이 모호한 사례를 재현 SQL별 고정 코드로 보완했다. | 실제 SQL 실행 결과가 아닌 설계·명세 검토다. |
| 문서 정적 검사 | 8개 문서의 로컬 링크와 UTF-8 읽기, 67개 인수 ID 유일성, Git 공백 검사를 확인했다. Wiki 검토 등록·마감과 Graphify 갱신도 수행했다. | 정적 검사·색인 생성으로 SQL 정확성이나 성능을 보장하지 않는다. |
| SQL 기능 검사 | 67개 GWT 사례를 작성했다. | **전부 미실행이다. SQL 엔진과 자동 SQL 테스트는 아직 없다.** |

SQL 계약 문서화 Goal은 완료 상태다. 전체 DBMS·Phase 1 구현 완료를 뜻하지 않는다. 현재 변경은 미커밋 상태이며 이번 작업에서 커밋·원격 게시를 수행하지 않았다.

## 2026-11-16 당시 남은 일과 다음 선택

당시 다음 사용자 협의 순서는 **스키마 제공 방식 → 파일 저장 형식과 문장 원자성 구현 → CLI 표시 계약**이었다. 스키마 제공은 아래 2026-11-17 후속 선택으로 확정했으며, 구현자는 현재 남은 미선택 사항을 임의 확정하지 않는다.

컴파일러·빌드·테스트 도구의 실제 설치, C23 기능 probe, 빌드/테스트 명령·결과 어댑터 연결과 설계 전용 하네스 경로도 미완료다. 상세 설계 선택 뒤 INSERT/SELECT의 입력→파싱→실행→저장 수직 흐름부터 구현한다. Phase 2·3의 상세 구조는 해당 단계에서 정한다.

## 2026-11-16 개인 작업 일지에 복사할 짧은 초안

> C23·Windows 기반 미니 DBMS 구현을 준비하며 Phase 1 SQL 문법·동작 계약을 확정했다. NULL·ID·변경 원자성·JOIN·집계·정렬/페이지·UNION의 경계 동작을 명시하고 67개 GWT 인수 사례를 작성했다. 계약과 명세를 교차 검토해 오류 코드를 구체화했으며 문서 링크·ID 유일성·Wiki 검사를 확인했다. 아직 SQL 엔진과 자동 기능 테스트는 없으므로 구현 완료로 기록하지 않았다. 다음으로 스키마 제공·저장 형식·CLI 표시 방식을 선택하고 수직 기능부터 구현할 예정이다.

후속 기록에는 작업일, 실제 변경, 결정 이유, 실행한 검사와 결과, 미실행, 남은 문제, 다음 행동을 덧붙인다. 이 문서의 날짜를 후속 작업 날짜로 복사하거나 실제 작업 시간을 추정하지 않는다.

## 2026-11-17 화요일 — 외부 JSON 스키마 계약 문서화 (Asia/Seoul)

마감 결과: `wiki-review --reviewed` 등록과 `python scripts/dev_tools.py finish`의 Graphify 갱신·Wiki 검사를 통과했다. 코드 전용 색인은 문서 의미나 DBMS 실행을 검증하지 않는다.

- 목적: 기존 SQL 계약의 스키마 제공 미선택 항목을 사용자 선택으로 확정하고 계약·인수 명세·상태 문서를 맞춘다.
- 결정: 테이블별 외부 JSON 1개, 파일 이름과 내부 이름의 대소문자 무시 일치, 지정 디렉터리 직속 JSON 전체의 시작 검증을 채택했다. 빈 디렉터리 또는 오류가 있으면 SQL 전에 중단한다. 스키마는 이름·타입·순서만 제공하며 id는 기존 규칙을 따른다.
- 결정 이유: SQL 실행 전에 스키마 집합의 일관성을 확인한다. 원철자를 보존하고 ASCII 대소문자 무시 조회·중복 검사, SQL:2023 예약어와 지원 문법 키워드 합집합을 적용한다. 알 수 없는 키·디코딩 후 중복 키·주석·후행 쉼표를 거부하고 UTF-8 BOM을 허용한다.
- 의존성: yyjson 0.13.0(MIT) 소스를 포함하는 방침을 채택했다. 실제 소스 반입·설치·빌드·파싱 동작 검증은 하지 않았다. 라이선스 출처와 보존 사항은 [제3자 기록](third-party-licenses.md)에 남겼다.
- 검증: UTF-8 문서 18개·로컬 링크 94개·스키마 GWT 41개와 기존 SQL GWT 67개의 ID 유일성·JSON 예시·Git 공백 검사를 확인했다. 독립 검토의 중복 키 escape 예시와 자원 부족 진단 누락 2건을 한 묶음으로 보완했다. C 빌드·SQL 실행·스키마 자동 기능 검사는 미실행이다. 과거 운영 검사를 이번 실행 성과로 재기록하지 않는다.
- 미완료: 저장 형식·문장 원자성 구현·CLI 표시 계약, C 도구 설치·C23 실행 검증·실제 하네스 검사 연결·설계 전용 하네스 경로와 DBMS 구현은 남아 있다.
- 다음 행동: 계약·명세 교차 검토와 문서 마감 뒤 사용자 최종 확인을 받고 저장 형식·문장 원자성 구현, CLI 표시를 순서대로 선택한다.
- 출처: 현재 대화의 사용자 스키마 문서화 승인과 [스키마 계약](schema-contract.md), [스키마 인수 명세](testing/phase1-schema-acceptance.md). 개인 대화 본문이나 과거 승인 기록은 복제하지 않았다.

## 2026-11-17 화요일 — 저장·명시적 스키마 변환 계약 문서화 (Asia/Seoul)

- 목적: 저장 형식과 문장 실패 처리의 미선택 사항을 확정하고 사용자 요청에 따른 명시적 스키마 변환 범위를 계약·인수 명세·기존 상태 문서에 연결한다.
- 결정: 가변 길이 바이너리, INSERT 추가 쓰기와 UPDATE·DELETE 임시 파일 교체를 채택했다. 명시적 초기 준비, 시작 전체 데이터 검증, 변경 전 진행 표식과 복원 실패 시 세션·재시작 차단을 적용한다. 반영 확정 후 정리 실패는 SQL 성공을 유지하고 정리 오류·세션 중단으로 구분한다.
- 결정 이유: 추가 쓰기는 INSERT의 전체 재쓰기를 피하고, UPDATE·DELETE는 변경 전 데이터를 보존하며 결과를 준비한다. 진행 표식으로 정상 형식의 부분 반영만 남은 경우에도 미해결 작업을 조용히 재사용하지 않게 한다. 성능 수치나 전원 차단 내구성을 검증한 것은 아니다.
- 변환: 일반 실행은 스키마 불일치를 거절한다. 사용자가 요청한 변환은 같은 테이블 집합의 새 디렉터리 전체를 만들고, 미변경 파일은 복사·변경 파일은 변환·전체 검증 후 준비 완료로 표시한다. 원본은 보존한다. 컬럼 추가·재배열·이름 변경·삭제·타입 변환을 포함하되 ID 이름·타입·값은 유지한다. TEXT→INT64는 양끝 ASCII 공백·탭·CR·LF를 제거한 뒤 엄격하게 검사한다.
- 연계: 같은 디렉터리에 한 프로세스만 접근하고 다른 디렉터리는 동시에 사용할 수 있다. 벤치마크는 한 프로세스에서 수행하며 Phase 3은 서버 내부 스레드가 공유 엔진을 사용하고 내부 동기화한다. 세부 잠금은 후속 Phase에서 정한다.
- 검증: 실제 마감 결과는 아래 별도 기록을 따른다. 이번 기록은 문서화이며 C 구현·빌드·Windows API·SQL·저장/변환 기능 실행은 하지 않았다. GWT는 구현 시 검증할 조건으로 남긴다.
- 미완료·다음 행동: 계약·명세의 독립 검토와 문서 마감·사용자 최종 확인 뒤 CLI 인자·표시·종료 코드 계약을 정한다. 도구 설치·실제 검사 명령·하네스 연결·DBMS 구현은 여전히 후속 작업이다.
- 출처: 현재 저장·변환 선택과 문서화 계획에 대한 `Implement the plan` 승인, [저장 계약](storage-contract.md), [저장 인수 명세](testing/phase1-storage-acceptance.md). 앞선 스키마 기록의 당시 미선택 항목은 이 후속 선택으로 대체한다.
