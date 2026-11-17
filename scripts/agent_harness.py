"""Main이 조율하고 로컬 검증기가 근거를 검사하는 작업 하네스."""
from pathlib import Path
from datetime import datetime, timezone
import argparse
import base64
import hashlib
import json
import os
import re
import subprocess
import sys
import time


class HarnessError(ValueError):
    pass


def safe_path(root, value):
    root = Path(root).resolve()
    path = (root / value).resolve()
    if root not in path.parents or any(part in {'.private', '.git'} for part in path.relative_to(root).parts):
        raise HarnessError('path outside allowed workspace: ' + str(value))
    return path


def knowledge_output_path(root, value):
    # 두 실행 종류가 같은 문서 경계를 쓴다. 루트 예외는 명시한 두 파일뿐이다.
    path = safe_path(root, value).relative_to(Path(root).resolve()).as_posix()
    parts = Path(path).parts
    if (Path(path).name.casefold() == 'agents.md' or any(part.casefold() == '.cache' for part in parts)
            or not (path in {'README.md', 'HANDOFF.md'}
                    or path.startswith('docs/') and path.endswith(('.md', '.json')))):
        raise HarnessError('knowledge outputs must be exact docs paths or approved README.md/HANDOFF.md')
    return path


def assert_changes(before, after, allowed):
    changed = {key for key in before.keys() | after.keys() if before.get(key) != after.get(key)}
    unexpected = changed - set(allowed)
    if unexpected:
        raise HarnessError('unapproved changes: ' + ', '.join(sorted(unexpected)))


def validate_result(packet, result):
    for key in ('run_id', 'node_id', 'role', 'actor_id', 'attempt', 'input_digest', 'source_digest'):
        if key not in result or result[key] != packet[key]:
            raise HarnessError('result identity mismatch: ' + key)
    statuses = {'planner': {'PASS', 'REWORK', 'ENV_ERROR', 'NEEDS_DECISION'},
                'implementer': {'PASS', 'REWORK', 'FAIL', 'ENV_ERROR', 'NEEDS_DECISION'},
                'reviewer_qa': {'APPROVED', 'REWORK', 'FAIL', 'ENV_ERROR', 'NEEDS_DECISION'},
                'knowledge_manager': {'PASS', 'REWORK', 'ENV_ERROR', 'NEEDS_DECISION'}}
    if result.get('status') not in statuses[packet['role']]:
        raise HarnessError('invalid role status')
    for key in ('findings', 'evidence_refs', 'requested_decisions'):
        if not isinstance(result.get(key), list):
            raise HarnessError('result must contain array: ' + key)
    if not all(isinstance(f, dict) and isinstance(f.get('id'), str) and f['id'] for f in result['findings']):
        raise HarnessError('findings must have stable IDs')
    if result['status'] in {'REWORK', 'FAIL'} and not result['findings']:
        raise HarnessError('rework/failure must identify findings')
    if result['status'] == 'NEEDS_DECISION' and not result['requested_decisions']:
        raise HarnessError('decision status must identify a question')


def validate_report(report, required_tests, run_id, require_metrics=False):
    if not required_tests or report.get('errors') or report.get('config', {}).get('metadata', {}).get('harnessRunId') != run_id:
        raise HarnessError('missing requirements, global errors, or stale run report')
    passed = []

    def visit(suite):
        for spec in suite.get('specs', []):
            tests = spec.get('tests', [])
            if not tests or any(t.get('expectedStatus') != 'passed' or not t.get('results') or
                any(r.get('status') != 'passed' for r in t['results']) for t in tests):
                raise HarnessError('non-passed test: ' + spec.get('title', '?'))
            if require_metrics:
                for test in tests:
                    for result in test['results']:
                        attachments = [a for a in result.get('attachments', []) if a.get('name') == 'M-01-M-07' and a.get('body')]
                        try:
                            metrics = json.loads(base64.b64decode(attachments[0]['body']))
                            if metrics.get('M-01') != 'passed' or not all(metrics.get('M-%02d' % i) is not None for i in range(1, 8)):
                                raise HarnessError('incomplete M-01~M-07')
                        except (IndexError, ValueError, TypeError, KeyError) as error:
                            raise HarnessError('metrics attachment missing/invalid: ' + spec['title']) from error
            passed.append(spec['title'])
        for child in suite.get('suites', []):
            visit(child)
    for suite in report.get('suites', []):
        visit(suite)
    if not set(required_tests).issubset(passed):
        raise HarnessError('required tests missing from report')
    return passed


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=True).encode()).hexdigest()


def packet_digest(packet):
    # 실행 후 배정되는 actor ID와 자기 자신의 digest 필드는 입력 내용 해시에서 분리한다.
    return digest({k: v for k, v in packet.items() if k not in {'actor_id', 'input_digest'}})


def file_hash(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def snapshot(root):
    # 추적 여부와 무관하게 기존 사용자 파일을 포함한다. 개인 자료와 실행 부산물은 별도 경계다.
    ignored = {'.git', '.cache', '.private', 'node_modules', '__pycache__', 'test-results', 'playwright-report', 'codex-setup-github'}
    result = {}
    for directory, folders, files in os.walk(root):
        folders[:] = sorted(x for x in folders if x not in ignored)
        for name in files:
            path = Path(directory) / name
            relative = path.relative_to(root).as_posix()
            result[relative] = file_hash(safe_path(root, relative))
    return result


class DirectoryLock:
    """로컬 CLI 동시 변경만 직렬화한다. OS 보안/actor 인증을 대신하지 않는다."""
    def __init__(self, path):
        self.path = Path(path)

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        try:
            self.path.mkdir()
        except FileExistsError as error:
            raise HarnessError('coordinator busy; retry after current mutation: ' + str(self.path)) from error
        return self

    def __exit__(self, *args):
        self.path.rmdir()


class Harness:
    """Master 조율용 원장. 실제 actor 생성·통신·권한은 실행 환경이 담당한다."""
    PHASES = {'planner': 'PLANNER', 'implementer': 'IMPLEMENT',
              'reviewer_qa': 'VERIFY', 'knowledge_manager': 'KNOWLEDGE'}

    def __init__(self, root, run_id):
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,80}', run_id):
            raise HarnessError('invalid run ID')
        self.root = Path(root).resolve()
        self.run_id = run_id
        self.directory = safe_path(self.root, '.cache/agent-harness/' + run_id)
        self.state_file = self.directory / 'state.json'
        self.state = read_json(self.state_file) if self.state_file.exists() else {}
        if self.state and self.state.get('version') != 2:
            raise HarnessError('legacy run: preserve receipts and create a new v2 run; no implicit migration')

    def save(self, event, **details):
        self.directory.mkdir(parents=True, exist_ok=True)
        with (self.directory / 'events.jsonl').open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(dict(at=datetime.now(timezone.utc).isoformat(), event=event,
                                         phase=self.state['phase'], **details), ensure_ascii=False) + '\n')
        write_json(self.state_file, self.state)

    @property
    def kind(self):
        # 이전 v2 원장은 구현 전용이다. 종류는 생성 시에만 지정한다.
        return self.state.get('kind', 'implementation')

    def outputs_digest(self):
        return digest({p: file_hash(safe_path(self.root, p)) if safe_path(self.root, p).is_file() else None
                       for p in self.outputs()})

    def require_phase(self, phase):
        if self.state.get('phase') != phase:
            raise HarnessError('expected phase ' + phase)

    def record_path(self):
        return 'docs/testing/records/' + self.state['requirement'] + '.md'

    def outputs(self):
        return self.state.get('knowledge_paths', [self.record_path(), 'docs/wiki/index.md', 'docs/wiki/sources.json'])

    def peers(self):
        for path in (self.root / '.cache/agent-harness').glob('*/state.json'):
            if path != self.state_file:
                state = read_json(path)
                if state.get('version') == 2:
                    yield state

    def assert_workspace(self, before, allowed):
        # 다른 run의 명시된 산출물은 이 작업의 무단 변경으로 오인하지 않는다.
        # 파일별 책임은 Master와 활성 read/write claim이 정한다. OS 쓰기 격리는 아니다.
        other_outputs = set()
        current = snapshot(self.root)
        for peer in self.peers():
            if peer.get('active'):
                other_outputs.update(peer['active']['writes'])
            # 종료된 peer는 실제 인수 당시 해시와 일치하는 산출물만 예외로 인정한다.
            other_outputs.update(name for name, sha in peer.get('produced_files', {}).items()
                                 if current.get(name) == sha)
        own_scope = set(self.state['inputs']) | set(self.state['write_paths']) | set(self.outputs())
        assert_changes(before, current, set(allowed) | (other_outputs - own_scope))

    def remember_outputs(self, active):
        current = snapshot(self.root)
        produced = self.state.setdefault('produced_files', {})
        for name in active['writes']:
            if active['before'].get(name) != current.get(name):
                produced[name] = current.get(name)

    def source_digest(self, sources=None):
        sources = snapshot(self.root) if sources is None else sources
        # 완료 기록은 기술 검사 해시에서 제외한다. 의존 파일은 반드시 inputs에 선언한다.
        scope = (set(self.state['inputs']) | set(self.state['write_paths'])) - set(self.outputs())
        return digest({name: sources.get(name) for name in sorted(scope)})

    def check_inputs(self):
        for name, expected in self.state['inputs'].items():
            if self.state.get('approval_ref') and name in self.state['write_paths']:
                continue
            phase = self.state.get('resume_phase') if self.state['phase'] == 'NEEDS_DECISION' else self.state['phase']
            if (phase in {'KNOWLEDGE', 'USER_REVIEW', 'DONE'} or self.kind == 'design' and phase == 'VERIFY') and name in self.outputs():
                continue
            if not safe_path(self.root, name).is_file() or file_hash(safe_path(self.root, name)) != expected:
                raise HarnessError('frozen input changed: ' + name)

    def require_node(self, node):
        if self.kind == 'design' and node == 'implementer':
            raise HarnessError('design runs have no implementer')
        if node not in self.PHASES:
            raise HarnessError('unknown delegated node')
        self.require_phase(self.PHASES[node])

    def create(self, requirement, inputs, kind='implementation'):
        if kind not in {'implementation', 'design'}:
            raise HarnessError('invalid run kind')
        if self.state_file.exists() or not re.fullmatch(r'DBMS-\d{3}', requirement) or not inputs:
            raise HarnessError('run exists or invalid requirement/inputs')
        if any(peer.get('requirement') == requirement for peer in self.peers()):
            raise HarnessError('requirement already has a run; resume it to preserve rejection history')
        paths = sorted({safe_path(self.root, p).relative_to(self.root).as_posix() for p in
                        inputs + ['tools/agent-harness/roles.json', 'tools/agent-harness/checks.json']})
        self.state = dict(version=2, kind=kind, run_id=self.run_id, requirement=requirement, phase='PLANNER',
                          inputs={name: file_hash(safe_path(self.root, name)) for name in paths},
                          baseline=snapshot(self.root), attempts={}, checks={}, history=[], active=None,
                          required_tests=[], write_paths=[], rejections=[], rejection_count=0,
                          rejection_limit=3, rejection_window_start=0)
        self.save('master_initialized')
        return self.state

    def prepare(self, node, available_slots):
        with DirectoryLock(self.root / '.cache/agent-harness/_scheduler.lock'):
            self.require_node(node)
            self.check_inputs()
            if self.state['active']:
                raise HarnessError('node already active; observe completion before replacing it')
            if self.state.get('reanalysis_required') and not self.state.get('reanalysis_note'):
                raise HarnessError('second rejection requires Master reanalysis before redispatch')
            if not isinstance(available_slots, int) or available_slots < 1:
                raise HarnessError('provide positive runtime child-slot capacity')
            profiles = read_json(self.root / 'tools/agent-harness/roles.json')
            peers = [state['active'] for state in self.peers() if state.get('active')]
            if len(peers) >= min(available_slots, profiles['max_concurrent_children']):
                raise HarnessError('runtime child-slot capacity reached')
            current = snapshot(self.root)
            self.assert_workspace(self.state['baseline'], [])
            reads = set(self.state['inputs']) | set(self.state['write_paths'])
            writes = set(self.state['write_paths']) if node == 'implementer' else set(self.outputs()) if node == 'knowledge_manager' else set()
            if node == 'knowledge_manager' or self.kind == 'design' and node == 'reviewer_qa':
                reads.update(self.outputs())
            for active in peers:
                if writes & (set(active['reads']) | set(active['writes'])) or reads & set(active['writes']):
                    raise HarnessError('parallel read/write scope conflict; wait or isolate workspaces')
            attempt = self.state['attempts'].get(node, 0) + 1
            packet = dict(run_id=self.run_id, kind=self.kind, node_id=node, role=node, actor_id=None, attempt=attempt,
                          inputs={p: current.get(p) for p in sorted(reads)}, source_digest=self.source_digest(current),
                          role_profile=profiles['roles'][node], approved_write_paths=sorted(writes),
                          required_tests=self.state['required_tests'], previous_results=self.state['history'],
                          command_receipts=self.state['checks'], prior_sources=self.state.get('prior_sources', {}),
                          coordination='Master only; no user contact, peer messaging or further delegation',
                          approval_summary=self.state.get('approval_ref', 'Planning only; user choices not yet approved'),
                          decision_protocol='Return NEEDS_DECISION for new choices or scope changes; never choose for user',
                          return_contract=['run_id', 'node_id', 'role', 'actor_id', 'attempt', 'input_digest',
                                           'source_digest', 'status', 'findings', 'evidence_refs', 'requested_decisions'])
            tasks = {'planner': 'Return plan with alternatives, test_cases, proposed_changes and unresolved user choices. Do not implement.',
                     'implementer': 'Implement approved files. Produce actual Red/Green/build/regression receipts; report only to Master.',
                     'reviewer_qa': 'Read-only code review plus fresh build then regression. Return separate review and qa fields. Do not edit source.',
                     'knowledge_manager': 'Write only approved documentation, verify sources and run finish. Return private-history summary to Master; never access .private.'}
            packet['task'] = tasks[node]
            if self.kind == 'design':
                packet['role_profile'] = dict(packet['role_profile'], instructions=profiles['roles'][node].get('design_instructions', tasks[node]))
                packet['task'] = {'planner': 'Return current Phase design alternatives, document checks and exact documentation outputs. Do not implement.',
                                  'knowledge_manager': 'Write approved design documents, compare original references and run finish. Report only to Master.',
                                  'reviewer_qa': 'Read-only design review and fresh docs verification. Return separate review and qa fields.'}[node]
            if node == 'reviewer_qa':
                packet['check_commands'] = ['python scripts/agent_harness.py --run ' + self.run_id + ' run-check ' + n for n in (('docs',) if self.kind == 'design' else ('build', 'regression'))]
            packet['input_digest'] = packet_digest(packet)
            path = self.directory / 'packets' / (node + '-' + str(attempt) + '.json')
            write_json(path, packet)
            self.state['attempts'][node] = attempt
            self.state['active'] = dict(node=node, packet=path.relative_to(self.root).as_posix(),
                                        packet_hash=file_hash(path), before=current, dispatch=None,
                                        reads=sorted(reads), writes=sorted(writes))
            self.save('packet_prepared', node=node)
            return packet

    def active_packet(self, node):
        active = self.state.get('active')
        if not active or active['node'] != node:
            raise HarnessError('no matching active packet')
        path = safe_path(self.root, active['packet'])
        if file_hash(path) != active['packet_hash']:
            raise HarnessError('packet changed after preparation/dispatch')
        packet = read_json(path)
        if packet_digest(packet) != packet['input_digest']:
            raise HarnessError('packet input digest mismatch')
        for ref in packet['previous_results']:
            if file_hash(safe_path(self.root, ref['result'])) != ref['result_hash']:
                raise HarnessError('upstream result modified')
        return active, path, packet

    def dispatch(self, node, actor_id, launch_message, receipt):
        self.require_node(node)
        self.check_inputs()
        active, path, packet = self.active_packet(node)
        self.assert_workspace(active['before'], [])
        if active['dispatch'] or not actor_id or not launch_message or not isinstance(receipt, dict) or \
                receipt.get('agent_id') != actor_id or actor_id not in json.dumps(receipt.get('response', {})):
            raise HarnessError('missing/mismatched Master-observed native creation receipt')
        packet['actor_id'] = actor_id
        write_json(path, packet)
        active['packet_hash'] = file_hash(path)
        message = ('START ' + node + '. Master bound runtime actor_id=' + actor_id + '. Read ' + str(path) +
                   '. Verify input_digest=' + packet['input_digest'] + '. Follow role_profile, task and approved_write_paths. '
                   'Return one JSON object with return_contract fields. No user/peer contact, private data or further delegation. Report only to Master.')
        active['dispatch'] = dict(actor_id=actor_id, launch_message=launch_message, native_creation_receipt=receipt,
                                  start_message=message, start_message_sha256=digest(message))
        write_json(self.directory / 'dispatch' / (node + '-' + str(packet['attempt']) + '.json'), active['dispatch'])
        self.save('native_actor_bound', node=node, actor_id=actor_id)
        return dict(packet=active['packet'], actor_id=actor_id, start_message=message)

    def accept(self, node, result, observation):
        self.require_node(node)
        active, _, packet = self.active_packet(node)
        dispatch = active['dispatch']
        if not dispatch or not isinstance(observation, dict) or \
                observation.get('runtime_actor_id') != dispatch['actor_id'] or \
                observation.get('delivery_message') != dispatch['start_message'] or not observation.get('completion'):
            raise HarnessError('native dispatch/delivery/completion must be observed by Master')
        self.check_inputs()
        self.assert_workspace(active['before'], active['writes'])
        validate_result(packet, result)
        status = result['status']
        if node != 'planner' and status in {'PASS', 'APPROVED'} and result['requested_decisions']:
            raise HarnessError('unresolved user choices require NEEDS_DECISION')
        if node == 'planner' and status == 'PASS':
            plan = result.get('plan', {})
            if not isinstance(plan, dict) or not plan.get('test_cases') or not plan.get('proposed_changes'):
                raise HarnessError('planner must return concrete tests and proposed changes')
        if node == 'implementer' and status == 'PASS':
            self.implementation_ready()
        if node == 'reviewer_qa' and status == 'APPROVED':
            if result.get('review', {}).get('status') != 'APPROVED' or result.get('qa', {}).get('status') != 'PASS':
                raise HarnessError('separate approved review and passed QA evidence are required')
            for name in (('docs',) if self.kind == 'design' else ('build', 'regression')):
                self.verify_check(name, current=True, phase='VERIFY', actor_id=packet['actor_id'])
        if node == 'knowledge_manager' and status == 'PASS':
            self.verify_check('finish', current=True, phase='KNOWLEDGE', actor_id=packet['actor_id'])
            if self.kind != 'design' and not safe_path(self.root, self.record_path()).is_file():
                raise HarnessError('task record missing')
            if self.kind == 'design' and any(not safe_path(self.root, p).is_file() for p in self.outputs()):
                raise HarnessError('approved design output missing')
        out = self.directory / 'results' / (node + '-' + str(packet['attempt']) + '.json')
        write_json(out, dict(result=result, master_observation=observation))
        self.state['history'].append(dict(node=node, status=status, result=out.relative_to(self.root).as_posix(),
                                         result_hash=file_hash(out), source_digest=packet['source_digest']))
        self.remember_outputs(active)
        self.state['active'] = None
        self.state['baseline'] = snapshot(self.root)
        if status in {'REWORK', 'FAIL'}:
            self._reject(node, 'role reported ' + status, result['findings'])
        elif status in {'NEEDS_DECISION', 'ENV_ERROR'}:
            self._pause('scope' if status == 'NEEDS_DECISION' else 'environment',
                        result['requested_decisions'] or [{'id': 'ENV', 'question': 'Resolve execution environment'}])
        else:
            self.state['phase'] = {'planner': 'CONTRACT', 'implementer': 'VERIFY',
                                   'reviewer_qa': 'KNOWLEDGE', 'knowledge_manager': 'USER_REVIEW'}[node]
            if self.kind == 'design' and node in {'reviewer_qa', 'knowledge_manager'}:
                self.state['phase'] = 'USER_REVIEW' if node == 'reviewer_qa' else 'VERIFY'
        self.save('result_accepted', node=node, actor_id=packet['actor_id'], status=status)
        return self.state

    def approve(self, approval_ref, write_paths=None, required_tests=None, decisions=None, knowledge_paths=None):
        self.require_phase('CONTRACT')
        self.check_inputs()
        if self.kind == 'design':
            if not approval_ref or write_paths or not knowledge_paths:
                raise HarnessError('design approval requires exact knowledge outputs and no implementation writes')
            for value in knowledge_paths:
                try:
                    knowledge_output_path(self.root, value)
                except HarnessError as error:
                    raise HarnessError('design outputs must be exact approved documentation paths') from error
            for ref in self.state['history'][-1:]:
                for question in read_json(safe_path(self.root, ref['result']))['result'].get('requested_decisions', []):
                    if not (decisions or {}).get(question['id']):
                        raise HarnessError('user choice missing: ' + question['id'])
            self.assert_workspace(self.state['baseline'], [])
            self.state.update(approval_ref=approval_ref, user_choices=decisions or {}, write_paths=[],
                              knowledge_paths=sorted({safe_path(self.root, p).relative_to(self.root).as_posix() for p in knowledge_paths}),
                              required_tests=required_tests or [], baseline=snapshot(self.root), phase='KNOWLEDGE', checks={})
            self.save('user_contract_approved', approval_ref=approval_ref)
            return self.state
        if not approval_ref or not write_paths or not required_tests:
            raise HarnessError('user approval, exact write paths and expected tests are required')
        decisions = decisions or {}
        for ref in self.state['history'][-1:]:
            result = read_json(safe_path(self.root, ref['result']))['result']
            for question in result.get('requested_decisions', []):
                if not decisions.get(question['id']):
                    raise HarnessError('user choice missing: ' + question['id'])
        paths = [safe_path(self.root, p).relative_to(self.root).as_posix() for p in write_paths]
        if any(p.casefold().startswith(('.cache/', 'tools/agent-harness/')) or Path(p).name.casefold() == 'agents.md' for p in paths):
            raise HarnessError('implementation writes cannot change harness controls/cache')
        outputs = knowledge_paths or [self.record_path(), 'docs/wiki/index.md', 'docs/wiki/sources.json']
        outputs = [knowledge_output_path(self.root, p) for p in outputs]
        if self.record_path() not in outputs:
            raise HarnessError('knowledge outputs must include task record')
        if set(paths) & set(outputs):
            raise HarnessError('implementation and knowledge writes must be disjoint')
        self.assert_workspace(self.state['baseline'], [])
        self.state.update(approval_ref=approval_ref, user_choices=decisions, write_paths=paths, knowledge_paths=outputs,
                          required_tests=required_tests, baseline=snapshot(self.root), phase='IMPLEMENT', checks={})
        self.state['prior_sources'] = {}
        for name in paths:
            source = safe_path(self.root, name)
            if source.is_file():
                prior = self.directory / 'snapshots/before' / name
                prior.parent.mkdir(parents=True, exist_ok=True)
                prior.write_bytes(source.read_bytes())
                self.state['prior_sources'][name] = dict(path=prior.relative_to(self.root).as_posix(), sha256=file_hash(prior))
        self.save('user_contract_approved', approval_ref=approval_ref)
        return self.state

    def _reject(self, node, reason, evidence):
        count = self.state['rejection_count'] + 1
        self.state['rejection_count'] = count
        self.state['rejections'].append(dict(number=count, node=node, reason=reason, evidence=evidence,
                                             at=datetime.now(timezone.utc).isoformat()))
        if node == 'knowledge_manager' and self.kind != 'design':
            self.state['checks'].pop('finish', None)
        else:
            self.state['checks'] = {}
            self.state.pop('red_confirmation', None)
        self.state.pop('reanalysis_note', None)
        self.state['reanalysis_required'] = count - self.state['rejection_window_start'] >= 2
        target = self.PHASES[node] if node in {'planner', 'knowledge_manager'} else 'IMPLEMENT'
        if self.kind == 'design' and node == 'reviewer_qa':
            target = 'KNOWLEDGE'
        self.state['phase'] = target
        if count >= self.state['rejection_limit']:
            self._pause('rejection-limit', [{'id': 'REWORK', 'question': 'Choose revision, replan, or stop after three rejections'}])
            report = self.directory / 'escalation.json'
            write_json(report, {'run_id': self.run_id, 'requirement': self.state['requirement'],
                                'rejection_count': count, 'attempts': self.state['rejections'],
                                'remaining_questions': self.state['pending_decisions'],
                                'master_action': 'Report attempts, remaining problem, suspected cause and options to user; no redispatch until user decision'})
            self.state['user_report'] = report.relative_to(self.root).as_posix()

    def reject(self, node, reason, evidence, completion_ref):
        self.require_node(node)
        active, _, _ = self.active_packet(node)
        if not active['dispatch'] or not reason or not evidence or not completion_ref:
            raise HarnessError('actor completion/stop reference, reason and correction evidence required')
        self.assert_workspace(active['before'], active['writes'])
        self.remember_outputs(active)
        self.state['active'] = None
        self.state['baseline'] = snapshot(self.root)
        self._reject(node, reason, evidence)
        self.save('master_rejected', node=node, reason=reason, completion_ref=completion_ref)
        return self.state

    def _pause(self, category, questions):
        self.state.update(resume_phase=self.state['phase'], phase='NEEDS_DECISION',
                          pause_category=category, pending_decisions=questions)

    def pause(self, reason, questions):
        if self.state.get('active') or self.state.get('phase') in {'DONE', 'NEEDS_DECISION'} or not reason or not questions:
            raise HarnessError('pause needs no active actor and explicit unresolved choices')
        self._pause(reason, questions)
        self.save('master_requested_user_decision')
        return self.state

    def resume(self, user_ref, decision, replan=False):
        self.require_phase('NEEDS_DECISION')
        if not user_ref or not decision:
            raise HarnessError('explicit user decision and source reference required')
        self.check_inputs()
        self.state.setdefault('decisions', []).append(dict(user_ref=user_ref, decision=decision,
                                                           questions=self.state['pending_decisions']))
        self.state['phase'] = 'PLANNER' if replan else self.state['resume_phase']
        if self.state['pause_category'] == 'scope' and not replan:
            # 새 선택은 재계약을 거친다. 이전 승인을 범위 변경에 재사용하지 않는다.
            self.state['phase'] = 'CONTRACT'
        if self.state['pause_category'] == 'rejection-limit':
            self.state['rejection_window_start'] = self.state['rejection_count']
            self.state['rejection_limit'] = self.state['rejection_count'] + 3
        self.state['baseline'] = snapshot(self.root)
        if replan or self.state['phase'] not in ({'KNOWLEDGE', 'VERIFY', 'USER_REVIEW'} if self.kind == 'design' else {'KNOWLEDGE', 'USER_REVIEW'}):
            self.state['checks'] = {}
            self.state.pop('red_confirmation', None)
        elif self.state['phase'] == 'KNOWLEDGE':
            self.state['checks'].pop('finish', None)
        if self.kind == 'design' and self.state.get('approval_ref'):
            # 문서 재작성 후 이전 검증을 보존하면 안 된다. 항상 기록 단계부터 다시 검사한다.
            if replan or self.state['phase'] in {'KNOWLEDGE', 'VERIFY', 'USER_REVIEW'} and any(
                    ref.get('knowledge_outputs_digest') != self.outputs_digest()
                    for ref in [read_json(safe_path(self.root, r['path'])) for r in self.state['checks'].values()]):
                self.state['checks'] = {}
                if not replan:
                    self.state['phase'] = 'KNOWLEDGE'
        if replan:
            self.state.pop('approval_ref', None)
        self.save('user_decision_received', user_ref=user_ref, replan=replan)
        return self.state

    def reanalyse(self, explanation):
        if self.state.get('phase') in {'DONE', 'NEEDS_DECISION'} or not explanation or self.state.get('active'):
            raise HarnessError('Master reanalysis requires an idle actionable task and explanation')
        self.state['reanalysis_note'] = explanation
        self.save('master_reanalysed')

    def run_check(self, name):
        phase = self.state.get('phase')
        node = {'IMPLEMENT': 'implementer', 'VERIFY': 'reviewer_qa', 'KNOWLEDGE': 'knowledge_manager'}.get(phase)
        allowed = {'IMPLEMENT': {'red', 'green', 'regression', 'build'}, 'VERIFY': {'build', 'regression'}, 'KNOWLEDGE': {'finish'}}
        if self.kind == 'design':
            allowed = {'VERIFY': {'docs'}, 'KNOWLEDGE': {'finish'}}
        if not node or name not in allowed[phase]:
            raise HarnessError('check not allowed in this phase')
        active, _, packet = self.active_packet(node)
        if not active['dispatch']:
            raise HarnessError('runtime actor is not bound')
        self.check_inputs()
        before = snapshot(self.root)
        self.assert_workspace(active['before'], active['writes'])
        config = read_json(self.root / 'tools/agent-harness/checks.json')
        command = [sys.executable, 'scripts/dev_tools.py', 'finish'] if name == 'finish' else config['commands'].get(name)
        if name == 'docs' and self.kind == 'design':
            self.verify_check('finish', current=True, phase='KNOWLEDGE')
            manifest = self.directory / 'design-manifest.json'
            write_json(manifest, dict(run_id=self.run_id, artifacts={p: file_hash(safe_path(self.root, p)) for p in self.outputs()},
                                      references={p: sha for p, sha in self.state['inputs'].items() if p not in self.outputs()}))
            command = [sys.executable, str(Path(__file__).with_name('check_design_docs.py')), '--root', str(self.root), '--manifest', str(manifest)]
        if not isinstance(command, list) or not command or not all(isinstance(arg, str) and arg for arg in command):
            raise HarnessError('DBMS check is not configured: ' + name)
        actor = packet['actor_id']
        if name in {'green', 'regression'}:
            self.verify_check('build', current=True, phase=phase, actor_id=actor)
        command = [sys.executable if arg == '{python}' else arg for arg in command]
        number = self.state.get('command_count', 0) + 1
        self.state['command_count'] = number
        folder = self.directory / 'commands' / (str(number) + '-' + name)
        folder.mkdir(parents=True, exist_ok=False)
        report_path = folder / 'test-report.json'
        work = self.directory / 'artifacts'
        work.mkdir(exist_ok=True)
        env = dict(os.environ, AGENT_HARNESS_RUN_ID=self.run_id, AGENT_HARNESS_NODE_ID=phase,
                   AGENT_HARNESS_RESULT_FILE=str(report_path), AGENT_HARNESS_WORK_DIR=str(work), PYTHONUTF8='1')
        started_at = datetime.now(timezone.utc).isoformat()
        started = time.monotonic()
        self.save('command_started', check=name)
        try:
            process = subprocess.run(command, cwd=self.root, env=env, capture_output=True, timeout=config.get('timeout_seconds', 180))
            code, stdout, stderr = process.returncode, process.stdout, process.stderr
        except OSError as error:
            code, stdout, stderr = 127, b'', str(error).encode('utf-8')
        except subprocess.TimeoutExpired as error:
            code, stdout, stderr = 124, error.stdout or b'', error.stderr or b''
        (folder / 'stdout.log').write_bytes(stdout)
        (folder / 'stderr.log').write_bytes(stderr)
        self.assert_workspace(before, [])
        receipt = dict(name=name, command=command, exit_code=code, source_digest=self.source_digest(before), passed=code == 0,
                       started_at=started_at, ended_at=datetime.now(timezone.utc).isoformat(), elapsed_ms=round((time.monotonic()-started)*1000, 3),
                       phase=phase, actor_id=actor, stdout=(folder / 'stdout.log').relative_to(self.root).as_posix(),
                       stderr=(folder / 'stderr.log').relative_to(self.root).as_posix(),
                       stdout_hash=file_hash(folder / 'stdout.log'), stderr_hash=file_hash(folder / 'stderr.log'), failed_tests=[])
        if name in {'finish', 'docs'}:
            receipt['knowledge_outputs_digest'] = digest({p: file_hash(safe_path(self.root, p))
                                                         if safe_path(self.root, p).is_file() else None
                                                         for p in self.outputs()})
        if name == 'docs':
            try:
                report = read_json(report_path)
                if report.get('run_id') != self.run_id or report.get('artifacts') != read_json(manifest)['artifacts'] or report.get('references') != read_json(manifest)['references'] or not report.get('passed'):
                    raise HarnessError('design report disagrees with approved artifacts/references')
                receipt.update(report=report_path.relative_to(self.root).as_posix(), report_hash=file_hash(report_path))
            except (OSError, ValueError, KeyError) as error:
                receipt.update(passed=False, report_error=str(error))
        if name not in {'build', 'finish', 'docs'}:
            if report_path.exists():
                try:
                    report = read_json(report_path)
                    receipt.update(report=report_path.relative_to(self.root).as_posix(), report_hash=file_hash(report_path))
                    receipt['test_titles'] = validate_report(report, self.state['required_tests'], self.run_id,
                                                            require_metrics=config.get('require_metrics', False))
                except (HarnessError, ValueError, TypeError, KeyError) as error:
                    receipt.update(passed=False, report_error=str(error))
                    report = locals().get('report', {})
                def failures(suite):
                    names = [spec['title'] for spec in suite.get('specs', []) if any(
                        r.get('status') == 'failed' for t in spec.get('tests', []) for r in t.get('results', []))]
                    return names + [n for child in suite.get('suites', []) for n in failures(child)]
                if isinstance(report, dict) and report.get('config', {}).get('metadata', {}).get('harnessRunId') == self.run_id and not report.get('errors'):
                    receipt['failed_tests'] = [n for s in report.get('suites', []) for n in failures(s)]
            else:
                receipt.update(passed=False, report_error='fresh report missing')
        path = folder / 'receipt.json'
        write_json(path, receipt)
        self.state['checks'][name] = dict(path=path.relative_to(self.root).as_posix(), sha256=file_hash(path))
        self.save('command_finished', check=name, passed=receipt['passed'], exit_code=code)
        return receipt

    def verify_check(self, name, current=False, phase=None, actor_id=None):
        reference = self.state['checks'].get(name)
        if not reference:
            raise HarnessError('missing check: ' + name)
        path = safe_path(self.root, reference['path'])
        if file_hash(path) != reference['sha256']:
            raise HarnessError('command receipt modified')
        value = read_json(path)
        if name != 'red' and (not value['passed'] or value['exit_code'] != 0):
            raise HarnessError('check failed: ' + name)
        if current and value['source_digest'] != self.source_digest():
            raise HarnessError('check is for an older source snapshot: ' + name)
        if current and name in {'finish', 'docs'}:
            current_outputs = digest({p: file_hash(safe_path(self.root, p))
                                      if safe_path(self.root, p).is_file() else None for p in self.outputs()})
            if value.get('knowledge_outputs_digest') != current_outputs:
                raise HarnessError('knowledge outputs changed after finish check')
        if (phase is not None and value['phase'] != phase) or (actor_id is not None and value['actor_id'] != actor_id):
            raise HarnessError('check belongs to another phase/actor')
        for key in ('stdout', 'stderr'):
            if file_hash(safe_path(self.root, value[key])) != value[key + '_hash']:
                raise HarnessError('command log modified')
        if value.get('report') and file_hash(safe_path(self.root, value['report'])) != value['report_hash']:
            raise HarnessError('test report modified')
        return value

    def confirm_red(self, explanation, failed_tests):
        self.require_phase('IMPLEMENT')
        _, _, packet = self.active_packet('implementer')
        receipt = self.verify_check('red', phase='IMPLEMENT', actor_id=packet['actor_id'])
        if receipt['exit_code'] == 0 or not explanation or not failed_tests or not set(failed_tests).issubset(receipt['failed_tests']):
            raise HarnessError('functional Red requires actual named failures and Master explanation')
        self.state['red_confirmation'] = dict(explanation=explanation, failed_tests=failed_tests)
        self.save('master_confirmed_functional_red')

    def implementation_ready(self):
        self.require_phase('IMPLEMENT')
        _, _, packet = self.active_packet('implementer')
        if not self.state.get('red_confirmation'):
            raise HarnessError('functional Red not confirmed')
        for name in ('green', 'regression', 'build'):
            self.verify_check(name, current=True, phase='IMPLEMENT', actor_id=packet['actor_id'])
        # 전이는 accept(implementer)에서만 한다. Master 직접 구현으로 단계를 우회하지 않는다.

    def finish(self, user_ref):
        self.require_phase('USER_REVIEW')
        if not user_ref:
            raise HarnessError('final user confirmation is required')
        self.check_inputs()
        self.assert_workspace(self.state['baseline'], [])
        for name in (('docs',) if self.kind == 'design' else ('build', 'regression')):
            self.verify_check(name, current=True, phase='VERIFY')
        self.verify_check('finish', current=True, phase='KNOWLEDGE')
        self.state.update(phase='DONE', completion_user_ref=user_ref)
        self.save('user_confirmed_complete', user_ref=user_ref)
        return self.state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', required=True)
    commands = parser.add_subparsers(dest='action', required=True)
    init = commands.add_parser('init'); init.add_argument('--requirement', required=True); init.add_argument('--inputs', nargs='+', required=True)
    init.add_argument('--kind', choices=('implementation', 'design'), default='implementation')
    prepare = commands.add_parser('prepare'); prepare.add_argument('node'); prepare.add_argument('--available-slots', type=int, required=True)
    dispatch = commands.add_parser('dispatch'); dispatch.add_argument('node'); dispatch.add_argument('--actor', required=True); dispatch.add_argument('--launch-message', required=True); dispatch.add_argument('--receipt', required=True)
    accept = commands.add_parser('accept'); accept.add_argument('node'); accept.add_argument('--result', required=True); accept.add_argument('--observation', required=True)
    approve = commands.add_parser('approve'); approve.add_argument('--contract', required=True)
    check = commands.add_parser('run-check'); check.add_argument('name')
    red = commands.add_parser('confirm-red'); red.add_argument('--explanation', required=True); red.add_argument('--tests', nargs='+', required=True)
    reject = commands.add_parser('reject'); reject.add_argument('node'); reject.add_argument('--reason', required=True); reject.add_argument('--evidence', nargs='+', required=True); reject.add_argument('--completion-ref', required=True)
    pause = commands.add_parser('pause'); pause.add_argument('--reason', required=True); pause.add_argument('--questions', required=True)
    resume = commands.add_parser('resume'); resume.add_argument('--user-ref', required=True); resume.add_argument('--decision', required=True); resume.add_argument('--replan', action='store_true')
    reanalyse = commands.add_parser('reanalyse'); reanalyse.add_argument('--explanation', required=True)
    verify = commands.add_parser('verify-packet'); verify.add_argument('node')
    finish = commands.add_parser('finish'); finish.add_argument('--user-ref', required=True)
    commands.add_parser('status')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    try:
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,80}', args.run):
            raise HarnessError('invalid run ID')
        # 같은 run의 CLI 갱신만 배타 처리한다. 다른 run의 명령은 병렬 실행 가능하다.
        with DirectoryLock(root / '.cache/agent-harness' / (args.run + '.mutation.lock')):
            h = Harness(root, args.run)
            if args.action == 'init': result = h.create(args.requirement, args.inputs, args.kind)
            elif args.action == 'prepare': result = h.prepare(args.node, args.available_slots)
            elif args.action == 'dispatch': result = h.dispatch(args.node, args.actor, Path(args.launch_message).read_text(encoding='utf-8'), read_json(args.receipt))
            elif args.action == 'accept': result = h.accept(args.node, read_json(args.result), read_json(args.observation))
            elif args.action == 'approve': result = h.approve(**read_json(args.contract))
            elif args.action == 'run-check': result = h.run_check(args.name)
            elif args.action == 'confirm-red': result = h.confirm_red(args.explanation, args.tests)
            elif args.action == 'reject': result = h.reject(args.node, args.reason, args.evidence, args.completion_ref)
            elif args.action == 'pause': result = h.pause(args.reason, read_json(args.questions))
            elif args.action == 'resume': result = h.resume(args.user_ref, args.decision, args.replan)
            elif args.action == 'reanalyse': result = h.reanalyse(args.explanation)
            elif args.action == 'verify-packet': result = h.active_packet(args.node)[2]
            elif args.action == 'finish': result = h.finish(args.user_ref)
            else: result = h.state
            print(json.dumps(result, ensure_ascii=True, indent=2))
            if args.action == 'run-check' and args.name != 'red' and not result['passed']:
                return 1
            return 0
    except (HarnessError, OSError, KeyError, json.JSONDecodeError) as error:
        print('HARNESS_ERROR: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
