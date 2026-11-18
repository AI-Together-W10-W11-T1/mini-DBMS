# M3 실제 구현·검증 결과

2026-11-17 화요일(Asia/Seoul). 요구사항 `DBMS-003`, 정식 run `m3-native-20261117`의 **구현·독립 QA 통과, 사용자 완료 승인·DONE** 기록이다. 사용자는 A로 UPDATE·DELETE·SET 계산·교체 실패 검증 전체와 최종 결과 확인 한 번을 선택했다. M1/M2는 사용자 승인·DONE이며 M1 Goal은 complete다. M3 새 Goal은 생성하지 않았다.

## 구현과 불변식

UPDATE의 비ID 다중 대입·리터럴/NULL·컬럼 복사·INT64 덧뺄셈/괄호와 DELETE를 기존 WHERE에 연결했다. 모든 이름/타입을 먼저 검사하고, SET 우변은 동일 old row에서 평가한다. 중간 overflow는 C signed overflow를 실행하기 전에 판정한다. 같은 값의 UPDATE도 일치 행 수로 세며, 미일치 0행은 성공이다. 삭제 ID의 명시 재사용도 지원한다.

변경 결과를 별도 메모리와 임시 파일에 완성하고 flush·종료한 뒤 백업을 지정한 실제 ReplaceFileW 성공을 반영 경계로 삼는다. 그 전 오류는 원본을 보존하거나 확인 후 복원한다. 예상 밖 파일·복원/확인 실패는 파일·표식을 보존하고 재시작을 차단한다. 반영 뒤 정리·출력 실패는 성공과 전체 건수를 유지한다.

공개 API·다섯 책임·바이너리 포맷1·CLI 의미를 유지했다. 변경은 `src/sql.c`, `src/storage.c`, `src/internal.h`, `tests/m3_cli.py`, `tests/unit_milestone.c`, `CMakeLists.txt`, `scripts/c_checks.py`다. 최종 기술 digest는 `a65b0137b85ca8a58dc59ad09195f899973f1795cb44d82a29c546205b7ebeb8`이다. [계획·GWT](../implementation-m3.md)가 구현 경계와 검사 이름을 설명한다.

## 실제 명령과 결과

아래는 `.cache/agent-harness/m3-native-20261117/commands/` 기준이다. 각 디렉터리의 `receipt.json`, stdout/stderr, 시험의 `test-report.json`과 새 CTest JUnit이 증거다. 캐시 삭제 뒤에는 고정 소스로 재실행해야 하며 캐시는 배포물/추적 소스가 아니다.

| 검사 | 실제 결과 | 증거 |
|---|---|---|
| 최초 build·기능 Red | build exit 0, Red exit 8·errors 빈 배열. 신규 M3 20개와 추가 단위 함수의 runtime 실패 | `1-build`, `2-red` |
| 최초 Green | build·Green exit 0, 50/50 PASS | `3-build`, `4-green` |
| 내부 증거·진단 보완 뒤 고정 검사 | build·Green·회귀 exit 0, 50/50 PASS | `9-build`, `10-green`, `11-regression` |
| 1차 독립 QA | 새 build·회귀 50/50 PASS, 추가 74 PASS. 별도 음성 실험에서 M3-QA-001 발견 | `12-build`, `13-regression` |
| 진단 결함 Red | build exit 0, Red exit 8·errors 빈 배열. M3-STORE-01만 실제 assertion 실패, 나머지 49 PASS | `14-build`, `15-red` |
| 최종 Implementer | build·Green·회귀 각각 exit 0, 50/50 PASS | `16-build`, `17-green`, `18-regression` |
| 최종 독립 Reviewer_QA | 새 build·회귀 각각 exit 0, 50/50 PASS·errors 빈 배열. review APPROVED, qa PASS | `19-build`, `20-regression` |

최초 Red 때 생산 C 세 파일은 변경 전 snapshot과 같았고 기존 CLI 29개·기존 cmocka 함수 4개는 통과했다. 단위 CTest 하나 안의 새 M3 함수가 미지원 UPDATE에서 실패했다. 재작업 Red도 수정 전 생산 소스에서 진단 assertion 실패를 확인한 뒤 Master가 정상 confirm-red로 기록했다. 환경/import 실패를 기능 Red로 세지 않았다.

현재 총 50개 CTest는 기존 M1 16개·M2 14개와 M3 20개다. 단위 CTest 하나에는 cmocka 함수 5개가 있으며 이를 별도 CTest 5개로 더하지 않는다. 최종 Implementer·독립 검사는 모두 같은 최종 기술 digest다.

## 해결한 실패와 독립 추가 검사

**M3-QA-001, 품질 반려 1회**. 준비 완료를 SELECT 응답으로 확인한 실제 운영 프로세스에서 원본 테이블을 Windows share=0 읽기 handle로 잡고 두 번째 UPDATE를 보냈다. 원본 읽기가 실패할 때 exit 4·원본 보존은 맞았지만 진단이 `phase=prepare`, `source=stdin`으로 나왔다. 공통 읽기 helper의 준비용 문맥이 SQL 실행 경로에 남은 것이 원인이다.

M3 저장 호출 경계에서 기존 코드·경로를 보존하고 `phase=execute`, `source=data`로 보정했다. 같은 실제 실패를 UPDATE와 DELETE의 M3-STORE-01에 추가해 실패→수정→회귀를 확인했다. 독립 재검토는 실제 경로·문장2·command·committed=false·원본 바이트·재시작까지 통과했다. 공통 helper·공개 계약은 변경하지 않았다.

최종 독립 추가 검사는 76/76 PASS다. `.cache/m3-reviewer/attempt2/extra-results.json`의 산술 Python oracle 60개·정적/잘못된 입력 12개·실제 OS 공유 거부 2개와 `runtime-read-fixed-receipts.json`의 실행 중 읽기 진단/재시작 2개다. seed는 20261117이다. 1차 74개와 결함 재현 영수증은 별도로 보존하며 합산해 독립 표본 수를 부풀리지 않는다.

정식 검사는 old row 컬럼 교환, NULL, 경계 INT64·중간/후반 overflow의 전체 취소, 빈 테이블 정적 오류, 0행/같은 값 건수, 전체 삭제와 ID 재사용, 2,048층 괄호·512항 식, 중간 위치 ID, 다른 테이블 바이트 불변, 조각 입력과 소유 이벤트를 포함한다. 성공 교체는 실제 ReplaceFileW 호출이다. 희귀 1175/1176/1177은 문서화된 파일 배치·실패 상태를 주입한 검사이며 해당 오류가 OS에서 자연 발생했다고 주장하지 않는다.

Master의 `.cache/m3_demo.py`, `.cache/m3-demo/receipt.json`은 실제 M2 실행 파일로 만든 DB를 최종 M3 실행 파일로 열어 UPDATE 2행·DELETE 1행·삭제 ID INSERT 1행과 별도 프로세스 재시작을 확인한 증거다. 최종 행·타입·NULL이 일치했다.

## 원본 인수 명세와 대응

| 기존 명세 | 실제 대응 범위 |
|---|---|
| P1-SQL-016~027 | M3 UPDATE/DELETE·old row·계산/NULL·overflow·ID/대상·건수/WHERE 검사. INSERT/VALUES 관련 변형은 기존 M1/M2 회귀와 함께 대조 |
| P1-STORE-021~024 | 후반 overflow·임시 준비 실패·old row 교환·실제 교체·재시작 |
| P1-STORE-025~027 | 희귀 교체 실패 상태 주입·복원·예상 밖 파일/확인 실패 보존 |
| P1-STORE-018/028 | 이번 UPDATE/DELETE의 표식 준비와 반영 후 정리 실패에 해당하는 부분 |
| CLI의 실행/정리/복원/출력 실패 계약 | 파일/stdin·table/JSONL·진단 문맥/건수·후속 SQL 중단과 종료 우선순위에 해당하는 assertions |

이는 원문과 실제 assertions의 의미 대응이며 SQL 67·스키마 41·저장 48·CLI 48, 전체 204개 사례 완료를 뜻하지 않는다. 관리/변환·정렬/JOIN 등 미구현 기능이 섞인 원본 사례는 부분 검증으로 남긴다.

## 한계·마감·다음 행동

기존 md_read_file은 원본 읽기 handle의 CloseHandle 반환값을 확인하지 않는다. 독립 소스 검토로 확인한 비차단 한계이며 실제 OS 종료 실패를 재현한 것은 아니다. 임시 작성 handle 종료 확인/주입 검사와 구분하고 모든 handle 종료 실패를 검증했다고 주장하지 않는다.

현재 전체 행 staging·직렬화·원본 읽기·파일 재작성 비용과 기존 시작 시 이차 시간 ID 검사·선형 SELECT를 유지한다. 대용량 성능·100만 건·전원 차단/강제 종료 복구·모든 손상/할당 실패·실제 콘솔 렌더링은 검증하지 않았다. 정렬/페이지·JOIN·집계·UNION·migrate와 Phase 2/3는 후속이다.

문서·Wiki 원문 대조 후 실제 정식 `21-finish`가 exit 0으로 Graphify 갱신·Wiki changed_sources 빈 배열/page_changed false를 확인했다. vendor yyjson 두 파일의 부분 추출·네 파일 no-symbol 경고는 정적 도구 한계로 남는다. 이 결과 기록 뒤 Wiki를 재등록하고 최신 정식 `22-finish`도 exit 0으로 마쳤다. `commands/*-finish/receipt.json`과 [DBMS-003](records/DBMS-003.md)을 따른다. 정적 색인은 SQL 실행의 대체 증거가 아니다. Master가 전체 결과를 보고했고 사용자가 M3 완료를 명시 승인했다. 정상 `finish --user-ref`로 DONE을 확인한 뒤 이 완료 상태 문서를 별도 유지보수로 갱신했다. 다음 행동은 후속 정렬/페이지 증분 계획 협의이며 새 구현 승인은 아니다.
