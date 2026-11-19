# M13 공유 읽기·협력 취소 구현 결과

2026-11-19 목요일(Asia/Seoul). 구현·독립 QA 뒤 사용자 완료 승인·정상 하네스 DONE을 확인했다.
사용자 전체A와 후속 실구현 승인, 최신 비용 지시의 final-once-v1을 적용했다.
GWT14·선택 원본은 [설계 계약](../implementation-m13.md), 현재 실제 결과는 이 문서를 따른다. 설계 당시 문구와 과거 승인 범위는 변경하지 않았다.

## 구현과 불변식

- DB 소유 gate가 병렬 READ와 배타 WRITE를 관리한다. 대기 writer가 새 reader를 막고 취소/timeout 때 계수 복원과 깨우기를 수행한다.
- 기존 scanner/lexer로 준비·분류하고 요청 소유 scratch를 사용한다. 준비 전 health 확인과 최종 gate 안 재확인으로 중간에 중단된 DB의 후속 실행을 차단한다.
- READ는 빌린 TEXT/index의 사용과 결과 소유 복사까지, WRITE는 staging·저장·메모리/index 공개·health 확정까지 유지한다. 직렬화·송신은 gate 밖이다.
- [공개 API](../../include/minidb.h)의 `MdExecutionOptions`·`md_request_next_ex`가 문장별 누적 lock budget, 최종 gate 뒤 시작하는 SQL 기한과 취소 callback을 전달한다. 기존 next는 0 옵션을 사용한다.
- 기본 SQL 기한0과 양수 협력 기한·INT64_MAX 상한을 지원한다. 준비 시간은 SQL 기한과 lock 대기에 포함하지 않으며 재취득마다 lock budget을 늘리지 않는다.
- lexer·scan·JOIN·TEXT·group·sort·UNION·페이지·VALUES·DML/index/직렬화 준비 내부에서 취소를 관찰하고 부분 결과와 소유 메모리를 정리한다.
- 첫 marker 변경 직전 마지막 확인 뒤에는 marker·flush/replace·rollback·cleanup·publication 보호구간을 완료한다. 반영 성공·affected_rows·cleanup/recovery 진단·health와 6>5 우선순위를 보존하고 후속 SQL을 중단한다.
- disconnect/shutdown은 worker 수명 안 callback으로 관찰한다. busy 연결/요청을 완료까지 보존하고 닫힌 연결 완료를 폐기하며 worker join 뒤 DB를 닫는다.
- 신규 strict SDK는 `stage=sql`을 처리한다. 구 stage whitelist 재현 실행 파일은 안전한 protocol failure와 앞선 성공 보존을 검사하며 미확인 후속 반영을 false로 단정하거나 재전송하지 않는다.

## 실제 검증과 정정

- 최초 실제 기능 Red는 [1-red](../../.cache/agent-harness/m13-native-20261119/commands/1-red/receipt.json)에 보존했다. 이후 작성자 영향 검사로 구현을 인수했다.
- 최신 지시는 작성자 전체 회귀 중복을 제거했다. 작성자 full0, 수정 후 최신 실제 configure/build·선택20/20은 [14-targeted](../../.cache/agent-harness/m13-native-20261119/commands/14-targeted/receipt.json)다.
- 독립 Reviewer는 소스를 수정하지 않고 첫 검토에서 두 지적을 묶어 반려1회 했다. [첫 검토 원문](../../.cache/m13/reviewer-attempt1.json)의 M13-QA-01·02를 수정 후 재검토해 해결했다.
- QA-01은 페이지 정리 내부 안전 지점 누락이다. 실제 SQL result/EOF 양성 대조 후 진행3에서 취소하고 부분 소유권을 유지하도록 수정했다. 독립 실제 helper의5개 경우에서 각각128값 단일 해제·누수0을 확인했다.
- QA-02는 실제 두 gate 누적 대기 근거 부족이다. next_ex health4+최종WRITE6=10, 준비1000 제외·budget10 timeout/11 성공·계수 복원과 다음 요청 진행을 결정적 사건/시계로 확인했다.
- [8-red](../../.cache/agent-harness/m13-native-20261119/commands/8-red/receipt.json)는 DUPLICATE_OUTPUT_NAME인 잘못된 fixture를 checkpoint Red로 잘못 설명하고 Master도 확인했던 기록이다. 별칭·양성 대조를 보강한 [11-red](../../.cache/agent-harness/m13-native-20261119/commands/11-red/receipt.json)의 실제 누락 assertion 실패로 정정했다.
- 9/10-red compile 실패와 13-targeted의 loopback sandbox WinError10013은 기능 Red가 아니다. 같은 고정 소스를 허용된 환경에서 다시 실행한14-targeted가 통과했다.
- 초기 작성자 ENV_ERROR는 최신 지시와 중복 회귀 관문의 정책 전환 의존성으로 처리했으며 제품 품질 반려가 아니다. 기존 Red를 정상 하네스 전환에 보존했다.
- 수정 후 독립 QA가 최종 고정 구현에서 새 configure/build·전체 회귀를 **정확히1회**, **190/190 PASS**로 수행했다. 누락·skip·실패0이며 기존176+M13 14의 정확한 이름을 확인했다.
- 최종 실행 근거는 [15-regression 영수증](../../.cache/agent-harness/m13-native-20261119/commands/15-regression/receipt.json)·[보고서](../../.cache/agent-harness/m13-native-20261119/commands/15-regression/test-report.json)·[독립 최종 결과](../../.cache/m13/reviewer-result.json)다. 작성자 선택 검사와 독립 whole 실행을 합산하거나 복제하지 않는다.
- final-once-v1의 운영 구현·기본 strict 보존·packet 정렬은 [적용 JSON](../../.cache/final-regression-policy/application.json)·[검증 JSON](../../.cache/final-regression-policy/application-validation.json)을 따른다. Python84 운영 단위·독립7·packet2 검증은 subprocess/actor 모의 운영 검증이며 제품 C QA가 아니다.

## 한계와 다음 행동

- 협력 취소는 단일 OS I/O·할당을 즉시 중단하거나 정확한 반환 시각을 보장하지 않는다. 취소 후에도 남은 소유 값의 필수 정리는 완료한다.
- 구 SDK 검사는 역사적 배포 binary가 아닌 이전 strict stage whitelist 재현 binary다. v1 모든 조합의 완전 호환을 입증하지 않는다.
- 128값 pointer ledger는 실제 내부 페이지 helper의 지정5개 경우이며 전체 DBMS 할당·모든 fault를 증명하지 않는다. 재검토는 변경3파일·두 지적에 집중하고 첫 검토의 미변경 근거를 재사용했다.
- 전수 Phase1 감사·전원 손실/강제종료 복구·전체 fault·대규모 무작위·성능/시작 비용 최적화는 미실행이다. 정적 그래프는 실행 증거가 아니다.
- 다음은 기능 완성 후 동시접속·throughput·p95·memory·wait 측정 범위와 초기값 조정 협의다.100만 행 startup 진단은 전체 구현 마감 뒤 후보로 보존하며 새 Goal은 만들지 않았다.
- 원문 대조·Wiki 일괄 등록·문서 finish의 기계 근거는 [Knowledge JSON](../../.cache/m13/knowledge-result.json)에 둔다. 추가 제품 C 검사는 수행하지 않는다.
- 사용자 완료 승인을 Master가 정상 native finish에 반영해 DONE을 확인했다. 이후 상태 유지보수 근거는 [완료 JSON](../../.cache/m13/completion-result.json)을 따른다.
