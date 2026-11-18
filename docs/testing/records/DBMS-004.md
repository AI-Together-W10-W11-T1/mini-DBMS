# DBMS-004 — M4 구현·독립 QA 작업 기록

2026-11-18 수요일(Asia/Seoul). 목적은 M3 다음 SELECT·독립 VALUES의 정렬·페이지를 연결하는 것이다. 사용자 A 선택으로 전체 구현·독립 QA·문서화 뒤 최종 확인 한 번을 진행한다. [M4 계획](../../implementation-m4.md), [실제 결과](../m4-results.md)가 상세 출처이며 개인 원장·대화 본문은 복제하지 않는다.

## 결정과 구현

다중 ORDER BY·ASC/DESC·NULLS FIRST/LAST, OFFSET·FETCH FIRST의 기존 SQL 계약을 구현했다. 최종 출력 이름만 결합하고 NULL 위치와 방향을 분리한다. 요청별 힙 정렬은 저장 행을 바꾸지 않으며 offset/fetch를 실제 행 수로 제한해 더하지 않는다. 제외 TEXT 해제와 보존 값의 소유권 이전을 적용했다. 전체 VALUES 검증을 먼저 하므로 FETCH 0도 후반 오류를 숨기지 않는다. 저장 포맷·공개 API·다섯 책임은 유지했다.

정식 `m4-native-20261117`에서 Master가 Planner `/root/m4_scope_planner`, Implementer `/root/m4_native_implementer`, Reviewer_QA `/root/m4_native_reviewer`, Knowledge Manager `/root/m4_native_knowledge`를 실제 배정했다. 수정과 독립 검토를 순서대로 수행하고 자식간 통신·재위임은 하지 않았다. run 이름은 17일 착수를 보존한다.

## 실제 검증과 실패 구분

최초 `1-build` exit 0 뒤 `2-red` exit 8·errors 빈 배열, 48/63 PASS·신규 기능 관련 15개 runtime assertion 실패를 확인했다. Master가 생산/공개 파일 일곱 개 SHA 일치와 실제 SQL 실패를 확인한 뒤 구현했다. M2의 ORDER 미지원 임시 기대값 갱신을 위한 경계 재계약·사용량 중단은 품질 반려가 아니다.

`3-build` 뒤 `4-green`은 61/63이었다. Python 3.9 Counter 비교·table 진단 파싱의 드라이버 두 문제를 동등한 검증으로 수정했다. 최종 `5-build`·`6-green`·`7-regression`과 독립 `8-build`·`9-regression`은 모두 exit 0, 63/63 PASS다. 독립 추가는 시드 20261118의 oracle 60·경계 10·오류 18, 총 88/88 PASS다. 단위 CTest 하나에 cmocka 함수 여섯 개가 있다. 최종 기술 digest는 `941a5349fba7bb6d163d32b698d6b9b585c37b76ccb6268f1183f9fbf5b1fa88`이다. M4 누적 품질 반려는 0회다.

Master의 별도 실제 M3 작성 DB→M4 정렬·별칭·VALUES·페이지·재시작 시연과 테이블 SHA 불변도 PASS다. 독립 88개와 중복 합산하지 않는다. `.cache/m4-master/` 역할 결과, 정식 commands, `.cache/m4-reviewer/extra-results.json`, `.cache/m4-demo/receipt.json`이 증거다.

## 문서·미완료·다음 행동

승인 문서 열 개와 Wiki54개 출처를 원문 대조했다. UTF-8·첫 로컬 링크156개·비문서 입력 SHA·Git 공백 검사를 확인했고 실제 정식10-finish exit0·Wiki 변경 원문/페이지 없음을 확인했다. 결과 기록 뒤 재등록·최종11-finish도 exit0으로 마쳤고 최종 링크157개를 확인했다. 이후 완료 상태 반영은 run 종료 뒤 별도 문서 유지보수이며 Master가 Wiki 재등록·마감 검사를 수행한다. [M4 결과](../m4-results.md)와 실제 영수증을 따른다. vendor 부분 추출·네 파일 no-symbol 경고와 정적 색인은 DBMS 실행의 대체 증거가 아니다.

**2026-11-18 수요일(Asia/Seoul) 사용자가 M4 완료를 승인했고 하네스는 DONE**이다. Master가 USER_REVIEW의 인수 문서를 수정하기 전에 정상 `finish --user-ref`로 완료 처리했다. 출처는 현재 완료 지시의 Master 요약과 정식 state.json의 phase·completion_user_ref다. 이번 문서 유지보수에서 기능 검사를 재실행하지 않았다. M1/M2/M3도 사용자 승인·DONE, M1 Goal은 complete이고 M4 새 Goal은 없다. 다음 행동은 JOIN 증분 계획 협의이며 후속 구현 승인은 아니다.

전체 결과 메모리 적재 비용, 전체 204개 인수 명세·대규모 성능·전원 차단/강제 종료 복구·모든 할당/handle 종료 실패 검증은 남아 있다. 기존 M3 CloseHandle 반환 한계와 미재현 상태를 보존한다. 기존 미커밋을 유지했고 이번 작업에서 Git 커밋·게시·추가 과금을 수행하지 않았다.
