"""Interpreter wiring and bootstrap ownership; live install has separate evidence."""
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

from test_yolo_operator_profile import profile_fixture,ROOT
from test_yolo_submit import submit_module
from test_yolo_allocated_runner import allocated
from tools import spec183_operator_env as setup
from runtime.yolo_profile import load_operator_profile,effective_profile_document


def test_profile_resolves_physical_interpreter_without_changing_behavior(tmp_path):
    path,value=profile_fixture(tmp_path)
    old=effective_profile_document(load_operator_profile(path,stage='inputs')['profile'])
    value['runtime']['operatorPython']='operator-env/bin/python'
    path.write_text(json.dumps(value))
    loaded=load_operator_profile(path,stage='inputs')['profile']
    assert loaded['runtime']['operatorPython']==str(tmp_path/'operator-env/bin/python')
    assert effective_profile_document(loaded)==old


def test_operator_interpreter_symlink_is_not_a_profile_locator(tmp_path):
    path,value=profile_fixture(tmp_path)
    link=tmp_path/'python-alias'
    link.symlink_to('/usr/bin/python3')
    value['runtime']['operatorPython']=str(link)
    path.write_text(json.dumps(value))
    with pytest.raises(ValueError,match='PROFILE_SYMLINK'):
        load_operator_profile(path,stage='inputs')


@pytest.mark.parametrize('action,reconcile,use_remote',[('submit',False,True),('collect',False,False),('collect',True,True)])
def test_frozen_entry_uses_configured_interpreter_and_rejects_profile_change(tmp_path,monkeypatch,action,reconcile,use_remote):
    from runtime import yolo_bundle
    module=submit_module()
    profile=tmp_path/'profile.json'
    raw={'runtime':{'operatorPython':str(tmp_path/'operator-env/bin/python')}}
    profile.write_text(json.dumps(raw))
    prepared=dict(bundle=str(tmp_path/'bundle'),profileDigest=module._json_digest(raw),
                  harnessManifestSha256='sha256:'+'a'*64)
    args=SimpleNamespace(profile=profile,run_id='env-run',output=tmp_path/'runs',case='single-node-gpu',reconcile=reconcile)
    monkeypatch.setattr(yolo_bundle,'verify_harness',lambda *a,**k: None)
    commands=[]
    monkeypatch.setattr(subprocess,'run',lambda command,**kwargs: commands.append(command) or SimpleNamespace(returncode=78))
    assert module._enter_frozen(args,prepared,action)==78
    assert commands[0][0]==(raw['runtime']['operatorPython'] if use_remote else sys.executable)
    raw['runtime']['operatorPython']=str(tmp_path/'other')
    profile.write_text(json.dumps(raw))
    with pytest.raises(module.ClosureError,match='FROZEN_PROFILE_CHANGED'):
        module._enter_frozen(args,prepared,action)
    assert len(commands)==1


def test_sbatch_and_srun_use_the_same_selected_operator(allocated,monkeypatch):
    from runtime import worker
    module,args,prepared,profile,journal,root=allocated
    selected=str(root.parent/'operator-env/bin/python')
    profile['runtime']={'operatorPython':selected}
    profile['cluster'].update(account='devs',memoryGiB=4)
    wrapper=Path(prepared['bundle'])/'jobs/yolo/run.sbatch'
    wrapper.parent.mkdir(parents=True)
    wrapper.write_bytes((ROOT/'jobs/yolo/run.sbatch').read_bytes())
    wrapper.chmod(0o555)
    command=module._submission_command(args.profile,profile,args,prepared,journal.get(args.run_id)['submissionKey'])
    assert command[-1]==selected
    monkeypatch.setattr(module,'_gate_receipt',lambda *a,**k: None)
    monkeypatch.setattr(module,'_collect',lambda a: 78)
    def finite(name,command,log,cleanup,**kwargs):
        assert command[command.index('-B')-1]==selected
        cleanup.append(dict(name=name,kind='finite',reaped=True,forced=False,exitCode=0))
    monkeypatch.setattr(worker,'run_finite_application',finite)
    assert module._run(args)==78


def test_frozen_process_checks_its_dependencies_before_execution(tmp_path,monkeypatch):
    from runtime import yolo_bundle,yolo_submission
    module=submit_module()
    bundle=tmp_path/'bundle'
    raw={'runtime':{}}
    profile=tmp_path/'profile.json'
    profile.write_text(json.dumps(raw))
    prepared=dict(bundle=str(bundle),profileDigest=module._json_digest(raw),harnessManifestSha256='sha256:'+'a'*64)
    monkeypatch.setattr(module,'BUNDLE',bundle)
    monkeypatch.setattr(yolo_bundle,'verify_harness',lambda *a,**k: None)
    def reject(*a,**k): raise ValueError('missing pin')
    monkeypatch.setattr(yolo_submission,'verify_operator_python',reject)
    with pytest.raises(module.ClosureError,match='FROZEN_OPERATOR_DEPENDENCIES'):
        module._enter_frozen(SimpleNamespace(profile=profile),prepared,'collect')


def test_real_batch_shell_uses_bound_interpreter_argument(tmp_path):
    executable=tmp_path/'python-fixture'
    executable.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
    executable.chmod(0o700)
    command=['bash',str(ROOT/'jobs/yolo/run.sbatch'),str(tmp_path/'bundle'),
             str(tmp_path/'missing-profile.json'),str(tmp_path/'runs'),'env-run','single-node-gpu',str(executable)]
    result=subprocess.run(command,check=True,capture_output=True,text=True,
                          env={'PATH':'/usr/bin:/bin','SLURM_JOB_ID':'123'},timeout=5)
    assert result.stdout.splitlines()[:3]==['-B',str(tmp_path/'bundle/jobs/yolo/submit.py'),'run']
    # Wrapper does not parse mutable profile data to choose an executable.
    assert not (tmp_path/'missing-profile.json').exists()


def test_setup_installs_once_reuses_pins_and_never_clears_existing_prefix(tmp_path,monkeypatch):
    prefix=tmp_path/'env'
    commands=[]
    checks=[]
    def run(command,**kwargs):
        commands.append(command)
        assert kwargs['check'] and 0<kwargs['timeout']<=30
        if command[1:3]==['-m','venv']:
            assert '--copies' in command and '--clear' not in command
            (prefix/'bin').mkdir()
            (prefix/'bin/python').write_bytes(b'fixture')
        elif command[1:3]==['-m','pip']:
            assert '--only-binary=:all:' in command and '--index-url' in command
        else:
            return SimpleNamespace(stdout=b'{"python":"fixture","packages":{}}')
        return SimpleNamespace(returncode=0)
    monkeypatch.setattr(setup.subprocess,'run',run)
    monkeypatch.setattr(setup,'verify_operator_python',lambda bundle,**kwargs: checks.append(kwargs))
    first=setup.install(prefix,ROOT/'requirements-operator.txt','/usr/bin/python3',30)
    assert not first['reused'] and len(commands)==3
    second=setup.install(prefix,ROOT/'requirements-operator.txt','/usr/bin/python3',30)
    assert second['reused'] and len(commands)==3 and len(checks)==2
    assert all(c['operator_python']==prefix/'bin/python' for c in checks)


def test_partial_or_unpinned_install_is_rejected_before_commands(tmp_path,monkeypatch):
    prefix=tmp_path/'partial'
    prefix.mkdir()
    monkeypatch.setattr(setup.subprocess,'run',lambda *a,**k: pytest.fail('unexpected install'))
    with pytest.raises(ValueError,match='UNFINISHED_PREFIX'):
        setup.install(prefix,ROOT/'requirements-operator.txt','/usr/bin/python3',30)
    bad=tmp_path/'bad.txt'
    bad.write_text('numpy>=1.24\njsonschema==4.21.1\n')
    with pytest.raises(ValueError,match='UNPINNED'):
        setup.install(tmp_path/'new',bad,'/usr/bin/python3',30)
    assert not (tmp_path/'new').exists()
