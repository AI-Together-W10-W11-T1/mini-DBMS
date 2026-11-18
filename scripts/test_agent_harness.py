import base64
import tempfile
import unittest
import json
import io
from contextlib import redirect_stdout
from types import SimpleNamespace
from unittest.mock import patch
from pathlib import Path

from agent_harness import Harness, HarnessError, assert_changes, safe_path, snapshot, validate_report, validate_result


class ContractsTest(unittest.TestCase):
    def setUp(self):
        self.packet = dict(run_id='r1', node_id='planner', role='planner', actor_id='agent-1',
                           attempt=1, input_digest='input-sha', source_digest='source-sha')
        self.result = dict(self.packet, status='PASS', findings=[], evidence_refs=[], requested_decisions=[])

    def test_matching_result_is_accepted(self):
        validate_result(self.packet, self.result)

    def test_missing_result_field_is_rejected(self):
        del self.result['findings']
        with self.assertRaises(HarnessError): validate_result(self.packet, self.result)

    def test_other_actor_or_stale_packet_is_rejected(self):
        for key in ('run_id', 'node_id', 'actor_id', 'attempt', 'input_digest', 'source_digest'):
            with self.subTest(key=key):
                result = dict(self.result, **{key: 'wrong'})
                with self.assertRaises(HarnessError): validate_result(self.packet, result)

    def test_fake_success_status_is_rejected(self):
        with self.assertRaises(HarnessError): validate_result(self.packet, dict(self.result, status='DONE'))

    def test_result_arrays_are_not_free_text(self):
        with self.assertRaises(HarnessError): validate_result(self.packet, dict(self.result, findings='looks good'))

    def test_nonapproved_change_is_rejected_including_untracked_files(self):
        with self.assertRaises(HarnessError): assert_changes({'a.cpp': 'old'}, {'a.cpp': 'new', 'new.cpp': 'x'}, ['a.cpp'])

    def test_deletion_is_a_change(self):
        with self.assertRaises(HarnessError): assert_changes({'a.cpp': 'old'}, {}, [])

    def test_approved_change_preserves_other_dirty_files(self):
        assert_changes({'a.cpp': 'old', 'user.md': 'dirty'}, {'a.cpp': 'new', 'user.md': 'dirty'}, ['a.cpp'])

    def test_paths_cannot_escape_or_access_private_data(self):
        with tempfile.TemporaryDirectory() as directory:
            for value in ('../outside', '/outside', '.private/a.md', '.git/config'):
                with self.subTest(path=value):
                    with self.assertRaises(HarnessError): safe_path(Path(directory), value)

    def report(self):
        return {'config': {'metadata': {'harnessRunId': 'r1'}}, 'errors': [], 'suites': [
            {'specs': [{'title': 'P1-02-01 text', 'tests': [{'expectedStatus': 'passed', 'results': [{'status': 'passed'}]}]}]}]}

    def test_current_passed_report_is_accepted(self):
        self.assertEqual(validate_report(self.report(), ['P1-02-01 text'], 'r1'), ['P1-02-01 text'])

    def test_old_or_missing_test_report_is_rejected(self):
        with self.assertRaises(HarnessError): validate_report(self.report(), ['P1-02-02 missing'], 'r1')
        with self.assertRaises(HarnessError): validate_report(self.report(), ['P1-02-01 text'], 'other-run')

    def test_skipped_failed_and_expected_failure_are_not_green(self):
        for status in ('skipped', 'failed', 'timedOut'):
            report = self.report()
            report['suites'][0]['specs'][0]['tests'][0]['results'][0]['status'] = status
            with self.subTest(status=status):
                with self.assertRaises(HarnessError): validate_report(report, ['P1-02-01 text'], 'r1')
        report = self.report()
        report['suites'][0]['specs'][0]['tests'][0]['expectedStatus'] = 'failed'
        with self.assertRaises(HarnessError): validate_report(report, ['P1-02-01 text'], 'r1')

    def test_global_errors_are_not_green(self):
        report = self.report(); report['errors'] = [{'message': 'setup error'}]
        with self.assertRaises(HarnessError): validate_report(report, ['P1-02-01 text'], 'r1')

    def test_metrics_are_required_when_requested(self):
        with self.assertRaises(HarnessError): validate_report(self.report(), ['P1-02-01 text'], 'r1', require_metrics=True)


class FlowTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / 'input.md').write_text('contract', encoding='utf-8')
        rolepath = self.root / 'tools/agent-harness/roles.json'
        rolepath.parent.mkdir(parents=True)
        real = Path(__file__).resolve().parents[1] / 'tools/agent-harness/roles.json'
        rolepath.write_bytes(real.read_bytes())
        (rolepath.parent / 'checks.json').write_text(json.dumps({'require_metrics': False,
            'commands': {key: ['{python}', '-c', 'pass'] for key in ('red', 'green', 'regression', 'build', 'targeted')}}), encoding='utf-8')
        self.h = Harness(self.root, 'run-1')
        self.h.create('DBMS-001', ['input.md'])

    def bind(self, node, harness=None, slots=3):
        h = harness or self.h
        h.prepare(node, slots)
        actor = 'unit-' + h.run_id + '-' + node + '-' + str(h.state['attempts'][node])
        bound = h.dispatch(node, actor, 'synthetic READY handshake',
                           {'agent_id': actor, 'response': {'agent_id': actor}, 'fixture': True})
        packet = h.active_packet(node)[2]
        result = {key: packet[key] for key in ('run_id', 'node_id', 'role', 'actor_id', 'attempt', 'input_digest', 'source_digest')}
        result.update(findings=[], evidence_refs=[], requested_decisions=[])
        observation = dict(runtime_actor_id=actor, delivery_message=bound['start_message'], completion='synthetic unit completion')
        return result, observation

    def planned(self, harness=None, writes=None, knowledge_paths=None):
        h = harness or self.h
        result, observation = self.bind('planner', h)
        result.update(status='PASS', plan={'test_cases': ['text test'], 'proposed_changes': writes or ['impl.cpp']})
        h.accept('planner', result, observation)
        h.approve('synthetic user approval', writes or ['impl.cpp'], ['text test'], knowledge_paths=knowledge_paths)

    def execute(self, name, code=0, report_run=None, harness=None):
        h = harness or self.h
        def fake_run(command, **kwargs):
            if name not in {'build', 'finish'}:
                report = {'config': {'metadata': {'harnessRunId': report_run or h.run_id}}, 'errors': [], 'suites': [
                    {'specs': [{'title': 'text test', 'tests': [{'expectedStatus': 'passed', 'results': [
                        {'status': 'failed' if name == 'red' else 'passed'}]}]}]}]}
                Path(kwargs['env']['AGENT_HARNESS_RESULT_FILE']).write_text(json.dumps(report), encoding='utf-8')
            return SimpleNamespace(returncode=1 if name == 'red' else code, stdout=b'unit fixture', stderr=b'')
        with patch('agent_harness.subprocess.run', side_effect=fake_run):
            return h.run_check(name)

    def implemented(self, knowledge_paths=None):
        self.planned(knowledge_paths=knowledge_paths)
        result, observation = self.bind('implementer')
        (self.root / 'impl.cpp').write_text('stub', encoding='utf-8')
        self.execute('red')
        self.h.confirm_red('synthetic functional assertion failure', ['text test'])
        (self.root / 'impl.cpp').write_text('green', encoding='utf-8')
        self.execute('build'); self.execute('green'); self.execute('regression')
        result['status'] = 'PASS'
        self.h.accept('implementer', result, observation)

    def verified(self, knowledge_paths=None):
        self.implemented(knowledge_paths=knowledge_paths)
        result, observation = self.bind('reviewer_qa')
        self.execute('build'); self.execute('regression')
        result.update(status='APPROVED', review={'status': 'APPROVED'}, qa={'status': 'PASS'})
        self.h.accept('reviewer_qa', result, observation)

    def test_cannot_approve_without_accepted_planner(self):
        with self.assertRaises(HarnessError):
            self.h.approve('user said yes', ['impl.cpp'], ['text test'])

    def test_cannot_implement_before_user_selection(self):
        with self.assertRaises(HarnessError): self.h.prepare('implementer', 3)
        result, observation = self.bind('planner')
        result.update(status='PASS', plan={'test_cases': ['test'], 'proposed_changes': ['impl.cpp']},
                      requested_decisions=[{'id': 'D1', 'question': 'C or C++?'}])
        self.h.accept('planner', result, observation)
        with self.assertRaisesRegex(HarnessError, 'user choice missing'):
            self.h.approve('approval', ['impl.cpp'], ['text test'])
        self.h.approve('approval', ['impl.cpp'], ['text test'], decisions={'D1': 'user chose C++'})
        self.h.prepare('implementer', 3)

    def test_same_task_cannot_have_two_active_roles(self):
        self.h.prepare('planner', 3)
        with self.assertRaises(HarnessError): self.h.prepare('planner', 3)
        with self.assertRaises(HarnessError): self.h.prepare('implementer', 3)

    def test_readonly_changes_are_rejected(self):
        result, observation = self.bind('planner')
        result.update(status='PASS', plan={'test_cases': ['test'], 'proposed_changes': ['impl.cpp']})
        (self.root / 'unexpected.cpp').write_text('unexpected', encoding='utf-8')
        with self.assertRaisesRegex(HarnessError, 'unapproved changes'):
            self.h.accept('planner', result, observation)

    def test_input_or_packet_tampering_rejected(self):
        packet = self.h.prepare('planner', 3)
        path = self.root / self.h.state['active']['packet']
        path.write_text('{}', encoding='utf-8')
        with self.assertRaises(HarnessError): self.h.active_packet('planner')

    def test_source_input_changes_invalidate_planning(self):
        (self.root / 'input.md').write_text('changed', encoding='utf-8')
        with self.assertRaisesRegex(HarnessError, 'frozen input'):
            self.h.prepare('planner', 3)

    def test_missing_native_dispatch_is_not_agent_evidence(self):
        packet = self.h.prepare('planner', 3)
        result = dict(packet, status='PASS', findings=[], evidence_refs=[], requested_decisions=[])
        with self.assertRaises(HarnessError): self.h.accept('planner', result, {})

    def test_unconfigured_build_is_retryable_without_receipt_collision(self):
        config = self.root / 'tools/agent-harness/checks.json'
        config.write_text(json.dumps({'commands': {'build': None}}), encoding='utf-8')
        self.h = Harness(self.root, 'unconfigured')
        self.h.create('DBMS-002', ['input.md'])
        self.planned(); self.bind('implementer')
        for unused in range(2):
            with self.assertRaisesRegex(HarnessError, 'not configured: build'):
                self.h.run_check('build')
        self.assertNotIn('build', self.h.state['checks'])

    def test_build_required_before_running_pass_tests(self):
        self.planned(); self.bind('implementer')
        for name in ('green', 'regression'):
            with self.assertRaisesRegex(HarnessError, 'missing check: build'):
                self.execute(name)

    def test_implementer_cannot_skip_red(self):
        self.planned(); result, observation = self.bind('implementer')
        self.execute('build'); self.execute('green'); self.execute('regression')
        result['status'] = 'PASS'
        with self.assertRaisesRegex(HarnessError, 'functional Red'):
            self.h.accept('implementer', result, observation)

    def test_implementer_cannot_edit_unapproved_file(self):
        self.planned(); result, observation = self.bind('implementer')
        (self.root / 'other.cpp').write_text('wrong file', encoding='utf-8')
        result['status'] = 'PASS'
        with self.assertRaisesRegex(HarnessError, 'unapproved changes'):
            self.h.accept('implementer', result, observation)

    def test_changed_report_invalidates_receipt(self):
        self.planned(); self.bind('implementer'); self.execute('build'); self.execute('regression')
        receipt = self.h.verify_check('regression')
        (self.root / receipt['report']).write_text('{}', encoding='utf-8')
        with self.assertRaisesRegex(HarnessError, 'report modified'):
            self.h.verify_check('regression')

    def test_build_failure_and_stale_source_are_not_passed(self):
        self.planned(); self.bind('implementer'); self.execute('build', code=1)
        with self.assertRaisesRegex(HarnessError, 'check failed'):
            self.h.verify_check('build')
        self.execute('build')
        (self.root / 'impl.cpp').write_text('changed', encoding='utf-8')
        with self.assertRaisesRegex(HarnessError, 'older source'):
            self.execute('regression')

    def test_stale_red_report_cannot_be_confirmed(self):
        self.planned(); self.bind('implementer'); self.execute('red', report_run='old-run')
        with self.assertRaises(HarnessError): self.h.confirm_red('not a current Red', ['text test'])

    def test_qa_cannot_reuse_implementer_build(self):
        self.implemented(); result, observation = self.bind('reviewer_qa')
        result.update(status='APPROVED', review={'status': 'APPROVED'}, qa={'status': 'PASS'})
        with self.assertRaisesRegex(HarnessError, 'another phase/actor'):
            self.h.accept('reviewer_qa', result, observation)

    def test_combined_reviewer_requires_separate_review_and_qa(self):
        self.implemented(); result, observation = self.bind('reviewer_qa')
        self.execute('build'); self.execute('regression')
        result.update(status='APPROVED')
        with self.assertRaisesRegex(HarnessError, 'separate approved review'):
            self.h.accept('reviewer_qa', result, observation)

    def test_full_flow_and_final_user_confirmation(self):
        self.verified()
        result, observation = self.bind('knowledge_manager')
        record = self.root / self.h.record_path(); record.parent.mkdir(parents=True)
        record.write_text('verified outcome', encoding='utf-8')
        self.execute('finish')
        result['status'] = 'PASS'
        self.h.accept('knowledge_manager', result, observation)
        self.assertEqual(self.h.state['phase'], 'USER_REVIEW')
        with self.assertRaises(HarnessError): self.h.finish('')
        self.h.finish('synthetic final user confirmation')
        self.assertEqual(self.h.state['phase'], 'DONE')
        self.assertEqual([x['node'] for x in self.h.state['history']],
                         ['planner', 'implementer', 'reviewer_qa', 'knowledge_manager'])

    def test_knowledge_manager_cannot_modify_source(self):
        self.verified(); result, observation = self.bind('knowledge_manager')
        (self.root / 'impl.cpp').write_text('unauthorized source edit', encoding='utf-8')
        result['status'] = 'PASS'
        with self.assertRaisesRegex(HarnessError, 'unapproved changes'):
            self.h.accept('knowledge_manager', result, observation)

    def test_implementation_knowledge_can_update_explicit_root_handoff_and_readme(self):
        for name in ('README.md', 'HANDOFF.md'):
            (self.root / name).write_text('original', encoding='utf-8')
        self.h = Harness(self.root, 'root-outputs')
        self.h.create('DBMS-091', ['input.md', 'README.md', 'HANDOFF.md'])
        outputs = [self.h.record_path(), 'README.md', 'HANDOFF.md']
        self.verified(knowledge_paths=outputs)
        technical_digest = self.h.source_digest()
        result, observation = self.bind('knowledge_manager')
        self.assertEqual(set(self.h.state['active']['writes']), set(outputs))
        for name in outputs:
            path = self.root / name; path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('verified knowledge', encoding='utf-8')
        self.assertEqual(self.h.source_digest(), technical_digest)
        self.execute('finish')
        (self.root / 'HANDOFF.md').write_text('changed after finish', encoding='utf-8')
        result['status'] = 'PASS'
        with self.assertRaisesRegex(HarnessError, 'outputs changed'):
            self.h.accept('knowledge_manager', result, observation)
        self.execute('finish')
        self.h.accept('knowledge_manager', result, observation)
        self.h.finish('user confirms root documentation')
        self.assertEqual(self.h.state['phase'], 'DONE')

    def test_implementation_knowledge_scope_rejects_other_root_and_control_paths(self):
        result, observation = self.bind('planner')
        result.update(status='PASS', plan={'test_cases': ['text test'], 'proposed_changes': ['impl.cpp']})
        self.h.accept('planner', result, observation)
        for name in ('other.md', 'readme-new.json', 'scripts/a.py', 'src/a.c',
                     'tools/agent-harness/checks.json', '.cache/a.md', '.private/a.md',
                     'docs/aGeNtS.md', 'aGeNtS.md', 'docs/.cache/a.md'):
            with self.subTest(path=name), self.assertRaises(HarnessError):
                self.h.approve('user approved', ['impl.cpp'], ['text test'], knowledge_paths=[self.h.record_path(), name])
        with self.assertRaises(HarnessError):
            self.h.approve('user approved', ['impl.cpp'], ['text test'], knowledge_paths=['README.md', 'HANDOFF.md'])
        with self.assertRaises(HarnessError):
            self.h.approve('user approved', ['docs/shared.md'], ['text test'],
                           knowledge_paths=[self.h.record_path(), 'docs/shared.md'])
        self.assertEqual(self.h.state['phase'], 'CONTRACT')

    def test_cumulative_different_findings_stop_at_third_rejection(self):
        for i in range(3):
            if i == 2: self.h.reanalyse('Master revised approach after second rejection')
            result, observation = self.bind('planner')
            result.update(status='REWORK', findings=[{'id': 'R-' + str(i), 'reason': 'different issue'}])
            self.h.accept('planner', result, observation)
        self.assertEqual(self.h.state['rejection_count'], 3)
        self.assertEqual(self.h.state['phase'], 'NEEDS_DECISION')
        self.assertTrue((self.root / self.h.state['user_report']).is_file())
        with self.assertRaises(HarnessError): self.h.prepare('planner', 3)

    def test_second_rejection_needs_reanalysis(self):
        for i in range(2):
            self.bind('planner')
            self.h.reject('planner', 'Master found missing requirement', ['correct requirement ' + str(i)], 'synthetic completion')
        with self.assertRaisesRegex(HarnessError, 'Master reanalysis'):
            self.h.prepare('planner', 3)
        self.h.reanalyse('reduce scope and clarify acceptance')
        self.h.prepare('planner', 3)

    def test_environment_and_scope_pause_do_not_count_as_rejection(self):
        result, observation = self.bind('planner')
        result.update(status='ENV_ERROR')
        self.h.accept('planner', result, observation)
        self.assertEqual(self.h.state['rejection_count'], 0)
        with self.assertRaises(HarnessError): self.h.resume('', 'fix environment')
        self.h.resume('user-message', 'environment repaired')
        self.assertEqual(self.h.state['phase'], 'PLANNER')

    def test_new_choice_requires_recontract_or_replan(self):
        self.planned(); result, observation = self.bind('implementer')
        result.update(status='NEEDS_DECISION', requested_decisions=[{'id': 'D2', 'question': 'change file format?'}])
        self.h.accept('implementer', result, observation)
        self.h.resume('user-message', 'choose new format')
        self.assertEqual(self.h.state['phase'], 'CONTRACT')
        with self.assertRaises(HarnessError): self.h.prepare('implementer', 3)

    def test_old_runs_not_silently_migrated(self):
        state = self.root / '.cache/agent-harness/legacy/state.json'
        state.parent.mkdir(parents=True)
        state.write_text(json.dumps({'version': 1}), encoding='utf-8')
        with self.assertRaisesRegex(HarnessError, 'legacy run'):
            Harness(self.root, 'legacy')

    def test_new_run_cannot_reset_same_task_rejection_history(self):
        other = Harness(self.root, 'duplicate')
        with self.assertRaisesRegex(HarnessError, 'preserve rejection history'):
            other.create('DBMS-001', ['input.md'])

    def test_success_cannot_hide_unresolved_user_choices(self):
        self.implemented()
        result, observation = self.bind('reviewer_qa')
        result.update(status='APPROVED', review={'status': 'APPROVED'}, qa={'status': 'PASS'},
                      requested_decisions=[{'id': 'D', 'question': 'choose new scope'}])
        with self.assertRaisesRegex(HarnessError, 'NEEDS_DECISION'):
            self.h.accept('reviewer_qa', result, observation)

    def test_knowledge_paths_cannot_escape_docs_after_normalization(self):
        result, observation = self.bind('planner')
        result.update(status='PASS', plan={'test_cases': ['test'], 'proposed_changes': ['impl.cpp']})
        self.h.accept('planner', result, observation)
        with self.assertRaisesRegex(HarnessError, 'exact docs paths'):
            self.h.approve('approval', ['impl.cpp'], ['text test'],
                           knowledge_paths=[self.h.record_path(), 'docs/../tools/agent-harness/checks.json'])

    def test_knowledge_rejection_preserves_upstream_qa_receipts(self):
        self.verified()
        self.bind('knowledge_manager')
        self.h.reject('knowledge_manager', 'missing explanation', ['add tested result'], 'synthetic completion')
        result, observation = self.bind('knowledge_manager')
        record = self.root / self.h.record_path(); record.parent.mkdir(parents=True)
        record.write_text('verified outcome', encoding='utf-8')
        self.execute('finish'); result['status'] = 'PASS'
        self.h.accept('knowledge_manager', result, observation)
        self.h.finish('user approves final result')
        self.assertEqual(self.h.state['phase'], 'DONE')

    def test_final_user_pause_resume_keeps_qa_evidence(self):
        self.verified()
        result, observation = self.bind('knowledge_manager')
        record = self.root / self.h.record_path(); record.parent.mkdir(parents=True)
        record.write_text('verified outcome', encoding='utf-8')
        self.execute('finish'); result['status'] = 'PASS'
        self.h.accept('knowledge_manager', result, observation)
        self.h.pause('clarification', [{'id': 'D', 'question': 'confirm outcome?'}])
        self.h.resume('user-message', 'outcome accepted')
        self.h.finish('user final confirmation')
        self.assertEqual(self.h.state['phase'], 'DONE')

    def test_documents_changed_after_finish_cannot_be_accepted(self):
        self.verified()
        result, observation = self.bind('knowledge_manager')
        record = self.root / self.h.record_path(); record.parent.mkdir(parents=True)
        record.write_text('verified outcome', encoding='utf-8')
        self.execute('finish')
        record.write_text('changed after verification', encoding='utf-8')
        result['status'] = 'PASS'
        with self.assertRaisesRegex(HarnessError, 'outputs changed'):
            self.h.accept('knowledge_manager', result, observation)

    def test_documents_changed_during_final_user_review_cannot_complete(self):
        self.verified()
        result, observation = self.bind('knowledge_manager')
        record = self.root / self.h.record_path(); record.parent.mkdir(parents=True)
        record.write_text('verified outcome', encoding='utf-8')
        self.execute('finish'); result['status'] = 'PASS'
        self.h.accept('knowledge_manager', result, observation)
        record.write_text('changed during user review', encoding='utf-8')
        with self.assertRaises(HarnessError):
            self.h.finish('user final confirmation')

    def test_idle_peer_does_not_mask_unauthorized_changes(self):
        other = Harness(self.root, 'other')
        other.create('DBMS-002', ['input.md'])
        self.planned(other, ['other.cpp'])
        self.bind('implementer', other)
        (self.root / 'other.cpp').write_text('observed other output', encoding='utf-8')
        other.reject('implementer', 'correction needed', ['fix behavior'], 'synthetic completion')
        # 인수한 peer 산출물은 이 작업의 기준선에 포함하고, 이후 변조를 검출한다.
        self.h.state['baseline'] = snapshot(self.root)
        result, observation = self.bind('planner')
        (self.root / 'other.cpp').write_text('unauthorized replacement', encoding='utf-8')
        result.update(status='PASS', plan={'test_cases': ['test'], 'proposed_changes': ['impl.cpp']})
        with self.assertRaisesRegex(HarnessError, 'unapproved changes'):
            self.h.accept('planner', result, observation)

    def test_independent_runs_can_overlap_within_runtime_slots(self):
        other = Harness(self.root, 'independent')
        other.create('DBMS-002', ['input.md'])
        self.planned(); self.planned(other, ['other.cpp'])
        self.bind('implementer')
        self.bind('implementer', other)
        (self.root / 'other.cpp').write_text('other task output', encoding='utf-8')
        self.h.assert_workspace(self.h.state['active']['before'], [])
        self.assertNotEqual(self.h.state['active']['node'], None)

    def test_writer_cannot_overlap_reader_of_same_file(self):
        (self.root / 'shared.cpp').write_text('shared input', encoding='utf-8')
        self.h = Harness(self.root, 'writer')
        self.h.create('DBMS-003', ['input.md'])
        self.planned(writes=['shared.cpp'])
        other = Harness(self.root, 'reader')
        other.create('DBMS-004', ['shared.cpp'])
        self.bind('implementer')
        with self.assertRaisesRegex(HarnessError, 'scope conflict'):
            other.prepare('planner', 3)

    def test_runtime_capacity_is_enforced(self):
        other = Harness(self.root, 'other')
        other.create('DBMS-002', ['input.md'])
        self.h.prepare('planner', 1)
        with self.assertRaisesRegex(HarnessError, 'capacity'):
            other.prepare('planner', 1)

    def test_independent_changes_do_not_stale_task_build(self):
        other = Harness(self.root, 'other')
        other.create('DBMS-002', ['input.md'])
        self.planned(); self.planned(other, ['other.cpp'])
        self.bind('implementer'); self.bind('implementer', other)
        self.execute('build')
        (self.root / 'other.cpp').write_text('unrelated output', encoding='utf-8')
        self.h.verify_check('build', current=True)


class SlimFlowTest(unittest.TestCase):
    setUp = FlowTest.setUp
    bind = FlowTest.bind
    planned = FlowTest.planned
    execute = FlowTest.execute

    def slim(self):
        self.planned()
        self.h.state.update(workflow_policy='slim-v1',
                            red_policy={'required': False, 'reason': 'documentation-only fixture', 'risk_flags': []})
        self.bind('implementer')

    def regression(self, check='regression', selected=None, exit_code=0, transform=None, **metadata):
        def run(command, **kwargs):
            self.last_env = kwargs['env']
            report = {'config': {'metadata': dict(harnessRunId=self.h.run_id,
                workflowPolicy='slim-v1', nodeId='IMPLEMENT', actorId=self.h.active_packet('implementer')[2]['actor_id'],
                sourceDigest=self.h.source_digest(), testScope='full', requestedTests=['text test'],
                build={'passed': True, 'configure_exit_code': 0, 'compile_exit_code': 0,
                       'directory': str(self.root / '.cache/build'), 'cache_reused': False}) | metadata},
                'errors': [], 'suites': [{'specs': [{'title': 'text test', 'tests': [
                    {'expectedStatus': 'passed', 'results': [{'status': 'passed'}]}]}]}]}
            if transform:
                transform(report)
            Path(kwargs['env']['AGENT_HARNESS_RESULT_FILE']).write_text(json.dumps(report), encoding='utf-8')
            return SimpleNamespace(returncode=exit_code, stdout=b'integrated fixture', stderr=b'')
        with patch('agent_harness.subprocess.run', side_effect=run):
            return self.h.run_check(check, selected)

    def test_slim_integrated_regression_does_not_require_separate_build(self):
        self.slim()
        try:
            receipt = self.regression()
        except HarnessError as error:
            self.fail('integrated build/full regression must run without separate build receipt: ' + str(error))
        self.assertTrue(receipt['passed'])

    def test_slim_rejects_selected_regression_as_final_evidence(self):
        self.slim(); self.execute('build')
        self.assertFalse(self.regression(testScope='selected')['passed'])

    def test_slim_rejects_report_from_other_actor(self):
        self.slim(); self.execute('build')
        self.assertFalse(self.regression(actorId='other-actor')['passed'])

    def test_slim_rejects_wrong_phase_stale_source_and_failed_build(self):
        self.slim()
        for metadata in ({'nodeId': 'VERIFY'}, {'sourceDigest': 'old'}, {'build': {'passed': False}},
                         {'requestedTests': ['other']}, {'requestedTests': ['text test', 'not executed']}):
            with self.subTest(metadata=metadata):
                self.assertFalse(self.regression(**metadata)['passed'])

    def test_slim_partial_check_cannot_complete_implementation(self):
        self.slim(); self.execute('build'); self.execute('green')
        with self.assertRaisesRegex(HarnessError, 'missing check: regression'):
            self.h.implementation_ready()
        self.regression(); self.h.implementation_ready()

    def test_slim_targets_cannot_shrink_regression_or_escape_approval(self):
        self.slim()
        for name, tests in (('regression', ['text test']), ('red', ['other'])):
            with self.assertRaisesRegex(HarnessError, 'approved exact'):
                self.h.run_check(name, tests)

    def test_slim_targeted_receipt_is_interim_and_environment_is_bound(self):
        self.slim()
        receipt = self.regression(check='targeted', selected=['text test'], testScope='selected')
        self.assertTrue(receipt['passed'])
        self.assertEqual(json.loads(self.last_env['AGENT_HARNESS_TEST_TARGETS']), ['text test'])
        self.assertEqual(self.last_env['AGENT_HARNESS_NODE_ID'], 'IMPLEMENT')
        self.assertEqual(self.last_env['AGENT_HARNESS_SOURCE_DIGEST'], self.h.source_digest())
        with self.assertRaisesRegex(HarnessError, 'missing check: regression'):
            self.h.implementation_ready()
        with patch.dict('os.environ', {'AGENT_HARNESS_TEST_TARGETS': '["other"]'}):
            self.regression()
        self.assertNotIn('AGENT_HARNESS_TEST_TARGETS', self.last_env)

    def test_opening_legacy_state_does_not_rewrite_it(self):
        before = self.h.state_file.read_bytes()
        reopened = Harness(self.root, self.h.run_id)
        self.assertFalse(reopened.slim)
        self.assertEqual(self.h.state_file.read_bytes(), before)

    def test_slim_red_rejects_skipped_missing_or_unknown_execution(self):
        self.slim()
        for status in ('skipped', 'unknown', None):
            def incomplete(report):
                test = report['suites'][0]['specs'][0]['tests'][0]
                test['results'] = [{'status': 'failed'}]
                report['suites'][0]['specs'][0]['tests'].append(
                    {'expectedStatus': 'passed', 'results': [{'status': status}] if status else []})
            with self.subTest(status=status):
                self.regression(check='red', exit_code=1, transform=incomplete)
                with self.assertRaisesRegex(HarnessError, 'functional Red'):
                    self.h.confirm_red('assertion plus incomplete execution', ['text test'])

    def test_slim_red_accepts_assertion_failure_and_rejects_global_errors(self):
        self.slim()
        def fail(report):
            report['suites'][0]['specs'][0]['tests'][0]['results'][0]['status'] = 'failed'
        self.regression(check='red', exit_code=1, transform=fail)
        self.h.confirm_red('actual fixture assertion', ['text test'])
        self.assertIn('receipt', self.h.state['red_confirmation'])
        def environment_error(report):
            fail(report); report['errors'] = [{'message': 'setup failed'}]
        self.regression(check='red', exit_code=1, transform=environment_error)
        with self.assertRaisesRegex(HarnessError, 'functional Red'):
            self.h.confirm_red('not a functional failure', ['text test'])
        self.assertNotIn('red_confirmation', self.h.state)

    def test_slim_result_path_reuses_one_approved_markdown_output(self):
        result, observation = self.bind('planner')
        result.update(status='PASS', plan={'test_cases': ['text test'], 'proposed_changes': ['impl.cpp']})
        self.h.accept('planner', result, observation)
        defaults = dict(workflow_policy='slim-v1', red_policy={'required': False, 'reason': 'docs', 'risk_flags': []})
        for path, outputs in (('README.md', ['README.md']), ('docs/wiki/index.md', ['docs/wiki/index.md']),
                              ('docs/results.md', ['docs/other.md'])):
            with self.assertRaises(HarnessError):
                self.h.approve('A', ['impl.cpp'], ['text test'], result_path=path, knowledge_paths=outputs, **defaults)
        self.h.approve('A', ['impl.cpp'], ['text test'], result_path='docs/results.md',
                       knowledge_paths=['docs/results.md', 'docs/wiki/index.md'], **defaults)
        self.assertEqual(self.h.record_path(), 'docs/results.md')
        self.assertNotIn('docs/testing/records/DBMS-001.md', self.h.outputs())

    def test_policy_requires_high_risk_red_and_rejects_unknown_risks(self):
        result, observation = self.bind('planner')
        result.update(status='PASS', plan={'test_cases': ['text test'], 'proposed_changes': ['impl.cpp']})
        self.h.accept('planner', result, observation)
        for flag in ('bugfix', 'storage_atomicity', 'data_structure_invariant', 'overflow',
                     'concurrency', 'ownership', 'harness_evidence', 'unknown'):
            with self.subTest(flag=flag):
                with self.assertRaisesRegex(HarnessError, 'explicit reason'):
                    self.h.approve('approval', ['impl.cpp'], ['text test'], workflow_policy='slim-v1',
                                   red_policy={'required': False, 'reason': 'reason', 'risk_flags': [flag]})
        with self.assertRaisesRegex(HarnessError, 'unknown workflow'):
            self.h.approve('approval', ['impl.cpp'], ['text test'], workflow_policy='future')
        self.h.approve('approval', ['impl.cpp'], ['text test'], workflow_policy='slim-v1',
                       red_policy={'required': True, 'reason': 'evidence gate', 'risk_flags': ['harness_evidence']})
        self.bind('implementer'); self.regression()
        with self.assertRaisesRegex(HarnessError, 'Red not confirmed'):
            self.h.implementation_ready()

    def test_amend_records_observed_actor_completion_and_preserves_rejections(self):
        path = self.root / 'tests/existing.c'; path.parent.mkdir(); path.write_text('assertion', encoding='utf-8')
        self.h.state['baseline'] = snapshot(self.root)
        self.slim(); self.regression()
        self.h.state['rejection_count'] = 2
        args = dict(approval_ref='user A', contract_ref='existing contract', reason='expectation alignment',
                    test_paths=['tests/existing.c'], required_tests=['text test'])
        with self.assertRaisesRegex(HarnessError, 'completion reference'):
            self.h.amend_test_scope(**args)
        actor = self.h.active_packet('implementer')[2]['actor_id']
        self.h.amend_test_scope(**args, completion_ref={'actor_id': actor, 'completion': 'Master observed complete'})
        self.assertEqual(self.h.state['rejection_count'], 2)
        self.assertEqual(self.h.state['checks'], {})
        self.assertIsNone(self.h.state['active'])
        self.assertIn('tests/existing.c', self.h.state['write_paths'])
        self.assertEqual(self.h.state['test_scope_amendments'][0]['completion_ref']['actor_id'], actor)
        self.bind('implementer')

    def test_amend_rejects_production_new_files_and_test_name_removal(self):
        self.slim()
        for paths, names in ((['input.md'], ['text test']), (['tests/missing.c'], ['text test']),
                             (['input.md'], ['replacement'])):
            with self.assertRaises(HarnessError):
                self.h.amend_test_scope('A', 'contract', 'reason', paths, names)

    def test_amend_accepts_existing_python_cli_test_with_same_guards(self):
        path = self.root / 'tests/existing.py'
        path.parent.mkdir(); path.write_text('assert True\n', encoding='utf-8')
        self.h.state['baseline'] = snapshot(self.root)
        self.slim(); self.regression()
        self.h.state['rejection_count'] = 2
        actor = self.h.active_packet('implementer')[2]['actor_id']
        args = dict(approval_ref='user A', contract_ref='existing SQL contract', reason='expectation alignment',
                    test_paths=['tests/existing.py'], required_tests=['text test'],
                    completion_ref={'actor_id': actor, 'completion': 'Master observed complete'})
        try:
            self.h.amend_test_scope(**args)
        except HarnessError as error:
            self.fail('existing tests Python CLI fixture must be amendable: ' + str(error))
        self.assertIn('tests/existing.py', self.h.state['write_paths'])
        self.assertEqual(self.h.state['rejection_count'], 2)
        self.assertEqual(self.h.state['checks'], {})
        self.assertIsNone(self.h.state['active'])
        self.assertEqual(self.h.state['test_scope_amendments'][0]['completion_ref']['actor_id'], actor)

    def test_summary_cli_preserves_full_state_and_normal_amend_command(self):
        from agent_harness import main, write_json
        path = self.root / 'tests/existing.c'; path.parent.mkdir(); path.write_text('assertion', encoding='utf-8')
        self.h.state['baseline'] = snapshot(self.root)
        self.slim()
        actor = self.h.active_packet('implementer')[2]['actor_id']
        contract = self.root / '.cache/amend.json'
        write_json(contract, dict(approval_ref='A', contract_ref='contract', reason='alignment',
                   test_paths=['tests/existing.c'], required_tests=['text test'],
                   completion_ref={'actor_id': actor, 'completion': 'observed completed'}))
        output = io.StringIO()
        with patch('agent_harness.__file__', str(self.root / 'scripts/agent_harness.py')), \
                patch('sys.argv', ['agent_harness.py', '--run', 'run-1', '--summary', 'amend-test-scope', '--contract', str(contract)]), \
                redirect_stdout(output):
            self.assertEqual(main(), 0)
        brief = json.loads(output.getvalue())
        self.assertEqual(brief['phase'], 'IMPLEMENT')
        self.assertNotIn('baseline', brief)
        state = json.loads(self.h.state_file.read_text(encoding='utf-8'))
        self.assertIn('baseline', state)
        self.assertEqual(state['test_scope_amendments'][0]['completion_ref']['actor_id'], actor)


class DesignFlowTest(unittest.TestCase):
    setUp = FlowTest.setUp
    bind = FlowTest.bind
    execute = FlowTest.execute
    # 기존 구현 흐름 검사는 그대로 두고 설계 문서의 독립 완료 경로를 검사한다.
    def design(self, outputs=None):
        self.h = Harness(self.root, 'design')
        self.h.create('DBMS-090', ['input.md'], kind='design')
        result, observation = self.bind('planner')
        result.update(status='PASS', plan={'test_cases': ['document checks'], 'proposed_changes': ['docs/design.md']})
        self.h.accept('planner', result, observation)
        self.h.approve('user design approval', [], [], knowledge_paths=outputs or ['docs/design.md'])

    def written(self):
        self.design()
        result, observation = self.bind('knowledge_manager')
        path = self.root / 'docs/design.md'; path.parent.mkdir(parents=True)
        path.write_text('# Design\n[Input](../input.md)\n', encoding='utf-8')
        self.execute('finish')
        result['status'] = 'PASS'
        self.h.accept('knowledge_manager', result, observation)

    def test_design_full_flow_uses_real_document_check_without_c_checks(self):
        self.written()
        result, observation = self.bind('reviewer_qa')
        self.assertIn('docs/design.md', self.h.state['active']['reads'])
        self.assertNotIn('build', self.h.active_packet('reviewer_qa')[2]['task'])
        receipt = self.h.run_check('docs')
        self.assertTrue(receipt['passed'])
        self.assertIn('knowledge_outputs_digest', receipt)
        result.update(status='APPROVED', review={'status': 'APPROVED'}, qa={'status': 'PASS'})
        self.h.accept('reviewer_qa', result, observation)
        with self.assertRaises(HarnessError): self.h.finish('')
        self.h.finish('user confirms docs')
        self.assertEqual(self.h.state['phase'], 'DONE')
        self.assertEqual(set(self.h.state['checks']), {'docs', 'finish'})

    def test_design_rejects_source_and_control_outputs(self):
        for output in ('src/a.c', 'AGENTS.md', 'docs/../scripts/a.py', '.cache/a.md', 'docs/a.txt'):
            with self.subTest(output=output):
                with self.assertRaises(HarnessError): self.design([output])
                # independent fixture reset preserves production duplicate-run guard
                self.setUp()

    def test_design_rejects_new_mixed_case_agents_filename_before_write(self):
        target = self.root / 'docs/aGeNtS.md'
        self.assertFalse(target.exists())
        with self.assertRaisesRegex(HarnessError, 'approved documentation paths'):
            self.design(['docs/aGeNtS.md'])
        self.assertFalse(target.exists())
        self.assertEqual(self.h.state['phase'], 'CONTRACT')

    def test_design_role_instructions_preserve_korean_and_coordination(self):
        roles = json.loads((self.root / 'tools/agent-harness/roles.json').read_text(encoding='utf-8'))
        for role in ('planner', 'reviewer_qa', 'knowledge_manager'):
            instructions = roles['roles'][role]['design_instructions']
            self.assertRegex(instructions, '[가-힣]')
            self.assertNotIn('?', instructions)
            self.assertIn('Master', instructions)
            self.assertIn('.private', instructions)

    def test_design_bad_links_fail_actual_check(self):
        self.written(); self.bind('reviewer_qa')
        (self.root / 'docs/design.md').write_text('[Missing](missing.md)', encoding='utf-8')
        with self.assertRaises(HarnessError): self.h.run_check('docs')

    def test_actual_document_validator_checks_utf8_links_and_hashes(self):
        from check_design_docs import check_documents
        from agent_harness import file_hash
        path = self.root / 'docs/design.md'; path.parent.mkdir(parents=True)
        for body in (b'[Missing](missing.md)', b'\xff'):
            path.write_bytes(body)
            manifest = dict(run_id='design', artifacts={'docs/design.md': file_hash(path)}, references={})
            self.assertFalse(check_documents(self.root, manifest)['passed'])
        path.write_text('[Input](../input.md)', encoding='utf-8')
        manifest = dict(run_id='design', artifacts={'docs/design.md': file_hash(path)}, references={'input.md': file_hash(self.root / 'input.md')})
        self.assertTrue(check_documents(self.root, manifest)['passed'])
        (self.root / 'input.md').write_text('changed', encoding='utf-8')
        self.assertFalse(check_documents(self.root, manifest)['passed'])

    def test_design_no_implementer_or_c_checks(self):
        self.design()
        with self.assertRaises(HarnessError): self.h.prepare('implementer', 3)
        self.bind('knowledge_manager')
        for name in ('red', 'build', 'green', 'regression'):
            with self.assertRaises(HarnessError): self.h.run_check(name)

    def test_design_resume_after_rewrite_requires_finish_again(self):
        self.written()
        self.h.pause('clarification', [{'id': 'D', 'question': 'explain?'}])
        (self.root / 'docs/design.md').write_text('rewritten', encoding='utf-8')
        self.h.resume('user', 'clarified')
        self.assertEqual(self.h.state['phase'], 'KNOWLEDGE')
        self.assertEqual(self.h.state['checks'], {})

    def test_design_reviewer_rejection_returns_to_writer_and_invalidates_finish(self):
        self.written(); self.bind('reviewer_qa')
        self.h.reject('reviewer_qa', 'missing decision', ['explain decision'], 'actor stopped')
        self.assertEqual(self.h.state['phase'], 'KNOWLEDGE')
        self.assertEqual(self.h.state['checks'], {})

    def test_legacy_v2_missing_kind_still_implementation(self):
        self.h.state.pop('kind', None); self.h.save('legacy fixture')
        self.assertEqual(Harness(self.root, 'run-1').kind, 'implementation')


if __name__ == '__main__':
    unittest.main()
