import test from 'node:test';
import assert from 'node:assert/strict';
import { GoalGuard, restartBoundary } from '../lib/goal-guard.js';

const end = (seq, kind, cause) => ({ type: 'turn/end', seq,
  data: { reason: { kind, ...(cause ? { reason: { kind: cause } } : {}) } } });
const restart = [end(2, 'aborted', 'disposed'), end(3, 'completed')];

function fixture({ events = restart, goalPatch = {}, enabled = true, resume } = {}) {
  let time = 100000;
  const goal = { id: 'g', revision: 1, phase: 'active', activation: 'disarmed',
    roundsStarted: 19, maxGoalRounds: 256, ...goalPatch };
  const agent = { id: 's', status: 'idle', model: { current: () => ({}) },
    inbox: { nextStep: [], nextTurn: [] } };
  const calls = [];
  const manager = { state: { sessions: { s: { enabled: true, nextDue: 999999,
    guard: { enabled, goalId: 'g', history: [] } } } },
    settings(id) { return this.state.sessions[id] ?? { enabled: false }; },
    async persist() {} };
  const goals = { get: () => ({ ...goal }), resume(a, ref) {
    calls.push(ref);
    if (resume) return resume(a, ref);
    assert.equal(ref.revision, goal.revision);
    goal.activation = 'armed'; goal.revision++;
  } };
  const guard = new GoalGuard({ manager, agent: () => agent, goals: () => goals,
    observe: async () => ({ events, [Symbol.dispose]() {} }), now: () => time });
  return { guard, manager, agent, goal, calls, events, time: t => { time = t; } };
}

test('restart proof excludes later decisions, failures and unrelated goals', () => {
  assert.equal(restartBoundary(restart, 'g').restartSeq, 2);
  for (const cause of ['user', 'parent', 'hook']) {
    assert.equal(restartBoundary([...restart, end(4, 'aborted', cause)], 'g').kind, 'decision');
  }
  assert.equal(restartBoundary([...restart, end(4, 'error')], 'g').kind, 'error');
  assert.equal(restartBoundary([end(1, 'aborted', 'user'), ...restart], 'g').kind, 'decision');
  assert.equal(restartBoundary([...restart, { type: 'user/message', seq: 4,
    data: { source: { kind: 'user' } } }], 'g').kind, 'decision');
  assert.equal(restartBoundary([...restart, { type: 'goal/change', seq: 4,
    data: { operation: 'create', goal: { id: 'new' } } }], 'new').restartSeq, null);
});

test('authorized restart uses exact native goal ref once, without sending continue text', async () => {
  const f = fixture();
  await Promise.all([f.guard.tick(), f.guard.tick()]);
  assert.deepEqual(f.calls, [{ id: 'g', revision: 1 }]);
  assert.equal(f.agent.inbox.nextStep.length, 0);
  assert.equal(f.manager.state.sessions.s.nextDue, 999999);
  assert.equal(f.manager.state.sessions.s.guard.history.at(-1).outcome, 'resumed');
});

test('never bypasses pause, blocking, completion, round limits, user stop, or another goal', async () => {
  const cases = [
    { goalPatch: { phase: 'paused' } }, { goalPatch: { phase: 'blocked' } },
    { goalPatch: { phase: 'complete' } }, { goalPatch: { roundsStarted: 256 } },
    { goalPatch: { id: 'other' } }, { events: [end(4, 'aborted', 'user')] },
    { events: [end(4, 'completed')] }, { events: [...restart, end(5, 'error')] },
    { enabled: false },
  ];
  for (const options of cases) {
    const f = fixture(options);
    await f.guard.tick();
    assert.equal(f.calls.length, 0, JSON.stringify(options));
  }
});

test('pending inputs, running agent and missing model prevent automatic resume', async () => {
  for (const setup of [f => { f.agent.status = 'running'; },
    f => { f.agent.inbox.nextTurn.push({}); }, f => { f.agent.model.current = () => null; }]) {
    const f = fixture(); setup(f); await f.guard.tick(); assert.equal(f.calls.length, 0);
  }
});

test('native armed goals remain authoritative and stalled state is visible', async () => {
  const f = fixture({ goalPatch: { activation: 'armed' } });
  assert.equal((await f.guard.view('s')).status, 'armed');
  f.time(170000);
  assert.equal((await f.guard.view('s')).status, 'stalled');
  await f.guard.tick(); assert.equal(f.calls.length, 0);
});

test('manual resume requires current revision and never resumes paused goals', async () => {
  const f = fixture({ events: [end(4, 'aborted', 'user')] });
  await assert.rejects(f.guard.resume('s', { id: 'g', revision: 0 }), /已变化/);
  await f.guard.resume('s', { id: 'g', revision: 1 });
  assert.equal(f.calls.length, 1);
  const paused = fixture({ goalPatch: { phase: 'paused' } });
  await assert.rejects(paused.guard.resume('s', { id: 'g', revision: 1 }), /active/);
});

test('failures back off, stop at three and remain visible instead of blocking audit tick', async () => {
  const f = fixture({ resume: () => { throw new Error('native resume failed'); } });
  await f.guard.tick(); await f.guard.tick(); assert.equal(f.calls.length, 1);
  f.time(200000); await f.guard.tick();
  f.time(400000); await f.guard.tick();
  f.time(800000); await f.guard.tick();
  assert.equal(f.calls.length, 3);
  assert.equal((await f.guard.view('s')).status, 'recovery-failed');
});

test('crash after persisted admission does not repeat an uncertain resume', async () => {
  const f = fixture();
  f.manager.state.sessions.s.guard.pending = { restartSeq: 2 };
  await f.guard.tick(); assert.equal(f.calls.length, 0);
  assert.equal((await f.guard.view('s')).status, 'uncertain');
});

test('guard enable and disable bind one goal and preserve audit schedule', async () => {
  const f = fixture({ enabled: false });
  await f.guard.configure('s', true, { id: 'g', revision: 1 });
  assert.equal(f.manager.state.sessions.s.guard.enabled, true);
  await f.guard.configure('s', false);
  assert.equal(f.manager.state.sessions.s.guard.enabled, false);
  assert.equal(f.manager.state.sessions.s.nextDue, 999999);
});

test('missing native service is visible and closing guard prevents new work', async () => {
  const f = fixture();
  f.guard.goals = () => undefined;
  assert.equal((await f.guard.view('s')).status, 'unsupported');
  await f.guard.close();
  await f.guard.tick(); assert.equal(f.calls.length, 0);
});

test('native state is rechecked after persistence before recovery mutates it', async () => {
  const f = fixture();
  f.manager.persist = async () => { f.goal.phase = 'paused'; f.goal.revision++; };
  await f.guard.tick(); assert.equal(f.calls.length, 0);
});
