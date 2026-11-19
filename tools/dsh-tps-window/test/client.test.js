import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import vm from 'node:vm'
import { webcrypto } from 'node:crypto'

test('actual client requests its own session, labels aggregate/self, reports failures and releases polling', async () => {
  const rows = []
  const timers = new Set()
  let cleanup
  let exports
  let payload = { status: 'ok', scope: 'family', reading: 12.5, tokens: 125,
    spanMs: 10000, fresh: true, estimated: false }
  const day = 86400000
  const updatedAt = Date.UTC(2026, 10, 18, 12)
  let quota = { status: 'ok', label: 'Test quota', updatedAt, windows: [
    { label: '5h', percent: 10, durationMinutes: 120 },
    { label: '周', percent: 20, resetsAt: updatedAt + 5 * day },
    { label: '月', percent: 25, resetsAt: updatedAt + 20 * day },
    { label: '天', percent: 5 }
  ] }
  const requests = []
  const opened = []
  let provider = 'test-provider'
  let loginId
  let quotaHttpStatus = 200
  let now = Date.now()
  const sandbox = { AbortController, AbortSignal, crypto: webcrypto, URL, Number, Map, WeakMap, Promise,
    Date: class extends Date { static now() { return now } },
    document: { baseURI: 'http://localhost/dsh/' },
    fetch: async (url, options) => {
      requests.push(url)
      if (url.includes('/open-design-login?')) {
        assert.equal(options.method, 'POST')
        assert.equal(options.headers['x-dsh-tps-window'], 'device-login')
        const query = new URL(url).searchParams
        assert.equal(query.get('provider'), 'open-design-2')
        const action = query.get('action')
        if (action === 'start') {
          loginId = query.get('id')
          assert.match(loginId, /^[a-f0-9-]{36}$/)
          return { ok: true, json: async () => ({ status: 'pending', id: loginId,
            url: 'https://open-design.ai/cloud/cli/activate', code: 'ABC123', expiresAt: Date.now() + day }) }
        }
        assert.equal(query.get('id'), loginId)
        if (action === 'poll') return { ok: true, json: async () => ({ status: 'confirm',
          account: 'two@example.invalid', workspaceId: 'ws_two' }) }
        assert.equal(action, 'cancel')
        return { ok: true, json: async () => ({ status: 'cancelled' }) }
      }
      if (url.includes('/quota?') && quotaHttpStatus !== 200) return { ok: false, status: quotaHttpStatus }
      return { ok: true, json: async () => url.includes('/quota?') ? quota : payload }
    },
    window: {
      open: (...args) => { opened.push(args); return null },
      setInterval: fn => { timers.add(fn); return fn },
      clearInterval: fn => timers.delete(fn), setTimeout: () => 1, clearTimeout() {},
      __ModuleLoader__: { load({ factory }) { exports = factory(() => ({
        createElement: (type, props, ...children) => ({ type, props, children })
      })) } }
    }
  }
  vm.runInNewContext(await readFile(new URL('../lib/client.js', import.meta.url), 'utf8'), sandbox)
  const ctx = { effect: effect => { cleanup = effect() },
    sessions: { binding: () => ({ session: { projections: { faceOf: () => ({
      getSnapshot: () => ({ next: { provider } }), subscribe: () => () => {}
    }) } } }) },
    slots: { register: (definition, component) => { rows.push({ definition, component }); return definition },
      inject: (name, generator) => { [...generator()] } } }
  exports.apply(ctx)
  const speed = rows.find(row => row.definition.id === 'tps-window')
  const hook = speed.definition.inject('parent').hooks.tpsWindow
  const stop = hook.subscribe(() => {})
  await new Promise(resolve => setImmediate(resolve))
  assert.equal(requests[0], 'http://localhost/dsh/tps-window/speed?sessionId=parent')
  let element = speed.component({ useTpsWindow: selector => selector(hook.getSnapshot()) })
  assert.equal(element.props['data-tps-scope'], 'family')
  assert.ok(element.children.some(child => child?.children?.includes('60s 合计')))
  payload = { ...payload, scope: 'self', reading: 0, tokens: 0, fresh: false }
  hook.tick()
  await new Promise(resolve => setImmediate(resolve))
  element = speed.component({ useTpsWindow: selector => selector(hook.getSnapshot()) })
  assert.ok(element.children.some(child => child?.children?.includes('0 tok/s')))
  assert.ok(element.children.some(child => child?.children?.includes('60s 自身')))
  payload = { status: 'unavailable', reason: 'capacity' }
  hook.tick()
  await new Promise(resolve => setImmediate(resolve))
  element = speed.component({ useTpsWindow: selector => selector(hook.getSnapshot()) })
  assert.match(element.props.title, /capacity/)
  assert.ok(element.children.some(child => child?.children?.includes('— tok/s')))
  stop()
  const quotaSlot = rows.find(row => row.definition.id === 'tps-quota')
  const quotaHook = quotaSlot.definition.inject('parent').hooks.quotaWindow
  await new Promise(resolve => setImmediate(resolve))
  const quotaElement = quotaSlot.component({ useQuotaWindow: selector => selector(quotaHook.getSnapshot()) })
  assert.match(quotaElement.props.title, /5h 10%/)
  assert.match(quotaElement.props.title, /周 20%/)
  assert.match(quotaElement.props.title, /月 25%/)
  assert.deepEqual(quotaElement.props.title.match(/Averaging .* a day/g), ['Averaging 2.5% a day'])
  quota = { status: 'ok', label: 'OD 2', updatedAt, amounts: [{ label: '额度', value: '$5.25' }],
    windows: [{ label: '7天', percent: 20, durationMinutes: 10080, resetsAt: updatedAt + 5 * day }] }
  now += 300000
  quotaHook.tick()
  await new Promise(resolve => setImmediate(resolve))
  const odPill = quotaSlot.component({ useQuotaWindow: select => select(quotaHook.getSnapshot()) })
  assert.equal(odPill.children[0].children[0], 'OD 2 7天 20% · 额度 $5.25')
  assert.match(odPill.props.title, /Averaging 10% a day/)
  assert.doesNotMatch(odPill.props.title, /5h/)
  provider = 'open-design-2'
  const renderQuota = () => quotaSlot.component({ useQuotaWindow: select => select(quotaHook.getSnapshot()) })
  quotaHttpStatus = 401
  quotaHook.tick()
  await new Promise(resolve => setImmediate(resolve))
  assert.equal(renderQuota()?.props['data-composer-stat'], 'tps-quota')
  assert.match(renderQuota()?.children[0].children[0], /正在重试/)
  const beforeRetry = requests.length
  quotaHook.tick()
  await new Promise(resolve => setImmediate(resolve))
  assert.equal(requests.length, beforeRetry)

  quotaHttpStatus = 200
  quota = { status: 'unavailable', reason: 'unreachable' }
  now += 15000
  quotaHook.tick()
  await new Promise(resolve => setImmediate(resolve))
  assert.equal(renderQuota()?.props['data-composer-stat'], 'tps-quota')
  quota = { status: 'ok', label: 'OD 2', updatedAt, windows: [{ label: '7天', percent: 21 }] }
  now += 15000
  quotaHook.tick()
  await new Promise(resolve => setImmediate(resolve))
  assert.match(renderQuota()?.children[0].children[0], /OD 2.*21%/)
  assert.equal(requests.some(url => url.includes('/open-design-login?')), false)

  quota = { status: 'unavailable', reason: 'no-control-key' }
  now += 300000
  quotaHook.tick()
  await new Promise(resolve => setImmediate(resolve))
  renderQuota().children.find(child => child?.children?.includes('连接 OpenDesign 额度 · open-design-2')).props.onClick()
  await new Promise(resolve => setImmediate(resolve))
  const link = renderQuota().children.find(child => child?.type === 'a')
  assert.equal(link.props.href, 'https://open-design.ai/cloud/cli/activate')
  let prevented = false
  link.props.onClick({ preventDefault() { prevented = true } })
  assert.equal(prevented, true)
  assert.deepEqual(opened, [[link.props.href, '_blank', 'noopener,noreferrer']])
  const manual = renderQuota().children.find(child => child?.type === 'details')
  assert.equal(manual.children.find(child => child.type === 'input').props.value, link.props.href)
  quotaHook.nextAuthPoll = 0
  quotaHook.tick()
  await new Promise(resolve => setImmediate(resolve))
  assert.ok(renderQuota().children.includes('open-design-2 · two@example.invalid · ws_two'))
  provider = 'open-design'
  quotaHook.tick()
  await new Promise(resolve => setImmediate(resolve))
  assert.ok(requests.some(url => url.includes('provider=open-design-2&action=cancel')))
  assert.ok(renderQuota().children.some(child => child?.children?.includes('连接 OpenDesign 额度 · open-design')))
  cleanup()
  assert.equal(timers.size, 0)
})
