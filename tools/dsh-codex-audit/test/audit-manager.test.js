import test from 'node:test';
import assert from 'node:assert/strict';
import { AuditManager, AuditError, DEFAULT_PROMPT, LEGACY_PROMPT, AUDIT_MODEL, AUDIT_EFFORT } from '../lib/audit-manager.js';
import { auditScope } from '../lib/audit-scope.js';

async function fixture(overrides = {}) {
  let durable;
  let clock = 100;
  const deliveries = [];
  const manager = new AuditManager({
    load: async () => durable,
    save: async value => { durable = structuredClone(value); },
    captureBaseline: async () => ({ basis: { cwd: '/fixture', seq: 0,
      capturedAt: clock, head: 'fixed', manifestHash: 'initial' }, hashes: { 'source.cpp': 'old' } }),
    snapshot: async (id, signal, record, period, settings) => {
      const basis = { cwd: '/fixture', head: 'fixed', seq: 42, capturedAt: clock };
      const hashes = { 'source.cpp': 'new' };
      basis.scope = auditScope({ period, trigger: record.trigger, basis, hashes,
        intervalHours: settings.intervalHours, events: [{ seq: 42, time: clock }] }).scope;
      return { basis, hashes, dispose: async () => {} };
    },
    run: async () => ({ report: 'source-grounded report' }),
    deliver: async record => { deliveries.push(record.id); return { messageId: 'message-1' }; },
    active: () => true,
    now: () => clock,
    ...overrides,
  });
  await manager.open();
  return { manager, deliveries, durable: () => durable, time: value => { clock = value; } };
}

test('GUI interval bounds, disabled default, and durable scheduling', async () => {
  const f = await fixture();
  assert.equal(f.manager.settings('s').enabled, false);
  for (const intervalHours of [0, 6, 1.5]) {
    await assert.rejects(f.manager.configure('s', { intervalHours }), /1–5/);
  }
  await f.manager.configure('s', { enabled: true, intervalHours: 1 });
  assert.equal(f.durable().sessions.s.nextDue, 3600100);
  await f.manager.configure('s', { enabled: false });
  assert.equal(f.durable().sessions.s.nextDue, null);
});

test('parallel audit reserves one job per session and delivery does not await DSH completion', async () => {
  let finish;
  const f = await fixture({ run: () => new Promise(resolve => { finish = resolve; }) });
  const first = f.manager.start('s');
  assert.equal(f.manager.start('s'), first);
  while (!finish) await new Promise(resolve => setImmediate(resolve));
  assert.equal(f.manager.running.size, 1);
  finish({ report: 'report' });
  const record = await first;
  assert.equal(record.delivery, 'delivered');
  assert.equal(f.deliveries.length, 1);
  assert.equal(f.manager.running.size, 0);
});

test('manual audit resets the durable deadline and consumes the old periodic slot', async () => {
  const f = await fixture();
  const hour = 3600000;
  await f.manager.configure('s', { enabled: true, intervalHours: 3 });
  const oldDue = f.manager.settings('s').nextDue;
  f.time(oldDue - hour);
  const record = await f.manager.start('s');
  const nextDue = oldDue + 2 * hour;
  assert.equal(record.trigger, 'manual');
  assert.equal(f.durable().sessions.s.nextDue, nextDue);
  f.time(oldDue);
  await f.manager.tick();
  assert.equal(f.manager.state.records.length, 1, 'old deadline must not launch another audit');
  f.time(nextDue);
  await f.manager.tick();
  await Promise.all([...f.manager.running.values()].map(job => job.promise));
  assert.deepEqual(f.manager.state.records.map(r => r.trigger), ['manual', 'scheduled']);
  assert.equal(f.durable().sessions.s.nextDue, nextDue + 3 * hour);
});

test('joining a running audit does not move its deadline or create another record', async () => {
  let finish;
  const f = await fixture({ run: () => new Promise(resolve => { finish = resolve; }) });
  await f.manager.configure('s', { enabled: true, intervalHours: 1 });
  f.time(1000);
  const pending = f.manager.start('s');
  while (!finish) await new Promise(resolve => setImmediate(resolve));
  const due = f.manager.settings('s').nextDue;
  f.time(2000);
  assert.equal(f.manager.start('s'), pending);
  assert.equal(f.manager.settings('s').nextDue, due);
  finish({ report: 'report' });
  await pending;
  assert.equal(f.manager.state.records.length, 1);
  assert.equal(f.durable().sessions.s.nextDue, 3601000);
});

test('blocked audit replaces the periodic slot without enabling a disabled timer', async () => {
  const f = await fixture();
  await f.manager.configure('s', { enabled: true, intervalHours: 3 });
  const oldDue = f.manager.settings('s').nextDue;
  f.time(oldDue - 1000);
  const record = await f.manager.start('s', 'goal-blocked', { id: 'g', revision: 1 });
  assert.equal(record.blockedGoal.id, 'g');
  f.time(oldDue);
  await f.manager.tick();
  assert.equal(f.manager.state.records.length, 1);
  assert.equal(f.durable().sessions.s.nextDue, oldDue - 1000 + 3 * 3600000);
  await f.manager.configure('s', { enabled: false });
  await f.manager.start('s');
  assert.equal(f.durable().sessions.s.enabled, false);
  assert.equal(f.durable().sessions.s.nextDue, null);
});

test('due maintenance and manual clicks share one admitted audit; missed slots do not queue', async () => {
  let finish;
  const f = await fixture({ run: () => new Promise(resolve => { finish = resolve; }) });
  await f.manager.configure('s', { enabled: true, intervalHours: 1 });
  const due = f.manager.settings('s').nextDue;
  f.time(due);
  const tick = f.manager.tick();
  const pending = f.manager.start('s');
  await tick;
  while (!finish) await new Promise(resolve => setImmediate(resolve));
  assert.equal(f.manager.state.records.length, 1);
  f.time(due + 3600000);
  await f.manager.tick();
  finish({ report: 'long-running report' });
  await pending;
  await f.manager.tick();
  assert.equal(f.manager.state.records.length, 1, 'long-running audit must not leave queued work');
  assert.equal(f.durable().sessions.s.nextDue, due + 2 * 3600000);
});

test('delivery failure preserves complete report for retry', async () => {
  let unavailable = true;
  const f = await fixture({ deliver: async () => {
    if (unavailable) throw new Error('session unavailable');
    return { messageId: 'stable-message' };
  } });
  const record = await f.manager.start('s');
  assert.equal(record.status, 'completed');
  assert.equal(record.delivery, 'pending');
  assert.equal(f.durable().records[0].report, 'source-grounded report');
  unavailable = false;
  await f.manager.retryDelivery(record.id);
  assert.equal(f.durable().records[0].delivery, 'delivered');
});

test('acknowledgements require matching audit, session, and message identity', async () => {
  const f = await fixture();
  const record = await f.manager.start('s');
  const reply = { auditId: record.id, sessionId: 's', messageId: 'reply',
    text: `审计回应 ${record.id}：需要验证`, at: 500 };
  assert.equal(await f.manager.acknowledge({ ...reply, sessionId: 'other' }), false);
  assert.equal(await f.manager.acknowledge({ ...reply, text: 'ordinary next message' }), false);
  assert.equal(await f.manager.acknowledge(reply), true);
  assert.equal(await f.manager.acknowledge(reply), false);
});

test('restart marks live work interrupted while retaining pending report', async () => {
  const f = await fixture({ load: async () => ({ version: 1, sessions: {}, records: [
    { id: 'running', status: 'running' },
    { id: 'finished', status: 'completed', delivery: 'pending', report: 'retained' },
  ] }) });
  assert.equal(f.durable().records[0].status, 'interrupted');
  assert.equal(f.durable().records[1].report, 'retained');
});

test('cancellation releases job and never injects an incomplete report', async () => {
  const f = await fixture({ run: ({ signal }) => new Promise((resolve, reject) => {
    signal.addEventListener('abort', () => reject(signal.reason), { once: true });
    if (signal.aborted) reject(signal.reason);
  }) });
  const pending = f.manager.start('s');
  f.manager.cancel('s');
  const record = await pending;
  assert.equal(record.status, 'cancelled');
  assert.equal(f.deliveries.length, 0);
  await f.manager.close();
});

test('skip-next persists one full interval without disabling the session', async () => {
  const f = await fixture();
  await assert.rejects(f.manager.skipNext('s'), /尚未开启/);
  await f.manager.configure('s', { enabled: true, intervalHours: 3 });
  const original = f.manager.settings('s').nextDue;
  await f.manager.skipNext('s');
  assert.equal(f.durable().sessions.s.nextDue, original + 3 * 3600000);
  assert.equal(f.durable().sessions.s.enabled, true);
  f.time(original);
  await f.manager.tick();
  assert.equal(f.manager.running.size, 0);
  assert.equal(f.durable().sessions.s.lastSkip.skippedDue, original);
});

test('completed report storage failure cannot publish before a durable retry', async () => {
  let failed = false;
  const f = await fixture({ save: async state => {
    if (!failed && state.records.some(r => r.status === 'completed')) {
      failed = true;
      throw new Error('injected storage failure');
    }
  } });
  const record = await f.manager.start('s');
  assert.equal(record.status, 'completed');
  assert.equal(record.delivery, 'pending');
  assert.equal(f.deliveries.length, 0);
  await f.manager.retryDelivery(record.id);
  assert.equal(f.deliveries.length, 1);
});

test('permanent model error pauses scheduling without fallback', async () => {
  const f = await fixture({ run: async () => {
    throw new AuditError('CODEX_MODEL', 'not supported', 'check model access');
  } });
  await f.manager.configure('s', { enabled: true });
  const record = await f.manager.start('s');
  assert.equal(record.error.code, 'CODEX_MODEL');
  assert.equal(f.durable().sessions.s.enabled, false);
  assert.equal(f.durable().sessions.s.model, AUDIT_MODEL);
  assert.equal(f.deliveries.length, 0);
});

test('oversized report is rejected explicitly and never injected', async () => {
  const f = await fixture({ run: async () => ({ report: 'x'.repeat(1024 * 1024 + 1) }) });
  const record = await f.manager.start('s');
  assert.equal(record.error.code, 'REPORT_CAPACITY');
  assert.equal(f.deliveries.length, 0);
  assert.equal(record.report, undefined);
});

test('automatic delivery stops at five attempts and manual retry remains available', async () => {
  let calls = 0;
  const f = await fixture({ deliver: async () => { calls++; throw new Error('unavailable'); } });
  const record = await f.manager.start('s');
  for (let i = 0; i < 10; i++) {
    await f.manager.retryDelivery(record.id, { automatic: true }).catch(() => {});
  }
  assert.equal(calls, 5);
  assert.equal(f.durable().records[0].delivery, 'pending');
  await f.manager.retryDelivery(record.id).catch(() => {});
  assert.equal(calls, 6);
});

test('enable sessions reset the coverage baseline; period edits and skipping preserve it', async () => {
  const f = await fixture();
  await f.manager.configure('s', { enabled: true });
  const first = structuredClone(f.manager.settings('s').auditPeriod);
  await f.manager.start('s');
  const covered = f.manager.settings('s').auditPeriod.checkpoint;
  assert.equal(covered.basis.seq, 42);
  await f.manager.configure('s', { intervalHours: 1 });
  await f.manager.skipNext('s');
  assert.equal(f.manager.settings('s').auditPeriod.id, first.id);
  assert.deepEqual(f.manager.settings('s').auditPeriod.checkpoint, covered);
  await f.manager.configure('s', { enabled: false });
  f.time(9999);
  await f.manager.configure('s', { enabled: true });
  assert.notEqual(f.manager.settings('s').auditPeriod.id, first.id);
  assert.equal(f.manager.settings('s').auditPeriod.baseline.basis.capturedAt, 9999);
  assert.equal(f.manager.settings('s').auditPeriod.checkpoint, null);
  assert.equal(f.manager.publicSettings('s').auditPeriod, undefined, 'hash maps stay out of GUI polling');
});

test('failure and blocked review do not consume full-work coverage; success uses capture cutoff', async () => {
  let fail = true;
  const f = await fixture({ run: async () => {
    if (fail) throw new Error('temporary audit failure');
    f.time(2000);
    return { report: 'done after more DSH work' };
  } });
  await f.manager.configure('s', { enabled: true });
  await f.manager.start('s');
  assert.equal(f.manager.settings('s').auditPeriod.checkpoint, null);
  fail = false;
  await f.manager.start('s', 'goal-blocked', { id: 'g' });
  assert.equal(f.manager.settings('s').auditPeriod.checkpoint, null);
  f.time(1000);
  const record = await f.manager.start('s');
  assert.equal(record.finishedAt, 2000);
  assert.equal(f.manager.settings('s').auditPeriod.checkpoint.basis.capturedAt, 1000);
});

test('old in-flight results cannot advance a newly enabled period', async () => {
  let finish;
  const f = await fixture({ run: () => new Promise(resolve => { finish = resolve; }) });
  await f.manager.configure('s', { enabled: true });
  const pending = f.manager.start('s');
  while (!finish) await new Promise(resolve => setImmediate(resolve));
  await f.manager.configure('s', { enabled: false });
  await f.manager.configure('s', { enabled: true });
  finish({ report: 'old period result' });
  await pending;
  assert.equal(f.manager.settings('s').auditPeriod.checkpoint, null);
});

test('newer disable wins against a delayed activation baseline', async () => {
  let release;
  const f = await fixture({ captureBaseline: () => new Promise(resolve => { release = resolve; }) });
  const enabling = f.manager.configure('s', { enabled: true });
  await f.manager.configure('s', { enabled: false });
  release({ basis: { capturedAt: 1, seq: 0 }, hashes: {} });
  await assert.rejects(enabling, /后续操作/);
  assert.equal(f.manager.settings('s').enabled, false);
});

test('upgrade updates only the exact legacy default and never rewrites historical requests', async () => {
  const f = await fixture({ load: async () => ({ version: 1, sessions: {
    old: { prompt: LEGACY_PROMPT }, custom: { prompt: 'custom instruction' },
  }, records: [{ id: 'old', status: 'completed', prompt: LEGACY_PROMPT }] }) });
  assert.equal(f.manager.settings('old').prompt, DEFAULT_PROMPT);
  assert.equal(f.manager.settings('custom').prompt, 'custom instruction');
  assert.equal(f.manager.state.records[0].prompt, LEGACY_PROMPT);
});

test('opening old settings preserves the selected audit model without rewriting history', async () => {
  const oldSettings = { enabled: true, intervalHours: 3, nextDue: 7000,
    model: 'gpt-6.1-sol', effort: 'xhigh', prompt: 'custom instruction' };
  const historical = { id: 'old', status: 'completed',
    model: 'gpt-6.1-sol', effectiveModel: 'gpt-6.1-sol', effort: 'xhigh' };
  const f = await fixture({
    load: async () => ({ version: 1, sessions: { s: { ...oldSettings } },
      records: [{ ...historical }] }),
    run: async ({ settings }) => {
      assert.equal(settings.model, oldSettings.model);
      assert.equal(settings.effort, AUDIT_EFFORT);
      return { report: 'audit using new selection' };
    },
  });
  const expected = oldSettings;
  assert.deepEqual(f.manager.publicSettings('s'), { ...expected, auditWindow: null });
  assert.deepEqual(f.durable().sessions.s, expected);
  const next = await f.manager.start('s');
  assert.equal(next.status, 'completed');
  assert.equal(next.model, oldSettings.model);
  assert.equal(next.effort, AUDIT_EFFORT);
  assert.deepEqual(f.durable().records[0], historical);
});

test('host close prevents a delayed activation from re-enabling its disposed manager', async () => {
  let release;
  const f = await fixture({ captureBaseline: () => new Promise(resolve => { release = resolve; }) });
  const pending = f.manager.configure('s', { enabled: true });
  await f.manager.close();
  release({ basis: { capturedAt: 1, seq: 0 }, hashes: {} });
  await assert.rejects(pending, /disposed/);
  assert.equal(f.manager.settings('s').enabled, false);
});

test('reopening a session retains each supported effort and defaults missing or invalid values', async () => {
  const efforts = ['medium', 'high', 'xhigh', 'ultra'];
  const first = await fixture();
  for (const effort of efforts) await first.manager.configure(effort, { effort });
  const stored = first.durable();
  stored.sessions.old = { effort: 'low' };
  stored.sessions.missing = {};
  const restarted = await fixture({ load: async () => stored });
  for (const effort of efforts) assert.equal(restarted.manager.settings(effort).effort, effort);
  assert.equal(restarted.manager.settings('old').effort, AUDIT_EFFORT);
  assert.equal(restarted.manager.settings('missing').effort, AUDIT_EFFORT);
});

test('a per-run prompt applies to that audit only and is not persisted', async () => {
  const f = await fixture({});
  await f.manager.configure('s', { enabled: true });
  const before = f.manager.settings('s').prompt;
  const record = await f.manager.start('s', 'manual', null, '只审计这一次：看 task.md 的依赖顺序');
  assert.equal(record.prompt, '只审计这一次：看 task.md 的依赖顺序');
  assert.equal(record.promptSource, 'run');
  // The standing configuration is unchanged, so the next scheduled audit uses it.
  assert.equal(f.manager.settings('s').prompt, before);
});

test('an empty per-run prompt falls back to the configured one', async () => {
  const f = await fixture({});
  await f.manager.configure('s', { enabled: true });
  const configured = f.manager.settings('s').prompt;
  const record = await f.manager.start('s', 'manual', null, '   ');
  assert.equal(record.prompt, configured);
  assert.equal(record.promptSource, 'settings');
});
