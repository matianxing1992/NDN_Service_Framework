import test from 'node:test';
import assert from 'node:assert/strict';
import { assess } from '../lib/worker.js';

const candidate = { commit: 'frozen', tree: 'tree', head: 'base' };
function harness({ verdict = true } = {}) {
  const calls = {}, registered = new Map(), observers = new Set();
  const parent = { ctx: {}, session: { id: 'parent' }, model: {
    current: () => ({ provider: 'fixture', model: 'fixture-model', reasoningEffort: 'high' }) } };
  let event;
  const ctx = { on: (_name, handler) => { event = handler; return () => { calls.removed = true; }; },
    agents: { create: async options => {
      calls.options = options;
      await options.setup({ get: () => ({ composeFrom: (_scope, source) => { calls.source = source; } }),
        tools: { restrict: value => { calls.restriction = value; },
          register: tool => registered.set(tool.name, tool) }, systemPrompt: { context: x => { calls.prompt = x; } } });
      return { dispose: async () => { calls.disposed = true; }, agent: {
        cancel: () => {}, followup: input => {
          calls.input = input;
          queueMicrotask(async () => {
            if (verdict) await registered.get('validation_verdict').execute({ decision: 'accept', summary: 'checked', findings: [] });
            else event({ id: options.sessionId }, { type: 'turn/end', data: { reason: { kind: 'completed' } } });
          });
        } } };
    } } };
  return { ctx, calls, parent, observers, registered };
}
test('native worker factory contract uses independent ownership and only scoped readonly tools', async () => {
  const f = harness();
  const result = await assess(f.ctx, f.parent, { id: 'job', candidate, title: 'fixture',
    approvedProfile: { acceptance: 'fixture behavior' }, commands: [] },
  'review', '/tmp', new AbortController().signal, f.observers);
  assert.equal(result.decision, 'accept');
  assert.equal('parentAgent' in f.calls.options, false);
  assert.equal(f.calls.options.meta.parentSession, 'parent');
  assert.deepEqual(f.calls.restriction, { allow: [] });
  assert.deepEqual([...f.registered.keys()], ['validation_read', 'validation_verdict']);
  assert.equal(f.calls.source, f.parent.ctx);
  assert.equal(f.calls.input.source.kind, 'validation-queue');
  assert.ok(f.calls.disposed && f.calls.removed); assert.equal(f.observers.size, 0);
});
test('a summary without a structured verdict is not a successful review', async () => {
  const f = harness({ verdict: false });
  await assert.rejects(assess(f.ctx, f.parent, { id: 'job', candidate, title: 'fixture',
    approvedProfile: { acceptance: 'fixture' }, commands: [] },
  'review', '/tmp', new AbortController().signal, f.observers), /WITHOUT_REPORT/);
  assert.ok(f.calls.disposed); assert.equal(f.observers.size, 0);
});
