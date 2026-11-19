import test from 'node:test';
import assert from 'node:assert/strict';
import { Context } from '@deepseek-ai/cordis';
import { remoteMethods } from '@deepseek-ai/dsh-typert-protocol';
import CodexAudit from '../lib/index.js';
import { mkdtemp, writeFile, readFile, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { digest } from '../lib/snapshot.js';
import { execute } from '../lib/codex-runner.js';

test('real host enable captures a baseline and dispatches only the new focus with full background', async () => {
  const root = await mkdtemp('/tmp/dsh-audit-host-scope-');
  const ctx = new Context();
  let host;
  let durable;
  let cursor = 10;
  let leases = 0;
  const events = [{ seq: 10, time: Date.now(), type: 'user/message', data: 'old task background' }];
  try {
    await execute('git', ['-C', root, 'init', '-q']);
    await writeFile(join(root, 'source.cpp'), 'before enable');
    await execute('git', ['-C', root, 'add', 'source.cpp']);
    await execute('git', ['-C', root, '-c', 'user.name=Test', '-c', 'user.email=t@example.invalid',
      '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture']);
    ctx.provide('logger', { error() {} });
    ctx.provide('agents', { get: id => id === 's' ? { status: 'idle' } : undefined });
    ctx.provide('sessionQuery', { observeSession: async () => {
      leases++;
      return { header: { cwd: root }, cursor, events: events.slice(),
        [Symbol.dispose]() { leases--; } };
    } });
    ctx.provide('storageDomain', { open: async () => ({ table: () => ({
      get: () => durable, put: async (key, value) => { durable = structuredClone(value); },
    }), close() {} }) });
    host = new CodexAudit(ctx);
    await host.ready;
    assert.equal(host.bootError, undefined);
    host.manager.models = [
      { id: 'gpt-6.2-sol', efforts: ['medium', 'high', 'xhigh', 'ultra'], defaultEffort: 'xhigh' },
      { id: 'gpt-6.2-astra', efforts: ['medium', 'high'], defaultEffort: 'medium' },
    ];
    await host.manager.configure('s', { enabled: true });
    assert.equal(host.manager.settings('s').auditPeriod.baseline.basis.seq, 10);
    const due = host.manager.settings('s').nextDue;
    const options = await host.control({ sessionId: 's', action: 'configure', effort: 'ultra' });
    assert.deepEqual(options.efforts, ['medium', 'high', 'xhigh', 'ultra']);
    assert.equal(options.settings.effort, 'ultra');
    assert.equal(options.settings.nextDue, due);
    assert.equal(durable.sessions.s.effort, 'ultra');
    await assert.rejects(host.control({ sessionId: 's', action: 'configure', effort: 'max' }),
      { code: 'UNSUPPORTED_SELECTION' });
    const switched = await host.control({ sessionId: 's', action: 'configure', model: 'gpt-6.2-astra' });
    assert.deepEqual(switched.efforts, ['medium', 'high']);
    assert.equal(switched.settings.effort, 'medium');
    const chosen = await host.control({ sessionId: 's', action: 'configure', effort: 'high' });
    assert.equal(chosen.settings.nextDue, due);
    assert.equal(durable.sessions.s.model, 'gpt-6.2-astra');
    cursor = 11;
    events.push({ seq: 11, time: Date.now(), type: 'tool/result', data: 'new work since activation' });
    await writeFile(join(root, 'source.cpp'), 'after enable');
    host.manager.run = async ({ snapshot, settings }) => {
      assert.equal(settings.model, 'gpt-6.2-astra');
      assert.equal(settings.effort, 'high');
      assert.equal(snapshot.basis.scope.kind, 'since-enable');
      assert.equal(snapshot.basis.scope.sourceChangeCount, 1);
      const focus = await readFile(join(snapshot.path, '__audit_input__/focus-0000.jsonl'), 'utf8');
      assert.ok(focus.includes('new work since activation') && !focus.includes('old task background'));
      const history = await readFile(join(snapshot.path, '__audit_input__/conversation-0000.jsonl'), 'utf8');
      assert.ok(history.includes('old task background'));
      return { report: 'scope verified without a model request' };
    };
    host.manager.deliver = async () => ({ messageId: 'fixture-delivery' });
    const record = await host.manager.start('s');
    assert.equal(record.status, 'completed');
    assert.equal(record.effort, 'high');
    assert.equal(host.manager.settings('s').auditPeriod.checkpoint.basis.seq, 11);
    const view = await host.view({ sessionId: 's' });
    assert.equal(view.settings.auditPeriod, undefined);
    assert.equal(view.settings.auditWindow.fromSeqExclusive, 11);
    assert.equal(leases, 0);
  } finally {
    await ctx.fiber.dispose();
    await rm(root, { recursive: true, force: true });
  }
});

test('goal guard controls share native goal ownership and audit an idle unfinished target', async () => {
  const ctx = new Context();
  const events = [];
  const goal = { id: 'goal', revision: 1, phase: 'active', activation: 'disarmed',
    roundsStarted: 19, maxGoalRounds: 256 };
  const agent = { id: 's', status: 'idle', model: { current: () => ({}) },
    inbox: { nextStep: [], nextTurn: [] } };
  let durable;
  let resumes = 0;
  ctx.provide('logger', { error() {} });
  ctx.provide('agents', { get: id => id === 's' ? agent : undefined });
  ctx.provide('goals', { get: () => ({ ...goal }), resume(a, ref) {
    assert.equal(ref.revision, goal.revision); goal.activation = 'armed'; resumes++;
  } });
  ctx.provide('sessionQuery', { observeSession: async () => ({ events: events.slice(),
    [Symbol.dispose]() {} }) });
  ctx.provide('storageDomain', { open: async () => ({ table: () => ({
    get: () => durable, put: async (key, value) => { durable = structuredClone(value); },
  }), close() {} }) });
  const host = new CodexAudit(ctx);
  try {
    await host.ready;
    assert.equal(host.auditEligible('s'), false);
    await host.control({ sessionId: 's', action: 'configure-guard', enabled: true,
      goalRef: { id: 'goal', revision: 1 } });
    assert.equal(host.auditEligible('s'), true);
    assert.equal((await host.view({ sessionId: 's' })).guard.status, 'disarmed');
    const event = { type: 'turn/end', seq: 8,
      data: { reason: { kind: 'aborted', reason: { kind: 'disposed' } } } };
    events.push(event);
    ctx.emit('session/event', { id: 's' }, event);
    await host.guard.tick();
    assert.equal(resumes, 1, 'session event invalidates previously cached boundary');
    assert.equal(durable.sessions.s.guard.history.at(-1).outcome, 'resumed');
    goal.phase = 'paused'; assert.equal(host.auditEligible('s'), false);
    await assert.rejects(host.control({ sessionId: 's', action: 'resume-goal',
      goalRef: { id: 'goal', revision: 1 } }), /active/);
    await host.control({ sessionId: 's', action: 'configure-guard', enabled: false });
  } finally { await ctx.fiber.dispose(); }
});

test('host registers public RPC metadata against installed DSH contracts', async () => {
  const ctx = new Context();
  ctx.provide('logger', { error() {} });
  const host = new CodexAudit(ctx);
  await host.ready;
  assert.ok(host.bootError, 'missing services must produce visible initialization error');
  const markers = remoteMethods(host);
  assert.deepEqual(markers.map(marker => marker.method), ['view', 'control']);
  for (const marker of markers) assert.equal(marker.resources.session({ sessionId: 's' }), 's');
  const result = await host.view({ sessionId: 's' });
  assert.equal(result.error.code, 'INITIALIZATION_FAILED');
  await ctx.fiber.dispose();
});

test('host delivers through steer while DSH remains running and correlates only explicit reply', async () => {
  const ctx = new Context();
  const events = [];
  let durable;
  let leases = 0;
  const agent = { status: 'running', model: { current: () => ({ provider: 'fixture', model: 'fixture' }) },
    inbox: { nextStep: [], nextTurn: [] },
    steer(message) { this.inbox.nextStep.push(message); } };
  ctx.provide('logger', { error() {} });
  ctx.provide('agents', { get: id => id === 's' ? agent : undefined });
  ctx.provide('sessionQuery', { observeSession: async () => {
    leases++;
    return { events: events.slice(), cursor: events.length - 1,
      [Symbol.dispose]() { leases--; } };
  } });
  ctx.provide('storageDomain', { open: async () => ({ table: () => ({
    get: () => durable, put: async (key, value) => { durable = structuredClone(value); },
  }), close() {} }) });
  const host = new CodexAudit(ctx);
  await host.ready;
  assert.equal(host.bootError, undefined);
  host.manager.snapshot = async () => ({ basis: { seq: 0, head: 'fixture',
    sourceMode: 'working-tree', capturedAt: Date.now(),
    workingTreeChanges: [{ path: 'source.cpp', index: 'M', worktree: 'M' }] }, dispose() {} });
  host.manager.run = async () => ({ report: 'verified report', documentChanges: [] });
  const record = await host.manager.start('s');
  assert.equal(agent.status, 'running');
  assert.equal(agent.inbox.nextStep.length, 1);
  assert.match(agent.inbox.nextStep[0].content[0].text, /HEAD=fixture 仅为基线/);
  assert.match(agent.inbox.nextStep[0].content[0].text, /另含 1 项/);
  assert.equal(record.delivery, 'delivered');
  const ownedRecord = host.manager.state.records[0];
  agent.inbox.nextStep = [];
  agent.status = 'idle';
  ownedRecord.deliveredAt = Date.now() - 20000;
  await host.reconcileDeliveries();
  assert.equal(ownedRecord.delivery, 'pending', 'dropped pre-step input must be retried');
  await host.manager.retryDelivery(record.id);
  assert.equal(agent.inbox.nextStep.length, 1);
  agent.status = 'running';
  const listing = await host.view({ sessionId: 's' });
  assert.equal(listing.total, 1);
  assert.equal(listing.records[0].report, undefined);
  const detail = await host.view({ sessionId: 's', auditId: record.id });
  assert.equal(detail.records[0].report, 'verified report');
  assert.equal((await host.view({ sessionId: 'other', auditId: record.id })).records.length, 0);
  await host.deliver(record);
  assert.equal(agent.inbox.nextStep.length, 1, 'pending inbox deduplicates stable audit identity');
  events.push({ type: 'assistant/message', seq: 1, time: 100,
    data: { message: { id: 'ordinary', content: [{ type: 'text', text: 'continued work' }] } } });
  await host.collectReplies();
  assert.equal(durable.records[0].replies.length, 0);
  events.push({ type: 'assistant/message', seq: 2, time: 200,
    data: { message: { id: 'acknowledged', content: [{ type: 'text',
      text: `审计回应 ${record.id}：接受，继续验证。` }] } } });
  await host.collectReplies();
  assert.equal(durable.records[0].replies.length, 0, 'reply before report admission is not acknowledgement');
  events.push({ type: 'user/message', seq: 3, time: 300, data: agent.inbox.nextStep[0] });
  await host.reconcileDeliveries();
  assert.equal(ownedRecord.admittedSeq, 3);
  agent.inbox.nextStep = [];
  await host.deliver(record);
  assert.equal(agent.inbox.nextStep.length, 0, 'consumed durable user message deduplicates delivery');
  events.push({ type: 'assistant/message', seq: 4, time: 400,
    data: { turn: 2, step: 1, message: { id: 'actual-ack', content: [{ type: 'text',
      text: `审计回应 ${record.id}：接受，继续验证。` }] } } });
  await host.collectReplies();
  assert.equal(durable.records[0].replies[0].messageId, 'actual-ack');
  assert.equal(durable.records[0].replies[0].seq, 4);
  assert.equal(durable.records[0].replies[0].turn, 2);
  assert.equal(durable.records[0].replies[0].step, 1);
  const root = await mkdtemp('/tmp/dsh-auto-doc-test-');
  try {
    const path = join(root, 'README.md');
    await writeFile(path, 'old factual statement');
    const owned = host.manager.state.records[0];
    owned.sourceCheck = { stale: false };
    owned.documentChanges = [{ path: 'README.md', originalCwd: root,
      baseHash: digest('old factual statement'), content: 'corrected statement', status: 'pending' }];
    await host.applyPendingDocuments();
    assert.equal(await readFile(path, 'utf8'), 'old factual statement', 'active DSH prohibits automatic writes');
    agent.status = 'idle';
    owned.sourceCheck.stale = true;
    await host.applyPendingDocuments();
    assert.equal(await readFile(path, 'utf8'), 'old factual statement', 'stale report stays a proposal');
    owned.sourceCheck.stale = false;
    await host.applyPendingDocuments();
    assert.equal(await readFile(path, 'utf8'), 'corrected statement');
    assert.equal(durable.records[0].documentChanges[0].status, 'applied');
  } finally { await rm(root, { recursive: true, force: true }); }
  assert.equal(leases, 0);
  await ctx.fiber.dispose();
  assert.equal(host.manager.disposed, true);
});
