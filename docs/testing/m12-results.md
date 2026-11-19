# M12 서버·C SDK·원격 CLI 구현 결과

2026-11-18 수요일(Asia/Seoul). 사용자 all-A·초기값 선택과 검토한 설계의 실제 구현 승인을 적용했다. 구현·독립 QA 통과 뒤 사용자 ‘M12 완료 승인’과 정상 하네스 DONE을 확인했다.

## 범위와 구현 결정

- 승인한 15개 파일에 Windows C23 서버 `minidbd`, 정적 `minidb_client`, SDK 전용 `minidb-remote`, 공통 포맷·결과 해제와 검사를 연결했다. 정확한 파일 범위와 선택 출처는 [구현 계약 JSON](../../.cache/m12/contract.json)을 따른다.
- SQL·저장·로컬 CLI 의미를 재사용하고 길이 프레임 TCP/JSON과 B/D/E 이벤트 바이트 청크·F 완료를 구현했다. SDK는 전체 이벤트를 재조립하고 UTF-8·타입·수치·필드·순서를 검증한 뒤 `MdEvent`를 넘긴다. 수명·재사용·실패 조건과 핵심 GWT18의 원본은 [설계 계약](../implementation-m12.md#7-핵심-given--when--then)에 보존한다.
- reactor만 socket을 소유한다. worker는 문장 하나를 실행하고 완료 소유권을 넘기며 E 송신 뒤 후속 문장을 FIFO에 재등록한다. 닫힌 연결의 실행 context는 완료까지 유지하고 stale 완료는 세대 번호로 거절한다.
- `request_open/next/close`의 엔진 접근은 직렬화한다. 잠금 획득 뒤 취소를 재확인하고 `md_request_next` 시작마다 DB health를 재검사해 타 요청이 만든 중단 상태의 후속 SQL을 차단한다.
- 결과 포맷과 해제를 공통 소스로 옮겨 정적 SDK·원격 CLI를 엔진 구현 없이 연결했다. 독립 QA는 archive·실행 파일의 심벌도 확인했다.

## 설정과 실패 불변식

- 선택한 초기값은 연결32·worker4·pending16, request/frame 각1 MiB·event64 MiB·response256 MiB다. queue/lock/receive/send 각30000 ms·idle60000 ms, SDK connect5000 ms·receive/send 각30000 ms를 사용한다. 이 값은 사용자 승인 초기값이며 최적화 측정은 후속 작업이다.
- 자원 값은 양수이며 pending0을 거절한다. count 간 대소 관계는 독립적으로 허용한다. `FD_SETSIZE=1024`에서 listener·깨우기 socket을 포함하므로 `max-connections`는1022 이하다. SQL timeout은0만 허용한다.
- request≤frame≤UINT32_MAX, frame/event≥4096, response≥16384와 overflow를 검사한다. 작은 실패 응답용16 KiB를 예약하고 event·누적 payload 상한을 함께 적용한다. 기한은 단조 시계의 전체 구간을 사용한다.
- DML 반영 뒤 전달 실패는 확정 committed·affected_rows·cleanup/rollback·health를 보존한다. 완료 미수신 후의 실행 가능성은 SDK 오류의 미확인 상태로 남기며 자동 재전송하지 않는다.
- 초과 Q는 응답 half-close 뒤 고정4 KiB 버퍼로 기한 내 drain해 unread payload에 의한 TCP reset으로 오류 응답이 사라지는 문제를 수정했다. shutdown은 현재 SQL 정합성 처리·worker join 뒤 DB close 순서를 지킨다.
- 운영 오류의 원격 `stage`는 protocol/queue/lock/receive/send/shutdown으로 전달한다. 기존 SQL 진단 필드·종료 우선순위와 반영 메타데이터를 유지한다.

## 실제 검사와 수정

- 작성자 최종 새 configure/build·전체 CTest는176/176 PASS, 독립 Reviewer_QA의 수정 후 새 configure/build·전체 CTest도176/176 PASS이며 skip0이다. 최종 근거는 각각 commands/24-regression·25-regression이다. 수정 전 첫 전체 통과19/20과 구분하며 [작성자 결과](../../.cache/m12/implementer-result.json)·[독립 결과](../../.cache/m12/reviewer-result.json)에 영수증·보고서·시간·해시를 둔다.
- 기존158개와 M12의18개가 함께 통과했다. 실제 server/SDK/remote CLI, 부분 프레임·UTF-8 token 분할·잘못된 이벤트·cap 경계, 지속 연결·유휴 연결, 동시 DML·유한 큐·잠금/송수신/idle 기한, 끊김·stale 완료·cleanup/rollback·Ctrl+Break 종료·stdout 실패를 검사했다.
- 초기 실제 기능 Red는 health 변경 뒤 next 차단, 정상 fake-peer 바이트 분할의 SDK assertion, cleanup 종료 코드 보존, request cap 경계 응답 소실에서 확인했다. 3/4-red 연결 실패를 decoder 실패로 설명한 기록은 Master의 원문 대조로 정정했고5-red의 실제 assertion을 근거로 삼았다. 2-red 컴파일 실패와21-red 실행 timeout도 환경/실행 실패로 구분했다. [초기 작성자 결과](../../.cache/m12/implementer-attempt1.json)와 [실행 원장](../../.cache/agent-harness/m12-native-20261118/)을 따른다.
- 독립 QA는 전체176개 통과 뒤 추가 fake-peer 검사에서 두 문제를 재현해 묶음 품질 반려1회를 기록했다. M12-QA-01은 wire 십진 문자열의 선행0과 명시한 SQL 위치0 허용, M12-QA-02는 diagnostic phase/source/command의 명시 도메인 누락이었다. [첫 검토](../../.cache/m12/reviewer-attempt1.json)에 실제 재현과 계약 근거를 보존한다.
- 승인된3개 파일에서 canonical wire 숫자·양의 명시 SQL 위치와 phase/source/command 검사를 보강했다. 수정 전11개 invalid event 수용의 실제 Red 뒤 targeted·전체 회귀를 통과했고 독립 QA는 기존8개 오류 재현이 protocol failure로 거절됨과 정상 control을 확인했다.
- UINT64/INT64 경계·byte_offset0·nullable 필드·CLI 숫자 파싱과 별도 원격 stage를 보존했다. 오류 code의 새 whitelist와 음수0 규칙은 추가하지 않았다. 두 지적은 해결됐고 독립 최종 상태는 APPROVED다.
- 기본 sandbox 안의 loopback 연결은 실패했고 실제 native loopback 검사는 automatic approval을 거친 sandbox 밖 권한으로 실행했다. CIM 조회는0회다. timing 경계 검사는 시험 전용 fault server의 hold/socket-buffer 제어를 사용하며 일반 server는 해당 hook을 제외한다.

## 미실행·한계와 다음 행동

- M13의 병렬 읽기·배타 쓰기·SQL 협력 취소가 남아 있다. M12의 기한은 실행 중 SQL을 강제 종료하지 않으며 SQL timeout은 비활성이다.
- 출력 ACK를 생략했다. 연결 종료 감지 전 시작한 DML은 반영될 수 있고 socket 전송 완료와 원격 stdout 성공의 관찰 시점이 다르다.
- 엔진 결과 전체 materialization·이벤트 JSON 직렬화·SDK 재조립 메모리를 사용한다. frame/event cap과 총 프로세스 메모리 사용은 별도다. 처리량·p95·메모리·큐 대기 측정과 초기값 조정은 기능·M13 완성 뒤 진행한다.
- Phase 1 전체204개 감사는 사용자 지시로 생략·미실행이다. 전원 손실·전수 할당 실패·배포/타언어 SDK·TLS/인증·ISO 정식 호환도 검증 범위에 포함하지 않았다.
- M11의 Debug100만 행 DB open118.711853초 관측은 유지한다. 비정상 여부·원인·시작 비용 개선 필요성은 전체 구현 마감 뒤 진단할 후보로 보류했다.
- 문서 원문 대조·UTF-8·링크·해시·Wiki 등록·정상 하네스 finish의 실제 결과와 기계 근거는 [Knowledge JSON](../../.cache/m12/knowledge-result.json)에 둔다. 문서 담당은 C 검사를 추가 실행하지 않았다.
- 사용자 완료 승인·정상 하네스 DONE 뒤 상태 문서를 갱신했다. 해당 유지보수의 대조·검사·전후 해시는 [완료 상태 JSON](../../.cache/m12/completion-result.json)을 따른다. 새 Goal은 없으며 다음은 M13 상세 설계·구현 범위 협의다.
