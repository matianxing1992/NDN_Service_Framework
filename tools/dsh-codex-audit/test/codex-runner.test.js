import test from 'node:test';
import assert from 'node:assert/strict';
import { execute, parseResult, classifyFailure, resolveCodex } from '../lib/codex-runner.js';
import { fileURLToPath } from 'node:url';
const childFixture = fileURLToPath(new URL('./child-fixture.js', import.meta.url));

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

test('process adapter preserves literal stdin without shell evaluation', async () => {
  const input = '$(touch /tmp/should-never-exist) `echo secret`';
  const result = await execute('/bin/cat', [], { input, timeoutMs: 2000 });
  assert.equal(result.stdout, input, JSON.stringify(result));
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
