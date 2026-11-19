import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, readFile, mkdir, rm, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import * as repository from '../lib/repository.js';
import { ValidationQueue, configuration } from '../lib/queue.js';
import { run } from '../lib/process.js';

async function fixture(t) {
  const base = await mkdtemp(join(tmpdir(), 'dsh-validation-test-'));
  t.after(() => rm(base, { recursive: true, force: true }));
  const validationRoot = join(base, 'validation'), authorRoot = join(base, 'author');
  await mkdir(validationRoot);
  const git = args => repository.git(validationRoot, args);
  await git(['init', '-q']);
  await git(['config', 'user.name', 'Fixture']); await git(['config', 'user.email', 'fixture@localhost']);
  await writeFile(join(validationRoot, '.gitignore'), 'build/\n');
  await writeFile(join(validationRoot, 'source.txt'), 'initial\n');
  await git(['add', '.']); await git(['commit', '-qm', 'baseline']);
  await git(['worktree', 'add', '-qb', 'coding', authorRoot]);
  return { base, validationRoot, authorRoot, git };
}
const profile = { acceptance: 'The declared fixture must run and exit zero.',
  commands: [{ kind: 'test', file: process.execPath, args: ['-e', 'console.log("actual fixture executed")'], timeoutMs: 10000 }] };
async function queue(t, f, options = {}) {
  const delivered = [];
  const q = new ValidationQueue({ load: async () => null, save: async () => {},
    available: async () => {}, review: async () => ({ decision: 'accept', summary: 'reviewed', findings: [] }),
    deliver: async j => { delivered.push(j.id); }, ...options });
  await q.open();
  await q.configure({ authorRoot: f.authorRoot, validationRoot: f.validationRoot,
    enabled: true, profiles: { test: profile } });
  t.after(() => q.close());
  return { q, delivered };
}
async function settle(q) {
  for (let i = 0; i < 1000; ++i) {
    if (!q.running && !q.state.jobs.some(x => x.status === 'queued')) return;
    await delay(10);
  }
  throw new Error('queue did not settle');
}
async function submit(q, f, title = 'fixture', extra = {}) {
  return q.enqueue({ sessionId: 'author', cwd: f.authorRoot, profile: 'test', title, ...extra });
}

test('private candidate includes staged, unstaged, new files and deletion without changing author index/branch', async t => {
  const f = await fixture(t);
  await writeFile(join(f.authorRoot, 'source.txt'), 'staged\n');
  await repository.git(f.authorRoot, ['add', 'source.txt']);
  const index = await repository.git(f.authorRoot, ['write-tree']);
  await writeFile(join(f.authorRoot, 'source.txt'), 'unstaged\n');
  await writeFile(join(f.authorRoot, 'new.txt'), 'new\n');
  await writeFile(join(f.authorRoot, '.env.private'), 'never checkpoint this');
  await rm(join(f.authorRoot, '.gitignore'));
  const c = await repository.capture(f.authorRoot);
  assert.equal(await repository.git(f.authorRoot, ['write-tree']), index);
  assert.equal(await repository.git(f.authorRoot, ['branch', '--show-current']), 'coding');
  assert.equal(await repository.git(f.authorRoot, ['show', `${c.commit}:source.txt`]), 'unstaged');
  assert.equal(await repository.git(f.authorRoot, ['show', `${c.commit}:new.txt`]), 'new');
  const files = await repository.git(f.authorRoot, ['ls-tree', '-r', '--name-only', c.commit]);
  assert.ok(!files.includes('.env.private')); assert.ok(!files.includes('.gitignore'));
  await repository.materialize(f.validationRoot, c);
  assert.equal(await readFile(join(f.validationRoot, 'source.txt'), 'utf8'), 'unstaged\n');
  await writeFile(join(f.authorRoot, 'source.txt'), 'next version');
  assert.equal(await readFile(join(f.validationRoot, 'source.txt'), 'utf8'), 'unstaged\n');
});
test('dirty validation checkout and identical roots are rejected', async t => {
  const f = await fixture(t);
  await assert.rejects(repository.pair(f.authorRoot, f.authorRoot), /DISTINCT_WORKTREES/);
  await writeFile(join(f.validationRoot, 'source.txt'), 'unrelated edits');
  await assert.rejects(repository.clean(f.validationRoot), /DIRTY/);
});
test('cross-process build lock is exclusive and released by its owner', async t => {
  const f = await fixture(t), unlock = await repository.lock(f.validationRoot);
  await assert.rejects(repository.lock(f.authorRoot), /BUILD_LOCK_HELD/);
  await unlock(); await (await repository.lock(f.authorRoot))();
});
test('worker source read rejects escape, secrets and untracked files', async t => {
  const f = await fixture(t);
  await writeFile(join(f.validationRoot, 'extra.txt'), 'not a candidate input');
  await assert.rejects(repository.inspect(f.validationRoot, '../author/source.txt'), /REJECTED/);
  await assert.rejects(repository.inspect(f.validationRoot, '.git/config'), /GIT_FAILED/);
  await assert.rejects(repository.inspect(f.validationRoot, 'extra.txt'), /GIT_FAILED/);
  assert.equal(await repository.inspect(f.validationRoot, 'source.txt'), 'initial\n');
});
test('source symlink escape is rejected rather than copied', async t => {
  const f = await fixture(t);
  await symlink('/etc/hostname', join(f.authorRoot, 'escape'));
  await assert.rejects(repository.capture(f.authorRoot), /SYMLINK_(ESCAPES|ABSOLUTE)/);
});
test('absolute links into author sources are rejected because they would remain mutable in validation', async t => {
  const f = await fixture(t);
  await symlink(join(f.authorRoot, 'source.txt'), join(f.authorRoot, 'mutable-alias'));
  await assert.rejects(repository.capture(f.authorRoot), /SYMLINK_ABSOLUTE/);
});
test('real command result and immutable candidate are retained and delivered', async t => {
  const f = await fixture(t), { q, delivered } = await queue(t, f);
  const id = await submit(q, f); await settle(q);
  assert.equal(q.state.jobs[0].status, 'passed');
  assert.match(q.state.jobs[0].commands[0].stdout, /actual fixture executed/);
  assert.equal(q.state.jobs[0].applicability, 'exact-current-tree');
  assert.deepEqual(delivered, [id]);
});
test('failed command cannot be upgraded by an accepting AI report', async t => {
  const f = await fixture(t), { q } = await queue(t, f);
  q.state.config.profiles.test.commands[0].args = ['-e', 'process.exit(7)'];
  await submit(q, f); await settle(q);
  assert.equal(q.state.jobs[0].status, 'failed');
  assert.match(q.state.jobs[0].error, /exit=7/);
});
test('rejecting source review prevents any command from running', async t => {
  const f = await fixture(t); let calls = 0;
  const { q } = await queue(t, f, { review: async () => ({ decision: 'reject' }),
    command: async () => { calls++; } });
  await submit(q, f); await settle(q);
  assert.equal(calls, 0); assert.equal(q.state.jobs[0].status, 'failed');
});
test('one running plus one queued candidate; commands never overlap; source advance is historical', async t => {
  const f = await fixture(t); let unblock, entered;
  const gate = new Promise(r => { unblock = r; }), began = new Promise(r => { entered = r; });
  let active = 0, maximum = 0, count = 0;
  const { q } = await queue(t, f, { command: async () => {
    maximum = Math.max(maximum, ++active); entered(); if (++count === 1) await gate;
    active--; return { code: 0, stdout: '', stderr: '' };
  } });
  await submit(q, f, 'first'); await began;
  await writeFile(join(f.authorRoot, 'source.txt'), 'second');
  await submit(q, f, 'second');
  await assert.rejects(submit(q, f, 'third'), /BACKPRESSURE/);
  unblock(); await settle(q);
  assert.equal(maximum, 1); assert.equal(q.state.jobs.length, 2);
  assert.equal(q.state.jobs[0].applicability, 'historical-only');
  assert.equal(q.state.jobs[1].applicability, 'exact-current-tree');
});
test('failed dependency blocks queued child without running its commands', async t => {
  const f = await fixture(t); let unblock, entered;
  const gate = new Promise(r => { unblock = r; }), began = new Promise(r => { entered = r; });
  let calls = 0;
  const { q } = await queue(t, f, { command: async () => {
    calls++; entered(); await gate; return { code: 1, stdout: '', stderr: '' };
  } });
  const id = await submit(q, f); await began;
  await submit(q, f, 'dependent', { dependsOn: id }); unblock(); await settle(q);
  assert.equal(calls, 1); assert.equal(q.state.jobs[1].status, 'blocked');
});
test('reconfiguration cannot change an in-flight approved command list', async t => {
  const f = await fixture(t); let unblock, entered;
  const gate = new Promise(r => { unblock = r; }), began = new Promise(r => { entered = r; });
  const { q } = await queue(t, f, { command: async () => {
    entered(); await gate; return { code: 0, stdout: '', stderr: '' };
  } });
  await submit(q, f); await began;
  await assert.rejects(q.configure({ ...f, profiles: { test: profile } }), /DRAINED/);
  unblock(); await settle(q);
});
test('restart retains interrupted state, disables execution and does not replay commands', async () => {
  const q = new ValidationQueue({ load: async () => ({ version: 1, config: { enabled: true },
    jobs: [{ status: 'build', id: 'interrupted' }] }), save: async () => {} });
  await q.open(); assert.equal(q.state.config.enabled, false);
  assert.equal(q.state.jobs[0].status, 'interrupted'); await q.close();
});
test('source mutation during command cannot receive PASS', async t => {
  const f = await fixture(t);
  const { q } = await queue(t, f, { command: async () => {
    await writeFile(join(f.validationRoot, 'source.txt'), 'outside writer');
    return { code: 0, stdout: '', stderr: '' };
  } });
  await submit(q, f); await settle(q);
  assert.equal(q.state.jobs[0].status, 'error'); assert.match(q.state.jobs[0].error, /DIRTY/);
});
test('build output is hashed before tests and must remain identical afterward', async t => {
  const f = await fixture(t), { q } = await queue(t, f);
  q.state.config.profiles.test = { acceptance: 'build/run fixture', artifacts: ['build/output.txt'], commands: [
    { kind: 'build', file: process.execPath, args: ['-e', 'require("fs").mkdirSync("build",{recursive:true});require("fs").writeFileSync("build/output.txt","built")'] },
    { kind: 'test', file: process.execPath, args: ['-e', 'require("fs").writeFileSync("build/output.txt","changed")'] }
  ] };
  await submit(q, f); await settle(q);
  assert.equal(q.state.jobs[0].status, 'error'); assert.match(q.state.jobs[0].error, /ARTIFACT_CHANGED/);
});
test('profiles reject missing artifact identity and arbitrary phase ordering', () => {
  const c = { authorRoot: '/a', validationRoot: '/b', profiles: { x: {
    acceptance: 'fixture', commands: [{ kind: 'build', file: 'tool', args: [] }] } } };
  assert.throws(() => configuration(c), /ARTIFACT/);
  c.profiles.x.artifacts = ['build/a'];
  c.profiles.x.commands.unshift({ kind: 'test', file: 'tool', args: [] });
  assert.throws(() => configuration(c), /PRECEDE/);
});
test('prototype names cannot be submitted as approved profiles or create candidates', async t => {
  const f = await fixture(t), { q } = await queue(t, f);
  await assert.rejects(submit(q, f, 'invalid', { profile: 'toString' }), /UNKNOWN_APPROVED_PROFILE/);
  assert.equal(await f.git(['for-each-ref', '--format=%(refname)', 'refs/dsh-validation']), '');
});
test('timeout and output bounds stop owned commands', async () => {
  await assert.rejects(run(process.execPath, ['-e', 'setInterval(()=>{},1000)'], { timeoutMs: 100 }), /TIMEOUT/);
  await assert.rejects(run(process.execPath, ['-e', 'console.log("x".repeat(30000))'], { limit: 1000 }), /OUTPUT_LIMIT/);
});
test('cancelling an active job drains its process and releases the build lease', async t => {
  const f = await fixture(t), { q } = await queue(t, f);
  q.state.config.profiles.test.commands[0].args = ['-e',
    'require("fs").mkdirSync("build",{recursive:true});require("fs").writeFileSync("build/pid",String(process.pid));setInterval(()=>{},1000)'];
  const id = await submit(q, f);
  let pid;
  for (let i = 0; i < 200; i++) {
    pid = await readFile(join(f.validationRoot, 'build/pid'), 'utf8').catch(() => null);
    if (pid) break;
    await delay(10);
  }
  assert.ok(pid, 'command actually started');
  await q.cancel(id); await settle(q);
  assert.equal(q.state.jobs[0].status, 'cancelled');
  assert.throws(() => process.kill(Number(pid), 0), /ESRCH/);
  await (await repository.lock(f.validationRoot))();
});
test('storage failure stops dispatch instead of spinning the pump', async t => {
  const f = await fixture(t); let unblock, entered;
  const gate = new Promise(r => { unblock = r; }), began = new Promise(r => { entered = r; });
  let calls = 0;
  const { q } = await queue(t, f, { command: async () => {
    calls++; entered(); await gate; return { code: 0, stdout: '', stderr: '' };
  } });
  await submit(q, f); await began;
  q.save = async () => { throw new Error('STORAGE_FAILURE'); };
  unblock();
  while (q.running) await delay(10);
  assert.equal(q.state.config.enabled, false); assert.equal(calls, 1);
  assert.match(q.lastError, /STORAGE_FAILURE/);
});
