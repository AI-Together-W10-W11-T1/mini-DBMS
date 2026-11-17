# 제3자 라이선스와 반입 상태

2026-11-17 화요일(Asia/Seoul). M1에서 실제 반입/빌드한 의존성의 고지 원문과 lock을 대조했다. 프로젝트 자체 MIT 라이선스를 변경하지 않았다. 아래는 확인·보존 위치의 기록이며 모든 배포 상황에 대한 법률 판단은 아니다.

## yyjson 0.13.0

- 외부 JSON 스키마·저장 헤더 검증에 실제 사용하며 `vendor/yyjson/yyjson.c`, `yyjson.h`를 정적 라이브러리로 빌드했다.
- `vendor/yyjson/LICENSE`는 MIT License, Copyright (c) 2020 YaoYuan 고지를 포함한다. 소스와 배포물에 저작권·허가 고지를 보존한다.
- 버전·공식 tag 원본·세 vendor 파일 hash는 [lock](../tools/c-toolchain-lock.json)에 기록했다. 로컬 공식 HTTPS archive hash이며 업스트림 서명 인증이라고 주장하지 않는다.
- 기존 출처: [yyjson 0.13.0 LICENSE](https://github.com/ibireme/yyjson/blob/0.13.0/LICENSE).

## cmocka 2.0.1

- 테스트 전용 의존성이다. `.cache/tooling/cmocka/2.0.1/cmocka-2.0.1`에 설치했고 `cmocka::cmocka` 정적 대상으로 실제 단위 검사에 사용했다. 운영 엔진 소스와 구분한다.
- 설치 원문의 `LICENSE`에서 Apache License Version 2.0을 확인했다. 해당 라이선스 사본과 파일별 저작권·귀속 고지를 보존한다. 배포 시 포함된 NOTICE가 있으면 필요한 표시를 보존하고 수정 파일은 수정 사실을 표시한다.
- 공식 archive SHA-256은 로컬 계산이며 detached signature는 보존했지만 암호학적 검증을 하지 않았다. 독립 검토의 업스트림 재다운로드도 불가했다. 실제 배포 패키징·모든 파일별 재배포 조건의 최종 감사는 별도 작업이다.
- 기존 출처: [cmocka](https://cmocka.org/), [Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0).

## 도구 고지와 범위

설치 lock은 LLVM-MinGW `LICENSE.TXT`, CMake `doc/cmake/LICENSE.rst`, cmocka `LICENSE`, yyjson vendor 고지와 Ninja 공식 COPYING 출처를 보존한다. 도구·런타임을 외부 배포할 때는 해당 구성요소 전체의 고지를 별도로 확인한다. M1 로컬 개발 환경 구성은 공개 게시/배포 승인이 아니며 원격 게시나 유료 의존성을 도입하지 않았다.
