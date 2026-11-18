# M11 대량 생성·정확성·성능 비교 결과

2026-11-18 수요일(Asia/Seoul). 구현·독립 QA·사용자 완료 승인·정상 하네스 DONE을 확인했다. 품질 반려0회이며 새 Goal은 없다.

## 선택과 구현

사용자1A/2A는 결정적 SQL 파일 생성·실제 CLI의10000행 INSERT 배치와 C 공개 엔진 API·동일 프로세스 QPC 비교를 선택했다. 10000→100000→1000000행을 실행했다. 생성·검증 Python, C 측정 도구, 작은 CTest8개와 빌드 연결을 추가했다. 선택·대안·GWT 원본은 [계약](../../.cache/m11/contract.json)과 [Planner](../../.cache/m11/planner-result.json)을 따른다.

자동 ID와 고유 비인덱스 scan_key, 결정적 payload를 생성한다. 독립 바이너리 decoder는 모든 ID·scan_key·payload·행 수·중복·checksum을 검증한다. 동일 projection의 ID/scan_key SQL을 한 DB open 뒤 교대로 실행한다. 100개 target은 hit80·miss20이며 warmup1회 뒤 측정3회, 방법별300표본(hit240·miss60)이다. 요청 생성부터 결과·EOF 소비까지 측정하며 oracle 검증·JSON 출력은 구간 밖이다. QPC 정수 ns 변환은 부호·overflow를 검사하고 nearest-rank p50/p95를 산출한다. [작성자](../../.cache/m11/implementer-result.json), [QPC 원문](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps)

## Given / When / Then

| 검사 | Given | When | Then |
|---|---|---|---|
| M11-GENERATE-01 | 고정 입력·배치 경계 | 두 번 생성 | 동일 바이트·자동 ID·마지막 부분 배치 |
| M11-DECODE-01 | 정상·손상·중복·절단 파일 | 전 필드 decode | 정상 일치·잘못된 파일 거절 |
| M11-ORACLE-01 | hit/miss·틀린 기대값 | 실제 결과 대조 | 값·메타데이터 일치·오류 거절 |
| M11-BENCH-01 | 작은 정상 DB | warmup1·측정3 | 동일 projection·교대 순서·600표본 |
| M11-STATISTICS-01 | ticks·경계·기지 표본 | ns·분위수 계산 | 정확 통계·부호/overflow 거절 |
| M11-ARGUMENT-01 | 잘못된 인자·기존 목적지 | 도구 실행 | 비정상 종료·기존 데이터 보존 |
| M11-OUTPUT-01 | 정상·불완전 JSON | 독립 재계산 | 원시값·집계 일치·잘못된 결과 거절 |
| M11-REGRESSION-01 | 새 작은 캐시 경로 | 생성→CLI→decode→측정 | 전체 경로 정확성·단계별 시간 |

## 실제 검사와 실패

- configure/build 성공 후 ORACLE·BENCH·STATISTICS의 assertion 실패를 실제 Red로 확인했다. 초기 Red는 미구현 도구/statistics stub을 대상으로 했으며 완성 기능의 회귀 증거는 최종 검사에 있다. [Red 보고서](../../.cache/agent-harness/m11-native-20261118/commands/1-red/test-report.json)
- 최종 작성자와 독립 Reviewer_QA는 각각 새 configure/build·전체 CTest158/158 PASS, skip0을 확인했다. 독립 QA는 세 데이터셋의 전체 SQL·저장 전 필드·원시 통계·바이너리 provenance를 재검증하고 자체137행 실제 실행·payload 손상 거절도 확인했다. 지적·품질 반려0회다. [작성자 회귀](../../.cache/agent-harness/m11-native-20261118/commands/6-regression/test-report.json), [독립 결과](../../.cache/m11/reviewer-result.json)
- 작성자 자체 검토에서 Python bool이 숫자로 수용될 수 있는 메타데이터 검증을 엄격화하고 fixture를 추가했다. 측정 당시 source와 최종 source는 다르며 native 도구·엔진 소스·실행 파일 해시는 유지됐다. 기존 전 규모 근거를 최종 검증기로 재검증했고100만 DB 재실행은 하지 않았다. digest·해시·검사 상태는 [최종 검증 JSON](../../.cache/m11/validation-final.json)에 보존한다.
- 독립 보조 스크립트의 Windows cp949 기본 읽기는 명시적 UTF-8로 수정 후 통과했다. 환경 실패이며 기능 Red나 제품 결함으로 집계하지 않는다. [독립 결과](../../.cache/m11/reviewer-result.json)

## 측정 결과

고정 Windows C23 **Debug**, DB open·전체 메모리 적재 후 warm 상태의 SQL 실행 결과다. 각 규모 실제 실행1회 안에서 측정3라운드를 수행했다. CLI 적재·전체 바이너리 검증·open은 조회와 별도 측정했다.

| 행 수 | CLI 적재 s | 전체 검증 s | open s | ID p50/p95 µs | scan_key p50/p95 µs |
|---|---:|---:|---:|---:|---:|
| 10000 | 0.060548 | 0.034880 | 0.005448 | 27.4 / 29.4 | 175.9 / 182.9 |
| 100000 | 0.400547 | 0.562797 | 1.643653 | 51.7 / 58.7 | 2409.5 / 2503.3 |
| 1000000 | 6.422285 | 3.366470 | 118.711853 | 37.2 / 48.9 | 15673.2 / 18346.5 |

원본은 [10000행](../../.cache/m11/fullscale/rows-10000/report.json)·[100000행](../../.cache/m11/fullscale/rows-100000/report.json)·[1000000행](../../.cache/m11/fullscale/rows-1000000/report.json)이다.100만 행 p50의 scan_key/ID 비율은 약421배다. 표는 이 실행 조건에서 관측한 값이며 Release·디스크 전용 검색·일반 성능 보장으로 확대하지 않는다.

## 한계와 다음 행동

100만 행의 open118.7초는10만 행1.64초보다 크게 증가했다. 시작 비용의 profiling·원인 분리·소스 최적화는 수행하지 않았다. 독립 QA는100만 근거를 전수 읽었으며100만 DB 재open·재측정은 하지 않았다. 메모리/누수·전원 손실·전수 fault·Phase 1 전체204개 감사는 미실행이다. Phase 3 상세 설계 또는 시작 비용 진단·최적화 중 다음 범위를 Master가 사용자와 협의한다.

문서의 원문 대조·UTF-8·링크·보호 입력 해시·Wiki 등록·정상 finish 관측은 [Knowledge JSON](../../.cache/m11/knowledge-result.json)을 따른다. 사용자 완료 승인·DONE 반영의 문서 마감은 [완료 JSON](../../.cache/m11/completion-result.json)을 따른다.
