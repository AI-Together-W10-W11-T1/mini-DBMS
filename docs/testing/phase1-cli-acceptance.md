# Phase 1 CLI 인수 검증 명세

출처: 2026-11-17 화요일(Asia/Seoul), 사용자가 `ㄱㄱ`로 승인한 CLI 문서화 계획. 단일 원본은 [CLI 계약](../cli-contract.md)이며 [SQL](../sql-contract.md)·[스키마](../schema-contract.md)·[저장](../storage-contract.md) 계약과 함께 적용한다.

**48개 사례 전부 미실행(UNEXECUTED)**이다. Given/When/Then은 구현 후 검증할 요구이며 현재 C 바이너리·SQL 실행·실패 주입·Windows API 검사 통과를 뜻하지 않는다. 이 문서 작성 때 가능한 정적 검사는 기능 Red/Green이 아니다.

## 공통 준비와 판정

- 별도 임시 작업 디렉터리에서 UTF-8 스키마 S와 계약에 맞는 users 데이터 D를 준비한다. users는 `id INT64`, `name TEXT`, `age INT64`이며 id는 필수 양수·고유다. 새 출력 NEW는 존재하지 않는다. 저장/스키마 오류 사례는 해당 계약의 명시적 잘못된 fixture를 사용한다.
- 아래 명령은 `minidb`를 실행 파일로 사용하며 공통 필수 경로를 생략해 적은 경우 유효한 경로를 제공한다. JSONL 검사는 명시 `--format jsonl`, table 검사는 기본값 또는 명시 `--format table`을 사용한다.
- 실제 프로세스의 종료값, stdout/stderr 원시 바이트, 생성/보존한 파일, 이후 SQL 미실행을 함께 확인한다. 종료값만으로 저장 반영을 추론하지 않는다. 순서가 필요한 쿼리에는 고유 id ORDER BY를 붙인다.
- JSONL 객체 키 순서와 진단 message 문구는 고정하지 않는다. 필수 필드·타입·값·배열 순서·LF·UTF-8·BOM 부재는 검사한다. 진단은 table 모드에서는 사람이 읽는 한 줄, JSONL 모드에서는 stderr JSONL이다. 두 형식의 논리 필드는 같다. SQL 위치는 전체 입력 기준이며 비해당 위치를 null로 둔다.
- I/O 실패·복원 실패·정리 실패·자원 부족은 실제 구현의 재현 가능한 실패 주입 지점으로 검증한다. 환경/import 실패나 모의 운영 하네스 테스트를 이 사례의 기능 Red로 세지 않는다.

## Given / When / Then

| ID | Given | When | Then | 상태 |
|---|---|---|---|---|
| P1-CLI-001 | 유효한 S, 존재하지 않는 NEW | `init --schema-dir S --data-dir NEW` | 0, 전체 빈 데이터 준비, table 관리 성공 한 묶음, 각 action created·row_count 0·output_ready true | UNEXECUTED |
| P1-CLI-002 | 유효한 S/D와 INSERT 뒤 SELECT인 F | 필수 경로·`--file F --format jsonl`로 run | 0, statement_ok 뒤 query_result, 문장 번호 1·2, 반영과 반환값 일치 | UNEXECUTED |
| P1-CLI-003 | 유효한 S/D와 열린 stdin 파이프 | 완성 INSERT를 보내고 EOF 전에 결과를 관찰한 뒤 SELECT와 EOF 전송 | EOF 전체 사전 검증 없이 문장별 실행, 앞선 성공 확인 가능, 0 | UNEXECUTED |
| P1-CLI-004 | 원본 OLD와 호환 새 스키마, NEW 없음 | plan 없이 migrate의 필수 경로 지정 | 0, 원본 보존, 전체 새 출력 검증·준비 완료, 동일 정의 파일 action copied | UNEXECUTED |
| P1-CLI-005 | 서로 다른 CWD·실행 파일 위치, 상대 S/D/F | CWD에서 상대 경로로 run | CWD 기준 경로만 사용, 실행 파일 위치·환경 변수·설정 fallback 없음 | UNEXECUTED |
| P1-CLI-006 | 같은 유효한 결과를 터미널과 리디렉션으로 실행 가능 | format 생략 및 table 명시로 각각 실행 | 모두 같은 table 규칙, TTY 기반 JSONL 전환·색상·프롬프트 없음 | UNEXECUTED |
| P1-CLI-007 | DB 경로를 열면 실패하는 감시 fixture | 루트/하위 명령의 단독 help와 단독 version 실행 | 각각 0, stdout 도움말/실제 버전, DB·스키마·SQL·잠금 접근과 출력 생성 없음 | UNEXECUTED |
| P1-CLI-008 | 임의의 유효 경로 | 미지 명령/옵션, 대문자 명령, 잘못된 format, 위치 인자, `--key=value`, 짧은 옵션을 각각 전달 | ARGUMENT_ERROR·2, 성공 stdout 없음, DB 변경 없음 | UNEXECUTED |
| P1-CLI-009 | 임의의 유효 경로 | 필수 옵션 누락·값 누락·중복 옵션·값 없는 옵션의 값·help 혼합을 각각 전달 | ARGUMENT_ERROR·2, 다음 옵션을 값으로 소비하지 않음 | UNEXECUTED |
| P1-CLI-010 | 유효한 S/D | run에 file/stdin 모두 또는 둘 다 없이, 다른 명령 전용 옵션을 전달 | ARGUMENT_ERROR·2, SQL 실행 없음 | UNEXECUTED |
| P1-CLI-011 | 엄격한 UTF-8 SQL, 시작 BOM, 한글·실제 CRLF가 든 TEXT | file과 stdin으로 각각 run | BOM 허용, TEXT 원바이트 보존, 입력 줄바꿈 치환 없음, 성공 결과 일치 | UNEXECUTED |
| P1-CLI-012 | 앞에 주석·빈 문장·CRLF, 뒤에 잘못된 SQL | run | 비어 있지 않은 SQL만 1부터 번호, CRLF 한 줄, BOM 제외 첫 문자 열 1, 전체 입력 행/열의 정확한 진단 | UNEXECUTED |
| P1-CLI-013 | INSERT 성공 후 잘못된 SELECT 뒤 다른 INSERT인 입력 | file과 stdin으로 각각 run | 첫 SQL 오류·4, 첫 INSERT 유지, 뒤 INSERT 미실행, 기존 SQL 코드 보존 | UNEXECUTED |
| P1-CLI-014 | 유효 DB, 끝 세미콜론이 없는 INSERT | EOF 도달 | SYNTAX_ERROR·4, 불완전 INSERT 미반영, 이전 성공이 있으면 유지 | UNEXECUTED |
| P1-CLI-015 | 닫히지 않은 문자열/주석 또는 잘못된 UTF-8 SQL | run | LEXICAL_ERROR·4, 해당 문장 미실행, 임의 관리 코드로 재분류하지 않음 | UNEXECUTED |
| P1-CLI-016 | 유효 DB와 공백·주석·빈 문장뿐인 입력 | file 및 stdin으로 run | DB 준비 검증 수행 후 stdout 0바이트·종료 0 | UNEXECUTED |
| P1-CLI-017 | 잘못된 데이터 파일과 빈 SQL | run | 빈 입력이어도 준비 오류·3, SQL 성공으로 우회하지 않음 | UNEXECUTED |
| P1-CLI-018 | 유효 DB와 최초 열 수 없는 F | `run --file F` | IO_ERROR·prepare·3, SQL 실행/반영 없음, 모르는 SQL 위치 null | UNEXECUTED |
| P1-CLI-019 | 첫 INSERT 성공 뒤 읽기 장치 오류, 뒤 SQL 있음 | 입력 중간 읽기 실패 주입 | IO_ERROR·input·4, 정상 EOF로 오인하지 않음, 앞선 반영 유지·다음 문장 미실행 | UNEXECUTED |
| P1-CLI-020 | 기존 NEW 또는 원본과 겹치는 출력 경로 | init/migrate 실행 | 출력 생성 전 경로 준비 오류·3, 기존 파일 덮어쓰기·원본 변경 없음 | UNEXECUTED |
| P1-CLI-021 | 잘못된 외부 스키마 또는 저장 데이터/비호환 헤더 | run 및 해당 관리 준비 실행 | SCHEMA_ERROR/STORAGE_FORMAT_ERROR/SCHEMA_MISMATCH 중 실제 원인 코드·3, SQL/출력 생성 없음 | UNEXECUTED |
| P1-CLI-022 | 다른 프로세스가 동일 실제 DBdir 사용권 보유 | 별칭 경로로 같은 디렉터리 실행, 별개 디렉터리도 실행 | 같은 디렉터리는 DATABASE_BUSY·3, 별개 디렉터리는 독립 실행 가능 | UNEXECUTED |
| P1-CLI-023 | 시작 시 미해결 진행 표식/작업 잔여물 존재 | run | RECOVERY_REQUIRED·prepare·3, 전수 행 검사가 정상이어도 차단·파일 보존 | UNEXECUTED |
| P1-CLI-024 | INT64 최솟값/최댓값, 한글 TEXT, NULL, TEXT 'NULL', 빈 TEXT 결과 | JSONL 쿼리 | query_result 필수 필드·순서 있는 columns/rows, 정수 문자열·TEXT 문자열·NULL null, UTF-8 BOM 없음·레코드 LF | UNEXECUTED |
| P1-CLI-025 | 0행 SELECT와 NULL 전용 VALUES, 혼합 NULL/INT64 UNION | JSONL 쿼리 | 빈 결과에도 columns·rows []·row_count 문자열 0, NULL 전용 type NULL, 상대 타입 추론 시 INT64 | UNEXECUTED |
| P1-CLI-026 | 컬럼 별칭과 JOIN/UNION 출력 이름·순서가 정해진 쿼리 | table/JSONL 출력 | SQL 계약의 최종 이름과 컬럼 순서 보존, 입력 이름으로 되돌리거나 별도 정렬하지 않음 | UNEXECUTED |
| P1-CLI-027 | INSERT 성공과 0행을 포함한 UPDATE/DELETE 성공 | JSONL 출력 | statement_ok, 대문자 command, 정확한 문자열 affected_rows·statement_index, committed true | UNEXECUTED |
| P1-CLI-028 | 유효한 ASCII 컬럼명과 값에 pipe·따옴표·역슬래시·TAB·CR/LF가 든 TEXT, 긴 TEXT·많은 행 | table 쿼리 | 헤더 name:type, pipe 구분, TEXT JSON 인용·pipe를 역슬래시 u007c로 escape, 행당 물리적 1줄, 생략·폭 자르기 없음 | UNEXECUTED |
| P1-CLI-029 | table 0행 쿼리와 DML·관리 성공 | 기본 출력 | 계약의 statement/query_result·헤더·row_count 및 DML/operation/table 고정 줄, 빈 결과 메타데이터 보존 | UNEXECUTED |
| P1-CLI-030 | 후반 VALUES/UNION 타입 오류 또는 전체 결과 준비 자원 부족 | 쿼리 run | 해당 쿼리 성공 stdout 없음, TYPE_MISMATCH 또는 RESOURCE_ERROR·4, 부분 결과를 먼저 공개하지 않음 | UNEXECUTED |
| P1-CLI-031 | 첫 쿼리 출력 write/flush 실패, 다음 INSERT 대기 | stdout 실패 주입 | IO_ERROR·output·7, 다음 INSERT 미실행, 잘린 출력 가능성을 진단으로 구별 | UNEXECUTED |
| P1-CLI-032 | INSERT 반영 확정, stdout write/flush 실패, 다음 SQL 존재 | JSONL/table 출력 실패 주입 | 7, INSERT 유지, stderr committed true·command INSERT·확정 affected_rows, 롤백/자동 재실행·다음 SQL 없음 | UNEXECUTED |
| P1-CLI-033 | DML 확정 전 저장 write 실패·복원 성공 | run | IO_ERROR·4, committed false·rollback confirmed, 해당 문장 변경 0·이전 성공 유지 | UNEXECUTED |
| P1-CLI-034 | DML 확정 후 임시/백업 정리 또는 표식 삭제 실패 | run | 성공·affected_rows 유지, cleanup 진단·committed true·종료 5, 다음 SQL 중단·잔여물 보존 | UNEXECUTED |
| P1-CLI-035 | DML 도중 복원 실패 또는 저장 상태 확인 불가 | run | RECOVERY_REQUIRED·6, rollback failed/unknown·committed null, 파일/표식 보존·다음 SQL 및 재시작 차단 | UNEXECUTED |
| P1-CLI-036 | 관리 출력 생성 시작 후 파일 작성/검증/정리 실패 | init/migrate | 4, output_ready false, operation_ok 없음, 원본 보존, 불완전 새 출력 일반 run 차단. 정상 불완전을 6으로 바꾸지 않음 | UNEXECUTED |
| P1-CLI-037 | 모든 관리 파일 준비 완료 뒤 stdout만 실패 | init/migrate | 7, output_ready true 진단, 준비된 새 출력은 유지·자동 작업 재실행 없음 | UNEXECUTED |
| P1-CLI-038 | 복수 실패를 주입할 수 있는 실패 fixture | 미해결 저장+출력, 확정 후 정리+출력, 실행+출력의 조합 검증 | 각 진단 보존, 종료값 6 > 5 > 7 > 4 > 3 > 2 규칙 일치, 낮은 우선순위로 상태를 숨기지 않음 | UNEXECUTED |
| P1-CLI-039 | 원인 오류 뒤 stderr write/flush도 실패 | 실제 프로세스에서 오류 재현 | 원래 의미의 비0 종료값 유지, 진단 전달 한계 외 DB 재실행/롤백/성공 둔갑 없음 | UNEXECUTED |
| P1-CLI-040 | SQL 오류·JSON 오류·입력 I/O 오류 각각 | table/JSONL run | table에서는 사람용 한 줄·JSONL에서는 진단 객체, 공통 필드 전부 보존, SQL만 line/column, JSON field_path/byte_offset, 미확인 null, 성공 stdout과 분리 | UNEXECUTED |
| P1-CLI-041 | BOM 포함 유효 plan, tables 빈 배열 또는 일부 테이블 생략, map/drop 생략 | migrate | JSON 구조 허용, 생략 테이블 동일 이름 대응/drop 빈 배열, 저장 계약 의미에 맞으면 성공 | UNEXECUTED |
| P1-CLI-042 | plan에 root/version/tables 누락·잘못된 타입·version !=1·null·map/drop 원소 구조 오류 | migrate | ARGUMENT_ERROR·2, field_path/byte_offset 아는 범위 진단, 출력 생성 없음 | UNEXECUTED |
| P1-CLI-043 | plan 주석·후행 쉼표·잘못된 UTF-8/Unicode·unknown field·디코딩 후 중복 object 키 | migrate | 엄격 JSON ARGUMENT_ERROR·2, 중복 키 덮어쓰기 없음 | UNEXECUTED |
| P1-CLI-044 | plan 파일 접근 불가 또는 casefold 중복 테이블/target/source/drop·없는 이름·사용하지 않는 매핑 | migrate | 접근은 IO_ERROR·3, 나머지는 MIGRATION_ERROR·3, 출력 생성 전 거절, 구조 오류 2와 구별 | UNEXECUTED |
| P1-CLI-045 | 명시 rename/drop·동일 이름 기본 대응·새 비ID NULL·컬럼 순서 변경·id 유지인 유효 plan | migrate | 전체 변환 성공, 새 순서/값 일치, 출력 tables name 정렬·새 원철자·converted/row_count, 원본 보존 | UNEXECUTED |
| P1-CLI-046 | 원본/새 테이블 집합 차이, 미사용 원본 drop 누락, id 삭제/재대응 등 잘못된 plan | migrate | 저장 매핑 의미대로 MIGRATION_ERROR·3, 출력 생성 없음·원본 보존 | UNEXECUTED |
| P1-CLI-047 | 동일 스키마지만 동일 타입 두 컬럼을 교환하는 명시 비항등 map | migrate | copied로 생략하지 않고 converted, 모든 출력 값이 명시 매핑대로 이동, id/원본 유지 | UNEXECUTED |
| P1-CLI-048 | TEXT→INT64에서 허용 ASCII 양끝 공백·부호·경계값/NULL과 잘못된 값·범위 초과 각각 | migrate | 유효 변환/INT64→TEXT는 저장 계약대로 보존, 잘못된 실제 행은 MIGRATION_ERROR·4·output_ready false, 부분 관리 성공 공개 없음·원본 보존 | UNEXECUTED |

## 실행 기록

향후 실제 구현 검증 때 각 ID별 입력 fixture·실행 명령·종료값·stdout/stderr·저장 전후 상태·실패 주입 위치·최신 실행 출처를 남긴다. 현재 48/48 미실행이며 자동 테스트 코드·C 빌드·Windows 파일 API 실행·출력 장치 실패 검증은 수행하지 않았다. 최종 문서 검토 통과와 실제 기능 통과는 별도로 기록한다.
