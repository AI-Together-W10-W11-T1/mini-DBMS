# 2분 발표·CLI 시연 준비 결과

2026-11-19 목요일(Asia/Seoul). 사용자 요청에 따라 [README 발표](../../README.md#발표--파일에서-작은-rdbms로--2분)를 기존 문서와 운영 절 뒤에 추가하고, [demo.ps1](../../demo.ps1)로 동일 행의 SQL/파일 작업을 비교하도록 만들었다. DB 기능 추가가 아닌 발표용 운영 도구 변경이다.

## 구성과 실행

- README: 문제 정의15초·해결25초·지표20초의 세 부분. 도구 역할·OS/DB 인덱스·ID·저장 실패·SDK·동시성 및473자 발화 예시는 접기 영역에 두었다. 말하는1분과 설명을 포함한 시연45~60초는 발표 리허설 목표이며 음성 녹음으로 검증한 시간은 아니다.
- 사전 준비 ` .\demo.ps1 setup`은1만 행의 DB/CSV 모든 필드를 대조하고 새 `.cache/presentation-demo/runs/<GUID>`를 만든다. 이전 실행은 삭제하지 않는다. 현재 저장소에 존재하는 M14 검증 빌드를 기본 사용하며, 다른 빌드는 ` .\demo.ps1 setup -Exe <minidb.exe 경로>`로 지정한다. 새 기기 빌드/배포는 별도 준비다.
- 발표 중 ` .\demo.ps1 insert`와` .\demo.ps1 select`는 각각 SQL과 실제 PowerShell `Add-Content`/`Select-String`을 한 쌍으로 실행하고 결과 일치·작업별 ms·부호 있는 DB−PS 차이를 표시한다. 반복 INSERT는 DB의 자동 ID를 실제 검증하며 partial pair는 pending 상태로 재사용을 막는다. 두 저장소를 하나의 트랜잭션으로 주장하지 않는다.
- 스크립트 실행 정책·전역 PATH·새 의존성은 변경하지 않았다. 정책이 스크립트를 차단하는 별도 환경에서는 조직 정책에 맞는 실행 허용이 필요하다.

## 실제 검사

작성자는 실제 roundtrip·반복/음성14개를 실행했다. 독립 Reviewer는 Windows PowerShell **5.1.26100.9818**에서 실제12개 검사를 실행하고 README와 코드·수치·원본을 검토했다. 전체 필드1만 행 일치, 자동 ID10001→10002, 결과 불일치 거절·pending 재사용 차단·누락 실행 파일·잘못된 행 수·이전 run 보존을 확인했다.

아래는 독립 Reviewer가 새 `powershell.exe -NoProfile -File` 프로세스를 시작해서 종료할 때까지 외부 시계로 잰 실제 시간이다. 시연에 추가하는 인위적 지연은 없다.

| 시연 명령 | 첫 실행 | 반복 실행 |
|---|---:|---:|
| `insert` | 0.919초 | 0.876초 |
| `select` | 0.783초 | 0.796초 |

네 번 모두 요청한5초 이내였다. 사전 setup도1.179초였지만 발표 시간에서 제외한다. 현재 기기에서의 관측이며 다른 기기의5초 상한은 보장하지 않는다. 마지막 setup으로 새1만 행 상태를 준비했다.

첫 INSERT의 개별 작업 시간은 DB38.562ms·PowerShell5.991ms, SELECT는 DB35.017ms·PowerShell3.975ms였다. 이 표본에서는 PowerShell 쪽이 더 빨랐다. **DB 시간에는 매번 CLI 시작·전체 행 검증·메모리 적재·인덱스 재구성이 포함**되고, PowerShell 작업은 실행 중인 호스트에서 동작한다. 바이너리 DB와 CSV 추가의 검증·복원·flush·인덱스 유지 비용도 다르므로 인덱스 알고리즘의 직접 비교가 아니다. 표시하는 wrapper 내부 시간은 보고서/화면 출력 전까지이고, 위 외부 시간은 호스트 시작과 종료까지 포함한다.

README의100만 행37.2µs 대15.67ms·약421배는 [M11 같은 엔진의 Debug warm p50](m11-results.md)이며 PowerShell 비교 배율이 아니다. 서버11,027req/s·성공p95 1.776ms는 [M14](m14-results.md)의16접속·짧은Debug/loopback3반복 통계 중앙값이다. OS 파일명 인덱스와 DB 레코드 인덱스를 구분했고, 현재 SQL 범위 조건은 인덱스가 아닌 스캔임을 명시했다.

## 근거와 한계

- 실제 명령·표준 출력·환경·시간·코드 해시: [독립 검사 JSON](../../.cache/presentation/reviewer-evidence/actual-check-result.json), [작성자 결과](../../.cache/presentation/implementer-result.json), [최종 독립 검토](../../.cache/presentation/reviewer-result.json).
- C 엔진·헤더·빌드/검사 입력22개의 해시는 M14 최종 검증본과 동일하다. [보호 입력 대조](../../.cache/presentation/native-protected-check.json)를 보존했다. 이번에는 wrapper 통합 검사와 문서 검사를 수행했으며, 기존198개 CTest를 재실행하거나 그 결과로 대신 보고하지 않았다.
- 역사·PowerShell·파일시스템·B-tree/해시의 배경은 README의 공식 원문 링크로 연결했다. 실행 가능한 SDK 전체 프로그램이나 완전한 SQL 표준·ACID·전원 손실 복구를 새로 구현한 작업은 아니다.
- 원문·링크·Wiki 등록·문서 마감의 기계 근거는 [마감 JSON](../../.cache/presentation/close-result.json)에 기록한다. 사용자 실제 발표 리허설은 남아 있으며, 이번 작업을 근거로 새로운 성능 최적화를 시작하지 않는다.
