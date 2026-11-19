import test from 'node:test'
import assert from 'node:assert/strict'
import { Context } from '@deepseek-ai/cordis'
import { apply, seams } from '../lib/index.js'

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
    assert.equal((await handler(new Request('http://localhost/tps-window/quota?provider=open-design-2'))).status, 401)
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

test('the second OpenDesign route uses its own workspace and never borrows the first key', async (t) => {
  const values = { OPEN_DESIGN_CONTROL_KEY: 'first-fixture-key' }
  const ctx = { credentials: { resolve: async ref => values[ref] === undefined ? undefined : { value: values[ref] } } }
  const requests = []
  const week = { durationSeconds: 604800, usedCredits: '20', limitCredits: '100', resetsAt: '2026-11-20T00:00:00Z' }
  const plan = { workspaceId: 'ws_second_account', eligible: true, windows: [week] }
  t.mock.method(globalThis, 'fetch', async (url, options) => {
    requests.push({ url, ...options.headers })
    if (url.endsWith('/billing/preflight')) return Response.json({ workspaceId: 'ws_second_account', codingPlan: plan })
    return Response.json({ balanceUsd: '5.25' })
  })
  async function readSecond() {
    const service = new seams.QuotaService(ctx)
    return (await service.handle(new Request('http://localhost/tps-window/quota?provider=open-design-2'))).json()
  }
  assert.equal((await readSecond()).reason, 'no-control-key')
  values.OPEN_DESIGN_2_CONTROL_KEY = 'second-fixture-key'
  assert.equal((await readSecond()).reason, 'no-workspace-id')
  assert.equal(requests.length, 0)
  values.OPEN_DESIGN_2_WORKSPACE_ID = 'ws_second_account'
  const result = await readSecond()
  assert.equal(result.status, 'ok')
  assert.deepEqual(result.amounts, [{ label: '额度', value: '$5.25' }])
  assert.deepEqual(result.windows, [{ label: '7天', durationMinutes: 10080, percent: 20,
    resetsAt: Date.parse(week.resetsAt) }])
  assert.deepEqual(requests, ['wallet/balance', 'billing/preflight'].map(path => ({
    url: 'https://amr-api.open-design.ai/api/v1/workspaces/ws_second_account/' + path,
    authorization: 'Bearer second-fixture-key', accept: 'application/json', 'x-vela-workspace-id': 'ws_second_account' })))
  plan.windows.push({ ...week, durationSeconds: 18000 })
  assert.deepEqual((await readSecond()).windows.map(window => window.label), ['5h', '7天'])
  plan.workspaceId = 'ws_first_account'
  const mismatch = await readSecond()
  assert.equal(mismatch.kind, 'amounts')
  assert.deepEqual(mismatch.windows, [])
})

test('a new quota service retries a transient upstream failure with the saved binding', async (t) => {
  let now = Date.now()
  t.mock.method(Date, 'now', () => now)
  const resolve = async ref => {
    assert.equal(ref, 'OPEN_DESIGN_2_QUOTA_BINDING')
    return { value: JSON.stringify({ controlKey: 'vela_ctrl_existing_fixture', workspaceId: 'ws_second' }) }
  }
  const read = () => new seams.QuotaService({ credentials: { resolve } })
  let ready = false
  let requests = 0
  t.mock.method(globalThis, 'fetch', async (url, options) => {
    assert.match(url, /^https:\/\/amr-api\.open-design\.ai\/api\/v1\/workspaces\/ws_second\//)
    assert.equal(options.method, undefined)
    assert.equal(options.headers.authorization, 'Bearer vela_ctrl_existing_fixture')
    assert.equal(options.headers['x-vela-workspace-id'], 'ws_second')
    requests++
    if (!ready) return new Response(null, { status: 503 })
    if (url.endsWith('/billing/preflight')) {
      return Response.json({ workspaceId: 'ws_second', codingPlan: { windows: [
        { durationSeconds: 604800, usedCredits: '21', limitCredits: '100' }
      ] } })
    }
    return Response.json({ balanceUsd: '7.25' })
  })
  const service = read()
  assert.equal((await service.quotaFor('open-design-2')).reason, 'unreachable')
  ready = true
  assert.equal((await service.quotaFor('open-design-2')).reason, 'unreachable')
  assert.equal(requests, 2)
  now += 15000
  const recovered = await service.quotaFor('open-design-2')
  assert.equal(recovered.status, 'ok')
  assert.equal(recovered.windows[0].percent, 21)
  assert.equal(requests, 4)
  assert.equal((await read().quotaFor('open-design-2')).windows[0].percent, 21)
})
