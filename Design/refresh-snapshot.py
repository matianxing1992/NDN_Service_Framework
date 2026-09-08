#!/usr/bin/env python3
"""Capture listed source bytes and provenance; run only after source review."""
from pathlib import Path
from datetime import datetime, timezone
import difflib, hashlib, io, json, subprocess, tarfile

root = Path(__file__).resolve().parent.parent
dest = root / 'Design/source-baseline.json'
manifest = json.loads(dest.read_text())
now = datetime.now(timezone.utc)
run = root / '.codex-tmp' / ('design-source-' + now.strftime('%Y%m%dT%H%M%S%fZ'))
run.mkdir()
head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
archive = run / 'reviewed-source.tar.gz'
patch_lines = []
with tarfile.open(archive, 'w:gz') as tar:
    for name in manifest['files']:
        data = (root / name).read_bytes()
        info = tarfile.TarInfo(name)
        info.size = len(data)
        tar.addfile(info, io.BytesIO(data))
        committed = subprocess.run(['git', 'show', head + ':' + name], cwd=root, capture_output=True)
        if committed.returncode != 0:
            raise RuntimeError('Snapshot requires a Git baseline for ' + name)
        if committed.stdout != data:
            patch_lines.extend(difflib.unified_diff(committed.stdout.decode().splitlines(True), data.decode().splitlines(True), fromfile='a/' + name, tofile='b/' + name, n=0))
        manifest['files'][name] = dict(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data), matches_head=committed.returncode == 0 and committed.stdout == data)
manifest.update(snapshot_utc=now.isoformat(), baseline_commit=head, local_source_archive=str(archive.relative_to(root)), archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
dest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
(root / 'Design/evidence/source-baseline-worktree.patch').write_text(''.join(patch_lines))
(root / 'Design/snapshot.tex').write_text('\\newcommand{\\SourceBaseline}{' + head[:12] + '}\n\\newcommand{\\SnapshotDate}{2026-09-07}\n')
print(manifest['snapshot_utc'], head, len(manifest['files']))
