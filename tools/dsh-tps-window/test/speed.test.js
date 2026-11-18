import test from 'node:test'
import assert from 'node:assert/strict'
import { SpeedCollector } from '../lib/speed.js'

function fixture() {
  let now = 110000
  const agents = new Map()
  const history = new Map()
  let leases = 0
  const ctx = {
    agents: { get: id => agents.get(id), list: () => [...agents.values()],
      isOwnedBy: (id, parent) => agents.get(id)?.session.header.parentSession === parent.id },
    sessionQuery: { observeSession: async id => {
      leases++
      return { events: history.get(id) ?? [], [Symbol.dispose]() { leases-- } }
    } }
  }
  function agent(id, parentSession, inherited = 0) {
    const value = { id, session: { id, header: { parentSession }, isOwnSeq: seq => seq >= inherited } }
    agents.set(id, value)
    return value
  }
  const collector = new SpeedCollector(ctx, () => now)
  return { collector, ctx, agent, agents, history, leases: () => leases, time: n => { now = n } }
}

function message(seq, tokens, at = 100000, turn = 1, step = 0) {
  return { type: 'assistant/message', seq, time: 109000, data: { turn, step,
    usage: { outputTokens: tokens }, stream: [{ type: 'chunk', time: at,
      chunk: { type: 'text-delta', text: 'x'.repeat(100) } }] } }
}

test('root aggregates descendants once while each child reports only itself', () => {
  const f = fixture()
  const root = f.agent('root')
  const child = f.agent('child', 'root')
  const sibling = f.agent('sibling', 'root')
  const grandchild = f.agent('grandchild', 'child')
  const other = f.agent('unrelated')
  for (const [a, tokens, at] of [[root, 100, 99000], [child, 200, 100000],
    [sibling, 300, 101000], [grandchild, 400, 102000], [other, 500, 99000]]) {
    f.collector.event(a.session, message(101, tokens, at))
  }
  const total = f.collector.snapshot('root')
  assert.equal(total.scope, 'family')
  assert.equal(total.tokens, 1000)
  assert.equal(total.spanMs, 11000)
  assert.equal(total.members, 4)
  assert.equal(f.collector.snapshot('child').tokens, 200)
  assert.equal(f.collector.snapshot('child').scope, 'self')
  assert.equal(f.collector.snapshot('grandchild').tokens, 400)
})

test('root still reports child output while root generates nothing; retired children age out', () => {
  const f = fixture()
  f.agent('root')
  const child = f.agent('child', 'root')
  f.collector.event(child.session, message(101, 200))
  f.agents.delete('child')
  assert.equal(f.collector.snapshot('root').tokens, 200)
  f.time(161000)
  assert.equal(f.collector.snapshot('root').tokens, 0)
  assert.equal(f.collector.snapshot('root').reading, 0)
})

test('live chunks, final settlement and seed replay do not double count one attempt', () => {
  const f = fixture()
  const a = f.agent('root')
  f.collector.frame(a, { type: 'start', attemptId: 'a', turn: 1, step: 0 })
  const frame = { type: 'chunk', attemptId: 'a', index: 0, time: 100000,
    chunk: { type: 'text-delta', text: 'x'.repeat(100) } }
  f.collector.frame(a, frame)
  f.collector.frame(a, frame)
  assert.equal(f.collector.snapshot('root').tokens, 32)
  assert.equal(f.collector.snapshot('root').estimated, true)
  const event = message(101, 50)
  f.collector.event(a.session, event)
  f.collector.event(a.session, event)
  f.collector.frame(a, { type: 'end', attemptId: 'a', outcome: { kind: 'committed', eventType: 'assistant/message' } })
  f.collector.frame(a, frame)
  assert.equal(f.collector.snapshot('root').tokens, 50)
  assert.equal(f.collector.snapshot('root').estimated, false)
})

test('inherited history is excluded when a forked child is seeded', async () => {
  const f = fixture()
  f.agent('root')
  f.agent('child', 'root', 100)
  f.history.set('child', [message(1, 999), message(101, 50)])
  await f.collector.seed()
  await f.collector.seed()
  assert.equal(f.collector.snapshot('child').tokens, 50)
  assert.equal(f.collector.snapshot('root').tokens, 50)
  assert.equal(f.leases(), 0)
})

test('failed and abandoned attempts are removed without dropping successful output', () => {
  const f = fixture()
  const a = f.agent('root')
  f.collector.event(a.session, message(101, 50))
  f.collector.frame(a, { type: 'start', attemptId: 'retry', turn: 1, step: 1 })
  f.collector.frame(a, { type: 'chunk', attemptId: 'retry', index: 0, time: 105000,
    chunk: { type: 'text-delta', text: 'discarded output' } })
  f.collector.frame(a, { type: 'end', attemptId: 'retry', outcome: { kind: 'abandoned' } })
  assert.equal(f.collector.snapshot('root').tokens, 50)
})

test('capacity or malformed data is unavailable, never silently zero or an incomplete total', () => {
  const f = fixture()
  const a = f.agent('root')
  f.collector.safely(() => f.collector.event(a.session, { ...message(101, 50),
    data: { stream: [{ type: 'invalid' }] } }), a.session)
  assert.equal(f.collector.snapshot('root').status, 'unavailable')
  f.time(171000)
  assert.equal(f.collector.snapshot('root').status, 'ok')
  assert.equal(f.collector.snapshot('root').reading, 0)
})

test('a look-alike fork parent is not a runtime ownership edge', () => {
  const f = fixture()
  f.agent('root')
  const a = f.agent('manual-fork', 'root')
  f.ctx.agents.isOwnedBy = () => false
  f.collector.event(a.session, message(101, 50))
  assert.equal(f.collector.snapshot('root').tokens, 0)
})

test('startup retains captured ancestry if a child exits during history observation', async () => {
  const f = fixture()
  f.agent('root')
  f.agent('child', 'root', 100)
  f.ctx.sessionQuery.observeSession = async id => {
    if (id === 'root') {
      f.agents.delete('child')
      f.collector.gc()
    }
    return { events: id === 'child' ? [message(101, 50)] : [], [Symbol.dispose]() {} }
  }
  await f.collector.seed()
  assert.equal(f.collector.snapshot('root').tokens, 50)
  assert.equal(f.collector.snapshot('child').scope, 'self')
})
