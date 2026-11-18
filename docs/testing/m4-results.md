# M4 실제 구현·검증 결과

2026-11-18 수요일(Asia/Seoul). `DBMS-004`, 정식 run `m4-native-20261117`의 구현·독립 QA 기록이다. 사용자 A 선택은 정렬·페이지 전체와 최종 확인 한 번이며 **사용자 완료 승인·하네스 DONE**을 확인했다. M1/M2/M3도 사용자 승인·DONE, M1 Goal은 complete다. M4 새 Goal은 생성하지 않았다.

## 구현과 실제 명령

SELECT·독립 VALUES에 다중 ORDER BY, ASC/DESC, NULLS FIRST/LAST, OFFSET/FETCH FIRST를 연결했다. 최종 출력 이름으로 결합하고 요청별 힙 정렬 후 안전하게 페이지를 자른다. 기존 공개 API·포맷1·CLI·저장 처리를 유지했다. 여섯 변경 파일과 13개 신규 GWT는 [M4 계획](../implementation-m4.md)을 따른다.

최종 기술 digest는 `941a5349fba7bb6d163d32b698d6b9b585c37b76ccb6268f1183f9fbf5b1fa88`이다. 아래 증거는 `.cache/agent-harness/m4-native-20261117/commands/`의 receipt·로그·test-report와 새 CTest JUnit이다. 캐시는 배포 아티팩트나 추적 소스가 아니며 삭제 후에는 고정 소스로 다시 실행해야 한다.

| 검사 | 실제 결과 | 명령 디렉터리 |
|---|---|---|
| 최초 build·Red | build exit 0, Red exit 8·errors 빈 배열, 48/63 PASS·15개 runtime assertion 실패 | `1-build`, `2-red` |
| 최소 구현 뒤 검사 | build exit 0, Green exit 8·61/63 PASS, 드라이버 결함 두 건 확인 | `3-build`, `4-green` |
| 최종 Implementer | build·Green·회귀 각각 exit 0, 63/63 PASS | `5-build`, `6-green`, `7-regression` |
| 독립 Reviewer_QA | 새 build·회귀 각각 exit 0, 63/63 PASS·errors 빈 배열, review APPROVED·qa PASS | `8-build`, `9-regression` |

첫 Red는 생산/공개 파일 일곱 개가 변경 전 SHA와 같을 때 신규 M4 13개, 기존 단위 CTest 안의 새 함수, 갱신한 M2-WHERE-03의 ORDER 성공 기대가 실패했다. 기존 cmocka 다섯 함수와 나머지 48개 등록 검사는 통과했다. Master가 실제 SQL 미지원 결과와 baseline을 확인하고 정상 confirm-red 후 구현을 진행했다.

최종 CTest 63개는 M1 16·M2 14·M3 20·M4 13이다. 단위 CTest 하나 안에는 기존 다섯 함수와 새 정렬/페이지 소유권·1바이트 입력 함수, 총 **cmocka 여섯 함수**가 있으며 이를 별도 CTest 여섯 개로 더하지 않는다. 최종 Implementer와 독립 QA의 기술 digest는 같다.

## 해결한 검사 문제와 독립 증거

- M2의 오래된 ORDER BY 미지원 기대값 하나가 승인된 새 기능과 충돌했다. `tests/m2_cli.py`를 추가한 정상 재계약 뒤 실제 성공 행 검증으로 바꿨다. NEEDS_DECISION 경계 조정과 서브에이전트 사용량 중단은 품질 반려가 아니며 **M4 누적 품질 반려는 0회**다.
- 첫 Green의 두 실패는 Python 3.9의 `Counter <=` 미지원과 table stderr를 JSON으로 파싱한 드라이버 결함이었다. 동등한 다중집합 포함 검사와 table 진단의 코드·문장 번호·행/열·후속 중단 검증으로 고쳤다. JSONL 구조 필드 검증은 유지했다. 최신 63개는 이 수정 뒤 실행한 결과다.
- 독립 추가 **88/88 PASS**: 시드 `20261118`, SELECT 30·VALUES 30의 Python 정렬/페이지 oracle 60, 긴 2,048키·중복 키·부호 페이지·NULL 전용 등 경계 10, 오류 18이다. `.cache/m4-reviewer/extra_qa.py`와 `extra-results.json`이 SQL·기대 비교·영수증을 보존한다. 임의 동률 순서를 요구하지 않도록 고유 마지막 키를 사용했다.
- Master 별도 시연도 PASS다. 실제 M3 독립 QA 실행 파일이 DB를 쓰고 M4 독립 `8-build`의 운영 실행 파일이 다중 정렬·출력 별칭·VALUES·페이지·별도 프로세스 재시작을 수행했다. `table_users.tbl` 전후 SHA는 모두 `1ff7159925622e120405387ebc1b5a99db49f4af94b37ca47639ca9b56131e52`다. `.cache/m4_demo.py`와 `.cache/m4-demo/receipt.json`을 따르며 독립 88개에 합산하지 않는다.

## 원본 인수 명세와 대응

| 원본 ID | 실행 대응과 범위 |
|---|---|
| P1-SQL-028 | M4-ORDER-01의 복합 WHERE 뒤 정렬, id=1/2/3 |
| P1-SQL-033 | M4-ORDER-04의 출력 AS 우선·숨겨진 이름 오류 |
| P1-SQL-034 | M4-ORDER-04의 비출력 정렬 이름/중복 출력 오류, 기존 M2-NAME 검사의 서로 다른 AS |
| P1-SQL-035 | M4-PAGE-01·CLI-01의 정렬 후 offset/fetch, id=2/4 |
| P1-SQL-036 | M4-ORDER-02의 DESC 기본 NULL 마지막·명시 FIRST |
| P1-SQL-037 | M4-PAGE-03의 무정렬 FETCH 건수와 원본 다중집합 부분집합 |
| P1-SQL-038 | M4-PAGE-03·ERROR-01의 무정렬 OFFSET 0·잘못된 절 순서 |
| P1-SQL-039 | M4-PAGE-02·ERROR-01의 0/행 수/INT64_MAX·음수/범위 초과 |
| P1-SQL-052~054 | 기존 M2 VALUES 회귀를 유지하고 M4-VALUES-01/02에서 정렬/작은 페이지·FETCH 0이 전체 타입/개수 오류를 숨기지 않는 조건 추가 |
| P1-SQL-060 | M4-ERROR-01의 끝절 뒤 UNION 두 오류에 부분 대응. 괄호 쿼리·UNION 전체 구현 완료를 뜻하지 않음 |

이 표는 해당 실행 의미의 대응이며 SQL 67·스키마 41·저장 48·CLI 48, 총 204개 전부의 통과 표가 아니다. 원문과 정확한 변형은 [SQL 인수 명세](phase1-sql-acceptance.md), [M4 시험](../../tests/m4_cli.py), 기존 [M2 시험](../../tests/m2_cli.py)을 함께 확인한다.

## 마감·한계·다음 행동

승인 문서·Wiki를 실제 원문과 대조하고 기존 Wiki 50개 출처에 M4 계획·결과·기록·시험을 추가했다. UTF-8 읽기·첫 로컬 링크156개·변경하지 않는 입력 SHA·Git 공백 검사를 확인했다. 정식 `10-finish` exit 0으로 Wiki `missing_page=false`, `changed_sources=[]`, `page_changed=false`를 확인했다. 결과 기록 뒤 원문 재등록·최종 `11-finish`도 exit 0으로 마쳤으며 최종 로컬 링크는157개였다. 이는 USER_REVIEW 인수 전 검증이다. 아래 완료 상태 반영은 run 종료 뒤 별도 문서 유지보수이며 Master가 Wiki 재등록·마감 검사를 수행한다.

마감 Graphify는 코드32개·노드1199·간선4701, LLM 의미 추출 없이 색인을 갱신했다. vendor yyjson 두 파일 부분 추출·네 파일 no-symbol 경고는 기존 정적 도구 한계다. 검토 당시 stale 경고도 최신 원문 대조로 구분했으며 정적 색인은 실행 증거를 대체하지 않는다.

현재 전체 결과 적재 뒤 정렬/페이지를 수행한다. 외부 정렬·top-K·100만 건 성능, 모든 할당 실패·주소 sanitizer·전원 차단/강제 종료 복구는 검증하지 않았다. 기존 M3 원본 읽기 helper의 CloseHandle 반환 미확인도 그대로이며 실제 OS 종료 실패를 재현한 것이 아니다. JOIN·집계·UNION·migrate·Phase 2/3는 후속이다.

2026-11-18 수요일 사용자가 M4 완료를 명시 승인했다. Master는 USER_REVIEW의 인수 문서를 수정하기 전에 정상 `finish --user-ref`로 DONE을 확인했다. 출처는 현재 완료 승인에 대한 Master 요약과 정식 state.json의 phase·completion_user_ref다. 이번 상태 교정에서는 기능 검사를 재실행하지 않았다. 다음 증분은 JOIN 계획을 사용자와 협의하며 현재 승인을 후속 구현 승인으로 확대하지 않는다. 커밋·게시·새 Goal 생성은 하지 않았다.
