# 문서 마감 정책 정렬 결과

2026-11-18 수요일(Asia/Seoul). 사용자 명시 승인에 따른 문서 정책 유지보수다. 구현·독립 검토는 APPROVED이며 **사용자의 최종 ‘완료 승인’을 받아 이 수동 유지보수를 완료**했다. 새 Goal은 없다. 별도 M8은 사용자 완료 승인과 정상 하네스 DONE을 확인했다.

## 변경 이유와 승인 범위

사용자가 요청한 다섯 기준을 [작업 흐름의 문서 마감 절](../workflow/agent-workflow.md#문서-마감)에 모았다. 상세 결과는 증분 결과 원본에 한 번 기록하고 HANDOFF는 현재 상태·남은 일·다음 행동·링크를 짧게 유지한다. README는 실행 방법 변경에 맞춰 갱신하고 Wiki에는 검증한 새 결정·실패만 남긴다. 본문 편집과 원문 대조를 끝낸 뒤 필요한 Wiki 등록을 묶고 finish를 한 차례 실행한다. 이후 실패·추가 변경의 영향받은 검사와 출처·산출물 해시를 갱신하며 유효한 근거를 보존한다. 경과 시간·로그 경로·해시는 JSON 영수증과 보고서를 참조한다.

Implementer의 변경 범위는 AGENTS.md와 docs/workflow/agent-workflow.md·agent-harness.md·dev-tools.md 네 Markdown이다. 중복 규칙을 단일 원본 참조로 정렬했다. Master 단일 소통·역할 분리·독립 QA·작성자와 검토자의 각각 새 configure/build/전체 회귀·strict/slim의 기술 입력 관문·최신성/산출물 해시 검사·최종 사용자 확인을 유지한다. 기존 finish의 stale-only Graphify 갱신과 Wiki 검사를 그대로 사용한다. 정책 원문은 [AGENTS](../../AGENTS.md), [작업 흐름](../workflow/agent-workflow.md#문서-마감), [하네스](../workflow/agent-harness.md), [개발 도구](../workflow/dev-tools.md)다.

정상 하네스 계약 검사는 AGENTS.md 제어 파일 변경을 보호해 승인을 거절했다. Master의 구체 승인으로 실제 Implementer·Reviewer_QA에게 별도 수동 유지보수를 배정했다. 원래 보호와 운영 원장은 보존했다. 초기 정책 run에는 계획 후 중단 흔적이 남아 있다. 아래 완료된 실행 근거는 실제 수동 유지보수의 actor별 결과·영수증을 따른다.

## GWT와 검증

| ID | Given·When | Then |
|---|---|---|
| DOC-CLOSE-01 | 인수된 증분 근거를 문서화 | 상세 이유·결정·검사·실패·한계·다음 행동을 결과 원본에 한 번 기록 |
| DOC-CLOSE-02 | 현재/대기 작업을 인수인계 | HANDOFF에 짧은 상태·남은 일·다음·원본 링크 |
| DOC-CLOSE-03 | 실행 방법의 변경 여부를 검토 | 실제 실행 변경에 맞춰 README 갱신 |
| DOC-CLOSE-04 | 검증한 새 결정·실패를 Wiki에 반영 | 원문 대조 후 출처와 함께 등록 |
| DOC-CLOSE-05 | 승인 문서 편집이 모두 끝남 | Wiki 등록을 묶고 finish 한 차례, 기존 stale-only 그래프 갱신 |
| DOC-CLOSE-06 | 마감 실패 또는 이후 승인 편집 | 영향 검사·출처·해시 갱신, 기존 최신성·인수 조건 유지 |
| DOC-CLOSE-07 | 기계 증거와 최종 사용자 확인을 인계 | JSON 참조와 결과 해석, 역할·독립 전체 회귀·사용자 확인 유지 |

작성자 문서 검사29개와 독립 검사37개가 PASS다. UTF-8·공백·로컬 링크/원문 내 anchor·중앙 정책 참조·위 GWT와 기존 관문 보존을 확인했다. 독립 검토는 수정 전후 diff와 실제 dev_tools·하네스·roles 원문도 대조했다. 문서 정책 GWT는 검토 조건으로 사용했다.

작성자와 독립 Reviewer_QA가 각각 새 configure/build/전체 CTest를 실행해 기존123개를 모두 통과했다. 두 역할의 새 빌드는 configure/compile 종료0·errors=[]이며 역할별 별도 트리를 사용했다. 공개 보호 대상30개 소스·시험·빌드·runtime·설정 SHA와 실행 전후 핵심 runtime/설정5개가 일치했다. 문서 정책만 변경하는 명시적 slim-v1 사유에 따라 Red를 요구하지 않았다.

## 실패·미실행·한계

작성자의 첫 Unicode 편집 스크립트는 인코딩 해독 실패로 적용 전에 중단됐다. UTF-8 patch로 수정했다. 첫 문서 검사기는 CRLF 제목을 LF로만 분리해 실패했고 읽기 정규화 후29개를 통과했다. 독립 검토의 품질 지적은0건이다.

문서 검사는 정책 일관성과 지정 파일 범위를 확인한다. 실제 DBMS 기능 회귀는 별도로 실행한 CTest 근거를 따른다. 공개 보호30개에 대한 동일성을 확인했고 전체 workspace·개인 자료 감사는 수행하지 않았다. 네 정책 원문 밖 링크는 존재를 확인했으며 외부 URL 접속은 미검증이다. 실제 provider token이 제공되지 않았고 절감률·금액·TDD 인과효과는 미측정이다. 전체204개 인수 감사와 M7 무정렬 결과 순서 관찰은 다음 협의 대상으로 남는다.

## M8 완료 후 정합화와 마감

Master가 비동기 질문에 대한 실제 M8 완료 승인을 받고 정상 pause/resume/finish로 DONE을 확인한 뒤 공유 문서를 갱신했다. [M8 결과](m8-results.md)의 승인 상태와 완료 근거만 맞추고 실행 당시 상세·수치·한계는 보존했다. HANDOFF는 짧게 줄였고 README 첫머리의 진행 이력을 상시 HANDOFF 링크로 대체했다. 이 README 정렬은 이번 정책 전환에서 한 번 적용하며 실행 예제는 유지한다. Wiki에는 새 문서 마감 결정만 추가하고 기존 출처 목록을 보존한다.

작성자 결과는 `.cache/doc-close/implementation/result.json`, 독립 검토는 `.cache/doc-close/reviewer-result.json`이다. 각 역할의 policy-report.json·native-report.json·native-command-receipt.json에 실제 시간·명령·로그/보고서 해시를 보존한다. Knowledge의 문서/링크/범위·정책4개/보호30개 SHA 확인, 일괄 Wiki 등록과 직접 finish 실행 결과는 `.cache/doc-close/knowledge/` 및 `.cache/doc-close/knowledge-result.json`을 참조한다. 모든 본문을 확정한 뒤 Wiki 등록과 finish를 각각 한 차례 수행하며 마감 관측을 본문에 다시 옮기지 않는다. Knowledge에서는 제품 검사를 추가 실행하지 않는다.

현재 사용자의 최종 ‘완료 승인’을 Master가 이 정책 작업의 완료 확인으로 전달했다. 승인 후 상태 문서의 영향 검사·Wiki 등록·직접 finish 근거는 [완료 기록](../../.cache/doc-close/completion-result.json)을 따른다. 기존 작성자·독립 검증과 초기 정책 run의 계획 후 중단 이력을 보존했다. 다음 제품 작업은 전체204개 인수 감사 계획을 사용자와 협의한다.
