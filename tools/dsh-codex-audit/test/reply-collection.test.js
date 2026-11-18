import test from 'node:test';
import assert from 'node:assert/strict';
import CodexAudit from '../lib/index.js';

function record(id, sessionId = 's') {
  return { id, sessionId, delivery: 'delivered', messageId: `delivery-${id}`, replies: [] };
}

function message(type, seq, id, text = '') {
  return { type, seq, time: seq, data: { turn: 3, step: 2,
    message: { id, content: [{ type: 'text', text }] } } };
}

test('reply collection reads a long observation once and indexes admissions outside the message loop', async () => {
  const first = record('first');
  const second = record('second');
  const missing = record('missing');
  const offline = record('offline', 'offline');
  const events = Array.from({ length: 20000 }, (_, index) =>
    message('assistant/message', index, `history-${index}`, 'ordinary progress'));
  events.push(message('assistant/message', 20000, 'early', '审计回应 first'));
  events.push(message('user/message', 20001, first.messageId));
  events.push(message('user/message', 20002, second.messageId));
  events.push(message('assistant/message', 20003, 'reply',
    '审计回应 first：接受。审计回应 second：需要验证。审计回应 missing：不能算已送达。'));
  let reads = 0;
  let searches = 0;
  let disposed = 0;
  const calls = [];
  events.find = () => {
    searches++;
    throw new Error('full-history find must not run inside the assistant-message loop');
  };
  const host = {
    manager: { state: { records: [first, second, missing, offline] },
      async acknowledge(reply) { calls.push(reply); } },
    ctx: {
      agents: { get: id => id === 's' ? {} : undefined },
      sessionQuery: { async observeSession(id) {
        assert.equal(id, 's');
        return { get events() {
          assert.equal(++reads, 1, 'a lazy history must be materialized only once');
          return events;
        }, [Symbol.dispose]() { disposed++; } };
      } },
    },
  };
  await CodexAudit.prototype.collectReplies.call(host);
  assert.equal(reads, 1);
  assert.equal(searches, 0);
  assert.equal(disposed, 1);
  assert.deepEqual(calls.map(value => value.auditId), ['first', 'second']);
  assert.ok(calls.every(value => value.messageId === 'reply' && value.seq === 20003 &&
    value.turn === 3 && value.step === 2));
});

test('reply admission preserves the first matching delivery and releases observations on failure', async () => {
  const owned = record('a');
  const events = [message('user/message', 0, owned.messageId),
    message('assistant/message', 1, 'answer', '审计回应 a'),
    message('user/message', 2, owned.messageId)];
  let disposed = false;
  const host = {
    manager: { state: { records: [owned] }, async acknowledge(reply) {
      assert.equal(reply.messageId, 'answer');
      throw new Error('storage unavailable');
    } },
    ctx: { agents: { get: () => ({}) }, sessionQuery: {
      async observeSession() { return { events,
        [Symbol.dispose]() { disposed = true; } }; },
    } },
  };
  await assert.rejects(CodexAudit.prototype.collectReplies.call(host), /storage unavailable/);
  assert.equal(disposed, true);
});

test('reply collection does not observe history when all delivered audits already have replies', async () => {
  const owned = record('done');
  owned.replies.push({ messageId: 'reply' });
  const host = { manager: { state: { records: [owned] } },
    ctx: { agents: { get() { throw new Error('no history should be read'); } } } };
  await CodexAudit.prototype.collectReplies.call(host);
});
