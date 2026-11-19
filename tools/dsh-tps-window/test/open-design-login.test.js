import test from 'node:test'
import assert from 'node:assert/strict'
import { randomUUID } from 'node:crypto'
import { OpenDesignLogin } from '../lib/open-design-login.js'

const device = {
  deviceId: 'device-fixture', deviceSecret: 'never-send-to-browser', userCode: 'ABC123',
  activationUrl: 'https://open-design.ai/cloud/cli/activate?code=ABC123', pollIntervalSeconds: 2
}

test('device flow requires confirmation and only saves the selected route and workspace', async (t) => {
  let now = Date.now()
  t.mock.method(Date, 'now', () => now)
  const writes = []
  const fresh = []
  const calls = []
  t.mock.method(globalThis, 'fetch', async (url, options) => {
    assert.equal(new URL(url).origin, 'https://amr-api.open-design.ai')
    assert.equal(options.redirect, 'error')
    calls.push(url)
    if (url.endsWith('/token')) {
      assert.deepEqual(JSON.parse(options.body), { deviceSecret: device.deviceSecret })
      return Response.json({ controlKey: 'vela_ctrl_fixture', runtimeKey: 'ignored-runtime-key',
        user: { email: 'second@example.invalid' } })
    }
    if (url.endsWith('/workspaces/current')) {
      assert.equal(options.headers.authorization, 'Bearer vela_ctrl_fixture')
      return Response.json({ activeWorkspaceId: 'ws_second' })
    }
    return Response.json(device)
  })
  const login = new OpenDesignLogin({ set: async (...args) => writes.push(args) }, id => fresh.push(id))
  t.after(() => login.close())
  const begin = await login.handle('open-design-2', 'start', randomUUID())
  assert.equal(begin.url, device.activationUrl)
  assert.equal(Object.hasOwn(begin, 'deviceSecret'), false)
  assert.equal((await login.handle('open-design-2', 'start', randomUUID())).status, 'busy')
  assert.equal((await login.handle('open-design', 'poll', begin.id)).status, 'expired')
  assert.equal((await login.handle('open-design-2', 'confirm', begin.id)).status, 'unavailable')
  assert.equal((await login.handle('open-design-2', 'poll', begin.id)).status, 'pending')
  assert.equal(calls.length, 1)
  now += 2001
  assert.deepEqual(await login.handle('open-design-2', 'poll', begin.id),
    { status: 'confirm', account: 'second@example.invalid', workspaceId: 'ws_second' })
  assert.deepEqual(writes, [])
  assert.equal((await login.handle('open-design-2', 'confirm', begin.id)).status, 'saved')
  assert.deepEqual(writes, [['OPEN_DESIGN_2_QUOTA_BINDING',
    '{"controlKey":"vela_ctrl_fixture","workspaceId":"ws_second"}']])
  assert.deepEqual(fresh, ['open-design-2'])
  assert.equal((await login.handle('open-design-2', 'poll', begin.id)).status, 'expired')
})

test('closing or cancelling aborts owned requests and cannot save a late approval', async (t) => {
  let now = Date.now()
  t.mock.method(Date, 'now', () => now)
  let blockedSignal
  t.mock.method(globalThis, 'fetch', async (url, options) => {
    if (!url.endsWith('/token')) return Response.json(device)
    blockedSignal = options.signal
    return new Promise((_, reject) => options.signal.addEventListener('abort', () => reject(options.signal.reason), { once: true }))
  })
  const login = new OpenDesignLogin({ set() { assert.fail('cancelled authorization was saved') } }, () => {})
  t.after(() => login.close())
  let begin = await login.handle('open-design', 'start', randomUUID())
  now += 2001
  const polling = login.handle('open-design', 'poll', begin.id)
  assert.equal((await login.handle('open-design', 'cancel', begin.id)).status, 'cancelled')
  assert.equal(blockedSignal.aborted, true)
  assert.equal((await polling).status, 'expired')
  begin = await login.handle('open-design-2', 'start', randomUUID())
  now += 2001
  const second = login.handle('open-design-2', 'poll', begin.id)
  await login.close()
  assert.equal(blockedSignal.aborted, true)
  assert.equal((await second).status, 'expired')
  assert.equal((await login.handle('open-design-2', 'start')).status, 'unavailable')
})
