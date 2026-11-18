import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import vm from 'node:vm'

test('actual client requests its own session, labels aggregate/self, reports failures and releases polling', async () => {
  const rows = []
  const timers = new Set()
  let cleanup
  let exports
  let payload = { status: 'ok', scope: 'family', reading: 12.5, tokens: 125,
    spanMs: 10000, fresh: true, estimated: false }
  const requests = []
  const sandbox = { AbortController, URL, Number, Map, WeakMap, Promise,
    document: { baseURI: 'http://localhost/dsh/' },
    fetch: async url => {
      requests.push(url)
      return { ok: true, json: async () => payload }
    },
    window: {
      setInterval: fn => { timers.add(fn); return fn },
      clearInterval: fn => timers.delete(fn), setTimeout: () => 1, clearTimeout() {},
      __ModuleLoader__: { load({ factory }) { exports = factory(() => ({
        createElement: (type, props, ...children) => ({ type, props, children })
      })) } }
    }
  }
  vm.runInNewContext(await readFile(new URL('../lib/client.js', import.meta.url), 'utf8'), sandbox)
  const ctx = { effect: effect => { cleanup = effect() },
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
  assert.equal(timers.size, 0)
  cleanup()
})
