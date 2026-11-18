# workflow slim-v1 유지보수 결과 — DBMS-090

2026-11-18 수요일(Asia/Seoul). 사용자 A 선택의 운영 유지보수에 대해 **사용자가 완료를 승인했고 하네스는 DONE**이다. Master가 문서 변경 전에 정상 CLI `finish --user-ref` exit0·DONE을 확인했다. 이번 갱신은 완료 후 상태 문서 유지보수이며 기능 검사를 재실행하지 않았다. 새 Goal·JOIN 구현은 없다.

## 범위와 결정

기존 strict·완료 run은 보존하고 새 계약의 명시적 `slim-v1`만 위험별 Red, 필요한 targeted 검사, 마지막 전체 통합 회귀를 적용한다. 고위험 `harness_evidence`인 이번 도구 유지보수에는 실제 Python Red가 필수다. Native smoke는 변경하지 않은 C 엔진의 운영 연결 검증이며 새 SQL 개발/TDD가 아니다.

- [하네스 계약](../workflow/agent-harness.md): run/phase/actor/source/정확한 전체 이름·build·증거 hash를 확인하고 독립 Reviewer의 새 전체 회귀를 요구한다. 부분 검사는 최종 근거가 아니다.
- [도구 계약](../workflow/dev-tools.md): 역할별 도구/옵션 서명 캐시만 재사용하고 호출마다 실제 configure/build/CTest와 새 JUnit·로그·보고서를 생성한다. docs JSON은 코드 그래프 지문에서 제외하고 자동 코드 변경 갱신과 Wiki 검토는 유지한다.
- 기대값 정렬은 Master의 정상 `amend-test-scope`로 승인·계약·정확한 기존 테스트·완료 관측·검사 무효화·누적 반려 보존을 기록한다. CLI 자체가 의미 보존을 증명하지는 못한다.
- `--summary`는 출력만 줄이고 전체 증거를 보존한다. 결과는 승인한 이 단일 Markdown 경로에 모은다. [역할 흐름](../workflow/agent-workflow.md)·[AGENTS](../../AGENTS.md)가 책임 경계의 원본이다.

## 실제 Red·수정·독립 검토

Master가 전달한 계약 `.cache/workflow-slim/contract.json`과 외부 유지보수 결과를 대조했다. 최초 fixture 오류·환경 경로 오류는 기능 Red에서 제외했다.

| 구간 | 실제 assertion Red | 근거 |
|---|---:|---|
| 하네스 초기·혼합 증거·WF-002 | 3·3·1 실패 | `harness-red.log`, `harness-wf002-red.log`, Master 관측 |
| 어댑터 초기·WF-001·짧은 layout | 2·2 subcase·1 실패 | `adapter-red.log`, `adapter-v3-red.log`, Master 관측 |

로그는 `.cache/workflow-slim/`의 로컬 실행 근거다. 상세 반환은 `harness-result-v2.json`, `adapter-result-v3.json`, `master-state.json`을 따른다. 캐시는 추적된 배포 산출물이 아니다.

Root 유지보수의 누적 품질 반려는 **2회**, 모두 독립 재검토로 해결했다. WF-001은 정상 CTest assertion 실패를 전역 infrastructure 오류로 분류하던 문제다. 실패 spec·비정상 종료는 보존하고 전역 errors와 분리했다. 이 수정 분기는 mocked CTest Python unit으로 확인했으며 새 native 실패를 유도한 증거는 아니다. WF-002는 기존 `tests/*.py` 기대값 정렬 누락이다. `.py` 허용만 추가하고 승인·경로·완료 관측·검사 무효화·반려 보존 방어를 유지했다.

두 번째 뒤 Master는 C 전용 amendment fixture와 얕은 경로 smoke가 실제 Python CLI/중첩 storage 경로를 놓친 원인을 재분석했다. 실제 경로 재현·좁은 수정·소스 고정 후 작성자/독립 전체 검사로 보완했다. Native smoke 반려는 **0회**다.

초기 native `2-regression`은 57/63 PASS, 중첩 fixture의 `WinError206` 환경 실패였다. 빌드 경로를 `IMPLEMENT|VERIFY/b/<서명 앞10자리>`로 줄이고 전체 서명 sidecar 불일치 시 재사용을 거절했다. 이 checkout의 중첩 사례를 검증했으며 임의로 더 긴 경로는 미검증이다.

## 최종 실행 근거

최종 기술 source digest는 `1a9ebf8b01394e59609aedbbbad7ebf479ffc120f6a0e82086679049489435c6`이며 작성자/독립 native 영수증과 보고서가 일치한다.

| 실제 검사 | 결과 | 관측 시간·조건 |
|---|---|---|
| 작성자 Python 운영 전체 | 91 PASS, exit0 | 33.014s |
| 독립 Python 운영 전체 | 91 PASS, exit0 | 31.573s; CTest subprocess fixture는 mock |
| 작성자 native targeted | M3-STORE-01 1/1 PASS, exit0 | 74.219s, IMPLEMENT cold |
| 작성자 native 전체 회귀 | CTest63/63 PASS, exit0 | 52.125s, IMPLEMENT build cache 재사용 |
| 독립 native 전체 회귀 | CTest63/63 PASS, exit0 | 108.438s, 별도 VERIFY cold |

실행은 `python scripts/agent_harness.py --run slim-smoke-20261118 --summary run-check targeted --tests M3-STORE-01` 및 같은 CLI의 `run-check regression`이다. 정식 `commands/3-targeted`, `4-regression`, `5-regression`의 receipt·test-report에서 정확한 이름/수·actor·phase·source·build·report hash를 대조했다. 독립 configure53.456s·compile7.648s·CTest46.817s는 report 단계 시간이며 CLI 총시간과 구분한다.

작성자 `native-implementer-result-v2.json`의 packet용 source_digest는 이전 packet 값이며 `actual_final_source_digest`와 실제 영수증이 위 최종값이다. 독립 `native-reviewer-result.json`은 APPROVED/findings=[]이며 최종12개 기술·정책 hash와 보호3157개 hash 불변(역사 run3138개 포함)을 확인했다. C/SQL·기존 tests63·M1~M4 원장은 변경하지 않았다. mutable 하네스의 새 gate PASS만으로 자체 합격을 판단하지 않고 외부 Python 실제 Red·독립 검토·실제 native 실행을 함께 사용했다.

## 문서 마감과 한계

Wiki 기존54개 출처 목록은 보존하며 변경 없는 원문 hash 일치로 이전 검토를 유지한다. 변경한 정책·도구·시험 원문과 이 결과를 대조해 필요한 출처를 추가하고 정상 `wiki-review --reviewed`·`run-check finish`로 마감한다. 실제 마감 관측은 아래 최신 항목과 해당 receipt를 따른다.

<!-- FINISH_OBSERVATION -->
정상 `run-check finish`의 `commands/6-finish/receipt.json`은 exit0·7.469s, 최종 기술 digest 불변, Wiki changed_sources=[]/page_changed=false였다. 유지보수 코드 변경의 자동 Graphify 갱신은 실제 1회(exit0·6.511s)였고 events.jsonl의 graph-build 누계는33→34다. vendor 부분 추출·네 파일 no-symbol은 정적 도구 한계다. 이 마감 관측 추가 후 Wiki를 재등록하고 최신 finish를 한 번 더 수행한다. 최신 영수증은 `commands/7-finish/receipt.json`이며 문서/JSON 변경만으로 추가 Graph 호출이 생겼는지는 해당 실행 후 cache 결과에 기록한다.

동일 M4 DONE status 관측은 full21209byte/summary1244byte였다(`cli-output-observation.json`). byte는 provider token이 아니다. 실제 provider token·비교 절감률·TDD 효과는 미측정이며 비용/성능 인과효과를 주장하지 않는다. cold/warm·검사 범위가 다른 위 시간은 동등 조건 비교가 아니다.

전체204개 SQL 인수·새 JOIN·대용량·전원 차단·모든 할당/handle 실패는 이번 범위가 아니다. 기존 이력은 [M1](m1-results.md)·[M2](m2-results.md)·[M3](m3-results.md)·[M4](m4-results.md)와 각 원래 계약에 보존한다. 다음 행동은 JOIN 증분 계획 협의이며 JOIN 구현 승인은 아니다. 다음 증분에서 시간·결함 발견 단계·재작업·가능한 실제 token을 관측한다.
