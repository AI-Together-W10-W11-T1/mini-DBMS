# Windows·C23 도구 선택과 실제 확인

2026-11-17 화요일(Asia/Seoul) 현재 상태다. 사용자는 공식 LTS를 우선하고 없으면 최신 안정판을 고정하는 정책, cmocka·CTest를 선택했다. 아래 버전은 선택 후 프로젝트 내부 설치·실제 실행 검증을 마쳤다. LTS가 아닌 도구를 LTS라고 부르거나 자동 최신화하지 않는다.

| 역할 | 고정 구성 | 실제 확인 범위 |
|---|---|---|
| 컴파일러·링커 | LLVM-MinGW 20261104 UCRT x86_64, Clang/LLD 23.1.5 | -std=c23 compile/link/run, __STDC_VERSION__=202311·static_assert·typeof·nullptr, int64/pointer8 및 실제 DBMS 빌드. 전체 C23 적합성 보장은 아님. |
| 디버거 | 동봉 LLDB 23.1.5 | native breakpoint·next·local42·backtrace·정상 종료 smoke를 독립 확인. 모든 디버깅 기능 검증은 아님. |
| 빌드·집계 | CMake/CTest 4.5.0, Ninja 1.13.2 | 실제 configure/compile/link와 CTest 성공/실패 전파 및 M1 회귀. |
| 단위 시험 | cmocka 2.0.1 | 정적 cmocka::cmocka 연결, upstream C99·자체 C23 분리, 세 함수의 실제 cmocka 검증. Unity 추가 도입 안 함. |
| JSON | yyjson 0.13.0 | 소스/헤더/라이선스 반입, 정적 빌드·실제 엄격한 스키마 파싱. |

경로·URL·SHA-256은 [tools/c-toolchain-lock.json](../tools/c-toolchain-lock.json)에 고정했다. 설치·probe 증거는 `.cache/bootstrap/install-receipt.json`, 독립 확인은 `.cache/bootstrap/toolchain-reviewer/qa-receipt.json`이다. cmocka/yyjson 해시는 공식 HTTPS 원본에서 로컬 계산했고 cmocka detached signature를 암호학적으로 검증하지 않았다. 독립 재다운로드는 불가했으며 로컬 hash/vendor 일치를 확인했다. 고지 위치는 [라이선스 기록](third-party-licenses.md)을 따른다.

`python scripts/c_checks.py build`와 `regression`이 명시한 고정 기본 경로·자식 프로세스 PATH를 사용한다. CMake preset 자체가 고정 컴파일러를 지정하지는 않는다. 재현 명령·실행 파일 경로는 [README](../README.md)에 있다. 전역 PATH/레지스트리 변경이나 CIM 조회·Node/npm 도입은 하지 않았다.

현재 M1은 [검증 결과](testing/m1-results.md)처럼 실제 구현·독립 QA를 통과했고 최종 사용자 확인을 기다린다. 후속 상세 설계는 [Phase별 원칙](phase-design-policy.md)을 유지한다. 최소 모듈·소유권·오류/실패 계약·GWT를 해당 Phase에서 검토하고 미래 API/동기화를 지금 일괄 고정하지 않는다. 설계 전용 하네스도 제공하며 구현용 실제 Red/build gate를 문서 검토로 대체하지 않는다.

기존 공식 출처: [LLVM-MinGW 20261104](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261104), [Clang C 지원](https://clang.llvm.org/c_status.html), [LLDB](https://lldb.llvm.org/), [CMake](https://cmake.org/download/), [Ninja](https://github.com/ninja-build/ninja/releases/tag/v1.13.2), [cmocka](https://cmocka.org/). 이 문서 갱신은 기존 lock·로컬 설치/실행 증거를 대조한 것이며 업스트림 최신 버전을 새로 추천하거나 재조회한 것은 아니다.
