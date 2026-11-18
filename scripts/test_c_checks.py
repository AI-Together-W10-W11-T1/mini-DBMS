"""Adapter operating tests: isolated subprocess fixtures, not real C execution."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

import c_checks as checks


class AdapterChecks(unittest.TestCase):
    def invoke(self, root, phase='regression', targets=None, extra=None, case_status=None,
               case_names=None, return_codes=None, emit_junit=True):
        commands = []
        env = {'AGENT_HARNESS_WORKFLOW_POLICY': 'slim-v1',
               'AGENT_HARNESS_WORK_DIR': str(root), 'AGENT_HARNESS_RUN_ID': 'run-a',
               'AGENT_HARNESS_NODE_ID': 'IMPLEMENT', 'AGENT_HARNESS_ACTOR_ID': 'actor-a',
               'AGENT_HARNESS_SOURCE_DIGEST': 'source-a',
               'AGENT_HARNESS_RESULT_FILE': str(root / 'report.json')}
        if targets is not None:
            env['AGENT_HARNESS_TEST_TARGETS'] = json.dumps(targets)
        env.update(extra or {})
        env = {key: value for key, value in env.items() if value is not None}

        def execute(command, **kwargs):
            commands.append(command)
            stage = 'ctest' if '--output-junit' in command else 'compile' if '--build' in command else 'configure'
            if stage == 'configure':
                build = Path(command[command.index('-B') + 1])
                build.mkdir(parents=True, exist_ok=True)
                (build / 'CMakeCache.txt').write_text('fixture cache')
            if stage == 'ctest' and emit_junit:
                names = list(checks.CASES)
                if '--tests-from-file' in command:
                    names = Path(command[command.index('--tests-from-file') + 1]).read_text().splitlines()
                if case_names is not None:
                    names = case_names
                suite = ET.Element('testsuite')
                for name in names:
                    case = ET.SubElement(suite, 'testcase', name=name)
                    status = case_status.get(name) if isinstance(case_status, dict) else case_status
                    if status:
                        ET.SubElement(case, status)
                ET.ElementTree(suite).write(command[command.index('--output-junit') + 1])
            return subprocess.CompletedProcess(command, (return_codes or {}).get(stage, 0))

        with patch.dict(os.environ, env, clear=True), patch.object(checks.sys, 'argv', ['c_checks.py', phase]), \
                patch.object(checks.subprocess, 'run', side_effect=execute), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = checks.main()
        report = json.loads((root / 'report.json').read_text())
        return code, report, commands

    def test_slim_full_report_binds_actor_source_and_build(self):
        with tempfile.TemporaryDirectory() as directory:
            code, report, commands = self.invoke(Path(directory))
            self.assertEqual(code, 0)
            meta = report['config']['metadata']
            self.assertEqual(meta.get('workflowPolicy'), 'slim-v1')
            self.assertEqual(meta['actorId'], 'actor-a')
            self.assertEqual(meta['nodeId'], 'IMPLEMENT')
            self.assertEqual(meta['sourceDigest'], 'source-a')
            self.assertEqual(meta['requestedTests'], checks.CASES)
            self.assertEqual(meta['testScope'], 'full')
            self.assertTrue(meta['build']['passed'])

    def test_targeted_and_red_select_exact_names(self):
        for phase in ['targeted', 'red']:
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as directory:
                names = [checks.CASES[2], checks.CASES[0]]
                code, report, commands = self.invoke(Path(directory), phase, names)
                self.assertEqual(code, 0)
                meta = report['config']['metadata']
                self.assertEqual(meta['testScope'], 'selected')
                self.assertEqual(meta['requestedTests'], names)
                self.assertEqual([s['title'] for s in report['suites'][0]['specs']], names)
                self.assertIn('--tests-from-file', commands[-1])

    def test_selection_rejects_bad_names_shapes_duplicates_and_full_phase(self):
        for targets in [[], ['unknown'], [checks.CASES[0]] * 2, 'M1-roundtrip', [7]]:
            with self.subTest(targets=targets), tempfile.TemporaryDirectory() as directory:
                code, report, commands = self.invoke(Path(directory), 'targeted', targets)
                self.assertNotEqual(code, 0)
                self.assertTrue(report['errors'])
                self.assertFalse(commands)
        with tempfile.TemporaryDirectory() as directory:
            self.assertNotEqual(self.invoke(Path(directory), targets=[checks.CASES[0]])[0], 0)

    def test_cache_reused_only_with_same_role_tool_options_and_fresh_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runs = [self.invoke(root) for unused in range(2)]
            first, second = [run[1]['config']['metadata'] for run in runs]
            self.assertFalse(first['build']['cache_reused'])
            self.assertTrue(second['build']['cache_reused'])
            self.assertEqual(first['build']['directory'], second['build']['directory'])
            self.assertNotEqual(first['artifactDirectory'], second['artifactDirectory'])
            for unused, report, commands in runs:
                self.assertEqual(len(commands), 3)
                artifact = Path(report['config']['metadata']['artifactDirectory'])
                for name in ['report.json', 'receipts.json', 'configure.log', 'compile.log', 'ctest.log', 'ctest.xml']:
                    self.assertTrue((artifact / name).is_file(), name)
            verify = self.invoke(root, extra={'AGENT_HARNESS_NODE_ID': 'VERIFY'})[1]['config']['metadata']
            self.assertNotEqual(first['build']['directory'], verify['build']['directory'])
            tool_path = root / 'compiler.exe'
            tool_path.write_bytes(b'v1')
            tool_env = {'MINIDB_CC': str(tool_path)}
            old = self.invoke(root, extra=tool_env)[1]['config']['metadata']
            tool_path.write_bytes(b'new-version')
            new = self.invoke(root, extra=tool_env)[1]['config']['metadata']
            self.assertNotEqual(old['build']['directory'], new['build']['directory'])
            self.assertTrue(Path(old['build']['directory']).exists())
            signature = Path(new['build']['directory']).with_suffix('.signature')
            signature.write_text('different full signature')
            code, report, commands = self.invoke(root, extra=tool_env)
            self.assertNotEqual(code, 0)
            self.assertIn('signature collision', report['errors'][0]['message'])
            self.assertFalse(commands)

    def test_missing_unknown_duplicate_failed_skipped_and_nonzero_rejected(self):
        bad = [{'case_names': []}, {'case_names': checks.CASES + ['unknown']},
               {'case_names': checks.CASES + checks.CASES[:1]},
               {'case_status': 'skipped'}, {'emit_junit': False},
               {'case_status': {checks.CASES[0]: 'failure', checks.CASES[1]: 'skipped'}},
               {'return_codes': {'ctest': 8}}, {'return_codes': {'configure': 2}},
               {'return_codes': {'compile': 2}}]
        for options in bad:
            with self.subTest(options=options), tempfile.TemporaryDirectory() as directory:
                code, report, commands = self.invoke(Path(directory), **options)
                self.assertNotEqual(code, 0)
                self.assertTrue(report['errors'])

    def test_previous_junit_cannot_satisfy_next_invocation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(self.invoke(root)[0], 0)
            self.assertNotEqual(self.invoke(root, emit_junit=False)[0], 0)

    def test_windows_build_layout_leaves_room_for_nested_fixture_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            code, report, commands = self.invoke(root)
            self.assertEqual(code, 0)
            build = Path(report['config']['metadata']['build']['directory'])
            self.assertEqual(build.parent, root / 'IMPLEMENT' / 'b')
            self.assertEqual(len(build.name), 10)
            self.assertEqual(len(build.with_suffix('.signature').read_text(encoding='ascii')), 64)

    def test_assertion_failure_is_functional_red_without_infrastructure_error(self):
        for phase in ['red', 'regression']:
            for tag in ['failure', 'error']:
                with self.subTest(phase=phase, tag=tag), tempfile.TemporaryDirectory() as directory:
                    targets = [checks.CASES[0]] if phase == 'red' else None
                    code, report, commands = self.invoke(Path(directory), phase, targets,
                        case_status={checks.CASES[0]: tag}, return_codes={'ctest': 8})
                    self.assertEqual(code, 8)
                    self.assertEqual(report['errors'], [])
                    specs = report['suites'][0]['specs']
                    self.assertEqual(specs[0]['tests'][0]['results'][0]['status'], 'failed')
                    if phase == 'regression':
                        self.assertEqual(specs[1]['tests'][0]['results'][0]['status'], 'passed')

    def test_legacy_report_and_paths_preserved_and_unknown_policy_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            code, report, commands = self.invoke(root, extra={'AGENT_HARNESS_WORKFLOW_POLICY': None})
            self.assertEqual(code, 0)
            self.assertEqual(report['config']['metadata'], {'harnessRunId': 'run-a'})
            self.assertTrue((root / 'IMPLEMENT/regression/ctest.xml').is_file())
            self.assertEqual(Path(commands[0][commands[0].index('-B') + 1]), root / 'IMPLEMENT/regression/build')
            with patch.dict(os.environ, {'AGENT_HARNESS_WORKFLOW_POLICY': 'future'}, clear=True):
                with self.assertRaises(SystemExit):
                    checks.main()


if __name__ == '__main__':
    unittest.main()
