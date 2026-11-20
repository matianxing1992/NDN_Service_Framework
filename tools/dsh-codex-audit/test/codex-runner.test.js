import test from 'node:test';
import assert from 'node:assert/strict';
import { execute, parseResult, classifyFailure, resolveCodex, runCodex, readAuditModels } from '../lib/codex-runner.js';
import { fileURLToPath } from 'node:url';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { delimiter, join } from 'node:path';
const childFixture = fileURLToPath(new URL('./child-fixture.js', import.meta.url));

test('the audit catalog exposes listed Codex models and only their declared efforts', async () => {
  const root = await mkdtemp(join(tmpdir(), 'dsh-codex-catalog-'));
  try {
    const path = join(root, 'models_cache.json');
    const model = { slug: 'gpt-6.2-astra', visibility: 'list', default_reasoning_level: 'high',
      supported_reasoning_levels: [{ effort: 'medium' }, { effort: 'high' }, { effort: 'bogus' }] };
    await writeFile(path, JSON.stringify({ models: [model, { ...model, slug: 'hidden', visibility: 'hide' }] }));
    assert.deepEqual(await readAuditModels(root),
      [{ id: 'gpt-6.2-astra', efforts: ['medium', 'high'], defaultEffort: 'high' }]);
    await writeFile(path, 'partial cache');
    assert.equal((await readAuditModels(root))[0].id, 'gpt-6.2-sol');
  } finally { await rm(root, { recursive: true, force: true }); }
});

test('completed JSONL retains report, thread and token usage', () => {
  const result = parseResult([
    { type: 'thread.started', thread_id: 'thread-1' },
    { type: 'item.completed', item: { type: 'agent_message',
      text: JSON.stringify({ report: '审计报告', documentChanges: [] }) } },
    { type: 'turn.completed', usage: { output_tokens: 123 } },
  ].map(value => JSON.stringify(value)).join('\n'));
  assert.equal(result.threadId, 'thread-1');
  assert.equal(result.report, '审计报告');
  assert.equal(result.usage.output_tokens, 123);
});

test('zero exit or partial assistant text alone never qualifies as complete audit', () => {
  assert.throws(() => parseResult(''), /completed turn/);
  assert.throws(() => parseResult(JSON.stringify({ type: 'item.completed',
    item: { type: 'agent_message', text: 'partial' } })), /completed turn/);
});

test('unsupported model and invalid auth never become transient retries', () => {
  assert.equal(classifyFailure('401 unauthorized').code, 'CODEX_AUTH');
  assert.equal(classifyFailure('model not supported').code, 'CODEX_MODEL');
  assert.equal(classifyFailure('429 rate limit').transient, true);
});

test('missing explicit executable reports actionable error', async () => {
  await assert.rejects(resolveCodex('/no/such/codex'), error =>
    error.code === 'CODEX_NOT_FOUND' && error.remedy.includes('GUI'));
});

test('discovers Codex on an explicit PATH and rejects directory candidates', async () => {
  const root = await mkdtemp(join(tmpdir(), 'dsh-audit-cli-'));
  const prior = process.env.PATH;
  try {
    const name = process.platform === 'win32' ? 'codex.exe' : 'codex';
    const bin = join(root, 'bin');
    const executable = join(bin, name);
    await mkdir(join(root, name));
    await mkdir(bin);
    await writeFile(executable, '', { mode: 0o700 });
    process.env.PATH = [root, bin].join(delimiter);
    assert.equal(await resolveCodex(), executable);
    await assert.rejects(resolveCodex(join(root, name)), { code: 'CODEX_NOT_FOUND' });
  } finally {
    if (prior === undefined) delete process.env.PATH;
    else process.env.PATH = prior;
    await rm(root, { recursive: true, force: true });
  }
});

test('process adapter preserves literal stdin without shell evaluation', async () => {
  const input = '$(touch /tmp/should-never-exist) `echo secret`';
  const result = await execute(process.execPath, [childFixture, 'echo'], { input, timeoutMs: 2000 });
  assert.equal(result.stdout, input, JSON.stringify(result));
});

test('the runner passes each chosen effort to the Codex executable', async () => {
  const root = await mkdtemp(join(tmpdir(), 'dsh-audit-effort-'));
  const previousHome = process.env.CODEX_HOME;
  try {
    process.env.CODEX_HOME = root;
    const binary = join(root, 'codex-fixture.cjs');
    await writeFile(binary, `#!${process.execPath}
const args = process.argv.slice(2);
if (args.includes('--help')) {
  console.log('--ignore-user-config --ignore-rules --json --sandbox --skip-git-repo-check --output-schema');
  process.exit(0);
}
const report = JSON.stringify(args);
console.log(JSON.stringify({ type: 'item.completed', item: { type: 'agent_message',
  text: JSON.stringify({ report, documentChanges: [] }) } }));
console.log(JSON.stringify({ type: 'turn.completed' }));
`, { mode: 0o700 });
    for (const effort of ['medium', 'high', 'xhigh', 'ultra']) {
      const result = await runCodex({ record: {}, settings: {
        codexPath: binary, model: 'gpt-6.2-sol', effort, timeoutMs: 3000 },
      snapshot: { path: root, schemaPath: join(root, 'schema.json'), prompt: 'fixture' },
      signal: new AbortController().signal });
      const args = JSON.parse(result.report);
      assert.equal(args[args.indexOf('-m') + 1], 'gpt-6.2-sol');
      assert.deepEqual(args.filter(arg => arg.startsWith('model_reasoning_effort=')),
        [`model_reasoning_effort="${effort}"`]);
    }
  } finally {
    if (previousHome === undefined) delete process.env.CODEX_HOME;
    else process.env.CODEX_HOME = previousHome;
    await rm(root, { recursive: true, force: true });
  }
});

test('deadline terminates owned child and returns timeout error', async () => {
  await assert.rejects(execute(process.execPath,
    [childFixture, 'wait'], { timeoutMs: 50 }),
  error => error.code === 'CODEX_TIMEOUT');
});

test('abort terminates child without killing unrelated process', async () => {
  const controller = new AbortController();
  const pending = execute(process.execPath, [childFixture, 'wait'],
    { signal: controller.signal, timeoutMs: 2000 });
  controller.abort(new Error('test cancellation'));
  await assert.rejects(pending, /test cancellation/);
});
