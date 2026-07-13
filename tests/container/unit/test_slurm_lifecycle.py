from __future__ import annotations
import subprocess,unittest
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
