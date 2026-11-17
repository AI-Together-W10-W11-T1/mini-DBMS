"""승인된 설계 산출물의 실제 바이트와 로컬 참조를 검사한다. C 검사는 하지 않는다."""
import argparse
import json
import os
import re
from pathlib import Path
from urllib.parse import unquote, urlsplit

from agent_harness import HarnessError, file_hash, read_json, safe_path, write_json


def check_documents(root, manifest):
    errors = []
    artifacts, references = {}, {}
    for group, actual in (('artifacts', artifacts), ('references', references)):
        for name, expected in manifest[group].items():
            path = safe_path(root, name)
            actual[name] = file_hash(path) if path.is_file() else None
            if actual[name] != expected:
                errors.append('hash mismatch: ' + name)
    for name in artifacts:
        path = safe_path(root, name)
        try:
            text = path.read_text(encoding='utf-8-sig')
            if path.suffix == '.json':
                json.loads(text)
            else:
                # 이미지/인라인 링크와 Markdown 참조 정의를 모두 확인한다.
                links = re.findall(r'!?\[[^\]]*\]\((<[^>]+>|[^\s)]+)(?:\s+[^)]*)?\)', text)
                links += re.findall(r'^\s{0,3}\[[^\]]+\]:\s*(<[^>]+>|\S+)', text, re.M)
                for target in links:
                    target = target.strip('<>')
                    parsed = urlsplit(target)
                    if parsed.scheme or parsed.netloc or not parsed.path:
                        continue
                    relative = (path.parent / unquote(parsed.path)).resolve().relative_to(Path(root).resolve()).as_posix()
                    if not safe_path(root, relative).exists():
                        errors.append('missing local link: ' + name + ' -> ' + target)
        except (OSError, ValueError, UnicodeError) as error:
            errors.append(name + ': ' + str(error))
    return dict(run_id=manifest['run_id'], artifacts=artifacts, references=references,
                passed=not errors, errors=errors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True)
    parser.add_argument('--manifest', required=True)
    args = parser.parse_args()
    manifest = read_json(args.manifest)
    result = check_documents(args.root, manifest)
    if os.environ.get('AGENT_HARNESS_RUN_ID') != result['run_id']:
        raise HarnessError('document check run ID mismatch')
    write_json(os.environ['AGENT_HARNESS_RESULT_FILE'], result)
    print(json.dumps(result, ensure_ascii=True))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
