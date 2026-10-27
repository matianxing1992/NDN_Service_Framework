"""Controlled scheduler responses; no native readiness or Tiger qualification."""
import copy
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

from contract._support import load_impl

slurm = load_impl('adapters/slurm_apptainer')


class DeploymentSchedulerTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.script = Path(self.directory.name) / 'allocation.sbatch'
        self.script.write_text('#!/bin/bash\nexit 0\n')
        self.script.chmod(0o600)
        self.config = dict(schema='ndnsf-di-deployment-v1', nodes=[dict(host='node-a')],
                           platform=dict(kind='slurm-apptainer', slurm=dict(
                               nodes=1, partition='test', account='test', wallTime='00:01:00',
                               tasksPerNode=1, cpusPerTask=2, memory='2G',
                               gpu=dict(type='test', count=1))))
        self.calls = []
        self.queue = ''
        self.account = ''
        self.never_allocated = ''
        self.adapter = slurm.SlurmApptainerAdapter(runner=self.runner)
        self.reference = self.adapter.prepare_deployment(
            self.config, self.script, boot_id='a' * 32, release_sha256='b' * 64)

    def runner(self, command, **options):
        self.calls.append((command, options))
        output = {'sbatch': '42\n', 'squeue': self.queue, 'sacct': self.account, 'scancel': ''}[command[0]]
        if command[0] == 'sacct' and 'JobIDRaw,UID,JobName%128,Start%32' in command[-1]:
            output = self.never_allocated
        return subprocess.CompletedProcess(command, 0, stdout=output, stderr='')

    def queue_row(self, job='42', *, uid=None, comment=None):
        uid = os.getuid() if uid is None else uid
        comment = self.adapter._deployment_comment(self.reference) if comment is None else comment
        return f'{job}|{uid}|{comment}|RUNNING|node-a\n'

    def account_row(self, job='42', state='COMPLETED', *, comment=None, name=None,
                    submitted=None, uid=None):
        comment = self.adapter._deployment_comment(self.reference) if comment is None else comment
        name = self.reference['job_name'] if name is None else name
        uid = os.getuid() if uid is None else uid
        submitted = (time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime())
                     if submitted is None else submitted)
        return f'{job}|{uid}|{comment}|{state}|0:0|node-a|{name}|{submitted}\n'

    def test_submit_uses_verified_snapshot_even_if_original_file_changes(self):
        original = self.script.read_text()
        def runner(command, **options):
            self.script.write_text('different script after verification\n')
            return self.runner(command, **options)
        self.adapter.runner = runner
        submitted = self.adapter.submit_deployment(self.config, self.reference, timeout=1)
        self.assertEqual(submitted['job_id'], '42')
        command, options = self.calls[0]
        self.assertEqual(options['input'], original)
        self.assertEqual(options['encoding'], 'utf-8')
        self.assertNotIn(str(self.script), command)
        self.assertIn('--nodelist=node-a', command)
        self.assertIn('--gres=gpu:test:1', command)
        self.assertIn('--export=NONE', command)
        self.assertEqual(options['timeout'], 1)
        with self.assertRaisesRegex(slurm.SlurmAdapterError, 'ALREADY_SUBMITTED'):
            self.adapter.submit_deployment(self.config, submitted)
        self.assertEqual(len(self.calls), 1)

    def test_changed_script_or_configuration_cannot_be_submitted(self):
        changed = copy.deepcopy(self.config)
        changed['platform']['slurm']['memory'] = '4G'
        with self.assertRaisesRegex(slurm.SlurmAdapterError, 'CONFIGURATION_CHANGED'):
            self.adapter.submit_deployment(changed, self.reference)
        self.script.write_text('changed')
        with self.assertRaisesRegex(slurm.SlurmAdapterError, 'SCRIPT_CHANGED'):
            self.adapter.submit_deployment(self.config, self.reference)
        self.assertEqual(self.calls, [])

    def test_unknown_submission_finds_one_bound_job_without_resubmit(self):
        self.queue = self.queue_row()
        value = self.adapter.deployment_status(self.reference)
        self.assertEqual(value['reference']['job_id'], '42')
        self.assertTrue(value['live_verified'])
        self.assertEqual([c[0][0] for c in self.calls], ['squeue', 'sacct'])

    def test_no_match_is_unknown_not_absent_or_zero(self):
        value = self.adapter.deployment_status(self.reference)
        self.assertEqual(value['status'], 'UNKNOWN')
        self.assertFalse(value['terminal'])
        self.assertEqual(value['actor_cleanup'], 'UNVERIFIED')

    def test_cancelled_pending_job_requires_exact_never_allocated_proof(self):
        self.account = self.account_row(state='CANCELLED').replace('|node-a|', '|None assigned|')
        fields = ['42', str(os.getuid()), self.reference['job_name'],
                  'None', 'None assigned', '0', '0', '0']
        self.never_allocated = '|'.join(fields) + '\n'
        result = self.adapter.deployment_status(self.reference)
        self.assertTrue(result['never_allocated'])
        self.assertEqual(result['never_allocated_evidence']['start'], 'None')
        self.assertEqual(result['actor_cleanup'], 'UNVERIFIED')
        self.assertIn('--duplicates', self.calls[-1][0])
        prior = fields.copy()
        prior[3:] = ['2026-10-25T23:03:57', 'node-a', '2', '12', '122']
        self.never_allocated = '|'.join(prior) + '\n' + '|'.join(fields) + '\n'
        self.assertFalse(self.adapter.deployment_status(self.reference).get('never_allocated'))
        for index, replacement in enumerate(('99', str(os.getuid() + 1), 'foreign',
                                             '2026-10-25T22:00:00', 'node-a', '1', '1', '1')):
            changed = fields.copy()
            changed[index] = replacement
            with self.subTest(index=index):
                self.never_allocated = '|'.join(changed) + '\n'
                self.assertFalse(self.adapter.deployment_status(self.reference).get('never_allocated'))
        for invalid in ('', '|'.join(fields[:-1]), '|'.join(fields) + '\n' + '|'.join(fields)):
            self.never_allocated = invalid
            self.assertFalse(self.adapter.deployment_status(self.reference).get('never_allocated'))
        self.never_allocated = '|'.join(fields) + '\n'
        self.queue = self.queue_row()
        self.assertFalse(self.adapter.deployment_status(self.reference).get('never_allocated'))

    def test_ambiguous_binding_is_not_adopted(self):
        self.queue = self.queue_row() + self.queue_row('43')
        with self.assertRaisesRegex(slurm.SlurmAdapterError, 'BINDING_AMBIGUOUS'):
            self.adapter.deployment_status(self.reference)

    def test_live_queue_overrides_terminal_accounting(self):
        self.queue = self.queue_row()
        self.account = self.account_row()
        value = self.adapter.deployment_status(self.reference)
        self.assertEqual(value['state'], 'RUNNING')
        self.assertFalse(value['terminal'])

    def test_empty_account_comment_does_not_override_verified_live_queue(self):
        self.reference['job_id'] = '42'
        self.queue = self.queue_row()
        self.account = self.account_row(comment='')
        value = self.adapter.deployment_status(self.reference)
        self.assertTrue(value['live_verified'])
        self.assertEqual(value['state'], 'RUNNING')

    def test_empty_account_comment_reconciles_exact_boot_and_submission(self):
        self.account = self.account_row(comment='')
        value = self.adapter.deployment_status(self.reference)
        self.assertTrue(value['terminal'])
        self.assertEqual(value['reference']['job_id'], '42')
        self.assertFalse(value['live_verified'])
        self.assertEqual([call[0][0] for call in self.calls], ['squeue', 'sacct'])

    def test_empty_account_comment_rejects_reused_or_foreign_job(self):
        self.reference['job_id'] = '42'
        stale = time.strftime('%Y-%m-%dT%H:%M:%S',
                              time.localtime(self.reference['submitted_after'] - 120))
        future = time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime(time.time() + 3600))
        for changes in ({'name': 'other-boot'}, {'uid': os.getuid() + 1},
                        {'submitted': stale}, {'submitted': future}):
            with self.subTest(changes=changes):
                self.account = self.account_row(comment='', **changes)
                with self.assertRaisesRegex(slurm.SlurmAdapterError, 'OWNERSHIP_MISMATCH'):
                    self.adapter.deployment_status(self.reference)

    def test_empty_account_comment_rejects_malformed_submit_time(self):
        self.reference['job_id'] = '42'
        self.account = self.account_row(comment='', submitted='Unknown')
        with self.assertRaisesRegex(slurm.SlurmAdapterError, 'QUERY_INVALID'):
            self.adapter.deployment_status(self.reference)

    def test_nonempty_wrong_account_comment_remains_rejected(self):
        self.reference['job_id'] = '42'
        self.account = self.account_row(comment='different-config')
        with self.assertRaisesRegex(slurm.SlurmAdapterError, 'OWNERSHIP_MISMATCH'):
            self.adapter.deployment_status(self.reference)

    def test_accounting_only_never_authorizes_cancellation(self):
        self.reference['job_id'] = '42'
        self.account = self.account_row(state='RUNNING', comment='')
        value = self.adapter.cancel_deployment(self.reference)
        self.assertEqual(value['status'], 'UNKNOWN')
        self.assertNotIn('scancel', [call[0][0] for call in self.calls])

    def test_cancel_requires_current_queue_identity(self):
        self.reference['job_id'] = '42'
        self.account = self.account_row(state='RUNNING')
        value = self.adapter.cancel_deployment(self.reference)
        self.assertEqual(value['status'], 'UNKNOWN')
        self.assertNotIn('scancel', [c[0][0] for c in self.calls])

    def test_verified_cancel_does_not_claim_cleanup(self):
        self.queue = self.queue_row()
        self.script.unlink()  # Closing a persisted job does not require its original inputs.
        value = self.adapter.cancel_deployment(self.reference)
        self.assertEqual(value['status'], 'CANCEL_REQUESTED')
        self.assertEqual(value['actor_cleanup'], 'UNVERIFIED')
        self.assertFalse(value['terminal'])
        self.assertEqual(self.calls[-1][0], ['scancel', '42'])

    def test_foreign_or_reused_numeric_job_is_never_signaled(self):
        self.reference['job_id'] = '42'
        for row in (self.queue_row(uid=os.getuid() + 1), self.queue_row(comment='other-boot')):
            with self.subTest(row=row):
                self.calls.clear()
                self.queue = row
                with self.assertRaisesRegex(slurm.SlurmAdapterError, 'OWNERSHIP_MISMATCH'):
                    self.adapter.cancel_deployment(self.reference)
                self.assertNotIn('scancel', [c[0][0] for c in self.calls])

    def test_foreign_reference_is_rejected_before_query(self):
        self.reference['owner_uid'] += 1
        with self.assertRaisesRegex(slurm.SlurmAdapterError, 'OWNER_MISMATCH'):
            self.adapter.deployment_status(self.reference)
        self.assertEqual(self.calls, [])

    def test_scheduler_terminal_is_not_native_or_zero(self):
        self.account = self.account_row()
        value = self.adapter.cancel_deployment(self.reference)
        self.assertTrue(value['terminal'])
        self.assertEqual(value['actor_cleanup'], 'UNVERIFIED')
        self.assertNotIn('scancel', [c[0][0] for c in self.calls])
