from __future__ import annotations
import subprocess,unittest
from unittest.mock import patch
from contract._support import load_impl
slurm=load_impl("adapters/slurm_apptainer")
class Runner:
    def __init__(self):self.n=0;self.commands=[]
    def __call__(self,cmd,**kw):
        self.commands.append(cmd)
        if cmd[0]=='scancel':return subprocess.CompletedProcess(cmd,0,stdout='',stderr='')
        self.n+=1;state='RUNNING' if self.n==1 else 'COMPLETED';exitcode='0:0'
        return subprocess.CompletedProcess(cmd,0,stdout=f'42|{state}|{exitcode}|00:00:01|itiger07|cpu=1|cpu=1\n',stderr='')
class SlurmLifecycleTest(unittest.TestCase):
    def test_bounded_wait_and_exact_cancel(self):
        runner=Runner();a=slurm.SlurmApptainerAdapter(runner=runner,sleeper=lambda _:None)
        self.assertTrue(a.wait('42',timeout=2,poll=0)['successful'])
        value=a.cancel('42',reason='test');self.assertEqual(value['reason'],'test');self.assertEqual(runner.commands[-1],['scancel','42'])

    def test_hung_accounting_is_bounded_by_remaining_wait(self):
        calls=[]
        def runner(command, **options):
            calls.append((command, options))
            raise subprocess.TimeoutExpired(command, options['timeout'])
        adapter=slurm.SlurmApptainerAdapter(runner=runner)
        with patch.object(slurm.time, 'monotonic', side_effect=[100, 100.25]):
            with self.assertRaisesRegex(slurm.SlurmAdapterError, '^SLURM_WAIT_TIMEOUT$'):
                adapter.wait('42', timeout=2)
        self.assertEqual(calls[0][0][0], 'sacct')
        self.assertEqual(calls[0][1]['timeout'], 1.75)
        self.assertEqual(len(calls), 1)

    def test_cancel_timeout_is_not_reported_as_cancellation(self):
        calls=[]
        def runner(command, **options):
            calls.append((command, options))
            raise subprocess.TimeoutExpired(command, options['timeout'])
        adapter=slurm.SlurmApptainerAdapter(runner=runner)
        with self.assertRaisesRegex(slurm.SlurmAdapterError, '^SLURM_COMMAND_TIMEOUT:scancel$'):
            adapter.cancel('42', timeout=0.5)
        self.assertEqual(calls[0][0], ['scancel', '42'])
        self.assertEqual(calls[0][1]['timeout'], 0.5)

    def test_poll_sleep_cannot_exceed_remaining_deadline(self):
        clock=[0.0]
        sleeps=[]
        calls=[]
        def runner(command, **options):
            calls.append(options['timeout'])
            clock[0] += 0.75
            return subprocess.CompletedProcess(command, 0,
                stdout='42|RUNNING|0:0|00:00:01|itiger07|cpu=1|cpu=1\n', stderr='')
        def sleep(seconds):
            sleeps.append(seconds)
            clock[0] += seconds
        adapter=slurm.SlurmApptainerAdapter(runner=runner, sleeper=sleep)
        with patch.object(slurm.time, 'monotonic', side_effect=lambda: clock[0]):
            with self.assertRaisesRegex(slurm.SlurmAdapterError, '^SLURM_WAIT_TIMEOUT$'):
                adapter.wait('42', timeout=1, poll=100)
        self.assertEqual(sleeps, [0.25])
        self.assertEqual(calls, [1])

    def test_invalid_deadlines_and_poll_are_rejected_without_scheduler_calls(self):
        runner=Runner()
        adapter=slurm.SlurmApptainerAdapter(runner=runner)
        for value in (None, 0, -1, True, float('inf'), float('nan')):
            with self.subTest(timeout=value):
                for action in (adapter.status, adapter.cancel, adapter.wait):
                    with self.assertRaisesRegex(slurm.SlurmAdapterError, 'SLURM_TIMEOUT_INVALID'):
                        action('42', timeout=value)
        for value in (-1, True, float('inf'), float('nan')):
            with self.subTest(poll=value):
                with self.assertRaisesRegex(slurm.SlurmAdapterError, 'SLURM_POLL_INVALID'):
                    adapter.wait('42', poll=value)
        self.assertEqual(runner.commands, [])
