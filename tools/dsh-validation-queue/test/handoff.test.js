import test from 'node:test';
import assert from 'node:assert/strict';
import { handoffChildren } from '../lib/handoff.js';

function fixture() {
  const state = { loaded: true, saved: [], messages: [], reads: 0 };
  const parent = { id: 'parent', steer: m => state.messages.push(m) };
  const ctx = { get: () => ({ listChildren: async () => [{ id: 'child', mode: 'continuable', label: 'fixture' }],
    drainChildren: async (p, ids) => { assert.equal(p, parent); assert.deepEqual(ids, ['child']); state.loaded = false; },
    drainDescendants: () => { throw new Error('must not close future admission'); } }),
    agents: { get: () => state.loaded ? {} : undefined },
    sessionQuery: { observeSession: async () => { state.reads++; return {
      cursor: state.reads, inheritedEventCount: 0, events: [
        { type: 'user/message', data: { source: { kind: 'user' }, content: [{ type: 'text', text: 'original broad task' }] } },
        { type: 'assistant/message', seq: 1, data: { message: { content: [{ type: 'text', text: 'already delivered result' }] } } }
      ], [Symbol.dispose]() {} }; } } };
  return { ctx, parent, state, save: async x => state.saved.push(structuredClone(x)) };
}
test('selective handoff preserves history, transfers remaining work only and keeps future admission', async () => {
  const f = fixture();
  const result = await handoffChildren(f.ctx, f.parent, ['child'], f.save, { child: 'only unfinished boundary' });
  assert.equal(result.status, 'delivered'); assert.equal(f.state.loaded, false);
  assert.equal(result.children[0].lastReplies[0].text, 'already delivered result');
  assert.equal(f.state.reads, 2);
  const text = f.state.messages[0].content[0].text;
  assert.match(text, /only unfinished boundary/); assert.ok(!text.includes('original broad task'));
  assert.ok(!text.includes('already delivered result')); assert.equal(f.state.saved[0].status, 'captured');
});
test('handoff refuses unrelated children or an unspecified remaining task before any drain', async () => {
  const f = fixture();
  await assert.rejects(handoffChildren(f.ctx, f.parent, ['unrelated'], f.save, {}), /DIRECT_CHILDREN/);
  await assert.rejects(handoffChildren(f.ctx, f.parent, ['child'], f.save, {}), /REMAINING_WORK/);
  assert.ok(f.state.loaded); assert.equal(f.state.messages.length, 0);
});
