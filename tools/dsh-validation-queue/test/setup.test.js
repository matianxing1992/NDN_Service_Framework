import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { discoverSetup, setupProblem } from '../lib/setup.js';
import { git } from '../lib/repository.js';
import { ValidationQueue } from '../lib/queue.js';

async function fixture(t) {
  const root = await mkdtemp(join(tmpdir(), 'dsh-setup-test-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const validationRoot = join(root, 'original checkout'), authorRoot = join(root, 'coding');
  await mkdir(validationRoot);
  await git(validationRoot, ['init', '-q']);
  await git(validationRoot, ['config', 'user.name', 'Fixture']);
  await git(validationRoot, ['config', 'user.email', 'fixture@localhost']);
  await writeFile(join(validationRoot, 'source.txt'), 'baseline');
  await writeFile(join(validationRoot, '.gitignore'), 'build/\n');
  await git(validationRoot, ['add', '.']); await git(validationRoot, ['commit', '-qm', 'fixture']);
  await git(validationRoot, ['worktree', 'add', '-qb', 'coding', authorRoot]);
  return { validationRoot, authorRoot };
}

test('first-run discovery identifies actual roots, build trees, dirty state and resident sessions without changing source', async t => {
  const f = await fixture(t), before = await git(f.validationRoot, ['rev-parse', 'HEAD']);
  await mkdir(join(f.validationRoot, 'build/c4che'), { recursive: true });
  await writeFile(join(f.validationRoot, 'build/c4che/_cache.py'), '# fixture');
  await writeFile(join(f.validationRoot, 'source.txt'), 'pending user change');
  const status = await git(f.validationRoot, ['status', '--porcelain=v1']);
  const result = await discoverSetup(f.validationRoot, [{ id: 'active-author', cwd: f.validationRoot, status: 'running' }]);
  assert.equal(result.currentRoot, f.validationRoot);
  const original = result.worktrees.find(x => x.path === f.validationRoot);
  assert.equal(original.hasNormalBuild, true); assert.equal(original.dirty, true);
  assert.equal(original.residents[0].id, 'active-author');
  assert.equal(result.worktrees.find(x => x.path === f.authorRoot).dirty, false);
  assert.equal(await git(f.validationRoot, ['rev-parse', 'HEAD']), before);
  assert.equal(await git(f.validationRoot, ['status', '--porcelain=v1']), status);
});

test('first-run enable reports missing configuration; saved configuration enables only after real preflight', async t => {
  const f = await fixture(t);
  const q = new ValidationQueue({ load: async () => null, save: async () => {},
    available: async () => {}, review: async () => {}, deliver: async () => {} });
  await q.open(); t.after(() => q.close());
  await assert.rejects(q.enable(true), /CONFIGURATION_REQUIRED/);
  assert.equal(q.state.config, null);
  await q.configure({ ...f, enabled: false, profiles: { tiny: {
    acceptance: 'run the declared fixture', commands: [{ kind: 'test', file: '/usr/bin/true', args: [] }] } } });
  await writeFile(join(f.validationRoot, 'source.txt'), 'pending user change');
  await assert.rejects(q.enable(true), /VALIDATION_WORKTREE_DIRTY/);
  assert.equal(q.state.config.enabled, false);
  await writeFile(join(f.validationRoot, 'source.txt'), 'baseline');
  await q.enable(true);
  assert.equal(q.state.config.enabled, true);
  assert.equal(q.state.jobs.length, 0, 'turning on creates no background model or build');
});

test('preflight errors distinguish setup, occupied checkout and missing paths in Chinese', () => {
  assert.match(setupProblem(new Error('CONFIGURATION_REQUIRED')).message, /首次设置/);
  assert.match(setupProblem(new Error('VALIDATION_WORKSPACE_HAS_RESIDENT_AGENT: occupied')).message, /会话驻留/);
  const missing = Object.assign(new Error('missing path'), { code: 'ENOENT' });
  assert.match(setupProblem(missing).message, /目录不存在/);
});
