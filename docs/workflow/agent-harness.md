# Master 조율형 하네스

`tools/agent-harness/roles.json`은 Master가 전달하는 역할 프로필이며 Codex 네이티브 자동 등록 설정이 아니다. Master 메인 1개와 Planner·Implementer·Reviewer_QA·Knowledge Manager 서브 4개로 책임을 나눈다. 설정 반영·모의 단위 검사와 실제 다중 에이전트 실행 검증은 구분한다.

사용자와 통신하는 창구는 Master뿐이다. 서브에이전트는 Master에게만 결과를 반환하며 동료 통신·재위임을 하지 않는다. Master가 각 역할에 승인 요약·고정 입력·필요한 이전 결과·수정 조건을 전달한다. 개인 승인 원장과 `.private` 자료는 전달하지 않는다.

한 작업의 단계는 `PLANNER → CONTRACT → IMPLEMENT → VERIFY → KNOWLEDGE → USER_REVIEW → DONE`이다. VERIFY의 Reviewer_QA 한 역할이 검토와 실행 검증을 맡되 두 결과는 구분해 남긴다. 실제 actor ID·입력 해시·결과·명령 영수증은 `.cache/agent-harness/<run_id>/`에 보존한다. 직접 원장을 수정해 단계를 건너뛰지 않는다.

CONTRACT에서 Master는 사용자의 선택 출처, 정확한 변경 파일, 완료 조건과 테스트를 확정한다. 사용자 승인 전 구현을 배정하지 않는다. 새 결정·범위 변경이 생기면 해당 작업을 멈추고 사용자 선택을 받아 재개하거나 계획을 다시 세운다. 기존 승인 범위를 실행할 때 같은 승인을 반복 요구하지 않는다. USER_REVIEW에서는 실제 결과·검증·제한·미완료를 보고하고 사용자 확인 뒤 DONE으로 이동한다.

## 병렬 실행과 파일 경계

동일 run의 의존 단계는 순차 실행한다. 서로 독립적인 run은 승인된 읽기/쓰기 범위를 지정하고 충돌이 없을 때 병렬 실행할 수 있다. 서브에이전트 요청 상한은 4명이지만 실제 동시 실행은 환경의 가용 슬롯을 넘지 않는다. 현재 전체 슬롯 4개 환경에서는 Master를 제외한 서브 최대 3명이 실행 가능하다. 슬롯 부족은 순서를 기다리는 사유이며 별도 사용자 선택으로 취급하지 않는다.

- Planner는 관련 입력을 읽고 계획을 반환하며 소스를 수정하지 않는다.
- Implementer는 승인된 구현·테스트 파일만 수정한다.
- Reviewer_QA는 고정된 소스를 읽고 검사한다. 검사 부산물은 승인된 `.cache` 경로에 두고 소스는 수정하지 않는다.
- Knowledge Manager는 검증된 근거를 바탕으로 승인된 문서만 수정한다.
- 쓰기끼리 또는 쓰기와 다른 작업의 읽기 범위가 충돌하면 병렬 배정을 막는다. 같은 작업의 구현과 검증도 겹치지 않는다. 공용 Wiki 같은 공유 문서는 Master가 직렬화한다.

읽기/쓰기 범위와 해시 검사는 운영 경계이며 OS 파일 권한이나 actor 인증을 대신하지 않는다. 작업별 고정 입력이 바뀌면 기존 검사 결과의 유효성을 재확인한다.

## 반려와 재개

Master가 결과를 인수하거나 문제·근거·수정 조건을 붙여 반려한다. 반려 횟수는 지적 ID 묶음이 아니라 같은 작업 단위로 누적한다. 한 번의 검토에 여러 지적이 있어도 반려 1회다. 두 번째 반려 후 Master가 원인을 재분석한다. 세 번째 반려 시 자동 재지시를 중단하고 사용자에게 세 번의 시도·남은 문제·원인 추정·해결 선택지를 보고한다.

환경 장애·사용자 선택 대기·요구사항 변경은 품질 반려와 별도로 기록한다. 중단 후에는 `resume --user-ref <사용자 결정 출처> --decision <결정 내용>`으로 재개하며, 새 계획이 필요하면 `--replan`을 지정한다. 누적 횟수는 보존한다. 세 번 반려 후 사용자가 재시도를 선택하면 다음 세 번의 시도를 허용하는 새 구간을 기록한다. 기술 입력이 그대로인 기록 단계 재시도는 상류 검증 근거를 보존하고, 재설계·구현 재개 시 영향받는 검사를 새로 수행한다. 최종 `finish --user-ref <사용자 확인 출처>`는 USER_REVIEW에서 DONE으로 이동한다.

Master가 `reject`를 호출할 때는 반려 이유·수정 근거와 `--completion-ref`에 실제 actor 완료 또는 중지 확인 출처를 제공한다. 살아 있는 actor를 방치한 채 claim을 해제하지 않는다. 세 번째 반려는 `escalation.json`을 생성하지만 사용자에게 자동 발송하지 않으며 Master가 내용을 확인해 보고한다.

## C 검사 연결

`python scripts/agent_harness.py --help`로 실제 지원 명령을 확인한다. 요구사항 ID는 `DBMS-001` 형식이다. 같은 요구사항에 run을 새로 만들어 반려 횟수를 초기화하지 않고 기존 run을 재개한다. 과거 v1 원장은 보존하며 자동 변환하지 않는다.

각 역할은 prepare → 실제 도구로 actor 생성 → dispatch → 결과 수신 → accept 순서로 배정·인수한다. prepare에는 `--available-slots <현재 런타임의 서브에이전트 허용 총 슬롯 수>`를 전달하며 설정 상한 4와 이 값 중 작은 한도를 적용한다. 현재 빈 슬롯 수가 아니라 해당 환경에서 동시에 허용하는 서브 총수를 전달한다. Reviewer_QA 결과에는 `review`와 `qa`를 별도 필드로 남겨 코드 검토와 실행 검증을 구분한다.

구현 언어 C, 표준 C23, 실행 환경 Windows 네이티브는 확정했다. 공식 LTS 우선, 없는 경우 최신 안정판 버전 고정 정책과 cmocka·CTest 테스트 구성 방향을 채택했다. 고정 도구 설치·특정 C23 실행과 실제 테스트 연결을 완료했다. 구성은 `tools/agent-harness/checks.json`의 red/green/regression/build 명령을 문자열 배열로 지정한다. `{python}`은 현재 Python 실행 파일로 치환한다. build는 실제 C 컴파일·링크 성공을 확인한다. 미설정 명령은 오류이며 운영 도구 검사를 DBMS 기능 검사로 대체하지 않는다.

기본 strict 구현에는 실제 기능 Red가 필요하며 slim-v1의 Red 조건은 아래 명시적 계약을 따른다. 모든 구현과 검증에는 최신 소스의 회귀 검사와 빌드, 종료 코드·실행 단계·actor·로그/보고서 해시 확인이 필요하다. Reviewer_QA는 자신의 단계에서 새로 검사하며 구현자의 결과만 재사용해 통과 처리하지 않는다. 최신 코드를 빌드한 뒤 테스트한다는 조건도 실제 검사 명령과 순서에 반영한다.

테스트 어댑터는 `AGENT_HARNESS_RESULT_FILE`에 새 JSON을 쓰고 `config.metadata.harnessRunId`를 `AGENT_HARNESS_RUN_ID`와 맞춰야 한다. `suites[].specs[]`의 title에는 승인한 테스트 이름, tests[].expectedStatus에는 passed, results[].status에는 실제 결과를 기록한다. skipped·실패·누락·오래된 보고서는 통과가 아니다. 이 로컬 호환 형식은 Playwright 설치를 요구하지 않는다. Mini React 전용 M-01~M-07 지표는 기본 비활성화 상태다.

빌드·임시 DB·검사 부산물은 run별 `AGENT_HARNESS_WORK_DIR` 아래에 둔다. `.cache` 밖에 부산물을 쓰면 소스 무단 변경 검사에 걸린다. 각 작업은 검사에 영향을 주는 헤더·소스·빌드 설정을 inputs 또는 승인한 변경 파일로 모두 선언한다. 다른 독립 작업의 변경을 현재 작업의 근거와 섞지 않는다. run별 CLI 갱신은 잠금으로 보호하며 프로세스 비정상 종료 후 잠금이 남으면 실제 실행 여부 확인 뒤 Master가 복구한다.

현재 build/red/green/regression은 scripts/c_checks.py의 실제 C 빌드·새 CTest 결과 어댑터에 연결돼 있다. 사용자가 승인한 [Phase별 상세 설계 원칙](../phase-design-policy.md)에 따라 현재 Phase만 상세 설계하고 전체 상세 설계를 일괄 고정하지 않는다. 설계 전용 kind=design 경로는 PLANNER→CONTRACT→KNOWLEDGE→VERIFY→USER_REVIEW→DONE 순서로 실제 문서 검사와 digest를 사용한다. 실제 Red/build 게이트를 형식적으로 우회하지 않는다.

DBMS 기능 범위와 단계별 재사용 요구사항은 [요구사항](../requirements.md)에 기록했다. 준비 상태를 보고하고 Plan 모드와 Goal은 사용자가 직접 시작한다. 이 문서 갱신으로 Goal을 생성하거나 설계·구현을 미리 시작하지 않는다.

검증 명령은 `python -m unittest discover -s scripts -p "test_*.py"`이다. 모의 actor·검사 명령을 쓰는 단위 검사는 실제 다중 에이전트 실행이나 DBMS 기능 통과를 증명하지 않는다.

## 2026-11-17 현재 실행·문서 경계

구현 Knowledge Manager 산출물은 승인한 정확한 docs/**/*.md|json과 루트 README.md/HANDOFF.md만 허용한다. 필수 요구사항 기록·구현/지식 파일 분리·AGENTS 및 제어/소스 파일 금지를 유지한다. design은 실제 문서 검사를 쓰며 가짜 C Red를 만들지 않는다. 구현은 기존 실제 기능 Red·build·회귀·독립 QA 관문을 그대로 유지한다.

유지보수 모의 검사는 작성자65·독립 reviewer61개 PASS였고 별도 bootstrap 반려2회를 해결했다. 실제 DBMS run m1-native-20261117은 native Planner·Implementer·Reviewer_QA·Knowledge Manager가 배정돼 진행했으며 최종 M1 코드/독립 QA 결과는 [실행 기록](../testing/m1-results.md)을 따른다. 이 run 반려는 Planner 반환 schema와 CLI stdout 결함으로 누적2회이며 해결했다. 다음 세 번째는 사용자 보고 관문이다.

문서 마감의 내용·순서·영향 검사와 기계 근거 참조는 [단일 문서 마감 정책](agent-workflow.md#문서-마감)을 따른다. 그 마감 뒤 Master가 결과를 인수해 USER_REVIEW로 이동하고 실제 사용자 확인을 받아야 DONE이다. 2026-11-17 사용자가 M1 완료를 명시 승인했고 정상 pause/resume/finish 절차로 해당 run DONE을 확인했다. 완료 후 문서 유지보수에도 같은 문서 마감 정책을 적용하며 Goal 최종 갱신은 Master가 수행한다.

## 명시적 slim-v1 계약

Master의 일반 CLI 호출은 `--run <id> --summary <명령>`으로 짧은 상태를 출력한다. 옵션이 없으면 기존 전체 JSON을 유지하며 원장·영수증·로그·보고서는 축약하지 않는다. dispatch와 verify-packet은 전달 원문을 유지한다.

기존 run과 workflow_policy 없는 계약은 strict 관문을 유지한다. 새 구현 계약만 `workflow_policy: "slim-v1"`과 `red_policy`의 required·reason·risk_flags를 명시할 수 있다. bugfix·storage_atomicity·data_structure_invariant·overflow·concurrency·ownership·harness_evidence는 실제 Red가 필수다. 나머지는 Red 생략 사유를 기록한다.

slim-v1 계약의 선택 `result_path`는 knowledge_paths에 포함한 정확한 docs Markdown 경로로 증분 결과를 모은다. Wiki·제어 파일은 사용할 수 없다. 요구사항 ID·검증·한계를 그 결과에 남기며 기본 strict의 요구사항별 record 경로와 finish/산출물 해시 검증은 유지한다.

`run-check targeted|red --tests <승인된 정확한 이름...>`은 중간 근거다. 구현 인수와 독립 QA에는 각각 새 configure/build/전체 CTest를 수행한 regression 하나가 필요하다. 보고서의 run·phase·actor·source·전체 범위·성공한 build 정보와 로그/보고서 해시를 대조한다. 별도 green/build 영수증을 만들어 채우지 않는다.

Master의 `amend-test-scope --contract <JSON>`은 approval_ref·contract_ref·reason·test_paths·기존 required_tests를 기록한다. 기존 정확한 테스트 파일의 승인된 기대값 정렬만 허용하며 의미 보존은 Master와 독립 검토가 판단한다. 활성 Implementer는 completion_ref의 actor_id·completion으로 Master의 완료 관측을 기록한 후 해제한다. 검사 근거를 무효화하고 다시 prepare하며 누적 반려 횟수는 보존한다. 삭제·기대값 약화·새 의미·제품 파일 변경은 이 경로로 승인하지 않는다.
