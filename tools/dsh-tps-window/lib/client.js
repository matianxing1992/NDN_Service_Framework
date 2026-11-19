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

    durationMinutes: function (window, label) {
      if (Number.isFinite(window.durationMinutes) && window.durationMinutes > 0) return window.durationMinutes
      return { '5h': 300, '天': 1440, '周': 10080, '7天': 10080, '月': 43200 }[label]
    },

    /** Normalize the longest window's usage to a day at the time of observation. */
    dailyAverage: function (window, minutes, updatedAt) {
      var day = 24 * 60
      if (minutes >= day) {
        if (!Number.isFinite(updatedAt) || !Number.isFinite(window.resetsAt)) return null
        minutes -= (window.resetsAt - updatedAt) / 60000
        if (updatedAt > window.resetsAt || minutes <= 0) return null
      }
      return (window.percent * day / minutes).toLocaleString('en-US', { maximumFractionDigits: 2 })
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
        if (detail.windowStatus === 'unavailable') amountLines.push('周期用量暂不可用')
        if (detail.windowStatus === 'no-windows') amountLines.push('未返回周期用量')
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
      var summaries = []
      var longest = null
      var longestMinutes = 0
      for (var i = 0; i < windows.length; i++) {
        var window = windows[i]
        if (window === null || typeof window !== 'object') continue
        var percent = typeof window.percent === 'number' && isFinite(window.percent) ? Math.round(window.percent) : null
        if (percent === null) continue
        var windowLabel = typeof window.label === 'string' && window.label.length > 0 ? window.label : '限额'
        var when = typeof window.resetsAt === 'number' && isFinite(window.resetsAt) ? QUOTA_VIEW.stamp(window.resetsAt) : null
        summaries.push(windowLabel + ' ' + String(percent) + '%')
        lines.push(windowLabel + ' ' + String(percent) + '%' + (when === null ? '' : ' · 重置 ' + when))
        var duration = QUOTA_VIEW.durationMinutes(window, windowLabel)
        if (duration > longestMinutes) {
          longestMinutes = duration
          longest = window
        }
      }
      if (longest !== null) {
        var average = QUOTA_VIEW.dailyAverage(longest, longestMinutes, payload.updatedAt)
        if (average !== null) lines.push('Averaging ' + average + '% a day')
      }
      if (Array.isArray(payload.amounts)) {
        for (var j = 0; j < payload.amounts.length; j++) {
          var figure = payload.amounts[j]
          if (typeof figure?.value !== 'string' || figure.value.length === 0) continue
          var summary = (figure.label || '金额') + ' ' + figure.value
          summaries.push(summary)
          lines.push(summary)
        }
        text = label + ' ' + summaries.join(' · ')
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
  var QUOTA_RETRY_MS = 15000
  var QUOTA_TICK_MS = 250
  var QUOTA_AUTH_READY = {}

  function isOpenDesign(provider) {
    return provider === 'open-design' || provider === 'open-design-2'
  }

  function shouldRetryQuota(payload) {
    return payload?.reason === 'transport' || payload?.reason === 'unreachable' ||
      payload?.reason === 'error' || payload?.detail?.windowStatus === 'unavailable'
  }

  /** Same-origin URL of the host half's quota route, honouring a served base path. */
  function quotaUrl(provider) {
    return ownUrl('tps-window/quota?provider=' + encodeURIComponent(provider))
  }

  function ownUrl(relative) {
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
    this.refreshAt = 0
    this.request = null
    this.timer = null
    this.authorization = null
    this.authRequest = null
    this.nextAuthPoll = 0
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
    var canAuthorize = isOpenDesign(this.provider)
    var reason = this.payload?.reason
    var missing = reason === 'no-control-key' || reason === 'no-workspace-id'
    var rejected = reason === 'unauthorized' || reason === 'invalid-binding' || reason === 'invalid-workspace-id'
    if (canAuthorize && (this.authorization || missing || rejected)) {
      var self = this
      var provider = this.provider
      next = Object.assign({}, next, { visible: true, provider: this.provider,
        authorization: this.authorization || QUOTA_AUTH_READY,
        authorizationLabel: rejected ? '重新连接额度' : '连接 OpenDesign 额度', authorize: function (action) {
        if (self.provider === provider) self.authorize(action)
      } })
    } else if (canAuthorize && !next.visible && shouldRetryQuota(this.payload)) {
      next = Object.assign({}, next, { visible: true, provider: this.provider,
        text: '额度暂不可用，正在重试', title: '将使用已保存的凭据重新读取额度。' })
    }
    var previous = this.value
    if (previous.visible === next.visible && previous.text === next.text && previous.title === next.title &&
        previous.provider === next.provider && previous.authorizationLabel === next.authorizationLabel &&
        previous.authorization === next.authorization) return
    this.value = next
    for (var i = 0; i < this.listeners.length; i++) this.listeners[i]()
  }

  QuotaStore.prototype.clearAuthorization = function () {
    if (this.authRequest) this.authRequest.abort()
    this.authRequest = null
    var id = this.authorization?.id
    this.authorization = null
    if (id && this.provider) {
      fetch(loginUrl(this.provider, 'cancel', id), loginOptions(AbortSignal.timeout(5000))).catch(function () {})
    }
  }

  function loginUrl(provider, action, id) {
    return ownUrl('tps-window/open-design-login?provider=' + encodeURIComponent(provider) +
      '&action=' + encodeURIComponent(action) + (id ? '&id=' + encodeURIComponent(id) : ''))
  }

  function loginOptions(signal) {
    return { method: 'POST', credentials: 'same-origin', headers: { 'x-dsh-tps-window': 'device-login' }, signal: signal }
  }

  QuotaStore.prototype.authorize = function (action) {
    var self = this
    if (action === 'cancel') {
      this.clearAuthorization()
      this.publish()
      return
    }
    if (this.authRequest || !this.provider) return
    var controller = new AbortController()
    this.authRequest = controller
    var current = this.authorization || {}
    if (action === 'start') {
      current = { status: 'starting', id: crypto.randomUUID() }
      this.authorization = current
    }
    if (action === 'confirm') this.authorization = Object.assign({}, current, { status: 'saving' })
    this.publish()
    fetch(loginUrl(this.provider, action, current.id), loginOptions(
      AbortSignal.any([controller.signal, AbortSignal.timeout(20000)])))
      .then(function (response) {
        if (!response.ok) throw new Error('Login request failed')
        return response.json()
      })
      .then(function (answer) {
        if (self.authRequest !== controller) return
        if (answer.status === 'saved') {
          self.authorization = null
          self.cancelRead()
          self.payload = null
          self.refreshAt = 0
        } else if (answer.status === 'pending' || answer.status === 'confirm') {
          self.authorization = Object.assign({}, current, answer)
          self.nextAuthPoll = Date.now() + Math.max(2000, answer.pollAfterMs || 2000)
        } else {
          self.authorization = { status: answer.status === 'expired' ? 'expired' : 'error' }
        }
      })
      .catch(function () {
        if (self.authRequest !== controller) return
        if (action === 'poll' && current.status === 'pending') {
          self.nextAuthPoll = Date.now() + 5000
          return
        }
        self.authorization = { status: 'error' }
      })
      .finally(function () {
        if (self.authRequest !== controller) return
        self.authRequest = null
        self.publish()
      })
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
    var controller = new AbortController()
    this.request = controller
    Promise.resolve()
      .then(function () {
        return fetch(quotaUrl(provider), { headers: { accept: 'application/json' },
          credentials: 'same-origin', signal: AbortSignal.any([controller.signal, AbortSignal.timeout(30000)]) })
      })
      .then(function (response) {
        if (!response.ok) throw new Error('Quota request failed')
        return response.json()
      })
      .catch(function () {
        return { status: 'unavailable', reason: 'transport' }
      })
      .then(function (payload) {
        if (self.request !== controller) return
        self.request = null
        self.payload = payload !== null && typeof payload === 'object' ? payload :
          { status: 'unavailable', reason: 'transport' }
        var retry = isOpenDesign(provider) && shouldRetryQuota(self.payload)
        self.refreshAt = Date.now() + (retry ? QUOTA_RETRY_MS : QUOTA_REFRESH_MS)
        self.publish()
      })
  }

  QuotaStore.prototype.cancelRead = function () {
    this.request?.abort()
    this.request = null
  }

  QuotaStore.prototype.tick = function () {
    this.attach()
    var provider = this.currentProvider()
    if (provider !== this.provider) {
      if (this.authorization) this.clearAuthorization()
      this.cancelRead()
      this.provider = provider
      this.payload = null
      this.refreshAt = 0
    }
    if (this.authorization?.status === 'pending') {
      if (Date.now() >= this.authorization.expiresAt) {
        this.clearAuthorization()
        this.authorization = { status: 'expired' }
      } else if (Date.now() >= this.nextAuthPoll) {
        this.authorize('poll')
      }
    }
    if (provider !== null && this.request === null && Date.now() >= this.refreshAt) this.fetch()
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
    if (this.authorization) this.clearAuthorization()
    if (this.timer !== null) {
      window.clearInterval(this.timer)
      this.timer = null
    }
    this.detachSource()
    this.binding = undefined
    this.listeners = []
    this.cancelRead()
    this.payload = null
    this.refreshAt = 0
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
    if (state.authorization) {
      var info = state.authorization
      var button = function (action, label) {
        return React.createElement('button', { type: 'button', key: action,
          onClick: function () { state.authorize(action) } }, label)
      }
      var items
      if (info.status === 'starting' || info.status === 'saving') {
        items = [info.status === 'starting' ? '正在创建授权…' : '正在保存授权…']
      } else if (info.status === 'pending') {
        items = [React.createElement('a', { key: 'open', href: info.url, target: '_blank',
          rel: 'noopener noreferrer', onClick: function (event) {
            // The VS Code panel bridges window.open, not ordinary iframe links.
            if (typeof window.open !== 'function') return
            event.preventDefault()
            window.open(info.url, '_blank', 'noopener,noreferrer')
          } }, '在浏览器授权'), '验证码 ' + info.code,
          React.createElement('details', { key: 'manual' },
            React.createElement('summary', null, '手动打开'),
            React.createElement('input', { readOnly: true, value: info.url, 'aria-label': 'OpenDesign 授权链接',
              onFocus: function (event) { event.currentTarget.select() } })),
          button('cancel', '取消')]
      } else if (info.status === 'confirm') {
        items = [state.provider + ' · ' + info.account + ' · ' + info.workspaceId,
          button('confirm', '确认绑定'), button('cancel', '取消')]
      } else {
        items = [info.status === 'expired' ? '授权已过期' : info.status === 'error' ? '授权未完成' : null,
          button('start', state.authorizationLabel + ' · ' + state.provider)]
      }
      return React.createElement('span', { style: PILL_STYLE, 'data-composer-stat': 'tps-quota-auth',
        title: state.provider }, ...items)
    }
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
