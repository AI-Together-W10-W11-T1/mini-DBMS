"""Build actual C23 targets and translate a fresh CTest JUnit report for the harness."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
CASES = ["M1-unit-event-lifetime", "M1-roundtrip", "M1-literals", "M1-stop-on-error",
         "M1-invalid-id", "M1-strict-schema", "M1-corruption", "M1-pipe",
         "M1-lease", "M1-pending", "M1-append-rollback", "M1-rollback-failure",
         "M1-cleanup-committed", "M1-output-failure", "M1-read-ahead", "M1-argv-wire"]


def tool(variable, relative):
    return str(Path(os.environ.get(variable, ROOT / relative)).resolve())


def main():
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
            report["suites"] = [{"title": "M1 actual C executable acceptance", "specs": list(found.values())}]
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


if __name__ == "__main__":
    raise SystemExit(main())
