# M7 UNION·UNION ALL 결과 — DBMS-007

2026-11-18 수요일(Asia/Seoul). run `m7-native-20261118`, slim-v1. 구현·독립 QA APPROVED이며 **사용자 완료 승인·정상 하네스 DONE을 확인**했다. 새 Goal은 없다.

## 승인과 핵심 불변식

현재 사용자 A 선택을 Master가 전달한 정식 계약·packet에 따라 UNION/ALL 전체, 혼합 좌결합, 기존 SELECT/VALUES/JOIN/집계와 전역 ORDER/OFFSET/FETCH를 구현했다. 기존 heap sort를 전체 출력 열의 키로 재사용하고 각 UNION에서 누적 결과 전체를 정렬·인접 중복 제거한다. ALL은 다중집합의 중복을 보존한다. 첫 분기 이름, NULL 전용 열의 구체 타입 추론, 빈 분기·FETCH0에서도 컬럼 수/타입/이름 검증을 유지한다. 의미 원본은 [SQL 계약](../sql-contract.md)과 [인수 명세](phase1-sql-acceptance.md)의 P1-SQL-055~060이다.

분기와 전역 tail 성공 뒤 이벤트를 노출한다. 합계 행 수·셀 수·바이트 수를 연산 전에 검사한다. 오른쪽 TEXT 소유권을 누적 결과로 옮겨 원본 소유권을 비우고, 버린 중복 TEXT는 해제·0 초기화한다. 이동 슬롯도 비워 유지 행이 하나의 소유 prefix를 이루며 실패 시 분기 잔여와 누적 결과를 정리한다. 관련 원문은 src/sql.c의 union_size·union_merge·query, src/internal.h의 시험 helper와 tests/unit_milestone.c다.

승인된 기술 변경은 src/sql.c, src/internal.h, tests/m7_cli.py, tests/unit_milestone.c, tests/m6_cli.py, CMakeLists.txt, scripts/c_checks.py다. 사용자 A 승인에는 M6의 정상 집계 UNION 미지원 기대 1건을 중복 제거 성공으로 정렬하는 변경이 포함된다. 잘못된 절 순서·COUNT/HAVING 피연산자 경계 기대는 보존했다. 최종 작성자/독립 기술 digest는 `c1dfc835b9a1af57685e5535dd6f9eeb234976a341b708fcb08672e5699d989c`다.

## Given/When/Then

| 등록 검사 | Given / When / Then 요약 |
|---|---|
| M7-UNION-01 | 전체 행 중복·같은 ID 다른 TEXT·NULL / UNION / 전체 행 동등성·UTF-8 바이트 구분. SQL055~056 |
| M7-ALL-01 | 양 분기 중복·NULL / ALL / 각 중복 개수 보존. SQL055 |
| M7-MIXED-01 | ALL·UNION 혼합 / 좌결합 / UNION마다 누적 전체 중복 제거·후속 ALL 보존. SQL057 |
| M7-TYPE-01 | NULL 전용·구체 타입 / 결합 / 타입 추론·후속 충돌·열 수 불일치 거절. SQL056·058 |
| M7-EMPTY-01 | 빈 SELECT·FETCH0 / 오류 분기 결합 / 정적 타입·이름·열 수 검증. SQL058 |
| M7-SELECT-01 | SELECT·VALUES·JOIN·집계 / 결합 / 기존 결과·첫 이름 유지 |
| M7-TAIL-01 | 혼합 결과·첫 별칭 / 전역 정렬·페이지 / 처리 순서·오른쪽 이름 거절. SQL059 |
| M7-ERROR-01 | RHS/ALL 누락·절 경계·괄호·어휘 / 파싱 / 계약별 진단·부분 결과 차단. SQL060 |
| M7-OVERFLOW-01 | 실제 helper·합성 차원 / 행 덧셈·셀/바이트 곱 / 경계 거절·정상 크기 |
| M7-RESOURCE-01 | merge·sort key·merge 뒤 fault / 실행·재조회 / 자원 오류·파일 불변·운영 hook 격리 |
| M7-OWNERSHIP-01 | 중복 owned TEXT·NULL / 압축·다음 요청·종료 / 이벤트 독립 수명·실패 정리 |
| M7-CLI-01 | 파일/stdin·table/JSONL·문장열 / 실행 / wire 출력·앞 성공 보존·뒤 중단 |
| M7-REGRESSION-01 | 기존 데이터·변경·재시작 / UNION 연계·전체 회귀 / 기존 기능 유지 |

CTest103개는 기존90개와 M7 13개다. 공통 native wrapper의 cmocka 반복은 별도 합산하지 않는다. 전체204개 인수 완료는 미검증이다.

## 실제 검사와 실패 이력

정상 `python scripts/agent_harness.py --run m7-native-20261118 --summary run-check <red|targeted|regression>`은 매번 실제 configure/build/CTest와 새 보고서를 생성했다. 근거는 `.cache/agent-harness/m7-native-20261118/commands/`와 `.cache/m7/implementer-result.json`, reviewer-result.json이다.

| 명령 | 실제 결과 |
|---|---|
| 1-red | build 성공·exit8·UNION/MIXED/OVERFLOW/OWNERSHIP 4개 실제 assertion 실패 |
| 2-targeted | 12/14 PASS. 예약어 first/second를 성공 별칭으로 사용한 새 fixture2건 오류 |
| 3-regression | initial_name/later_name으로 fixture 수정 뒤 작성자103/103 PASS·exit0 |
| 4-regression | 독립 새 configure/build/CTest103/103 PASS·exit0·review APPROVED·qa PASS |

최초 overflow Red는 UNION 미구현 증거였다. 이후 실제 production merge-size helper를 native 시험으로 호출해 행 덧셈·셀/바이트 크기·0행/0너비 경계를 통과했다. 최초 Red를 산술 overflow 결함 검출로 확대하지 않는다. fixture 재작업1회는 예약어 거절 계약을 유지하며 시험 입력을 고친 것이다. 제품 결함 관측0·누적 품질 반려0회다.

독립 추가는 **타입 포함 oracle129/129·음성99/99·stop-on-error1/1**, 총229/229 PASS다. `.cache/m7/reviewer/independent.py`와 independent-report.json을 대조했다. 최초 runner 호출1회는 새 보고서 생성 전에 실패해 SQL 실행 전 종료했고 전체회귀 완료 뒤 재실행했다. 환경 실행 실패를 기능 Red로 계산하지 않는다. 최종 영수증의 run·phase·actor·digest·full 범위·build와 보고서/로그 SHA를 대조했다.

비차단 관찰1건은 tests/m7_cli.py의 overflow_01이 ORDER 없는 ALL 결과를 `[1,2]` 순서로 단정하는 점이다. 후속 시험 유지보수에서 타입 포함 다중집합 비교로 교정할 후보이며 제품 결함·품질 반려로 처리하지 않았다.

## 측정·한계·다음 행동

작성자 영수증은 Red66.187s·targeted18.500s·최종회귀88.859s(CTest80.27s)다. 독립 회귀143.891s, 보고서 단계 합143.324s(configure50.804·compile7.810·CTest84.695), 별도 독립229건4.120s다. 실제 호출은 Red1·targeted1·전체회귀2회다. 새 fixture 오류2건을 targeted에서 발견해1회 수정했다. 실제 provider token과 전체 재작업 wall-clock은 제공되지 않았다. 캐시·시간 경계 차이가 있어 성능 절감률·비용·TDD 인과효과는 미측정이다.

분기 누적 행 Rᵢ·출력 열 K에서 dedup 시간은 O(ΣRᵢ log Rᵢ K)에 TEXT 비교 바이트 비용을 더한다. 누적/현재 분기 셀과 TEXT 전체를 메모리에 보관한다. 합성 크기 경계·선택 fault·이벤트 수명을 검사했으며 실제 SIZE_MAX 할당·모든 할당 실패·heap leak instrumentation·대용량/100만 건 성능은 미검증이다. 저장/복구의 기존 한계는 [M6 결과](m6-results.md)를 따른다.

현재 사용자 ‘M7 완료 승인’을 Master가 정상 finish로 반영해 DONE·누적 품질 반려0회를 확인했다. 출처는 현재 사용자 지시의 Master 요약과 정식 완료 상태다. 다음은 migrate·관리 기능과 전체 인수 회귀 계획 협의다. 후속 Phase2/3 상세 계획은 해당 단계에서 사용자와 정한다. Knowledge는 제품 검사를 재실행하지 않았다.

## 문서 마감

승인7개 문서만 갱신하고 Wiki 기존65개 출처를 보존하며 M7 결과/시험을 추가한다. 원문 대조·UTF-8·로컬 링크·기술7SHA 불변·승인 경로 cr-at-eol Git 공백 검사를 수행한다. Wiki 검토 등록 뒤 정상 run-check finish로 실제 scripts/dev_tools.py finish를 실행한다. 첫 관측을 이 절에 한 번 보완하고 Wiki 최종 재등록·마감 영수증은 반환 JSON에 둔다. 정적 Graphify는 기능 실행 근거를 대체하지 않는다.

첫5-finish는 exit0·6.766s·기술 digest 불변이다. 실제 Graphify task-end 색인1회(code36·노드1331·간선5234, LLM 추출 없음)와 Wiki missing_page=false·changed_sources=[]·page_changed=false를 확인했다. UTF-8 Markdown6개·로컬 링크144개·기술7SHA·승인 경로 Git 공백 검사를 통과했다. 기존65개 출처 목록을 보존하고 결과/시험을 추가해67개로 등록했다. 이 보완 뒤 Wiki 재등록·최종 finish를 수행하며 최종 결과는 반환 JSON에 남긴다.

완료 후 이번 상태 갱신은 승인7개 문서의 유지보수다. 기존 기술7개·native 원장·이전 Knowledge 결과를 보존하고 Wiki67개 원문 재등록 뒤 직접 `python scripts/dev_tools.py finish`로 마감한다. UTF-8·로컬 링크·범위별 공백·기술/원장 hash 불변과 실제 마감 결과는 `.cache/m7/completion-result.json`에 남긴다.
