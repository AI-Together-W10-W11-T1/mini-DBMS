# mini-DBMS 설정 상태

2026-11-17 화요일(Asia/Seoul). 승인된 M1 구현·독립 QA를 통과했고 **최종 사용자 확인 대기**다. [실제 결과](testing/m1-results.md)와 [인수인계](../HANDOFF.md)가 현재 상태의 원본이다. 과거 문서의 미설치·stub·검사 미연결은 당시 상태이며 현재 상태로 해석하지 않는다.

## 설치·빌드·실제 실행

| 구성 | 고정 버전·확인 범위 |
|---|---|
| 컴파일러·디버거 | llvm-mingw 20261104 UCRT x86_64, Clang/LLDB 23.1.5. 프로젝트 내부 설치. 특정 C23 기능 실행과 LLDB breakpoint/step/local42/backtrace/종료 확인. |
| 빌드 | CMake/CTest 4.5.0, Ninja 1.13.2. 실제 configure·compile·link·CTest 실행. |
| 테스트 | cmocka 2.0.1 정적 테스트 의존성, 자체 upstream C99·DBMS C23. 성공0/의도적 실패8 전달 probe와 실제 단위 검사. |
| JSON | yyjson 0.13.0 소스·헤더·MIT 고지를 vendor 반입, 실제 C 정적 라이브러리로 빌드·스키마 파싱에 사용. |
| 실제 DBMS | init·단일 INSERT·기본 SELECT·파일/명시적 stdin·table/JSONL·바이너리 저장/별도 프로세스 재시작·핵심 실패 처리를 구현. |
| 최종 독립 QA | 새 build 및 CTest 16/16 PASS, 추가 SQL/스키마/저장36·실제 stdout5 PASS. |

경로·URL·해시는 [lock](../tools/c-toolchain-lock.json), 설치 증거는 `.cache/bootstrap/install-receipt.json`, 독립 도구 증거는 `.cache/bootstrap/toolchain-reviewer/qa-receipt.json`이다. C23 probe의 `__STDC_VERSION__=202311`, int64/pointer 8은 특정 기능 확인이며 C23 전체 적합성을 보장하지 않는다.

llvm-mingw/CMake/Ninja 해시는 공식 릴리스 메타데이터를 출처로 썼다. cmocka/yyjson은 공식 HTTPS 원본의 로컬 해시다. cmocka detached signature는 암호학적으로 검증하지 않았고 독립 업스트림 재다운로드도 불가했다. 로컬 archive·vendor 일치와 안전한 추출은 확인했다. 라이선스 위치는 [고지 기록](third-party-licenses.md)을 따른다.

## 검사와 하네스

`python scripts/c_checks.py build|red|green|regression`은 실제 고정 C 도구와 새 CTest JUnit/로컬 결과 어댑터를 연결한다. 빌드·import 실패, 누락·skip·오래된 보고서는 기능 성공이나 Red가 아니다. 재현 명령과 기본 실행 파일 경로는 [README](../README.md)에 있다. CMake preset만으로 고정 도구가 선택되지는 않는다.

정식 `m1-native-20261117`은 실제 Planner→Implementer→Reviewer_QA→Knowledge Manager 배정을 수행한다. 최초 실제 Red는 단위1/CLI15 실패이며 최종 Reviewer2 회귀는 모두 PASS다. 현재 최종 확인은 사용자에게 남긴다. 하네스 유지보수의 작성자65/독립 reviewer61개 모의 단위 PASS와 DBMS 결과를 구분한다. 설계 전용 kind=design 경로와 docs 명시 whitelist도 구현됐고 기존 기능 Red/build gate를 유지했다.

과거 bootstrap finish는 checks 변경으로 Wiki 원문 해시 stale 때문에 실패했다. 현재 원문 대조·재등록 후 정식 21-finish exit 0으로 Graphify 갱신·Wiki 검사를 통과했다. 결과 문서화 뒤 최신 finish를 다시 수행하며 최신 영수증과 상세는 [DBMS-001](testing/records/DBMS-001.md)을 따른다. 이를 SQL 기능 검사의 대용으로 쓰지 않는다.

## 미완료·보류

- 나머지 Phase 1 SQL·migrate, Phase 2 B+ 트리/자동 ID/100만 건, Phase 3 API/Thread Pool은 남아 있다. 시작 시 행 적재와 이차 시간 ID 중복 검사, 선형 SELECT의 성능을 개선했다고 주장하지 않는다.
- RTK 1.2.0·Graphify 0.9.104와 전용 Python3.12.13은 기존 설치·실행 확인 구성이다. Graphify는 정적 구조 탐색이며 실행 증거가 아니다.
- 기존 플러그인 enabled/cache와 실제 현재 세션 호출 가능 여부는 다르다. 불필요한 외부 도구를 새로 도입하지 않는다.
- Node/TS/npm·새 MCP 제외, Playwright Test/Node worker·Context7·Superpowers·Ponytail·RAG·fine-tuning 보류를 유지한다.
- Goal 목표는 사용자가 명시 유지했다. Master 확인 도구 상태는 usageLimited이며 active/complete로 단정하지 않는다. task 재개 승인과 도구 Goal 상태는 구분한다.
