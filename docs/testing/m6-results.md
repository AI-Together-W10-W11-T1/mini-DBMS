# M6 COUNT·GROUP BY·HAVING 결과 — DBMS-006

2026-11-18 수요일(Asia/Seoul). run `m6-native-20261118`, slim-v1. **구현·독립 QA와 사용자 완료 승인·하네스 DONE을 확인**했다. 새 Goal은 없다.

## 선택과 구현

사용자 A 선택은 COUNT·GROUP BY·HAVING 전체·기존 JOIN/WHERE/정렬/페이지 통합·선형 그룹 배열·독립 QA와 문서화 뒤 최종 확인 한 번이다. A는 집계 상태를 한 번에 연결하며 변경·검토 범위가 크다. 대안 B는 전체 COUNT부터 나누어 첫 결함 확인을 앞당기지만 그룹 도입 시 재작업·회귀·완료 절차가 늘어난다. 출처는 Master의 현재 승인 요약과 `.cache/m6/contract.json`, planner-result.json, 정식 knowledge_manager packet이다.

의미 원본은 [SQL 계약](../sql-contract.md)과 [인수 명세](phase1-sql-acceptance.md)다. 기존 InputScope·Predicate·QueryTail·값 비교/복사를 재사용하며 JOIN→WHERE→GROUP/COUNT→HAVING→출력→ORDER/OFFSET/FETCH 순서다. COUNT(*)는 모든 행, COUNT(column)은 비NULL 행을 INT64로 센다. SELECT/HAVING의 같은 입력 카운터를 공유하고 HAVING 전용 COUNT·COUNT 없는 GROUP BY도 처리한다. 전체 빈 집계는 COUNT0 한 행, 빈 그룹 집계는0행이다. 대응 NULL 키는 한 그룹이며 TEXT는 바이트 동등성을 따른다. 기본명·AS·중복 헤더·비그룹 출력·이름·끝절 검증은 빈 입력과 FETCH0에도 선행한다.

그룹 키와 결과 TEXT를 각각 소유하고 JOIN scratch는 카탈로그에서 빌린다. 초기화한 그룹/결과 행을 먼저 등록해 부분복사 실패도 회수한다. 실제 카운터 증가 전 INT64_MAX, 키/카운터 합·행·셀·바이트 연산 전 크기를 검사한다. 변경 기술 파일은 src/sql.c, src/internal.h, [M6 시험](../../tests/m6_cli.py), tests/unit_milestone.c, CMakeLists.txt, scripts/c_checks.py다. 최종 작성자/독립 기술 digest는 `256b944655a6c4556146e61cd430b6880967285188064f15ce88c60a46ed4f85`다.

## Given/When/Then

| 등록 검사 | Given / When / Then 요약 |
|---|---|
| M6-COUNT-01 | NULL 포함 users / COUNT·AS / 전체4·비NULL3·이름4와 INT64 헤더. SQL046 |
| M6-GROUP-01 | 기본 팀·NULL / 팀별 COUNT / 그룹별 전체·비NULL 수. SQL048 |
| M6-GROUP-02 | TEXT·NULL / 단일·복합 키·COUNT 없는 그룹·전체 키 별표 / 바이트 동등성·NULL 통합. SQL050 |
| M6-HAVING-01 | 기본 그룹 / AND·OR·괄호·숨은 COUNT / 우선순위·전체 HAVING |
| M6-HAVING-02 | 팀별9/10/11행 / HAVING 전용 COUNT / 10·11행 선택. SQL049 |
| M6-JOIN-01 | 중복 매치 / JOIN·WHERE 뒤 집계 / 매치 조합·비NULL 수 |
| M6-TAIL-01 | 그룹 결과 / COUNT AS 정렬·페이지 / 최종 이름·처리 순서 |
| M6-EMPTY-01 | 빈 입력 / 전체·그룹·HAVING·FETCH0 / COUNT0 한 행·그룹0행·정적 오류. SQL047 |
| M6-ERROR-01 | 이름·그룹·헤더·미지원 식·필수 누락·절 경계 / 오류 SQL / 계약 진단·부분결과 차단. SQL051 |
| M6-OVERFLOW-01 | 시험 seed·실제 helper / MAX-1·MAX·합성 크기 / 증가 경계·값 보존·사전 크기 검사 |
| M6-RESOURCE-01 | 선택 그룹/키/출력 fault / 실패 후 재조회 / 자원 오류·파일 불변·재시도·운영 hook 무시 |
| M6-OWNERSHIP-01 | TEXT/NULL 이벤트 / 요청·DB 종료·부분 실패 / 독립 수명·카탈로그 보존·정리 |
| M6-CLI-01 | 파일/stdin·table/JSONL·문장열 / 실행 / 출력·진단·앞성공 보존·후속 중단 |
| M6-REGRESSION-01 | 기존 데이터 / 집계 전후 변경·재시작·VALUES / 기존 기능 유지 |

CTest90개는 기존76개+M6 14개다. 운영 cmocka9개·fault13개는 공통 반복을 포함하며 wrapper와 별도 합산하지 않는다. SQL046~051 의미에 대응하며 전체204개 인수 완료는 미검증이다.

## 실제 검사와 실패 이력

정상 `python scripts/agent_harness.py --run m6-native-20261118 --summary run-check <red|targeted|regression>`의 증거는 `.cache/agent-harness/m6-native-20261118/commands/`에 있다. 매 호출 실제 configure/build/CTest와 새 보고서를 생성했다.

| 명령 | 결과 |
|---|---|
| 1-red | build 성공·exit8·COUNT/GROUP/OVERFLOW/OWNERSHIP 4개 assertion 실패·errors=[] |
| 2-targeted | 14/14 PASS |
| 3·4-regression | 작성자90/90 PASS, 정리·경계 보강 후 재검사 |
| 5-regression | 독립90/90 PASS 후 별도 음성7건 발견 |
| 6-red | 첫 반려 수정 전 실제 M6-ERROR-01 실패 |
| 7-regression | 수정 뒤90개 중M6-ERROR-01 실패, 필수 세미콜론 없는 fixture 기대 오류 확인 |
| 8-regression | 첫 수정 작성자90/90 PASS |
| 9-regression | 독립90/90·oracle30·음성28 PASS, 추가 경계3건 실패 |
| 10-red | 두 번째 반려 수정 전 실제 assertion 실패·build 성공·exit8 |
| 11-regression | 최종 작성자90/90 PASS·exit0·errors=[] |
| 12-regression | 최종 독립90/90 PASS·exit0·errors=[], review APPROVED·qa PASS |

초기 overflow Red는 집계 미구현으로 실패했다. 실제 증가 산술·크기 helper 경계와 부분복사 hook은 그 뒤 추가했고 후속 실제 C/CLI 검사에서 통과했다. 초기 Red를 산술 MAX 경계 결함 검출로 해석하지 않는다. 오류 수정은 각각 실제 기능 Red 뒤 수행했다. 환경/import 실패는 기능 Red에 포함하지 않는다.

최종 독립 추가는 시드20261118·6데이터×5모드 **Python oracle30/30**, **음성/경계28/28**, **추가 절/어휘/UNION10/10** 각각 PASS다. 전체 COUNT·복합 그룹·HAVING 전용 COUNT·self-join·정렬/페이지를 포함한다. 추가68개를 CTest90개에 더하지 않는다. 출처는 `.cache/m6/reviewer/independent-report-v3.json`, additional-report-v3.json, reviewer-result-v3.json이다. 최종 영수증의 run·phase·actor·기술 digest·full 범위·성공 build·보고서/로그 SHA를 대조했다.

누적 품질 반려는 **2회**다. 첫 M6-QA-001은 COUNT/HAVING 필수 요소 누락5건, M6-QA-002는 COUNT(age+1)1건, M6-QA-003은 HAVING 뒤 GROUP BY1건의 오류 분류다. 첫 음성21/28이었고 세 지적은 반려1회다. 첫 수정은 세미콜론/EOF 등 token kind를 구분했지만 지원 절도 TK_WORD임을 놓쳤다. 두 번째 검토의 HAVING ORDER BY, HAVING COUNT(*)> ORDER BY, HAVING COUNT(*)>0 AND FETCH가 남아 추가7/10, 누적 반려2회가 됐다.

Master가 원인을 재분석한 뒤 공통 clause_boundary로 필수 COUNT/HAVING 피연산자 자리의 지원 절14개를 구분했다. 시험은14경계×4 HAVING 위치와 COUNT 인수 자리, 어휘·미지원 식·UNION 대조군을 보강했다. 실제10-red→최소 수정→작성자/독립 전체회귀와 경계 재검토로 세 지적을 해결했다. 별도 fixture 오류는 세미콜론 없는 COUNT(age+1)의 기대였다. 계약상 SYNTAX_ERROR로 정렬했으며 제품 결함·추가 반려와 구분한다. 상세 출처는 implementer/reviewer-result-v2/v3.json이다.

## 측정·한계·다음 행동

최종 작성자11 영수증64.109s, report 단계 합63.704s(configure4.326·compile1.795·CTest57.572), 독립12 영수증63.812s다. 실제 호출은 Red3·targeted1·전체회귀8회이며 회귀7의 fixture 실패를 포함한다. 결함은 두 차례 독립 음성/경계 검토에서 발견했고 두 번의 오류 분류 수정과 fixture 기대 정렬이 있었다. 작성자는 attempt3 최소 수정 뒤 재작업 없음을 기록했다. 전체 재작업 wall-clock·실제 provider token은 제공되지 않았다. 시간 경계·캐시 조건이 달라 절감률·비용·TDD 인과효과는 미측정이다.

통과 입력 M·그룹 G·키 K·COUNT C에서 시간 O(MGK+MC), 상태 O(G(K+C))와 TEXT 바이트다. 선형 그룹 탐색은 최악 이차 시간, JOIN은 O(L×R)이며 그룹·결과 전체를 메모리에 보관한다. 작은 결정적 데이터·선택 fault·실제 helper와 합성 경계를 검사했다. INT64_MAX행 실행·대용량/100만 건 성능·모든 할당 실패·heap leak instrumentation은 미검증이다. 기존 저장·복구 한계는 [M5 결과](m5-results.md)에 보존한다.

2026-11-18 사용자가 M6 완료를 명시 승인했고 Master가 문서 변경 전에 정상 finish --user-ref로 DONE을 확인했다. 출처는 현재 사용자 지시의 Master 요약과 정식 state.json의 phase·completion_user_ref다. 이번은 DONE 뒤 별도 문서 유지보수이며 직접 scripts/dev_tools.py finish로 마감한다. 다음은 UNION 계획 협의이며 migrate와 Phase2/3 상세 설계는 해당 단계에서 정한다. Knowledge는 제품 검사를 재실행하지 않았다.

## 문서 마감

승인7개 문서만 갱신하고 Wiki 기존63개 출처에 결과/시험2개를 추가한다. 원문 대조·UTF-8·로컬 링크·공백·기술 SHA 불변을 확인하고 정상 run-check finish로 실제 scripts/dev_tools.py finish를 수행한다. 첫 마감 관측을 이 절에 한 번 보완하고 최종 영수증·반환 JSON에 남긴다. Graphify 정적 색인은 실행 검증을 대체하지 않는다.

첫13-finish는 exit0·6.734s·기술 digest 불변이다. 실제 Graphify task-end 색인1회(code35·노드1308·간선5131, LLM 추출 없음)와 Wiki missing_page=false·changed_sources=[]·page_changed=false를 확인했다. UTF-8·로컬 링크141개·기술6SHA·최종 검사 영수증/로그 SHA, CRLF를 인식한 승인 경로 한정 Git diff-check를 통과했다. Wiki 기존63개 중 변경 밖54개 hash를 보존하고 결과/시험을 추가해65개로 등록했다. 초기 stdin 인코딩 쓰기 시도2회와 기본 인코딩 JSON 읽기1회는 실행 전 작성도구 실패였고 UTF-8 직접 패치/읽기로 해결했다. 광범위 Git 검사의 기존 CRLF 진단은 범위 지정·명령별 cr-at-eol로 재확인했다. 명령 목록 원문 대조에서 회귀7의 fixture 실패를 바로잡아 기록했다. 이 보완 뒤 Wiki 재등록·최종 finish를 수행하며 결과를 반환 JSON에 남긴다.
