# M8 명시적 스키마 변환·관리 실패 처리 결과 — DBMS-008

2026-11-18 수요일(Asia/Seoul). run `m8-native-20261118`, slim-v1. 구현·독립 QA APPROVED이며 **사용자 완료 승인·정상 하네스 DONE을 확인**했다. 새 Goal은 없다.

## 사용자 선택과 핵심 불변식

현재 사용자 A 선택은 저장 계약 7절의 full migrate와 직접 관련된 init 관리 실패 처리다. 단계적으로 동일 타입만 먼저 구현하는 B와 비교한 범위·장단점·선택 출처를 Master가 정식 계약과 Planner packet에 전달했다. 기존 다섯 책임 안에서 엄격한 스키마·바이너리 직렬화/전체 행 검증·프로세스 사용권·이벤트 출력을 재사용했다. 의미 원본은 [저장 계약](../storage-contract.md), [CLI 계약](../cli-contract.md), [저장 인수 명세](phase1-storage-acceptance.md)의 P1-STORE-033~045·048과 [CLI 인수 명세](phase1-cli-acceptance.md)의 P1-CLI-036~037·041~048이다.

plan 구조를 먼저 검사하고 원본 헤더에서 카탈로그를 얻어 모든 원본 행을 검증한다. 명시 대응이 기본 대응보다 우선하며 일대일 사용·명시 drop·ID 이름/타입/값 유지·대상 순서를 검사한다. 정의와 해결된 대응이 모두 항등인 파일을 바이트 복사하고 비항등 대응은 변환한다. 새 비ID 컬럼은 NULL이다. INT64↔TEXT는 부동소수점을 경유하지 않고 십진 문법·양끝 ASCII 공백·정수 경계를 직접 검사한다.

원본 사용권을 작업 수명 동안 유지한다. 신규 출력은 기존 부모의 실제 경로를 해석해 원본과 겹침을 검사한다. 생성 직후 guard 디렉터리와 pending 표식으로 불완전 출력을 차단한다. 전체 파일 재검증·행 수 대조·잔여 확인·guard 정리 뒤 pending을 마지막으로 삭제하고 단일 성공 이벤트를 공개한다. 실패 단계에 따라 구조 오류2·준비 오류3·생성 뒤 불완전4·준비 완료 뒤 stdout 실패7과 output_ready를 구별한다. TEXT·행·이벤트의 소유 메모리를 준비하고 실패 경로에서 정리한다.

원문 대조 대상은 src/engine.c의 md_operate, src/catalog.c의 md_plan_load/md_mapping_prepare, src/storage.c의 md_storage_source·migration_integer·migration_size·converted_rows·output_parent/output_path·generate_output, src/cli.c의 diagnostic_exit/event_output과 src/internal.h다. 실제 helper 경계·이벤트 수명은 tests/unit_milestone.c, 프로세스/독립 decoder·실패 주입은 tests/m8_cli.py, 등록은 CMakeLists.txt·scripts/c_checks.py로 확인했다.

승인된 기술 변경은 위 소스/시험/등록 9개 파일이다. 최종 기술 digest는 `dca48b4178a6ec5d98e06c926e42bf3a20137c36d2518fd9ef9c7fcaf2a00731`이다. Knowledge는 정식 packet의 고정 입력과 기술9개 SHA를 보존해 문서만 정리했다.

## GWT 20개와 실제 연결

각 이름은 `M8-` 접두사를 포함한 정확한 CTest 이름이다. 기존103개에 신규20개를 더해 전체123개다. 한 CTest가 여러 독립 fixture와 native cmocka 함수를 실행하므로 아래 수와 세부 assert 수는 다르다. 최종 작성자·독립 전체 회귀에서 모두 PASS다.

| CTest | Given | When | Then |
|---|---|---|---|
| M8-PLAN-01 | BOM·빈 tables·테이블/map/drop 생략 | 선택 plan 읽기 | 기본 동일 이름 대응·빈 drop 허용 |
| M8-PLAN-02 | 필수 필드/타입 오류·unknown/중복 키·잘못된 UTF-8/Unicode | 구조 준비 | ARGUMENT_ERROR·2, 출력 생성 없음 |
| M8-PLAN-03 | 접근 불가 plan·중복/없는/미사용 이름 | 의미 준비 | 접근 IO_ERROR 또는 MIGRATION_ERROR·3, 생성 없음 |
| M8-SOURCE-01 | 이전 JSON 없는 헤더·손상 헤더/후반 행 | 원본 발견·전체 검증 | 정상 수용, 손상은 출력 생성 전 거절 |
| M8-MAP-01 | 명시 rename·기본 대응·신규 열·drop·재배열 | 변환 | 대상 순서·매핑 값·신규 비ID NULL 유지 |
| M8-MAP-02 | 원본 재사용·잘못된/누락 drop·테이블 집합 차이 | 대응 검증 | MIGRATION_ERROR·3, 생성 없음 |
| M8-MAP-03 | ID 위치/대소문자 변경·삭제/재대응/타입 오류 | 검증·변환 | 허용 변경과 ID 값 유지, 나머지 거절 |
| M8-COPY-01 | 동일/변경 혼합 테이블·동일 스키마 값 교환 | 변환 | 항등 파일 바이트 일치, 비항등 converted/값 일치 |
| M8-TYPE-01 | INT64 양끝·0·양/음수·NULL | TEXT 변환 | 정확한 canonical 십진 문자열·NULL |
| M8-TYPE-02 | 허용 양끝 공백·부호·앞자리0·경계·NULL | INT64 변환 | 정확한 값·NULL |
| M8-TYPE-03 | 빈/내부/금지 공백·소수/지수/16진·범위초과·후반 오류 | INT64 변환 | 전체 실패·4, 원본 유지·출력 차단 |
| M8-PATH-01 | 기존/동일/겹침/별칭 출력·루트 부모 | 출력 준비 | 기존 경로 보존·거절, 실제 resolver의 루트 유지 |
| M8-LEASE-01 | 사용 중 원본·실제 별칭·작업 중 경쟁 접근 | 사용권 획득/유지 | 경쟁 거절·성공/실패 뒤 handle 해제 |
| M8-FAILURE-01 | 표식 create/write/flush/close·후반 copy/convert I/O 개별 fault | 출력 생성 | 4, 성공 이벤트 없음·원본 유지·불완전 차단 |
| M8-FAILURE-02 | 최종 검증·잔여 확인·표식 정리 개별 fault | 완료 공개 시도 | 부분 성공 없음·output_ready false·재시작 차단 |
| M8-RESOURCE-01 | plan/행/직렬화 개별 할당 fault·합성 크기 경계 | 준비/생성 | RESOURCE_ERROR·단계별 종료 코드·연산 전 크기 검사 |
| M8-CLI-01 | table/JSONL 성공/실패·공개 뒤 stdout fault | 실제 CLI | 정렬 이름/동작/건수, stdout 실패7·output_ready true |
| M8-OWNERSHIP-01 | 성공/실패 이벤트·소유 TEXT·NULL | API 반환·다음 호출·해제 | 이벤트/값 수명·안전 정리 |
| M8-INIT-01 | init 생성 뒤 lease/표식/쓰기/검증/정리 fault·ready 뒤 stdout fault | 실제 init | 불완전4·차단, ready 뒤 stdout7 |
| M8-REGRESSION-01 | 기존 M1~M7·원본 snapshot | 새 configure/build/전체 CTest·프로세스 재시작 | 123개 통과·원본 내용 보존 |

## 실제 Red·회귀·독립 검토

2-red는 M8-MAP-01·TYPE-01·TYPE-03·OWNERSHIP-01의 실제 미구현 실패4개다. configure/compile은0, errors는 빈 배열이며 Master가 확인했다. 이 Red는 기능 부재를 확인한 범위다. 개별 산술·ownership 결함의 독립 Red는 확보하지 않았고 해당 실제 helper 경계와 native 수명 검사는 Green·전체회귀에서 실행했다.

첫 독립 검토의 전체123개와 추가36개가 통과한 뒤 읽기 전용 경로 실험에서 **M8-QA-001**을 발견했다. 신규 `C:\name`의 부모가 `C:`로 잘려 드라이브 현재 디렉터리 아래로 출력 후보가 바뀌었다. 이 run의 품질 반려는 누적1회다. 최초 실험은 production 분리 표현과 동일 WinAPI를 사용했다. 이어 실제 production helper의 test-only wrapper를 연결해 11-red에서 정상 drive-root 입력의 assert 실패를 재현하고 Master가 confirm-red했다. output_parent가 마지막 구분자를 보존하도록 최소 수정했고, 부모 실제 경로 해석 뒤 join 중복 구분자만 제거했다. 독립 재검토에서 해결·추가 지적0을 확인했다.

아래는 정식 commands의 test-report.json과 receipt.json을 대조한 실행이다. 초 단위는 보고서 total이며 각 호출에서 새 configure/compile/CTest를 실행했다. role별 slim 빌드 트리는 재사용했다. 1-red는 테스트 등록 불일치 오류이며 기능 Red 집계에서 제외했다.

| 정식 명령 | PASS / FAIL | total 초 | build | 의미 |
|---|---|---|---|---|
| 1-red | 0 / 0 | 0.003 | 미실행 | 등록 환경 오류 |
| 2-red | 0 / 4 | 51.538 | 0/0 | 미구현 기능 Red |
| 3-targeted | 4 / 0 | 10.796 | 0/0 | 작성자 중간 검사 |
| 4-targeted | 14 / 6 | 26.771 | 0/0 | fixture/환경 실패 포함 |
| 5-targeted | 7 / 0 | 12.326 | 0/0 | 작성자 중간 검사 |
| 6-regression | 123 / 0 | 91.977 | 0/0 | 작성자 중간 검사 |
| 7-regression | 123 / 0 | 93.044 | 0/0 | 작성자 중간 검사 |
| 8-regression | 123 / 0 | 84.786 | 0/0 | 작성자 중간 검사 |
| 9-regression | 123 / 0 | 129.765 | 0/0 | 첫 독립 회귀, 이후 경로 반려 |
| 10-red | 0 / 1 | 9.362 | 0/0 | 경로 literal fixture 보완 전 |
| 11-red | 0 / 1 | 6.044 | 0/0 | 실제 production 경로 Red |
| 12-targeted | 2 / 2 | 10.767 | 0/0 | extended/UNC literal fixture 보완 전 |
| 13-targeted | 4 / 0 | 10.686 | 0/0 | 수정 후 부분 검사 |
| 14-regression | 123 / 0 | 87.027 | 0/0 | 최종 작성자 전체 |
| 15-regression | 123 / 0 | 100.297 | 0/0 | 최종 독립 전체 |

최종 14-regression과 15-regression은 동일 최종 digest, configure/compile0·errors=[]·각123/123 PASS다. 작성자 configure3.961/compile0.124/CTest82.930/total87.027초, 독립 configure4.010/compile4.173/CTest92.102/total100.297초다. 13-targeted는 PATH·LEASE·INIT·OWNERSHIP4/4다.

독립 Reviewer_QA는 새 VERIFY 빌드와 별도 바이너리 작성기·typed-row oracle로36/36 프로세스 사례를 재실행했다. Unicode·CRLF·NUL·빈 TEXT/NULL·정수 양끝·casefold 항등·명시 swap·drop/default/reorder·엄격 십진 문법·후반 변환 실패·손상/ID/잔여·실제8.3 별칭 사용권·생성 뒤 실패·production fault 격리를 확인했다. 원본 table와 양쪽 외부 JSON 바이트 보존, 복사 바이트·변환 값, 새 경로의 별도 프로세스 재시작도 검사했다.

독립 native probe는 새 VERIFY libengine_fault.a/libyyjson.a에 직접 링크해 production wrapper12/12를 통과했다. drive-root·extended drive-root는 실제 resolver의 읽기 전용 실행, UNC·extended UNC는 순수 production 부모 분리 검사다. 상대 경로·dot segment·끝 구분자·슬래시·비루트 resolver도 포함한다. 실제 루트 출력 생성·UNC 서버 접속/출력 생성은 미실행이다. 독립36개 명령 wall4.157초, native probe compile/check wall2.721초다.

## 실패·재작업과 환경 한계

- 첫 등록에서 Python 기본 CP949로 UTF-8 읽기에 실패해 CMake/어댑터 등록이 적용되지 않았다. UTF-8 명시 후 재등록했고 1-red scope mismatch를 기능 Red로 처리하지 않았다.
- 4-targeted는 예약어 VALUE 컬럼과 미투영 id ORDER BY fixture를 계약에 맞는 payload 및 SELECT id,name ORDER BY id로 수정했다. 기대 동작은 유지·확대했다.
- junction 생성이 Access denied여서 실제 GetShortPathNameW의8.3 별칭을 확인해 overlap/busy 시험에 사용했다. junction 동작은 미검증으로 남는다.
- 작성자 자가검토에서 원본 읽기 source=data 진단과 경로 할당 RESOURCE_ERROR 분류를 보완했고7-regression을 수행했다. 다음 자가검토에서 할당 실패 분기를 읽기 전에 분리하고 실제 파일 공유거부 fixture를 추가해8-regression을 수행했다.
- 독립 최초 fixture 실행은 ROOT parent index 오류의 StopIteration을 수정한 뒤36개를 통과했다. reviewer 도구 환경/fixture 오류로 기록했다.
- attempt2의10-red·12-targeted에는 C literal 과다 escape·extended/UNC 선행 역슬래시 축약 오류가 있었다. 11-red의 정상 drive-root 첫 실제 production 실패를 정식 Red로 사용했다. 문자 코드와 대조해 fixture를 고친 뒤13-targeted·14-regression·독립15-regression/12개 probe를 통과했다.

## 증거와 측정 한계

정식 근거는 `.cache/agent-harness/m8-native-20261118/`의 packets·results·commands·artifacts다. 승인 범위는 `.cache/m8/contract.json`, GWT는 planner-result.json, 작성자 결과는 implementer-result.json, 반려 원본은 reviewer-result-attempt1.json, 최종 검토/QA는 reviewer-result.json, 독립 원본은 reviewer/attempt2/{independent-report,extra-report,path-report}.json과 probe 소스다. 첫 읽기 전용 재현은 reviewer/root-path-proof.json에 있다. Knowledge의 증거 해시/실행 횟수/시간/기술SHA 확인은 `.cache/m8/knowledge/`에 보존한다.

정식 제품 검사 호출은15회이며 전체회귀6회·selected8회·등록 오류1회다. 이 중13회 작성자·2회 독립 검사다. 시간은 각 보고서의 실제 관측값이다. 역할 캐시와 cold/warm·소스·범위 조건이 달라 절감 인과효과를 측정하지 않았다. provider 실제 token은 런타임에서 제공되지 않아 null이다. 토큰·금액 절감률과 TDD 인과효과는 미측정이다.

전체204개 인수 명세 전수 감사는 다음 계획 협의 대상이다. 기존 M7 overflow_01의 ORDER 없는 ALL 결과 순서 단정 관찰은 미해결로 보존한다. 선택한 allocation/API fault·합성 크기 경계·native 수명을 검사했고 모든 malloc/WinAPI 실패 조합·sanitizer/leak 분석·강제 종료/전원 손실 복구·100만 행 성능은 미실행이다. 시작 전수 적재와 이차 ID 중복 검사 등의 대용량 한계도 유지한다.

## 문서 마감과 다음 행동

Knowledge는 Markdown6개·JSON1개의 UTF-8·로컬 링크147개·GWT20·승인 문서 범위의 Git 공백 검사(cr-at-eol)·기술9개 SHA 불변과 제품 검사15회 영수증/로그/보고서 해시를 확인했다. Wiki 기존67개 출처를 보존해 변경 원문과 신규 결과/시험을69개로 등록했다. 정상16-finish는 exit0·Wiki changed_sources=[]·page_changed=false이며 실제 Graphify 갱신을 수행했다. vendor yyjson2개 부분 추출과 include/minidb.h·src/internal.h·src/keywords.inc·install-rtk.ps1의 no-symbol 경고가 있었다. 정적 색인의 범위 한계다. 이 관측 반영 뒤 Wiki를 최종 재등록하고 정상 finish를 다시 실행하며, 마지막 영수증과 문서SHA는 Knowledge 결과JSON에 보존한다. 이 단계에서 제품 검사는 재실행하지 않았다.

Master가 실제 비동기 사용자 완료 승인을 정상 pause/resume/finish에 반영해 DONE과 completion_user_ref를 확인했다. 완료 근거와 후속 문서 정책 정합화는 [문서 마감 결과](document-close-results.md)를 따른다. 다음은 전체204개 인수 감사 계획 협의다. M1~M7·workflow의 기존 사용자 승인/DONE 이력은 해당 원본에 보존한다.
