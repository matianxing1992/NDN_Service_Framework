import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, mkdir, readFile, rm, symlink } from 'node:fs/promises';
import { join } from 'node:path';
import { execute } from '../lib/codex-runner.js';
import { createSnapshot, digest, checkSnapshotCurrent, describeBasis } from '../lib/snapshot.js';

test('snapshot includes dirty tracked, untracked and ignored active Spec without credentials or escaping links', async () => {
  const root = await mkdtemp('/tmp/dsh-audit-source-test-');
  let snapshot;
  try {
    const git = args => execute('git', ['-C', root, ...args]);
    await git(['init', '-q']);
    await writeFile(join(root, 'source.cpp'), 'old source');
    await writeFile(join(root, 'deleted.cpp'), 'retired source');
    await writeFile(join(root, 'staged.cpp'), 'old staged source');
    await writeFile(join(root, '.gitignore'), 'specs/\n');
    await git(['add', 'source.cpp', 'deleted.cpp', 'staged.cpp', '.gitignore']);
    await git(['-c', 'user.name=Audit Test', '-c', 'user.email=audit@example.invalid',
      '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture']);
    const baseline = await createSnapshot({ cwd: root, seq: 41, baselineOnly: true });
    assert.equal(baseline.path, undefined, 'activation capture retains hashes without copying a scratch tree');
    await writeFile(join(root, 'source.cpp'), 'intermediate staged source');
    await writeFile(join(root, 'staged.cpp'), 'staged-only change');
    await git(['add', 'source.cpp', 'staged.cpp']);
    await writeFile(join(root, 'source.cpp'), 'uncommitted source');
    await rm(join(root, 'deleted.cpp'));
    await writeFile(join(root, 'new.cpp'), 'untracked source');
    await mkdir(join(root, 'specs/224'), { recursive: true });
    await writeFile(join(root, 'specs/224/spec.md'), 'actual ignored active Spec');
    await mkdir(join(root, '.specify'));
    await writeFile(join(root, '.specify/feature.json'), JSON.stringify({ feature_directory: 'specs/224' }));
    await writeFile(join(root, '.env'), 'must not copy');
    await mkdir(join(root, 'credentials'));
    await writeFile(join(root, 'credentials/auth.json'), 'must not copy');
    await symlink('/etc/hosts', join(root, 'outside.txt'));
    snapshot = await createSnapshot({ cwd: root, conversation: [
      JSON.stringify({ seq: 41, time: Date.now(), type: 'user/message', data: 'historical task' }),
      JSON.stringify({ seq: 42, time: Date.now(), type: 'tool/result', data: 'current work' }),
    ].join('\n'), seq: 42, period: { id: 'enabled', baseline }, trigger: 'scheduled',
      prompt: 'audit only', signal: new AbortController().signal });
    assert.equal(await readFile(join(snapshot.path, 'source.cpp'), 'utf8'), 'uncommitted source');
    assert.equal(await readFile(join(snapshot.path, 'new.cpp'), 'utf8'), 'untracked source');
    assert.equal(await readFile(join(snapshot.path, 'specs/224/spec.md'), 'utf8'), 'actual ignored active Spec');
    assert.equal(snapshot.hashes['source.cpp'], digest('uncommitted source'));
    assert.equal(snapshot.hashes['.env'], undefined);
    assert.equal(snapshot.hashes['credentials/auth.json'], undefined);
    assert.equal(snapshot.hashes['outside.txt'], undefined);
    assert.equal(snapshot.basis.seq, 42);
    assert.equal(snapshot.basis.sourceMode, 'working-tree');
    assert.equal(snapshot.basis.scope.kind, 'since-enable');
    const focused = await readFile(join(snapshot.path, '__audit_input__/focus-0000.jsonl'), 'utf8');
    assert.ok(focused.includes('current work') && !focused.includes('historical task'));
    const changes = JSON.parse(await readFile(join(snapshot.path, '__audit_input__/source-changes.json'), 'utf8'));
    assert.equal(changes.changes.find(change => change.path === 'deleted.cpp').kind, 'deleted');
    assert.match(snapshot.prompt, /快照.*未提供的历史证据不等于/);
    assert.equal(await readFile(join(snapshot.path, 'staged.cpp'), 'utf8'), 'staged-only change');
    assert.equal(snapshot.hashes['deleted.cpp'], undefined);
    assert.deepEqual(snapshot.basis.workingTreeChanges.find(r => r.path === 'source.cpp'),
      { path: 'source.cpp', index: 'M', worktree: 'M' });
    assert.equal(snapshot.basis.workingTreeChanges.find(r => r.path === 'deleted.cpp').worktree, 'D');
    assert.match(snapshot.prompt, /HEAD=.*仅为基线/);
    assert.match(snapshot.prompt, /不能因为 HEAD 未变而忽略未提交修改/);
    assert.equal((await checkSnapshotCurrent(snapshot)).stale, false);
    await writeFile(join(root, 'source.cpp'), 'DSH work continued');
    const check = await checkSnapshotCurrent(snapshot);
    assert.equal(check.stale, true);
    assert.deepEqual(check.changedFiles, ['source.cpp']);
    await writeFile(join(root, 'added-consumer.cpp'), 'new code after audit began');
    assert.equal((await checkSnapshotCurrent(snapshot)).changedCount, 2);
    const next = await createSnapshot({ cwd: root, conversation: '', seq: 43, prompt: 'audit' });
    try {
      assert.equal(next.basis.head, snapshot.basis.head);
      assert.equal(await readFile(join(next.path, 'source.cpp'), 'utf8'), 'DSH work continued');
      assert.notEqual(next.basis.manifestHash, snapshot.basis.manifestHash);
    } finally { await next.dispose(); }
  } finally {
    await snapshot?.dispose();
    await rm(root, { recursive: true, force: true });
  }
});

test('legacy history does not invent an uncommitted-file count', () => {
  assert.match(describeBasis({ head: 'old' }), /旧记录未保存工作区改动清单/);
});
