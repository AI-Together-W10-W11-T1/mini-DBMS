# RTK · Graphify · Wiki

RTK 1.2.0은 긴 Git·테스트 로그에 선택 적용한다. Graphify 0.9.104는 code-only, worker 2, 질의 budget 1000으로 설정했다. 문서·Wiki 등록·finish 순서와 영향 검사·JSON 근거 참조는 [단일 문서 마감 정책](agent-workflow.md#문서-마감)을 따른다.

Graphify의 코드 변경 지문은 `docs` 아래 JSON을 제외한다. 문서 Markdown과 같은 취급이며, 코드와 도구 설정의 추가·수정·삭제는 계속 자동 갱신을 요구한다. Wiki 페이지·출처 검토는 별도 검사로 유지한다. `finish`는 실제 Graphify 실행과 Wiki 검사 실패를 그대로 반환한다.

`scripts/c_checks.py`는 정책 환경 변수가 없으면 기존 경로와 전체 검사 동작을 유지한다. `AGENT_HARNESS_WORKFLOW_POLICY=slim-v1`에서만 `targeted`와 `red`가 `AGENT_HARNESS_TEST_TARGETS`의 JSON 배열에 있는 정확한 CTest 이름을 선택한다. 빈 배열·중복·미등록 이름은 거절하고, `regression`은 항상 전체 검사한다. 선택 실행은 pinned CTest 4.5.0의 `--tests-from-file`을 사용한다.

slim 빌드는 실행별 작업 폴더의 `IMPLEMENT`/`VERIFY` 아래 도구·옵션 서명별 캐시를 사용한다. 역할과 도구 변경 시 트리가 분리되며 기존 트리를 삭제하지 않는다. 매 호출 실제 configure·compile을 실행하고 새 UUID 폴더에 JUnit·로그·report·receipts를 남긴다. 외부 결과 경로에는 해당 호출 보고서를 복사한다. 보고서에는 실행·actor·source digest·선택 범위·정확한 이름·빌드 종료 코드·캐시 재사용·단계별 시간이 포함된다. 실패·누락·중복·알 수 없는 테스트·skip·JUnit 미생성은 성공으로 인정하지 않는다. 이 시간과 호출 수만으로 비용 절감이나 TDD 효과를 추정하지 않는다.

`test_c_checks.py`는 격리된 임시 폴더에서 subprocess를 모의하는 Python 운영 검사다. 실제 C 컴파일·CTest 증거는 별도 실행한 네이티브 검사 로그로 구분한다. Python 3.9 표준 라이브러리만 사용한다.

정상 JUnit의 assertion 실패는 해당 spec의 `failed` 상태와 0이 아닌 종료 코드로 기록하고 전역 `errors`는 비워 기능 Red로 구분한다. 회귀 검사에서도 이 실패는 성공이 아니다. skip·누락·알 수 없는 이름·잘못된 보고서·모든 spec이 통과했는데 CTest가 비정상 종료한 경우는 전역 오류다.

```powershell
python scripts/dev_tools.py status
python scripts/dev_tools.py rtk -- git status
python scripts/dev_tools.py graph-build
python scripts/dev_tools.py graph explain source_hashes
python scripts/dev_tools.py wiki-check
python scripts/dev_tools.py finish
python -m unittest discover -s scripts -p 'test_*.py'
```

실행 파일 위치: `.cache/tooling/rtk/1.2.0/rtk.exe`, `.cache/tooling/graphify/Scripts/graphify.exe`. Graphify 전용 Python은 `.cache/tooling/python/`에 둔다. 전역 Python·Node·Git 설정은 변경하지 않는다. 그래프·개인 기록·이식 원본 폴더는 서로 분리하며 색인에서 `.private`, `.cache`, `codex-setup-github`를 제외한다.

RTK 공식 ZIP: https://github.com/rtk-ai/rtk/releases/download/v1.2.0/rtk-oss-x86_64-pc-windows-msvc.zip
ZIP SHA-256: d105aaaa55822252f04ac28989e9069c1ea61755ca561940d887cb0b89c785fc
EXE SHA-256: ede70a7ae605525670d6862d54dbdacbeed383e4a8d81cbcdd0b33b6ef530732
Graphify 의존성은 `tools/graphify-requirements.txt`에 고정한다. tree-sitter-typescript는 Graphify의 구문 분석 라이브러리이며 TypeScript 컴파일러나 Node 런타임 설치가 아니다.

측정: `python scripts/eval_dev_tools.py --rtk .cache/tooling/rtk/1.2.0/rtk.exe`. 출력 bytes/4를 실제 전체 세션 토큰·금액 절감으로 보고하지 않는다. 설치·실행·검사 상태는 `docs/setup-status.md`와 실제 status 결과를 확인한다.

final-once-v1의 IMPLEMENT는 build·red·targeted를 허용하고 red·targeted에 정확한 선택 이름을 요구한다. 작성자는 실제 configure/build를 포함한 최신 targeted로 인수한다. 독립 Reviewer_QA가 검토와 필요한 선택 검사 이후 최종 구현에서 전체 regression을 한 번 수행한다. 실패나 후속 수정에 필요한 재검사는 허용한다. 정상 정책 전환은 [하네스](agent-harness.md#최종-구현-이후-전체-회귀)를 따른다.
