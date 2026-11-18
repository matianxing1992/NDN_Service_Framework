// dsh-tps-window client half: a 60-second rolling output-speed pill.
//
// The built-in activity pill divides the whole session's decode tokens by its
// whole decode time, so on a long session that reading stops describing what the
// model is doing right now. This plugin adds one extra
// `conversation.composer.dock` control — order 0.5 puts it directly to the right
// of the built-in speed reading and left of the cache-hit reading — that reports
// only the last 60 seconds.
//
// Streaming carries no token counts: adapters emit `usage` when a step settles and
// stream text deltas in between. So a live sample counts streamed characters and
// weights them into tokens. While a step runs that weight is the running estimate;
// when the step settles its real `usage.outputTokens` recomputes the weight for
// that step's own samples, so settled contributions become exact and only the
// in-flight step stays an estimate.
window.__ModuleLoader__.load({ id: 'dsh-tps-window', factory: (require) => {
  var module = { exports: {} }
  var exports = module.exports
  var React = require('react')

  var STORE_TICK_MS = 1000
  var stores = new WeakMap()

  function speedUrl(sessionId) {
    var relative = 'tps-window/speed?sessionId=' + encodeURIComponent(sessionId)
    return typeof document !== 'undefined' ? new URL(relative, document.baseURI).href : '/' + relative
  }

  // The Host owns counts, even when a child's page is closed.
  function WindowStore(ctx, sessionId) {
    this.ctx = ctx
    this.sessionId = sessionId
    this.value = { reading: null, fresh: false, error: '正在读取统计' }
    this.listeners = []
    this.timer = null
    this.controller = null
    this.generation = 0
  }

  WindowStore.prototype.getSnapshot = function () { return this.value }
  WindowStore.prototype.subscribe = function (listener) {
    this.listeners.push(listener)
    this.start()
    var self = this
    return function () {
      var at = self.listeners.indexOf(listener)
      if (at >= 0) self.listeners.splice(at, 1)
      if (self.listeners.length === 0) self.stop()
    }
  }
  WindowStore.prototype.publish = function (value) {
    this.value = value
    this.listeners.slice().forEach(function (listener) { listener() })
  }
  WindowStore.prototype.tick = function () {
    if (this.controller !== null) return
    var self = this
    var generation = this.generation
    var controller = new AbortController()
    this.controller = controller
    var timeout = window.setTimeout(function () { controller.abort() }, 5000)
    Promise.resolve().then(function () {
      return fetch(speedUrl(self.sessionId), { credentials: 'same-origin',
        headers: { accept: 'application/json' }, signal: controller.signal })
    }).then(function (response) {
      if (!response.ok) throw new Error('HTTP ' + response.status)
      return response.json()
    }).then(function (value) {
      if (generation !== self.generation) return
      if (value.status !== 'ok' || !Number.isFinite(value.reading) || value.reading < 0) {
        throw new Error(value.reason || '统计响应无效')
      }
      self.publish(value)
    }).catch(function (error) {
      if (generation === self.generation) self.publish({ reading: null, fresh: false,
        error: '统计读取失败：' + error.message })
    }).finally(function () {
      window.clearTimeout(timeout)
      if (self.controller === controller) self.controller = null
    })
  }
  WindowStore.prototype.start = function () {
    if (this.timer !== null) return
    this.generation++
    var self = this
    this.timer = window.setInterval(function () { self.tick() }, STORE_TICK_MS)
    this.tick()
  }
  WindowStore.prototype.stop = function () {
    this.generation++
    if (this.timer !== null) window.clearInterval(this.timer)
    this.timer = null
    this.controller?.abort()
    this.controller = null
    this.listeners = []
  }

  function storeFor(ctx, sessionId) {
    var perContext = stores.get(ctx)
    if (!perContext) { perContext = new Map(); stores.set(ctx, perContext) }
    var store = perContext.get(sessionId)
    if (!store) {
      if (perContext.size >= 128) {
        for (var pair of perContext) {
          if (pair[1].timer === null) { perContext.delete(pair[0]); break }
        }
        if (perContext.size >= 128) throw new Error('速度统计视图数量超过上限')
      }
      store = new WindowStore(ctx, sessionId)
      perContext.set(sessionId, store)
    }
    return store
  }

  var PILL_STYLE = {
    boxSizing: 'border-box',
    borderRadius: 'var(--dsw-radius-sm)',
    color: 'var(--dsw-alias-label-tertiary)',
    font: 'inherit',
    fontVariantNumeric: 'tabular-nums',
    lineHeight: 'inherit',
    whiteSpace: 'nowrap',
    background: 'none',
    border: 'none',
    alignItems: 'center',
    display: 'inline-flex',
    gap: '6px',
    padding: '1px 8px'
  }

  // Dim while there is no fresh reading, but never unmount: a pill that vanishes
  // and reappears is read as a bug, and it also loses its dock position.
  var IDLE_PILL_STYLE = Object.assign({}, PILL_STYLE, { opacity: 0.45 })

  function clockIcon() {
    return React.createElement(
      'svg',
      {
        viewBox: '0 0 16 16',
        width: 14,
        height: 14,
        'aria-hidden': true,
        fill: 'none',
        stroke: 'currentColor',
        strokeWidth: 1.4,
        strokeLinecap: 'round',
        style: { flex: 'none' },
        key: 'icon'
      },
      React.createElement('circle', { cx: 8, cy: 8, r: 6, key: 'face' }),
      React.createElement('path', { d: 'M8 4.6V8l2.5 1.6', key: 'hands' })
    )
  }


  function TpsWindowPill(props) {
    var state = props.useTpsWindow(function (value) { return value })
    var reading = state?.reading
    var valid = Number.isFinite(reading) && reading >= 0
    var scope = state?.scope === 'family' ? '主代理及子代理合计' : '仅当前子代理自身'
    var title = valid ? scope + '；最近 ' + (state.spanMs / 1000).toFixed(1) +
      ' 秒输出 ' + Math.round(state.tokens) + ' tok；同一时间窗口统计，不相加各代理分母。' +
      (state.estimated ? '包含尚未结算的流式估算。' : '按实际 usage 校准。') :
      state?.error || '正在读取最近 60 秒统计'
    return React.createElement('span', {
      style: state?.fresh ? PILL_STYLE : IDLE_PILL_STYLE,
      'data-composer-stat': 'tps-window', 'data-tps-scope': state?.scope, title: title
    }, clockIcon(),
      React.createElement('span', { style: { opacity: 0.7 }, key: 'window' },
        state?.scope === 'family' ? '60s 合计' : '60s 自身'),
      React.createElement('span', { key: 'value' },
        valid ? (Math.round(reading * 10) / 10) + ' tok/s' : '— tok/s'))
  }
  // #region quota-view
  var QUOTA_VIEW = {
    EMPTY: { visible: false, text: '', title: '', provider: null, label: null, window: null, percent: null, resetsAt: null },

    /** The window closest to its limit, which is the one that stops the next request. */
    tightest: function (windows) {
      if (windows === null || typeof windows !== 'object' || windows.length === 0) return null
      var best = null
      for (var i = 0; i < windows.length; i++) {
        var window = windows[i]
        if (window === null || typeof window !== 'object') continue
        var percent = typeof window.percent === 'number' && isFinite(window.percent) ? window.percent : null
        if (percent === null) continue
        if (best === null || percent > best.percent) {
          best = {
            label: typeof window.label === 'string' && window.label.length > 0 ? window.label : '限额',
            percent: percent,
            resetsAt: typeof window.resetsAt === 'number' && isFinite(window.resetsAt) ? window.resetsAt : null
          }
        }
      }
      return best
    },

    pad: function (value) {
      return value < 10 ? '0' + String(value) : String(value)
    },

    /** Local clock reading `HH:MM`; the pill's "time" is the reset moment. */
    clock: function (ms) {
      if (typeof ms !== 'number' || !isFinite(ms)) return null
      var date = new Date(ms)
      return QUOTA_VIEW.pad(date.getHours()) + ':' + QUOTA_VIEW.pad(date.getMinutes())
    },

    /** Full local timestamp for the tooltip, where the clock alone would be ambiguous. */
    stamp: function (ms) {
      if (typeof ms !== 'number' || !isFinite(ms)) return null
      var date = new Date(ms)
      return (
        String(date.getFullYear()) +
        '-' +
        QUOTA_VIEW.pad(date.getMonth() + 1) +
        '-' +
        QUOTA_VIEW.pad(date.getDate()) +
        ' ' +
        QUOTA_VIEW.clock(ms)
      )
    },

    /**
     * Host payload -> what the pill shows. Anything other than a reading hides the
     * control: a provider without a readable limit API must not invent one.
     */
    view: function (payload) {
      if (payload === null || typeof payload !== 'object') return QUOTA_VIEW.EMPTY
      if (payload.status !== 'ok') return QUOTA_VIEW.EMPTY
      var label = typeof payload.label === 'string' && payload.label.length > 0 ? payload.label : '限额'
      var detail = payload.detail === null || typeof payload.detail !== 'object' ? {} : payload.detail
      var updated = QUOTA_VIEW.stamp(typeof payload.updatedAt === 'number' ? payload.updatedAt : null)

      // Amounts reading (Open Design): labelled USD figures, no percent windows.
      if (payload.kind === 'amounts') {
        var amounts = []
        if (Array.isArray(payload.amounts)) {
          for (var a = 0; a < payload.amounts.length; a++) {
            var amount = payload.amounts[a]
            if (amount === null || typeof amount !== 'object') continue
            if (typeof amount.value !== 'string' || amount.value.length === 0) continue
            amounts.push({ label: typeof amount.label === 'string' ? amount.label : '', value: amount.value })
          }
        }
        if (amounts.length === 0) return QUOTA_VIEW.EMPTY
        var figures = []
        for (var f = 0; f < amounts.length; f++) figures.push(amounts[f].label === '' ? amounts[f].value : amounts[f].label + ' ' + amounts[f].value)
        var amountLines = []
        for (var l = 0; l < amounts.length; l++) amountLines.push((amounts[l].label === '' ? '金额' : amounts[l].label) + ' ' + amounts[l].value)
        if (typeof detail.plan === 'string') amountLines.push('plan ' + detail.plan + (typeof detail.status === 'string' ? ' (' + detail.status + ')' : ''))
        if (typeof detail.creditsPerUsd === 'number') amountLines.push('creditsPerUsd ' + String(detail.creditsPerUsd))
        var periodEnd = QUOTA_VIEW.stamp(typeof detail.periodEnd === 'number' ? detail.periodEnd : null)
        if (periodEnd !== null) amountLines.push('当前周期至 ' + periodEnd)
        if (updated !== null) amountLines.push('更新 ' + updated)
        return {
          visible: true,
          text: label + ' ' + figures.join(' · '),
          title: label + ' 账户' + (amountLines.length === 0 ? '' : '\n' + amountLines.join('\n')),
          provider: typeof payload.provider === 'string' ? payload.provider : null,
          label: label,
          window: null,
          percent: null,
          resetsAt: typeof detail.periodEnd === 'number' ? detail.periodEnd : null
        }
      }

      var windows = Array.isArray(payload.windows) ? payload.windows : []
      var tightest = QUOTA_VIEW.tightest(windows)
      if (tightest === null) return QUOTA_VIEW.EMPTY
      var reset = QUOTA_VIEW.clock(tightest.resetsAt)
      var text = label + ' ' + tightest.label + ' ' + String(Math.round(tightest.percent)) + '%' + (reset === null ? '' : ' · ' + reset)
      var lines = []
      for (var i = 0; i < windows.length; i++) {
        var window = windows[i]
        if (window === null || typeof window !== 'object') continue
        var percent = typeof window.percent === 'number' && isFinite(window.percent) ? Math.round(window.percent) : null
        if (percent === null) continue
        var windowLabel = typeof window.label === 'string' && window.label.length > 0 ? window.label : '限额'
        var when = typeof window.resetsAt === 'number' && isFinite(window.resetsAt) ? QUOTA_VIEW.stamp(window.resetsAt) : null
        lines.push(windowLabel + ' ' + String(percent) + '%' + (when === null ? '' : ' · 重置 ' + when))
      }
      if (typeof detail.planType === 'string' && detail.planType.length > 0) lines.push('plan ' + detail.planType)
      if (typeof detail.credits === 'string' && detail.credits.length > 0) lines.push('余额 ' + detail.credits)
      if (updated !== null) lines.push('更新 ' + updated)
      return {
        visible: true,
        text: text,
        title: label + ' 用量' + (lines.length === 0 ? '' : '\n' + lines.join('\n')),
        provider: typeof payload.provider === 'string' ? payload.provider : null,
        label: label,
        window: tightest.label,
        percent: Math.round(tightest.percent),
        resetsAt: tightest.resetsAt
      }
    }
  }
  // #endregion quota-view

  var QUOTA_REFRESH_MS = 300000
  var QUOTA_TICK_MS = 250

  /** Same-origin URL of the host half's quota route, honouring a served base path. */
  function quotaUrl(provider) {
    var relative = 'tps-window/quota?provider=' + encodeURIComponent(provider)
    if (typeof document !== 'undefined' && typeof document.baseURI === 'string') {
      try {
        return new URL(relative, document.baseURI).href
      } catch (_error) {
        // A malformed base URL falls through to the document-relative form.
      }
    }
    return '/' + relative
  }

  /**
   * Per-session provider quota. The provider comes from the session's
   * `modelSelection` projection, so the pill follows the model the next step
   * actually uses; the numbers come from the host half, never from the page.
   */
  function QuotaStore(ctx, sessionId) {
    this.ctx = ctx
    this.sessionId = sessionId
    this.value = QUOTA_VIEW.EMPTY
    this.listeners = []
    this.binding = undefined
    this.selection = undefined
    this.unsubscribe = null
    this.provider = null
    this.payload = null
    this.fetchedAt = 0
    this.inflight = false
    this.timer = null
  }

  QuotaStore.prototype.getSnapshot = function () {
    return this.value
  }

  QuotaStore.prototype.subscribe = function (listener) {
    this.listeners.push(listener)
    var self = this
    return function () {
      var at = self.listeners.indexOf(listener)
      if (at >= 0) self.listeners.splice(at, 1)
    }
  }

  QuotaStore.prototype.publish = function () {
    var next = this.payload === null ? QUOTA_VIEW.EMPTY : QUOTA_VIEW.view(this.payload)
    var previous = this.value
    if (previous.visible === next.visible && previous.text === next.text && previous.title === next.title) return
    this.value = next
    for (var i = 0; i < this.listeners.length; i++) this.listeners[i]()
  }

  QuotaStore.prototype.attach = function () {
    var binding
    try {
      binding = this.ctx.sessions.binding(this.sessionId)
    } catch (_error) {
      return
    }
    if (binding === this.binding) return
    this.detachSource()
    this.binding = binding
    if (binding === undefined || binding === null) return
    var session = binding.session
    if (session === undefined || session === null || session.projections === undefined) return
    var source
    try {
      source = session.projections.faceOf('modelSelection')
    } catch (_error) {
      return
    }
    if (source === undefined || source === null) return
    this.selection = source
    var self = this
    this.unsubscribe = source.subscribe(function () {
      self.tick()
    })
  }

  QuotaStore.prototype.detachSource = function () {
    if (this.unsubscribe !== null) {
      this.unsubscribe()
      this.unsubscribe = null
    }
    this.selection = undefined
  }

  /** The provider route of the model the next step uses, or null. */
  QuotaStore.prototype.currentProvider = function () {
    if (this.selection === undefined) return null
    var value = this.selection.getSnapshot()
    if (value === null || typeof value !== 'object') return null
    var current = value.next !== undefined && value.next !== null ? value.next : value.lastUsed
    if (current === null || typeof current !== 'object') return null
    return typeof current.provider === 'string' && current.provider.length > 0 ? current.provider : null
  }

  QuotaStore.prototype.fetch = function () {
    var self = this
    var provider = this.provider
    this.inflight = true
    Promise.resolve()
      .then(function () {
        if (typeof fetch !== 'function') return null
        return fetch(quotaUrl(provider), { headers: { accept: 'application/json' }, credentials: 'same-origin' })
      })
      .then(function (response) {
        if (response === null || response === undefined) return null
        if (!response.ok) return null
        return response.json()
      })
      .then(function (payload) {
        if (self.provider !== provider) return
        self.payload = payload !== null && typeof payload === 'object' ? payload : { status: 'unavailable' }
        self.fetchedAt = Date.now()
        self.publish()
      })
      .catch(function () {
        // A failed read is held for the same TTL as a successful one: the next tick
        // must not hammer a route that just failed.
        if (self.provider !== provider) return
        self.payload = { status: 'unavailable' }
        self.fetchedAt = Date.now()
        self.publish()
      })
      .then(function () {
        self.inflight = false
      })
  }

  QuotaStore.prototype.tick = function () {
    this.attach()
    var provider = this.currentProvider()
    if (provider === null) {
      this.provider = null
      this.payload = null
      this.publish()
      return
    }
    if (provider !== this.provider) {
      this.provider = provider
      this.payload = null
      this.fetchedAt = 0
    }
    if (!this.inflight && (this.payload === null || Date.now() - this.fetchedAt >= QUOTA_REFRESH_MS)) this.fetch()
    this.publish()
  }

  QuotaStore.prototype.start = function () {
    if (this.timer !== null) {
      this.attach()
      return
    }
    var self = this
    this.attach()
    this.timer = window.setInterval(function () {
      self.tick()
    }, QUOTA_TICK_MS)
    this.tick()
  }

  QuotaStore.prototype.stop = function () {
    if (this.timer !== null) {
      window.clearInterval(this.timer)
      this.timer = null
    }
    this.detachSource()
    this.listeners = []
    this.inflight = false
    this.payload = null
  }

  var quotaStores = new WeakMap()

  function quotaStoreFor(ctx, sessionId) {
    var perContext = quotaStores.get(ctx)
    if (perContext === undefined) {
      perContext = new Map()
      quotaStores.set(ctx, perContext)
    }
    var store = perContext.get(sessionId)
    if (store === undefined) {
      store = new QuotaStore(ctx, sessionId)
      perContext.set(sessionId, store)
    }
    return store
  }

  function TpsQuotaPill(props) {
    var state = props.useQuotaWindow(function (value) {
      return value
    })
    if (state === null || state === undefined || state.visible !== true) return null
    return React.createElement(
      'span',
      {
        style: PILL_STYLE,
        'data-composer-stat': 'tps-quota',
        title: state.title
      },
      React.createElement('span', { key: 'text' }, state.text)
    )
  }

  function apply(ctx) {
    ctx.effect(function () {
      return function () {
        var perContext = stores.get(ctx)
        if (perContext !== undefined) {
          perContext.forEach(function (store) {
            store.stop()
          })
          perContext.clear()
        }
        var quotas = quotaStores.get(ctx)
        if (quotas !== undefined) {
          quotas.forEach(function (store) {
            store.stop()
          })
          quotas.clear()
        }
      }
    })

    ctx.slots.inject('conversation.composer.dock', function* () {
      yield ctx.slots.register(
        {
          name: 'conversation.composer.dock',
          // A distinct id adds a control instead of replacing one: replacement
          // compares id and order together.
          id: 'tps-window',
          // Between the built-in activity reading (order 0) and the usage reading
          // (order 1): the dock lays rows out by ascending order. Like the two
          // built-in controls, this entry declares no scope of its own — the dock
          // slot is session-scoped, so the renderer hands it the session binding and
          // calls `inject` with that session id.
          order: 0.5,
          inject: function (sessionId) {
            var store = storeFor(ctx, sessionId)
            store.start()
            return {
              hooks: {
                tpsWindow: store
              }
            }
          }
        },
        TpsWindowPill
      )
      yield ctx.slots.register(
        {
          name: 'conversation.composer.dock',
          // Immediately after the 60-second speed reading: quota is about how much
          // of the current provider is left, so it belongs beside the speed.
          id: 'tps-quota',
          order: 0.6,
          inject: function (sessionId) {
            var store = quotaStoreFor(ctx, sessionId)
            store.start()
            return {
              hooks: {
                quotaWindow: store
              }
            }
          }
        },
        TpsQuotaPill
      )
    })
  }

  exports.apply = apply
  exports.inject = ['slots', 'sessions']
  return module.exports
} })
