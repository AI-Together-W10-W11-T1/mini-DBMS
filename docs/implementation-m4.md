# Phase 1 M4 구현 계획과 불변식

2026-11-18 수요일(Asia/Seoul). 사용자는 **A — 정렬·페이지 전체 구현 뒤 독립 QA·문서화와 최종 확인 한 번**을 선택했다. 구현·독립 QA와 사용자 완료 승인을 마쳤고 Master가 정상 하네스 CLI로 DONE을 확인했다. 승인 출처는 현재 완료 지시의 Master 요약과 정식 state.json의 phase·completion_user_ref다. 정식 run은 `m4-native-20261117`, 요구사항은 `DBMS-004`다. run 이름은 착수일을 유지한다. M4 새 Goal은 생성하지 않았다. 실제 증거는 [M4 결과](testing/m4-results.md)를 따른다.

의미의 원본은 [SQL 계약](sql-contract.md)과 [SQL 인수 명세](testing/phase1-sql-acceptance.md)다. 새 SQL 의미를 선택한 것이 아니라 이미 합의한 정렬·페이지를 구현했다. 작은 대안 B는 정렬을 먼저 확인한 뒤 페이지를 별도 구현하는 방식이며, 중간 확인이 가능한 대신 계획·검토·기록이 두 번 필요해 미선택했다.

## 구현 경계와 순서

변경은 `src/sql.c`, `tests/m4_cli.py`, `tests/unit_milestone.c`, `CMakeLists.txt`, `scripts/c_checks.py`, `tests/m2_cli.py` 여섯 파일이다. M2의 오래된 ORDER BY 미지원 기대값 하나를 실제 성공 검증으로 교체하기 위해 정상 NEEDS_DECISION→재계약을 거쳤다. 나머지 기존 검증 조건과 50개 등록을 유지했다. 저장 형식·공개 API·engine/sql/catalog/storage/cli 다섯 책임·의존성은 그대로다.

1. 신규 13개 CLI 검사와 여섯 번째 cmocka 함수를 먼저 추가하고 실제 build→Red를 수행한다. Master가 생산 소스 일치와 기능 실패를 확인한 뒤 구현한다.
2. SELECT와 독립 VALUES의 공통 `QueryTail` 파싱·최종 출력 이름 결합을 연결한다. 모든 VALUES 행과 타입을 검사한 뒤 정렬·페이지를 적용한다.
3. 요청별 힙 정렬과 범위 내 페이지 자르기를 적용한다. Green·회귀 뒤 소스를 고정하고 Reviewer_QA가 새 build·회귀와 독립 기준값 비교를 수행한다.
4. Knowledge Manager가 계약·소스·시험·명령 영수증을 대조해 기록·Wiki·finish를 마감한다. Master가 최종 사용자 확인을 받은 뒤 DONE으로 전이한다.

## 핵심 동작과 이유

- 다중 키, 기본 ASC와 명시 ASC/DESC, 키별 NULLS FIRST/LAST를 지원한다. 양 방향 모두 기본 NULL 마지막이다. 동률 사이의 안정된 순서는 약속하지 않는다.
- 정렬 이름은 ASCII 대소문자를 무시한 **최종 출력 이름**만 참조한다. AS는 원래 이름을 대체하며 입력 테이블 fallback·숫자 위치 정렬은 없다.
- INT64는 뺄셈 없이 비교하고 TEXT는 UTF-8 바이트를 비교한다. 비교 결과의 부호를 정규화한 뒤 DESC를 적용하며 NULL 위치는 방향 반전과 분리한다.
- 힙 정렬은 요청의 결과 행에서 수행하며 전역 comparator 상태·재귀·별도 행 배열을 사용하지 않는다. 저장 행의 순서와 파일 바이트를 바꾸지 않는다.
- 절 순서는 `ORDER BY → OFFSET n ROWS → FETCH FIRST n ROWS ONLY`다. OFFSET은 0도 정렬이 필수이고 FETCH는 단독 사용 가능하다. n은 비음수 INT64다.
- offset을 실제 행 수 안으로 제한하고 fetch를 남은 행 수 안으로 제한한다. 두 수를 더하지 않으므로 INT64_MAX 조합에도 덧셈 overflow가 없다. 제외된 TEXT를 해제한 뒤 보존 구간의 소유권을 옮기고 row_count를 갱신한다.
- 빈 결과·FETCH 0도 이름·타입·후반 VALUES 오류를 숨기지 않는다. 성공 전 부분 결과를 노출하지 않으며 요청·DB 종료 후에도 결과 이벤트가 값을 소유한다.
- 변경문이 쓰는 기존 `finish_query` 의미를 넓히지 않는다. UPDATE·DELETE에 정렬·페이지 절을 새로 허용하지 않는다.

## Given/When/Then

기본 모델은 `users(id INT64,name TEXT,age INT64,team TEXT)`다. 아래 각 이름은 실제 CTest 등록과 일치하며 변형별 기대값은 [시험 원문](../tests/m4_cli.py)을 따른다.

| ID | Given | When | Then |
|---|---|---|---|
| M4-ORDER-01 | 삽입 순서가 키 순서와 다른 users | 단일·다중 키와 복합 WHERE 정렬 | 숫자/문자열 사전식 순서, 고유 마지막 키로 동률 판정 |
| M4-ORDER-02 | NULL이 섞인 여러 키 | 양 방향·기본/명시 NULL 위치 | 방향과 별개인 키별 NULL 위치 |
| M4-ORDER-03 | INT64 양끝·빈 문자열·공백·대소문자·한글·CR/LF | ASC/DESC | overflow 없는 숫자순·UTF-8 바이트순과 값 보존 |
| M4-ORDER-04 | 출력 AS·원철자 스키마 | 별칭/대소문자·숨겨진 이름·숫자 위치 참조 | 최종 이름만 결합, INVALID_ORDER_BY 또는 기존 중복 출력 오류 |
| M4-PAGE-01 | 기본 네 행 | 정렬 단독·OFFSET·FETCH·조합 | 정렬→건너뛰기→제한, 대표 조합 id=2/4 |
| M4-PAGE-02 | 빈/비어 있지 않은 테이블 | FETCH 0·행 수 이상 OFFSET·INT64_MAX 조합 | 올바른 빈/나머지 결과와 타입 메타데이터 |
| M4-PAGE-03 | 네 행 | 무정렬 FETCH와 무정렬 OFFSET 0 | FETCH 건수·다중집합 부분집합, OFFSET은 INVALID_PAGINATION |
| M4-ERROR-01 | 준비된 DB | 음수/범위 초과 수·누락 토큰·잘못된 절 순서 | 페이지/문법 오류, 부분 결과·저장 변경 없음 |
| M4-ERROR-02 | 빈 테이블·WHERE 0행·FETCH 0 | 없는 정렬 이름·숨겨진 별칭·타입 오류 | 행 수와 관계없이 정적 오류 |
| M4-VALUES-01 | NULL/INT64/TEXT·NULL 전용 열 | 공통 정렬·페이지 | 위치별 추론 타입과 값 유지, 파일 저장 없음 |
| M4-VALUES-02 | 후반 VALUES의 잘못된 타입/개수/정수 | FETCH 0 또는 작은 FETCH | 후반 오류 유지, 부분 결과 없음 |
| M4-CLI-01 | 파일/stdin·table/JSONL | 앞 성공 뒤 잘못된 정렬과 후속 변경 | 정확한 행/건수·문장/위치, 첫 오류 중단·앞 성공 보존 |
| M4-REGRESSION-01 | 기존 데이터·변경문·소유 결과 | 정렬/페이지 뒤 UPDATE·DELETE·재시작·종료 | 저장 순서/포맷·DML·소유권 보존, 기존 50개 회귀 유지 |

## 보류와 확인

JOIN·집계·UNION은 후속 결과의 최종 컬럼/행에 같은 끝절 처리를 연결할 수 있도록 경계만 유지한다. 해당 기능과 migrate, Phase 2 B+ 트리·자동 ID·100만 건, Phase 3 API·Thread Pool은 구현하지 않았다.

현재 조회는 일치 결과 전체를 메모리에 만든 뒤 정렬·페이지를 적용한다. 외부 정렬·top-K·대규모 성능·전체 204개 인수 사례·모든 할당 실패·전원 차단 복구 검증으로 확대하지 않는다. M4 완료 뒤 다음 행동은 JOIN 증분 계획 협의이며 후속 구현 승인으로 확대하지 않는다.
