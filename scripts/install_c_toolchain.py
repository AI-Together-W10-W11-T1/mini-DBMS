"""Pinned project-local Windows C tools. No registry or global PATH writes."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'tools/c-toolchain-lock.json'
CACHE = ROOT / '.cache/bootstrap'


def sha256(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def unpack(archive, destination):
    # 배포 압축 파일도 신뢰 경계 안에서만 풀고 경로 탈출/링크는 거부한다.
    destination.mkdir(parents=True, exist_ok=True)
    base = destination.resolve()
    if archive.suffix == '.zip':
        with zipfile.ZipFile(archive) as bundle:
            for member in bundle.infolist():
                if base not in (base / member.filename).resolve().parents:
                    raise ValueError('archive path escapes destination')
            bundle.extractall(destination)
    else:
        with tarfile.open(archive) as bundle:
            for member in bundle.getmembers():
                target = (base / member.name).resolve()
                if (target != base and base not in target.parents) or member.issym() or member.islnk():
                    raise ValueError('unsafe archive member')
            bundle.extractall(destination)


def run(command, name, env=None, expected=0):
    result = subprocess.run([str(x) for x in command], cwd=ROOT, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            encoding='utf-8', errors='replace', timeout=180)
    (CACHE / (name + '.log')).write_text(result.stdout, encoding='utf-8')
    if result.returncode != expected:
        raise RuntimeError(f'{name}: exit {result.returncode}; see .cache/bootstrap/{name}.log')
    return {'command': [str(x) for x in command], 'exit_code': result.returncode,
            'log': f'.cache/bootstrap/{name}.log', 'output': result.stdout.strip()}


def probe(paths):
    env = os.environ.copy()
    env['PATH'] = str(paths['clang'].parent) + os.pathsep + env.get('PATH', '')
    receipts = {}
    for key, version in [('clang', '23.1.5'), ('lldb', '23.1.5'), ('cmake', '4.5.0'),
                         ('ctest', '4.5.0'), ('ninja', '1.13.2')]:
        receipts[key] = run([paths[key], '--version'], key + '-version', env)
        if version not in receipts[key]['output']:
            raise RuntimeError(f'{key}: expected version {version}')
    source = CACHE / 'c23-probe.c'
    source.write_text('''#include <stdint.h>
#include <stdio.h>
static_assert(__STDC_VERSION__ >= 202311L, "C23 required");
static_assert(sizeof(int64_t) == 8, "int64_t width");
static_assert(sizeof(void *) == 8, "x64 required");
int inspect(int input) {
    volatile int local = input + 1;
    return local;
}
int main(void) {
    printf("C23=%ld int64=%zu pointer=%zu\\n", __STDC_VERSION__, sizeof(int64_t), sizeof(void *));
    return inspect(41) == 42 ? 0 : 1;
}
''', encoding='utf-8')
    executable = CACHE / 'c23-probe.exe'
    receipts['compile'] = run([paths['clang'], '-std=c23', '-g', '-O0', '-Wall', '-Wextra',
                               '-Werror', source, '-o', executable], 'c23-compile', env)
    receipts['run'] = run([executable], 'c23-run', env)
    receipts['debug'] = run([paths['lldb'], '--batch', '-o', 'breakpoint set --name inspect',
                             '-o', 'run', '-o', 'next', '-o', 'frame variable local',
                             '-o', 'bt', '-o', 'continue', executable], 'lldb-smoke', env)
    output = receipts['debug']['output']
    if 'local = 42' not in output or 'main' not in output or 'stop reason = breakpoint' not in output:
        raise RuntimeError('LLDB smoke evidence incomplete; see lldb-smoke.log')
    # 성공/의도적 실패를 별개 CTest 실행으로 확인한다. SQL 기능 Red가 아니다.
    project = CACHE / 'cmocka-probe'
    project.mkdir(exist_ok=True)
    (project / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 4.5)
project(cmocka_probe LANGUAGES C)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(UNIT_TESTING OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMOCKA_SOURCE}" cmocka)
add_executable(probe probe.c)
target_link_libraries(probe PRIVATE cmocka::cmocka)
enable_testing()
add_test(NAME bootstrap_pass COMMAND probe)
add_test(NAME bootstrap_expected_failure COMMAND probe fail)
''', encoding='utf-8')
    (project / 'probe.c').write_text('''#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>
static int should_fail;
static void sample(void **state) {
    (void)state;
    assert_int_equal(should_fail, 0);
}
int main(int argc, char **argv) {
    (void)argv;
    should_fail = argc > 1;
    const struct CMUnitTest tests[] = {cmocka_unit_test(sample)};
    return cmocka_run_group_tests(tests, NULL, NULL);
}
''', encoding='utf-8')
    build = project / 'build'
    receipts['cmocka_configure'] = run([paths['cmake'], '-S', project, '-B', build,
        '-G', 'Ninja', '-DCMAKE_C_COMPILER=' + paths['clang'].as_posix(),
        '-DCMAKE_MAKE_PROGRAM=' + paths['ninja'].as_posix(),
        '-DCMOCKA_SOURCE=' + paths['cmocka_source'].as_posix()], 'cmocka-configure', env)
    receipts['cmocka_build'] = run([paths['cmake'], '--build', build], 'cmocka-build', env)
    receipts['ctest_pass'] = run([paths['ctest'], '--test-dir', build, '--output-on-failure',
                                 '-R', '^bootstrap_pass$'], 'ctest-pass', env)
    receipts['ctest_failure'] = run([paths['ctest'], '--test-dir', build, '--output-on-failure',
        '-R', '^bootstrap_expected_failure$'], 'ctest-expected-failure', env, expected=8)
    return receipts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true', help='require already cached archives')
    parser.add_argument('--probe', action='store_true', help='compile/run/debug C23 smoke')
    args = parser.parse_args()
    if sys.platform != 'win32':
        raise RuntimeError('Windows native installation required')
    manifest = json.loads(LOCK.read_text(encoding='utf-8'))
    CACHE.mkdir(parents=True, exist_ok=True)
    downloads = CACHE / 'downloads'
    downloads.mkdir(exist_ok=True)
    receipts = {'timestamp_utc': datetime.now(timezone.utc).isoformat(), 'artifacts': []}
    try:
        for artifact in manifest['artifacts']:
            archive = downloads / artifact['archive']
            if not archive.exists():
                if args.offline:
                    raise RuntimeError(f'missing cached archive: {archive.name}')
                # Windows 시스템 인증서 저장소를 쓰며 TLS 검증을 우회하지 않는다.
                run(['curl.exe', '--fail', '--location', '--silent', '--show-error',
                     '--retry', '2', '--output', archive, artifact['url']],
                    artifact['id'] + '-download')
            actual = sha256(archive)
            if actual != artifact['sha256']:
                raise RuntimeError(f'SHA256 mismatch: {archive.name}')
            destination = ROOT / artifact['extract_to']
            marker = destination / '.verified-archive-sha256'
            if not marker.exists() or marker.read_text(encoding='ascii').strip() != actual:
                unpack(archive, destination)
                marker.write_text(actual + '\n', encoding='ascii')
            receipts['artifacts'].append({'id': artifact['id'], 'sha256': actual,
                                          'destination': artifact['extract_to']})
        yyjson = ROOT / manifest['paths']['yyjson_source']
        vendor = ROOT / 'vendor/yyjson'
        vendor.mkdir(parents=True, exist_ok=True)
        for relative in ['src/yyjson.c', 'src/yyjson.h', 'LICENSE']:
            target = vendor / Path(relative).name
            if target.exists() and sha256(target) != sha256(yyjson / relative):
                raise RuntimeError(f'local vendor modifications: {target.name}')
            shutil.copyfile(yyjson / relative, target)
        paths = {key: ROOT / value for key, value in manifest['paths'].items()}
        if args.probe:
            receipts['probes'] = probe(paths)
        receipts['status'] = 'PASS'
        print(json.dumps({key: str(value) for key, value in paths.items()}, indent=2))
    except Exception as error:
        receipts['status'] = 'ENV_ERROR'
        receipts['error'] = str(error)
        raise
    finally:
        (CACHE / 'install-receipt.json').write_text(
            json.dumps(receipts, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
