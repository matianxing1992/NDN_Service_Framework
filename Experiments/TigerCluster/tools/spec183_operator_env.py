#!/usr/bin/env python3
"""Build or verify an isolated pinned operator venv; never modify a live prefix."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from runtime.yolo_submission import verify_operator_python


def install(prefix,requirements,base_python,seconds):
    prefix,requirements=Path(prefix),Path(requirements)
    if (not prefix.is_absolute() or '..' in prefix.parts
            or any(p.is_symlink() for p in (prefix,*prefix.parents))
            or not prefix.parent.is_dir() or not requirements.is_file()
            or requirements.is_symlink() or type(seconds) is not int or not 10<=seconds<=1800):
        raise ValueError('OPERATOR_ENV_INPUT')
    content=requirements.read_bytes()
    if len(content)>65536: raise ValueError('OPERATOR_REQUIREMENTS_SIZE')
    names=set()
    for line in content.decode('ascii').splitlines():
        line=line.split('#',1)[0].strip()
        if not line: continue
        if re.fullmatch(r'[A-Za-z0-9_.-]+==[A-Za-z0-9_.+!-]+(?:;\s*python_version < "3.9")?',line) is None:
            raise ValueError('OPERATOR_REQUIREMENTS_UNPINNED')
        name=line.split('==',1)[0]
        if name in names: raise ValueError('OPERATOR_REQUIREMENTS_DUPLICATE')
        names.add(name)
    if not {'numpy','jsonschema'}<=names: raise ValueError('OPERATOR_REQUIREMENTS_INCOMPLETE')
    digest='sha256:'+hashlib.sha256(content).hexdigest()
    interpreter=prefix/'bin/python'
    stamp=prefix/'operator-environment.json'
    deadline=time.monotonic()+seconds
    def remaining():
        left=deadline-time.monotonic()
        if left<=0: raise TimeoutError('OPERATOR_ENV_DEADLINE')
        return left
    if prefix.exists():
        if not stamp.is_file() or stamp.is_symlink():
            raise ValueError('OPERATOR_ENV_UNFINISHED_PREFIX')
        value=json.loads(stamp.read_text())
        if (value.get('schema')!='tiger-yolo-operator-environment-v1'
                or value.get('status')!='VERIFIED' or value.get('requirementsDigest')!=digest
                or value.get('interpreter')!=str(interpreter)
                or (prefix/'requirements-operator.txt').read_bytes()!=content
                or interpreter.is_symlink()):
            raise ValueError('OPERATOR_ENV_EXISTING_BINDING')
        verify_operator_python(prefix,seconds=remaining(),operator_python=interpreter)
        return dict(value,reused=True)
    prefix.mkdir(mode=0o700)
    (prefix/'requirements-operator.txt').write_bytes(content)
    commands=[[str(base_python),'-m','venv','--copies',str(prefix)],
        [str(interpreter),'-m','pip','install','--disable-pip-version-check','--no-cache-dir',
         '--only-binary=:all:','--timeout','30','--retries','1','--index-url','https://pypi.org/simple',
         '-r',str(prefix/'requirements-operator.txt')]]
    try:
        with (prefix/'operator-install.log').open('xb') as log:
            for command in commands:
                subprocess.run(command,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=remaining(),
                    env={'PATH':'/usr/bin:/bin','LC_ALL':'C','PIP_CONFIG_FILE':os.devnull,
                         'PYTHONNOUSERSITE':'1'})
        if interpreter.is_symlink(): raise ValueError('OPERATOR_ENV_INTERPRETER_SYMLINK')
        verify_operator_python(prefix,seconds=remaining(),operator_python=interpreter)
        observed=subprocess.run([str(interpreter),'-c',
            'import json,sys,platform,jsonschema,numpy,importlib.metadata as m; print(json.dumps(dict(python=sys.version,prefix=sys.prefix,platform=platform.platform(),numpyPath=numpy.__file__,jsonschemaPath=jsonschema.__file__,packages={d.metadata["Name"]:d.version for d in m.distributions()})))'],
            check=True,capture_output=True,timeout=remaining(),env={'PATH':'/usr/bin:/bin','LC_ALL':'C'})
        value=dict(schema='tiger-yolo-operator-environment-v1',status='VERIFIED',
            qualification='OPERATOR_DEPENDENCIES_ONLY',requirementsDigest=digest,
            interpreter=str(interpreter),commands=commands,observed=json.loads(observed.stdout))
        with stamp.open('x') as stream:
            json.dump(value,stream,sort_keys=True)
            stream.flush(); os.fsync(stream.fileno())
        stamp.chmod(0o444)
        return dict(value,reused=False)
    except BaseException as exc:
        (prefix/'operator-install-failure.json').write_text(json.dumps(dict(errorType=type(exc).__name__)))
        raise


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix',type=Path,required=True)
    parser.add_argument('--requirements',type=Path,default=ROOT/'requirements-operator.txt')
    parser.add_argument('--python',default='/usr/bin/python3')
    parser.add_argument('--seconds',type=int,default=300)
    args=parser.parse_args()
    print(json.dumps(install(args.prefix,args.requirements,args.python,args.seconds),sort_keys=True))


if __name__=='__main__': main()
