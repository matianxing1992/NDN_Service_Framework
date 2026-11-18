import test from 'node:test';
import assert from 'node:assert/strict';
import { Context } from '@deepseek-ai/cordis';
import CodexAudit from '../lib/index.js';

async function fixture({ decision = 'actionable', stale = false, run, phase = 'blocked' } = {}) {
  const ctx = new Context();
  let durable;
  const events = [];
  const calls = [];
  const goal = { id: 'g', revision: 1, phase, activation: 'disarmed',
    roundsStarted: 19, maxGoalRounds: 256, blockedReason: { code: 'fixture', message: 'fixture blocker' } };
  const agent = { id: 's', status: 'idle', model: { current: () => ({}) },
    inbox: { nextStep: [], nextTurn: [] },
    steer(message) { this.inbox.nextStep.push(message); } };
  ctx.provide('logger', { error() {} });
  ctx.provide('agents', { get: id => id === 's' ? agent : undefined });
  ctx.provide('goals', { get: () => ({ ...goal }), resume(a, ref) {
    calls.push(ref); goal.phase = 'active'; goal.activation = 'armed'; goal.revision++;
  } });
  ctx.provide('sessionQuery', { observeSession: async () => ({ events: events.slice(),
    cursor: events.length, [Symbol.dispose]() {} }) });
  ctx.provide('storageDomain', { open: async () => ({ table: () => ({
    get: () => durable, put: async (key, value) => { durable = structuredClone(value); },
  }), close() {} }) });
  const host = new CodexAudit(ctx);
  await host.ready;
  host.manager.snapshot = async () => ({ basis: { seq: 0, head: 'fixture' }, dispose() {} });
  host.manager.run = run ?? (async () => ({ report: 'Root cause and repair plan', documentChanges: [],
    blockedRecovery: { decision, summary: 'repair a local defect', steps: ['inspect caller', 'fix and verify'] },
    sourceCheck: { stale } }));
  await host.guard.configure('s', true, { id: 'g', revision: 1 });
  const settle = async () => {
    await host.requestBlockedReview();
    await Promise.all([...host.manager.running.values()].map(job => job.promise));
    await host.requestBlockedReview();
  };
  const admit = async () => {
    const message = agent.inbox.nextStep.shift();
    events.push({ type: 'user/message', seq: 10, data: message });
    ctx.emit('session/event', { id: 's' }, events.at(-1));
    await host.reconcileDeliveries();
    await host.requestBlockedReview();
  };
  return { ctx, host, goal, agent, calls, events, settle, admit, durable: () => durable,
    close: () => ctx.fiber.dispose() };
}

test('blocked triggers immediate audit outside periodic schedule, then resumes only after report admission', async () => {
  const f = await fixture({ phase: 'active' });
  try {
    assert.equal(f.host.manager.settings('s').enabled, false);
    f.goal.phase = 'blocked';
    f.ctx.emit('goal/changed', { agent: f.agent, change: { operation: 'block' } });
    await f.settle();
    assert.equal(f.host.manager.state.records.length, 1);
    assert.equal(f.host.manager.state.records[0].trigger, 'goal-blocked');
    assert.equal(f.calls.length, 0, 'inbox insertion is not report admission');
    assert.match(f.agent.inbox.nextStep[0].content[0].text, /请按计划继续修复并验证/);
    await f.admit();
    assert.deepEqual(f.calls, [{ id: 'g', revision: 1 }]);
    assert.equal(f.goal.phase, 'active');
    assert.equal(f.durable().sessions.s.guard.blocked.status, 'resumed');
    await f.settle(); assert.equal(f.host.manager.state.records.length, 1);
  } finally { await f.close(); }
});

test('needs-user and external-wait report the missing condition without resuming blocked goal', async () => {
  for (const decision of ['needs-user', 'external-wait']) {
    const f = await fixture({ decision });
    try {
      await f.settle();
      assert.match(f.agent.inbox.nextStep[0].content[0].text, /保留 blocked/);
      await f.admit();
      assert.equal(f.calls.length, 0);
      assert.equal(f.goal.phase, 'blocked');
      assert.equal(f.durable().sessions.s.guard.blocked.status, 'waiting');
    } finally { await f.close(); }
  }
});

test('stale sources and exhausted round budgets retain audit without bypassing limits', async () => {
  for (const options of [{ stale: true }, {}]) {
    const f = await fixture(options);
    try {
      if (!options.stale) f.goal.roundsStarted = 256;
      await f.settle(); await f.admit();
      assert.equal(f.calls.length, 0);
      assert.equal(f.host.manager.state.records.length, 1);
      assert.equal(f.host.guard.settings('s').blocked.status, options.stale ? 'stale' : 'budget');
    } finally { await f.close(); }
  }
});

test('pause or revision changes during audit hold the report and do not wake DSH', async () => {
  let finish;
  const f = await fixture({ run: () => new Promise(resolve => { finish = resolve; }) });
  try {
    await f.host.requestBlockedReview();
    while (!finish) await new Promise(resolve => setImmediate(resolve));
    f.goal.phase = 'paused'; f.goal.revision++;
    finish({ report: 'outdated report', documentChanges: [],
      blockedRecovery: { decision: 'actionable', summary: 'repair', steps: ['fix'] } });
    await f.settle();
    assert.equal(f.host.manager.state.records[0].delivery, 'held');
    assert.equal(f.agent.inbox.nextStep.length, 0);
    assert.equal(f.calls.length, 0);
  } finally { await f.close(); }
});

test('repeated identical blocks stop after three audit cycles', async () => {
  const f = await fixture();
  try {
    for (let i = 0; i < 4; i++) {
      f.goal.phase = 'blocked';
      f.goal.revision++;
      await f.settle();
      if (i < 3) await f.admit();
    }
    assert.equal(f.host.manager.state.records.length, 3);
    assert.equal(f.host.guard.settings('s').blocked.status, 'limit');
  } finally { await f.close(); }
});

test('invalid recovery decision fails closed and never re-arms a blocked goal', async () => {
  const f = await fixture({ run: async () => ({ report: 'unstructured plan', documentChanges: [],
    blockedRecovery: { decision: 'actionable', summary: 'missing steps', steps: [] } }) });
  try {
    await f.settle(); await f.admit();
    assert.equal(f.calls.length, 0);
    assert.equal(f.host.manager.state.records[0].blockedRecovery.decision, 'needs-user');
  } finally { await f.close(); }
});

test('public downloads and database choices route as executable plans without extra approval', async () => {
  const f = await fixture({ run: async () => ({ report: 'Use the maintained setup path',
    documentChanges: [], sourceCheck: { stale: false }, blockedRecovery: {
      decision: 'actionable', summary: 'Download the public fixture and configure a compatible database',
      steps: ['Fetch the documented public resource and verify its identity',
        'Reuse the supported database configuration and verify it through the production API'],
    } }) });
  try {
    await f.settle(); await f.admit();
    assert.equal(f.calls.length, 1);
    assert.equal(f.host.guard.settings('s').blocked.status, 'resumed');
  } finally { await f.close(); }
});

test('snapshot races retry with backoff and stop after three captures', async () => {
  const f = await fixture();
  f.host.manager.snapshot = async () => { throw Object.assign(new Error('source changed'),
    { code: 'SNAPSHOT_CHANGED' }); };
  try {
    await f.settle();
    assert.equal(f.host.guard.settings('s').blocked.status, 'queued');
    await f.settle(); assert.equal(f.host.manager.state.records.length, 1);
    f.host.guard.settings('s').blocked.retryAt = 0; await f.settle();
    f.host.guard.settings('s').blocked.retryAt = 0; await f.settle();
    assert.equal(f.host.manager.state.records.length, 3);
    assert.equal(f.host.guard.settings('s').blocked.status, 'audit-failed');
    assert.equal(f.calls.length, 0);
  } finally { await f.close(); }
});

test('human stop after report admission vetoes blocked recovery', async () => {
  const f = await fixture();
  try {
    await f.settle();
    f.events.push({ type: 'user/message', seq: 10, data: f.agent.inbox.nextStep.shift() });
    const stop = { type: 'turn/end', seq: 11,
      data: { reason: { kind: 'aborted', reason: { kind: 'user' } } } };
    f.events.push(stop); f.ctx.emit('session/event', { id: 's' }, stop);
    await f.host.reconcileDeliveries(); await f.host.requestBlockedReview();
    assert.equal(f.calls.length, 0);
    assert.equal(f.host.guard.settings('s').blocked.status, 'held');
  } finally { await f.close(); }
});

test('another audit owns capacity, while blocked work remains queued without duplication', async () => {
  const f = await fixture();
  try {
    f.host.manager.running.set('other', { controller: new AbortController(), promise: Promise.resolve() });
    await f.host.requestBlockedReview();
    assert.equal(f.host.guard.settings('s').blocked.status, 'queued');
    assert.equal(f.host.manager.state.records.length, 0);
    f.host.manager.running.delete('other');
    await f.settle(); assert.equal(f.host.manager.state.records.length, 1);
  } finally { f.host.manager.running.delete('other'); await f.close(); }
});
