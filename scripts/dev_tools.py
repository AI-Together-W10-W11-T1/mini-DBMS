"""Project-local tool commands and observable usage; Python 3.9 standard library."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.cache/dev-tools'
RTK = ROOT / '.cache/tooling/rtk/1.2.0/rtk.exe'
GRAPHIFY = ROOT / '.cache/tooling/graphify/Scripts/graphify.exe'
GRAPH = CACHE / 'graph/graphify-out/graph.json'
WIKI = ROOT / 'docs/wiki'
SKIP = {'.git', '.cache', '.agents', '.codex', 'node_modules', 'dist', 'coverage',
        '__pycache__', 'playwright-report', 'test-results', '.private', 'codex-setup-github'}
CODE = {'.ts', '.tsx', '.js', '.jsx', '.mjs', '.cjs', '.py', '.ps1', '.html', '.css',
        '.json', '.toml', '.yaml', '.yml', '.c', '.h', '.cpp', '.hpp', '.rs', '.go', '.java', '.sql'}


def read_json(path, default):
    return json.loads(path.read_text(encoding='utf-8')) if path.exists() else default


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def stamp():
    return datetime.now(timezone.utc).isoformat()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def source_hashes():
    result = {}
    for directory, folders, files in os.walk(ROOT):
        folders[:] = sorted(name for name in folders if name not in SKIP)
        for name in sorted(files):
            path = Path(directory) / name
            if path.suffix.lower() in CODE:
                result[path.relative_to(ROOT).as_posix()] = digest(path)
    return result


def changed(previous, current):
    return sorted(key for key in previous.keys() | current.keys()
                  if previous.get(key) != current.get(key))


def wiki_status():
    manifest = read_json(WIKI / 'sources.json', {})
    for path in WIKI.rglob('*.md'):
        manifest.setdefault(path.relative_to(ROOT).as_posix(),
                            {'sources': {}, 'page_sha256': None, 'reviewed_at': None})
    result = {}
    for page, entry in manifest.items():
        missing = not (ROOT / page).is_file()
        stale = [source for source, sha in entry['sources'].items()
                 if digest(ROOT / source) != sha]
        result[page] = {'missing_page': missing, 'changed_sources': stale,
                        'page_changed': digest(ROOT / page) != entry.get('page_sha256'),
                        'reviewed_at': entry['reviewed_at']}
    return result


def record(event):
    CACHE.mkdir(parents=True, exist_ok=True)
    with (CACHE / 'events.jsonl').open('a', encoding='utf-8') as stream:
        stream.write(json.dumps(dict(at=stamp(), **event), ensure_ascii=False) + '\n')


def execute(tool, command):
    CACHE.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PYTHONIOENCODING='utf-8', PYTHONUTF8='1', NO_COLOR='1',
               RTK_DB_PATH=str(CACHE / 'rtk-history.db'),
               RTK_RECALL_DB=str(CACHE / 'rtk-recall.db'),
               CLAUDE_CONFIG_DIR=str(CACHE / 'rtk-config'))
    # Child-process-only Git ownership exception, including Graphify's Git calls.
    index = int(env.get('GIT_CONFIG_COUNT', '0'))
    env.update({'GIT_CONFIG_COUNT': str(index + 1),
                'GIT_CONFIG_KEY_' + str(index): 'safe.directory',
                'GIT_CONFIG_VALUE_' + str(index): ROOT.as_posix()})
    start = time.perf_counter()
    try:
        proc = subprocess.run([str(a) for a in command], cwd=ROOT, env=env,
                              capture_output=True)
    except FileNotFoundError as error:
        record({'tool': tool, 'exit_code': 127, 'error': str(error)})
        print('Tool not installed or child executable missing: ' + str(error), file=sys.stderr)
        return 127
    record({'tool': tool, 'argv': [str(a) for a in command], 'exit_code': proc.returncode,
            'seconds': round(time.perf_counter() - start, 3),
            'output_bytes': len(proc.stdout) + len(proc.stderr)})
    sys.stdout.buffer.write(proc.stdout)
    sys.stderr.buffer.write(proc.stderr)
    sys.stdout.buffer.flush()
    return proc.returncode


def status():
    events = []
    event_file = CACHE / 'events.jsonl'
    if event_file.exists():
        events = [json.loads(line) for line in event_file.read_text(encoding='utf-8').splitlines()]
    manifest = read_json(CACHE / 'graph-sources.json', None)
    current = source_hashes()
    observations = {}
    for event in events:
        if 'exit_code' not in event:
            continue
        totals = observations.setdefault(event['tool'], {'calls': 0, 'seconds': 0,
                                                         'output_bytes': 0})
        totals['calls'] += 1
        totals['seconds'] = round(totals['seconds'] + event.get('seconds', 0), 3)
        totals['output_bytes'] += event.get('output_bytes', 0)
    return {
        'settings': read_json(ROOT / 'dev-tools.json', {}),
        'installed': {'rtk': RTK.is_file(), 'graphify': GRAPHIFY.is_file(), 'wiki': WIKI.is_dir()},
        'graph': {'exists': GRAPH.is_file(),
                  'built_at': manifest['at'] if manifest else None,
                  'changed_files': changed(manifest['sources'], current) if manifest else list(current),
                  'scope': 'project code only; excludes caches, private records and setup bundle'},
        'wiki': wiki_status(),
        'usage': dict(Counter(e['tool'] for e in events if 'exit_code' in e)),
        'observations': observations,
        'feedback': dict(Counter(e['tool'] + ':' + e['outcome'] for e in events if 'outcome' in e)),
        'failed_calls': sum(e.get('exit_code', 0) != 0 for e in events),
        'measurement': 'Counts/time/output bytes are observed; usefulness is manual feedback. '
                       'No whole-session token or money savings inferred.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    commands.add_parser('status')
    commands.add_parser('gain')
    commands.add_parser('graph-build')
    commands.add_parser('wiki-check')
    commands.add_parser('finish')
    configure = commands.add_parser('configure')
    configure.add_argument('--rtk', choices=['selective', 'manual'])
    configure.add_argument('--graph-refresh', choices=['task-end', 'manual'])
    configure.add_argument('--wiki-write', choices=['task-end', 'manual'])
    review = commands.add_parser('wiki-review')
    review.add_argument('page', type=Path)
    review.add_argument('--sources', nargs='+', required=True)
    review.add_argument('--reviewed', action='store_true', required=True,
                        help='attest that page content was checked against these sources')
    for name in ('rtk', 'raw', 'graph'):
        commands.add_parser(name).add_argument('args', nargs=argparse.REMAINDER)
    feedback = commands.add_parser('feedback')
    feedback.add_argument('tool', choices=['rtk', 'graphify', 'wiki'])
    feedback.add_argument('outcome', choices=['useful', 'miss', 'wrong'])
    feedback.add_argument('--note', default='')
    args = parser.parse_args()
    settings = read_json(ROOT / 'dev-tools.json', {})
    if args.action == 'configure':
        for key in ('rtk', 'graph_refresh', 'wiki_write'):
            value = getattr(args, key)
            if value is not None:
                settings[key] = value
        write_json(ROOT / 'dev-tools.json', settings)
        print(json.dumps(settings, indent=2))
        return 0
    if args.action == 'finish':
        snapshot = read_json(CACHE / 'graph-sources.json', {})
        stale = not GRAPH.is_file() or changed(snapshot.get('sources', {}), source_hashes())
        if settings['graph_refresh'] == 'task-end' and stale:
            code = subprocess.call([sys.executable, str(Path(__file__).resolve()), 'graph-build'])
            if code:
                return code
        return subprocess.call([sys.executable, str(Path(__file__).resolve()), 'wiki-check'])
    if args.action == 'wiki-review':
        page = (ROOT / args.page).resolve(strict=True)
        if WIKI.resolve() not in page.parents or page.suffix != '.md':
            parser.error('page must be Markdown inside docs/wiki')
        sources = {}
        for name in args.sources:
            source = (ROOT / name).resolve(strict=True)
            if ROOT not in source.parents or '.cache' in source.relative_to(ROOT).parts:
                parser.error('sources must be project files outside .cache')
            sources[source.relative_to(ROOT).as_posix()] = digest(source)
        manifest = read_json(WIKI / 'sources.json', {})
        manifest[page.relative_to(ROOT).as_posix()] = {
            'reviewed_at': stamp(), 'page_sha256': digest(page), 'sources': sources}
        write_json(WIKI / 'sources.json', manifest)
        record({'tool': 'wiki', 'event': 'source-review', 'page': page.relative_to(ROOT).as_posix()})
        return 0
    if args.action == 'status':
        print(json.dumps(status(), ensure_ascii=True, indent=2))
        return 0
    if args.action == 'wiki-check':
        result = wiki_status()
        print(json.dumps(result, ensure_ascii=True, indent=2))
        return int(not result or any(r['missing_page'] or r['changed_sources'] or r['page_changed']
                                    for r in result.values()))
    if args.action == 'feedback':
        record({'tool': args.tool, 'outcome': args.outcome, 'note': args.note})
        return 0
    if args.action == 'gain':
        return execute('rtk-gain', [RTK, 'gain', '--all', '--format', 'json'])
    if args.action == 'graph-build':
        before = source_hashes()
        command = [GRAPHIFY, 'extract', ROOT, '--code-only', '--no-cluster', '--force',
                   '--max-workers', str(settings['graph_workers']), '--out', CACHE / 'graph']
        for folder in sorted(SKIP):
            command += ['--exclude', folder + '/**']
        code = execute('graph-build', command)
        if code == 0 and GRAPH.is_file():
            write_json(CACHE / 'graph-sources.json', {'at': stamp(), 'sources': before})
        elif code == 0:
            record({'tool': 'graph-validation', 'exit_code': 1, 'error': 'graph.json missing'})
        return code if code else int(not GRAPH.is_file())
    tail = args.args[1:] if args.args[:1] == ['--'] else args.args
    if not tail:
        parser.error('supply a command after --')
    if args.action == 'graph':
        if tail[0] not in {'query', 'explain', 'path', 'affected'}:
            parser.error('graph supports query/explain/path/affected; use graph-build to refresh')
        if not GRAPH.is_file():
            parser.error('no graph: run graph-build first')
        snapshot = read_json(CACHE / 'graph-sources.json', {})
        if changed(snapshot.get('sources', {}), source_hashes()):
            print('WARNING: graph is stale; refresh before trusting results.', file=sys.stderr)
        if tail[0] == 'query' and '--budget' not in tail:
            tail += ['--budget', str(settings['graph_query_budget'])]
        return execute('graph-query', [GRAPHIFY] + tail + ['--graph', GRAPH])
    return execute(args.action, ([RTK] if args.action == 'rtk' else []) + tail)


if __name__ == '__main__':
    raise SystemExit(main())
