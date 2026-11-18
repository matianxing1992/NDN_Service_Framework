import test from 'node:test'
import assert from 'node:assert/strict'
import { Context } from '@deepseek-ai/cordis'
import { apply } from '../lib/index.js'

test('host global event feed works without any child view; speed route is authenticated', async () => {
  const ctx = new Context()
  const now = Date.now()
  const parent = { id: 'parent', session: { id: 'parent', header: {}, isOwnSeq: () => true } }
  const child = { id: 'child', session: { id: 'child', header: { parentSession: 'parent' },
    isOwnSeq: seq => seq >= 100 } }
  const agents = new Map([['parent', parent], ['child', child]])
  let handler
  let rejected = true
  let leases = 0
  ctx.provide('credentials', { resolve: async () => undefined })
  ctx.provide('logger', { warn() {}, error() {} })
  ctx.provide('agents', { get: id => agents.get(id), list: () => [...agents.values()],
    isOwnedBy: (id, owner) => id === 'child' && owner === parent })
  ctx.provide('connection', { requestRejection: () => rejected ? 401 : undefined })
  ctx.provide('webServer', { register: route => { handler = route.resource; return () => {} } })
  ctx.provide('sessionQuery', { observeSession: async () => {
    leases++
    return { events: [], [Symbol.dispose]() { leases-- } }
  } })
  try {
    await apply(ctx)
    const request = id => new Request('http://localhost/tps-window/speed?sessionId=' + id)
    assert.equal((await handler(request('parent'))).status, 401)
    rejected = false
    ctx.emit('agent/assistant-stream', { agent: child,
      frame: { type: 'start', attemptId: 'test', turn: 1, step: 0 } })
    ctx.emit('agent/assistant-stream', { agent: child, frame: { type: 'chunk',
      attemptId: 'test', index: 0, time: now, chunk: { type: 'text-delta', text: 'x'.repeat(100) } } })
    let value = await (await handler(request('parent'))).json()
    assert.equal(value.tokens, 32)
    assert.equal(value.scope, 'family')
    ctx.emit('session/event', child.session, { type: 'assistant/message', seq: 100, time: now,
      data: { turn: 1, step: 0, usage: { outputTokens: 50 },
        stream: [{ type: 'chunk', time: now, chunk: { type: 'text-delta', text: 'x'.repeat(100) } }] } })
    value = await (await handler(request('parent'))).json()
    const own = await (await handler(request('child'))).json()
    assert.equal(value.tokens, 50)
    assert.equal(own.tokens, 50)
    assert.equal(own.scope, 'self')
    assert.equal(leases, 0)
    assert.equal((await handler(new Request('http://localhost/tps-window/speed'))).status, 400)
  } finally { await ctx.fiber.dispose() }
})
