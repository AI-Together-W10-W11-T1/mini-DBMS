# RTK · Graphify · Wiki

RTK 1.2.0은 긴 Git·테스트 로그에 선택 적용한다. Graphify 0.9.104는 code-only, worker 2, 질의 budget 1000으로 작업 종료 시 갱신한다. Wiki는 실제 원문과 대조한 검증 지식만 작업 종료 시 작성한다.

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
