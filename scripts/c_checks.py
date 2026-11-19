"""Build actual C23 targets and translate a fresh CTest JUnit report for the harness."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
CASES = ["M1-unit-event-lifetime", "M1-roundtrip", "M1-literals", "M1-stop-on-error",
         "M1-invalid-id", "M1-strict-schema", "M1-corruption", "M1-pipe",
         "M1-lease", "M1-pending", "M1-append-rollback", "M1-rollback-failure",
         "M1-cleanup-committed", "M1-output-failure", "M1-read-ahead", "M1-argv-wire",
         "M2-INSERT-01", "M2-INSERT-02", "M2-INSERT-03", "M2-INSERT-04", "M2-INSERT-05",
         "M2-WHERE-01", "M2-WHERE-02", "M2-WHERE-03", "M2-NAME-01", "M2-NAME-02",
         "M2-VALUES-01", "M2-VALUES-02", "M2-VALUES-03", "M2-REGRESSION-01",
         *[f"M3-UPDATE-{i:02}" for i in range(1, 9)],
         *[f"M3-DELETE-{i:02}" for i in range(1, 4)],
         *[f"M3-STORE-{i:02}" for i in range(1, 8)], "M3-CLI-01", "M3-REGRESSION-01",
         *[f"M4-ORDER-{i:02}" for i in range(1, 5)], *[f"M4-PAGE-{i:02}" for i in range(1, 4)],
         "M4-ERROR-01", "M4-ERROR-02", "M4-VALUES-01", "M4-VALUES-02", "M4-CLI-01", "M4-REGRESSION-01",
         *["M5-"+name for name in ["JOIN-01","JOIN-02","JOIN-03","JOIN-04","NAME-01","NAME-02",
              "TAIL-01","EMPTY-01","ERROR-01","RESOURCE-01","OWNERSHIP-01","CLI-01","REGRESSION-01"]],
         *["M6-"+name for name in ["COUNT-01","GROUP-01","GROUP-02","HAVING-01","HAVING-02",
              "JOIN-01","TAIL-01","EMPTY-01","ERROR-01","OVERFLOW-01","RESOURCE-01","OWNERSHIP-01","CLI-01","REGRESSION-01"]],
         *["M7-"+name for name in ["UNION-01","ALL-01","MIXED-01","TYPE-01","EMPTY-01","SELECT-01",
              "TAIL-01","ERROR-01","OVERFLOW-01","RESOURCE-01","OWNERSHIP-01","CLI-01","REGRESSION-01"]],
         *["M8-"+name for name in ["PLAN-01","PLAN-02","PLAN-03","SOURCE-01","MAP-01","MAP-02","MAP-03","COPY-01","TYPE-01","TYPE-02","TYPE-03","PATH-01","LEASE-01","FAILURE-01","FAILURE-02","RESOURCE-01","CLI-01","OWNERSHIP-01","INIT-01","REGRESSION-01"]],
         *["M9-"+name for name in ["EMPTY-01", "INSERT-01", "ORDER-01", "DUPLICATE-01",
             "SPLIT-01", "SPLIT-02", "RANGE-01", "BOUNDARY-01", "FAILURE-01", "FAILURE-02",
             "OWNERSHIP-01", "INVARIANT-01"]],
         *["M10-"+name for name in ["STARTUP-01", "AUTO-01", "AUTO-02", "AUTO-03", "INSERT-01",
             "RESERVE-01", "LOOKUP-01", "LOOKUP-02", "LOOKUP-03", "UPDATE-01", "DELETE-01",
             "DELETE-02", "STORAGE-01", "RESTART-01", "MIGRATE-01"]],
         *["M11-"+name for name in ["GENERATE-01", "DECODE-01", "ORACLE-01", "BENCH-01",
             "STATISTICS-01", "ARGUMENT-01", "OUTPUT-01", "REGRESSION-01"]],
         *["M12-"+name for name in ["WIRE-01","WIRE-02","CHUNK-01","CHUNK-02","SQL-01","SDK-01","POOL-01","QUEUE-01","HEALTH-01","OWN-01","DISCONNECT-01","STATE-01","LIMIT-01","CONFIG-01","TIME-01","SHUTDOWN-01","CLI-01","REGRESSION-01"]],
         *["M13-"+name for name in ["READ-01","RW-01","WW-01","ADMIT-01","WAIT-01","CLASSIFY-01","HEALTH-01","TIME-01","LOOP-01","PREP-01","COMMIT-01","LIFE-01","WIRE-01","REGRESSION-01"]],
         *["M14-"+name for name in ["SDK-01","QUEUE-01","STATISTICS-01","MIXED-01","FAILURE-01","MEMORY-01","OUTPUT-01","REGRESSION-01"]]]


def tool(variable, relative):
    return str(Path(os.environ.get(variable, ROOT / relative)).resolve())


def legacy_main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=["build", "red", "green", "regression"])
    options = parser.parse_args()
    work = Path(os.environ.get("AGENT_HARNESS_WORK_DIR", ROOT / ".cache/bootstrap/scaffold/manual")).resolve()
    node = os.environ.get("AGENT_HARNESS_NODE_ID", "manual")
    if not node or any(character not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for character in node):
        parser.error("AGENT_HARNESS_NODE_ID must be a simple actor identifier")
    artifact = work / node / options.phase
    artifact.mkdir(parents=True, exist_ok=True)
    build = artifact / "build"
    report = {"config": {"metadata": {"harnessRunId": os.environ.get("AGENT_HARNESS_RUN_ID", "manual")}},
              "suites": [], "errors": []}
    result_file = Path(os.environ.get("AGENT_HARNESS_RESULT_FILE", artifact / "report.json"))
    cmake = tool("MINIDB_CMAKE", ".cache/tooling/cmake/4.5.0/cmake-4.5.0-windows-x86_64/bin/cmake.exe")
    ctest = str(Path(cmake).with_name("ctest.exe"))
    ctest = os.environ.get("MINIDB_CTEST", ctest)
    ninja = tool("MINIDB_NINJA", ".cache/tooling/ninja/1.13.2/ninja.exe")
    compiler = tool("MINIDB_CC", ".cache/tooling/llvm-mingw/20261104/llvm-mingw-20261104-ucrt-x86_64/bin/clang.exe")
    cmocka = tool("CMOCKA_SOURCE_DIR", ".cache/tooling/cmocka/2.0.1/cmocka-2.0.1")
    environment = os.environ.copy()
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
    receipts = []

    def run(name, command):
        log = artifact / (name + ".log")
        with log.open("wb") as stream:
            process = subprocess.run(command, cwd=ROOT, env=environment, stdout=stream, stderr=subprocess.STDOUT)
        receipts.append({"name": name, "command": command, "exit_code": process.returncode, "log": str(log)})
        print(f"{options.phase}: {name} exit={process.returncode} log={log}", flush=True)
        return process.returncode

    exit_code = 1
    try:
        exit_code = run("configure", [cmake, "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug", "-DBUILD_TESTING=ON", "-DCMAKE_C_COMPILER=" + compiler,
            "-DCMAKE_MAKE_PROGRAM=" + ninja, "-DCMOCKA_SOURCE_DIR=" + cmocka,
            "-DPython3_EXECUTABLE=" + sys.executable])
        if exit_code:
            raise RuntimeError("CMake configuration failed; environment/build error is not functional Red")
        exit_code = run("compile", [cmake, "--build", str(build), "--parallel", "2"])
        if exit_code:
            raise RuntimeError("C compilation failed; build error is not functional Red")
        if options.phase != "build":
            junit = artifact / "ctest.xml"
            if junit.exists():
                junit.unlink()
            exit_code = run("ctest", [ctest, "--test-dir", str(build), "--output-on-failure", "--output-junit", str(junit)])
            if not junit.exists():
                raise RuntimeError("CTest did not produce a fresh JUnit report")
            cases = ET.parse(junit).getroot().findall(".//testcase")
            found = {}
            for case in cases:
                name = case.attrib["name"]
                if name not in CASES:
                    continue
                failed = case.find("failure") is not None or case.find("error") is not None
                skipped = case.find("skipped") is not None or case.attrib.get("status") == "notrun"
                status = "failed" if failed else "skipped" if skipped else "passed"
                log = case.findtext("system-out", "")
                found[name] = {"title": name, "tests": [{"expectedStatus": "passed", "results": [{"status": status, "stdout": [log]}]}]}
            missing = sorted(set(CASES) - set(found))
            if missing:
                raise RuntimeError("Missing CTest cases: " + ", ".join(missing))
            report["suites"] = [{"title": "M1 through M4 actual C executable acceptance", "specs": list(found.values())}]
            passed = sum(s["tests"][0]["results"][0]["status"] == "passed" for s in found.values())
            print(f"{options.phase}: {passed}/{len(CASES)} passed; JUnit={junit}")
    except (OSError, RuntimeError, ET.ParseError) as error:
        report["errors"].append({"message": str(error)})
        exit_code = exit_code or 1
        print(str(error), file=sys.stderr)
    finally:
        result_file.parent.mkdir(parents=True, exist_ok=True)
        result_file.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        (artifact / "receipts.json").write_text(json.dumps(receipts, indent=2), encoding="utf-8")
        print(f"report={result_file}")
    return exit_code


def slim_main():
    parser = argparse.ArgumentParser()
    parser.add_argument('phase', choices=['build', 'red', 'green', 'targeted', 'regression'])
    options = parser.parse_args()
    work = Path(os.environ.get('AGENT_HARNESS_WORK_DIR', ROOT / '.cache/bootstrap/scaffold/manual')).resolve()
    node = os.environ.get('AGENT_HARNESS_NODE_ID', '')
    if node not in {'IMPLEMENT', 'VERIFY'}:
        parser.error('slim-v1 requires IMPLEMENT or VERIFY node')
    artifact = work / node / options.phase / uuid.uuid4().hex
    artifact.mkdir(parents=True)
    result_file = Path(os.environ.get('AGENT_HARNESS_RESULT_FILE', artifact / 'report.json'))
    metadata = {'harnessRunId': os.environ.get('AGENT_HARNESS_RUN_ID', 'manual'),
                'workflowPolicy': 'slim-v1', 'nodeId': node,
                'actorId': os.environ.get('AGENT_HARNESS_ACTOR_ID', ''),
                'sourceDigest': os.environ.get('AGENT_HARNESS_SOURCE_DIGEST', ''),
                'testScope': 'full', 'requestedTests': list(CASES),
                'build': {'passed': False, 'configure_exit_code': None,
                          'compile_exit_code': None, 'directory': None, 'cache_reused': False},
                'durations': {}, 'targetCount': len(CASES), 'artifactDirectory': str(artifact)}
    report = {'config': {'metadata': metadata}, 'suites': [], 'errors': []}
    receipts = []
    started = time.perf_counter()
    cmake = tool('MINIDB_CMAKE', '.cache/tooling/cmake/4.5.0/cmake-4.5.0-windows-x86_64/bin/cmake.exe')
    ctest = tool('MINIDB_CTEST', str(Path(cmake).with_name('ctest.exe')))
    ninja = tool('MINIDB_NINJA', '.cache/tooling/ninja/1.13.2/ninja.exe')
    compiler = tool('MINIDB_CC', '.cache/tooling/llvm-mingw/20261104/llvm-mingw-20261104-ucrt-x86_64/bin/clang.exe')
    cmocka = tool('CMOCKA_SOURCE_DIR', '.cache/tooling/cmocka/2.0.1/cmocka-2.0.1')
    environment = dict(os.environ)
    environment['PATH'] = str(Path(compiler).parent) + os.pathsep + environment.get('PATH', '')

    def run(name, command):
        before = time.perf_counter()
        log = artifact / (name + '.log')
        with log.open('wb') as stream:
            process = subprocess.run(command, cwd=ROOT, env=environment,
                                     stdout=stream, stderr=subprocess.STDOUT)
        elapsed = time.perf_counter() - before
        metadata['durations'][name] = elapsed
        receipts.append({'name': name, 'command': command, 'exit_code': process.returncode,
                         'log': str(log), 'seconds': elapsed})
        print('{}: {} exit={} log={}'.format(options.phase, name, process.returncode, log), flush=True)
        return process.returncode

    exit_code = 1
    try:
        requested = os.environ.get('AGENT_HARNESS_TEST_TARGETS')
        if requested is not None:
            if options.phase not in {'red', 'targeted'}:
                raise RuntimeError('Test selection is allowed only for slim red/targeted')
            selected = json.loads(requested)
            if (not isinstance(selected, list) or not selected or
                    any(not isinstance(name, str) or name not in CASES for name in selected) or
                    len(set(selected)) != len(selected)):
                raise RuntimeError('Targets must be a nonempty list of unique exact CASES names')
            metadata.update(testScope='selected', requestedTests=selected, targetCount=len(selected))
        elif options.phase == 'targeted':
            raise RuntimeError('targeted requires explicit test targets')
        selected = metadata['requestedTests']
        configure_options = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug', '-DBUILD_TESTING=ON',
                             '-DCMAKE_C_COMPILER=' + compiler, '-DCMAKE_MAKE_PROGRAM=' + ninja,
                             '-DCMOCKA_SOURCE_DIR=' + cmocka, '-DPython3_EXECUTABLE=' + sys.executable]
        # 실행 결과는 재사용하지 않는다. 도구/옵션이 같은 역할의 빌드 트리만 재사용한다.
        identity = []
        for path in [cmake, ctest, ninja, compiler, sys.executable,
                     str(ROOT / 'tools/c-toolchain-lock.json')]:
            file = Path(path)
            stat = file.stat() if file.is_file() else None
            identity.append([str(file), stat.st_size if stat else None,
                             stat.st_mtime_ns if stat else None])
        # Windows의 중첩 fixture 경로 여유를 위해 짧게 만들고 전체 서명으로 충돌을 거절한다.
        signature = hashlib.sha256(json.dumps([identity, configure_options], sort_keys=True).encode()).hexdigest()
        build = work / node / 'b' / signature[:10]
        signature_file = build.with_suffix('.signature')
        if signature_file.is_file() and signature_file.read_text(encoding='ascii') != signature:
            raise RuntimeError('Build cache signature collision; refusing reuse')
        signature_file.parent.mkdir(parents=True, exist_ok=True)
        signature_file.write_text(signature, encoding='ascii')
        metadata['build'].update(directory=str(build), cache_reused=(build / 'CMakeCache.txt').is_file())
        exit_code = run('configure', [cmake, '-S', str(ROOT), '-B', str(build)] + configure_options)
        metadata['build']['configure_exit_code'] = exit_code
        if exit_code:
            raise RuntimeError('CMake configuration failed; environment/build error is not functional Red')
        exit_code = run('compile', [cmake, '--build', str(build), '--parallel', '2'])
        metadata['build']['compile_exit_code'] = exit_code
        if exit_code:
            raise RuntimeError('C compilation failed; build error is not functional Red')
        metadata['build']['passed'] = True
        if options.phase != 'build':
            junit = artifact / 'ctest.xml'
            command = [ctest, '--test-dir', str(build), '--output-on-failure', '--output-junit', str(junit)]
            if metadata['testScope'] == 'selected':
                targets = artifact / 'tests.txt'
                targets.write_text('\n'.join(selected) + '\n', encoding='utf-8')
                command += ['--tests-from-file', str(targets)]
            exit_code = run('ctest', command)
            if not junit.is_file():
                raise RuntimeError('CTest did not produce a fresh JUnit report')
            found = {}
            for case in ET.parse(junit).getroot().findall('.//testcase'):
                name = case.attrib.get('name')
                if name not in selected or name in found:
                    raise RuntimeError('Unexpected or duplicate CTest case: ' + str(name))
                failed = case.find('failure') is not None or case.find('error') is not None
                skipped = case.find('skipped') is not None or case.attrib.get('status') in {'notrun', 'disabled'}
                status = 'skipped' if skipped else 'failed' if failed else 'passed'
                found[name] = {'title': name, 'tests': [{'expectedStatus': 'passed',
                    'results': [{'status': status, 'stdout': [case.findtext('system-out', '')]}]}]}
            report['suites'] = [{'title': 'Actual C executable acceptance', 'specs': list(found.values())}]
            missing = sorted(set(selected) - set(found))
            if missing:
                raise RuntimeError('Missing CTest cases: ' + ', '.join(missing))
            statuses = [spec['tests'][0]['results'][0]['status'] for spec in found.values()]
            if 'skipped' in statuses:
                raise RuntimeError('CTest cases skipped')
            # assertion 실패는 기능 Red다. 실행/증거 오류와 구분해 global errors는 비워 둔다.
            if 'failed' in statuses:
                exit_code = exit_code or 1
            elif exit_code:
                raise RuntimeError('CTest returned nonzero exit status')
    except (OSError, RuntimeError, ValueError, ET.ParseError) as error:
        report['errors'].append({'message': str(error)})
        exit_code = exit_code or 1
        print(str(error), file=sys.stderr)
    finally:
        metadata['durations']['total'] = time.perf_counter() - started
        payload = json.dumps(report, ensure_ascii=False, indent=2)
        (artifact / 'report.json').write_text(payload, encoding='utf-8')
        (artifact / 'receipts.json').write_text(json.dumps(receipts, indent=2), encoding='utf-8')
        result_file.parent.mkdir(parents=True, exist_ok=True)
        result_file.write_text(payload, encoding='utf-8')
        print('report=' + str(result_file))
    return exit_code


def main():
    policy = os.environ.get('AGENT_HARNESS_WORKFLOW_POLICY')
    if policy is None:
        if 'AGENT_HARNESS_TEST_TARGETS' in os.environ:
            raise SystemExit('Test selection requires slim-v1')
        return legacy_main()
    if policy != 'slim-v1':
        raise SystemExit('Unknown workflow policy: ' + policy)
    return slim_main()


if __name__ == '__main__':
    raise SystemExit(main())
