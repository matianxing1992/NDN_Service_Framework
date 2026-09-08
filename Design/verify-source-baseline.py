#!/usr/bin/env python3
"""Reconstruct the documented source snapshot using Git and its tracked patch."""
from pathlib import Path
import hashlib, json, subprocess, tempfile, sys

root = Path(__file__).resolve().parent.parent
target='--target' in sys.argv
manifest = json.loads((root / ('Design/target-source-baseline.json' if target else 'Design/source-baseline.json')).read_text())
with tempfile.TemporaryDirectory(prefix='ndnsf-design-baseline-') as temporary:
    work = Path(temporary)
    for name in manifest['files']:
        data = subprocess.check_output(['git', 'show', manifest['baseline_commit'] + ':' + name], cwd=root)
        path = work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    patch = root / ('Design/evidence/target-source-baseline-worktree.patch' if target else 'Design/evidence/source-baseline-worktree.patch')
    if patch.stat().st_size:
        subprocess.run(['git', 'apply', '--unidiff-zero', '--check', str(patch)], cwd=work, check=True)
        subprocess.run(['git', 'apply', '--unidiff-zero', str(patch)], cwd=work, check=True)
    for name, entry in manifest['files'].items():
        assert hashlib.sha256((work / name).read_bytes()).hexdigest() == entry['sha256'], name
print(json.dumps({'result': 'PASS', 'files_verified': len(manifest['files']), 'baseline_commit': manifest['baseline_commit'], 'patch_sha256': hashlib.sha256(patch.read_bytes()).hexdigest()}))
