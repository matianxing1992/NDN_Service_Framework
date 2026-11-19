// dsh-tps-window host half: provider quota readings for the composer dock.
//
// The dock pill is a browser view, but the quota numbers come from provider APIs
// and local CLIs that need credentials. Those live here, never in the page: this
// half answers one same-origin JSON route and the client half renders it.
//
// Sources, per provider route:
//   opencode-go   GET https://opencode.ai/zen/go/v1/usage  (Bearer API key)
//                 -> { usage: { rolling, weekly, monthly: { percent, resetsAt } } }
//   openai-codex  `codex app-server` JSON-RPC `account/rateLimits/read`
//                 -> { rateLimits: { primary, secondary, credits, planType } }
//   open-design   GET https://amr-api.open-design.ai/api/v1/wallet/balance and
//                 .../billing/summary  (Bearer CONTROL key - the runtime key that
//                 /v1/models accepts is refused there with 401)
//                 -> { balanceUsd, membershipTier, subscriptionStatus, creditsPerUsd }
//                 The control key comes from `OPEN_DESIGN_CONTROL_KEY` or, when the
//                 OpenDesign desktop app has signed in, `~/.amr/config.json`
//                 (`profiles.prod.controlKey`). Without it there is no reading.
//   open-design-2 Queries only the workspace and control key configured for the
//                 second account. It never falls back to the first account.
//   command-code  GET https://api.commandcode.ai/alpha/billing/credits,
//                 .../alpha/usage/summary and .../alpha/billing/subscriptions
//                 (Bearer COMMANDCODE_API_KEY) -> windowLimits.fiveHour/weekly
//                 { used, cap, resetAt } plus a monthly window composed from
//                 usage.totalMonthlyCredits + credits.monthlyCredits.
//
// Everything is read-only: one GET or one short-lived CLI process, cached, and
// never logged with a credential in it.
import { spawn } from 'node:child_process'
import { readFile } from 'node:fs/promises'
import { homedir } from 'node:os'
import { join } from 'node:path'
import { SpeedCollector } from './speed.js'
import { AMR_API, OpenDesignLogin, openDesignBindingRef, validControlKey, validWorkspaceId } from './open-design-login.js'

export const name = 'dsh-tps-window'

export const inject = ['credentials', 'webServer', 'agents', 'sessionQuery', 'connection']

const ROUTE_PREFIX = '/tps-window'
const OPENCODE_USAGE_URL = 'https://opencode.ai/zen/go/v1/usage'
const COMMANDCODE_BASE = 'https://api.commandcode.ai'
const HTTP_TIMEOUT_MS = 8000
const CODEX_TIMEOUT_MS = 12000
// Command Code's account endpoints answer in 8-20 s (measured by the community
// plugin), so an 8 s timeout would kill valid reads.
const COMMANDCODE_TIMEOUT_MS = 25000
const OPENCODE_TTL_MS = 120000
const CODEX_TTL_MS = 300000
const COMMANDCODE_TTL_MS = 300000
const OPENDESIGN_TTL_MS = 120000
const OPENDESIGN_RETRY_MS = 15000

// #region quota-math
const QUOTA_MATH = {
  /** Minutes -> the window name the pill shows. */
  windowLabel: function (minutes) {
    if (typeof minutes !== 'number' || !isFinite(minutes) || minutes <= 0) return null
    if (minutes <= 330) return '5h'
    if (minutes <= 1560) return '天'
    if (minutes <= 11520) return '周'
    return '月'
  },

  /** The window closest to its limit: that is the one that will stop the next request. */
  tightest: function (windows) {
    if (windows.length === 0) return null
    var best = windows[0]
    for (var i = 1; i < windows.length; i++) {
      if (windows[i].percent > best.percent) best = windows[i]
    }
    return best
  },

  /**
   * One provider reading in the shape the client renders: a provider label and
   * the windows that were actually reported.
   */
  reading: function (label, windows, detail) {
    return {
      status: 'ok',
      label: label,
      windows: windows,
      detail: detail === undefined ? {} : detail,
      updatedAt: Date.now()
    }
  },

  /** An amounts reading: labelled USD strings instead of percent windows. */
  amountsReading: function (label, amounts, detail) {
    return {
      status: 'ok',
      kind: 'amounts',
      label: label,
      amounts: amounts,
      windows: [],
      detail: detail === undefined ? {} : detail,
      updatedAt: Date.now()
    }
  },

  /** opencode.ai `/zen/go/v1/usage` body -> windows. Percent is usage, not headroom. */
  opencodeWindows: function (body) {
    var usage = body === null || typeof body !== 'object' ? undefined : body.usage
    if (usage === null || typeof usage !== 'object') return []
    var named = [
      ['rolling', '5h'],
      ['weekly', '周'],
      ['monthly', '月']
    ]
    var windows = []
    for (var i = 0; i < named.length; i++) {
      var entry = usage[named[i][0]]
      if (entry === null || typeof entry !== 'object') continue
      var percent = typeof entry.percent === 'number' && isFinite(entry.percent) ? entry.percent : null
      if (percent === null) continue
      windows.push({
        label: named[i][1],
        percent: Math.max(0, Math.min(100, Math.round(percent))),
        resetsAt: QUOTA_MATH.isoToEpochMs(entry.resetsAt)
      })
    }
    return windows
  },

  /**
   * `codex app-server` rate-limit snapshot -> windows. `resetsAt` there is epoch
   * seconds; `usedPercent` is usage. Only present buckets are reported.
   */
  codexWindows: function (snapshot) {
    if (snapshot === null || typeof snapshot !== 'object') return []
    var windows = []
    var buckets = [snapshot.primary, snapshot.secondary]
    for (var i = 0; i < buckets.length; i++) {
      var bucket = buckets[i]
      if (bucket === null || typeof bucket !== 'object') continue
      var percent = typeof bucket.usedPercent === 'number' && isFinite(bucket.usedPercent) ? bucket.usedPercent : null
      if (percent === null) continue
      windows.push({
        label: QUOTA_MATH.windowLabel(bucket.windowDurationMins) === null ? '限额' : QUOTA_MATH.windowLabel(bucket.windowDurationMins),
        durationMinutes: bucket.windowDurationMins,
        percent: Math.max(0, Math.min(100, Math.round(percent))),
        resetsAt: QUOTA_MATH.secondsToEpochMs(bucket.resetsAt)
      })
    }
    return windows
  },

  /** ISO 8601 -> epoch milliseconds, or null. */
  isoToEpochMs: function (value) {
    if (typeof value !== 'string') return null
    var parsed = Date.parse(value)
    return isFinite(parsed) ? parsed : null
  },

  /** Epoch seconds -> epoch milliseconds, or null. */
  secondsToEpochMs: function (value) {
    if (typeof value !== 'number' || !isFinite(value) || value <= 0) return null
    return value * 1000
  },

  /** A finite number from a number or a numeric string; anything else is null (never zero). */
  number: function (value) {
    if (typeof value === 'number' && isFinite(value)) return value
    if (typeof value === 'string' && value.trim() !== '') {
      var parsed = Number(value)
      return isFinite(parsed) ? parsed : null
    }
    return null
  },

  /** Epoch seconds, epoch milliseconds, or an ISO string -> epoch milliseconds. */
  timeToEpochMs: function (value) {
    var numeric = QUOTA_MATH.number(value)
    if (numeric !== null) return numeric >= 1e12 ? numeric : numeric * 1000
    if (typeof value === 'string') {
      var parsed = Date.parse(value)
      return isFinite(parsed) ? parsed : null
    }
    return null
  },

  /** Money as USD text: two decimals, four when the amount is under a cent. */
  money: function (value) {
    var numeric = QUOTA_MATH.number(value)
    if (numeric === null) return null
    return '$' + (Math.abs(numeric) >= 0.01 ? numeric.toFixed(2) : numeric.toFixed(4))
  },

  /** used/cap -> whole percent, or null when the cap is missing or not positive. */
  percentOf: function (used, cap) {
    var numericUsed = QUOTA_MATH.number(used)
    var numericCap = QUOTA_MATH.number(cap)
    if (numericUsed === null || numericCap === null || numericCap <= 0) return null
    return Math.max(0, Math.min(100, Math.round((numericUsed / numericCap) * 100)))
  },

  /**
   * Command Code windows. The rolling 5-hour and weekly caps come straight from
   * `billing/credits`; the monthly window is a composite the provider's own panel
   * builds: used = usage.totalMonthlyCredits, cap = used + credits.monthlyCredits,
   * reset = the subscription period end.
   */
  commandCodeWindows: function (creditsBody, usageBody, subscriptionBody) {
    var windows = []
    var limits =
      creditsBody !== null && typeof creditsBody === 'object' && creditsBody.windowLimits !== null && typeof creditsBody.windowLimits === 'object'
        ? creditsBody.windowLimits
        : undefined
    var named = [
      ['fiveHour', '5h'],
      ['weekly', '周']
    ]
    for (var i = 0; i < named.length; i++) {
      var window = limits === undefined ? undefined : limits[named[i][0]]
      if (window === null || typeof window !== 'object') continue
      var percent = QUOTA_MATH.percentOf(window.used, window.cap)
      if (percent === null) continue
      windows.push({ label: named[i][1], percent: percent, resetsAt: QUOTA_MATH.timeToEpochMs(window.resetAt) })
    }
    var usage = usageBody !== null && typeof usageBody === 'object' ? usageBody : {}
    var credits = creditsBody !== null && typeof creditsBody === 'object' ? creditsBody.credits : undefined
    var monthlyUsed = QUOTA_MATH.number(usage.totalMonthlyCredits)
    var monthlyRemaining = QUOTA_MATH.number(credits === null || typeof credits !== 'object' ? null : credits.monthlyCredits)
    var monthlyPercent = QUOTA_MATH.percentOf(monthlyUsed, monthlyUsed !== null && monthlyRemaining !== null ? monthlyUsed + monthlyRemaining : null)
    if (monthlyPercent !== null) {
      var subscription =
        subscriptionBody !== null && typeof subscriptionBody === 'object' && subscriptionBody.data !== null && typeof subscriptionBody.data === 'object'
          ? subscriptionBody.data
          : undefined
      var periodEnd = subscription === undefined ? null : QUOTA_MATH.timeToEpochMs(subscription.currentPeriodEnd)
      windows.push({ label: '月', percent: monthlyPercent, resetsAt: periodEnd })
    }
    return windows
  },

  /**
   * AMR wallet + billing payloads -> the two amounts Open Design shows: the
   * wallet balance and, when the billing payload carries one, the top/premium
   * model allowance. Field names are scanned rather than assumed because the
   * account API is not documented field-by-field.
   */
  opendesignAmounts: function (wallet, billing) {
    var amounts = []
    var detail = {}
    var balance = QUOTA_MATH.money(wallet === null || wallet === undefined ? null : wallet.balanceUsd)
    if (balance !== null) amounts.push({ label: '额度', value: balance })
    if (billing !== null && typeof billing === 'object') {
      var rate = QUOTA_MATH.number(billing.creditsPerUsd)
      if (rate !== null && rate > 0) detail.creditsPerUsd = rate
      var tier = billing.membershipTier
      if (typeof tier === 'string' && tier.trim() !== '') detail.plan = tier.trim()
      var status = billing.subscriptionStatus
      if (typeof status === 'string' && status.trim() !== '') detail.status = status.trim()
      var periodEnd = QUOTA_MATH.timeToEpochMs(billing.subscriptionCurrentPeriodEnd)
      if (periodEnd !== null) detail.periodEnd = periodEnd
      var found = QUOTA_MATH.scanTopCredits(billing, ['billing'], 0)
      if (found !== null) {
        var value = found.credits !== null && rate !== null && rate > 0 ? found.credits / rate : found.usd
        var text = QUOTA_MATH.money(value)
        if (text !== null) {
          amounts.push({ label: '高级', value: text })
          detail.topField = found.field
        }
      }
    }
    return { amounts: amounts, detail: detail }
  },

  openDesignWindows: function (body, workspaceId) {
    const plan = body?.codingPlan
    if (plan?.eligible === false || !Array.isArray(plan?.windows)) return []
    if ([body.workspaceId, plan.workspaceId].some(id => id !== undefined && id !== workspaceId)) return []
    const windows = new Map()
    for (const entry of plan.windows) {
      const seconds = QUOTA_MATH.number(entry?.durationSeconds)
      const label = seconds === 18000 ? '5h' : seconds === 604800 ? '7天' : null
      const used = QUOTA_MATH.number(entry?.usedCredits)
      const limit = QUOTA_MATH.number(entry?.limitCredits)
      if (label === null || used === null || used < 0 || limit === null || limit <= 0) continue
      const percent = Math.min(100, used / limit * 100)
      const resetsAt = QUOTA_MATH.timeToEpochMs(entry.resetsAt)
      windows.set(seconds, { label, durationMinutes: seconds / 60, percent, resetsAt })
    }
    return [...windows.entries()].sort(([a], [b]) => a - b).map(([, window]) => window)
  },

  /**
   * Depth-first search for a top/premium credit or USD field. Returns the raw
   * value and the field path, or null when the payload carries none.
   */
  scanTopCredits: function (value, path, depth) {
    if (value === null || typeof value !== 'object' || depth > 3) return null
    var keys = Object.keys(value)
    for (var i = 0; i < keys.length; i++) {
      var key = keys[i]
      var child = value[key]
      var name = path.concat([key]).join('.')
      if (typeof child === 'number' || typeof child === 'string') {
        var lower = key.toLowerCase()
        if (/^(top|premium)/.test(lower)) {
          var numeric = QUOTA_MATH.number(child)
          if (numeric !== null && numeric >= 0) {
            if (/usd/.test(lower)) return { usd: numeric, credits: null, field: name }
            if (/credit|balance|remain|available|allowance/.test(lower)) return { usd: null, credits: numeric, field: name }
          }
        }
        continue
      }
      if (child !== null && typeof child === 'object') {
        var nested = QUOTA_MATH.scanTopCredits(child, path.concat([key]), depth + 1)
        if (nested !== null) return nested
      }
    }
    return null
  }
}
// #endregion quota-math

/** Trim a trailing slash so route paths join without doubling. */
function routePath(base) {
  return base.replace(/\/+$/, '') + '/quota'
}

async function resolveReference(ctx, names) {
  for (const name of names) {
    try {
      const resolved = await ctx.credentials.resolve(name)
      if (resolved !== undefined && typeof resolved.value === 'string' && resolved.value.length > 0) return resolved.value
    } catch (_error) {
      // A reference this composition cannot resolve is simply not configured.
    }
  }
  return undefined
}

/** Read the OpenDesign desktop app's stored prod sign-in, read-only. */
async function readAmrProfile() {
  const path =
    typeof process.env.OPENDESIGN_CONFIG_PATH === 'string' && process.env.OPENDESIGN_CONFIG_PATH.length > 0
      ? process.env.OPENDESIGN_CONFIG_PATH
      : join(homedir(), '.amr', 'config.json')
  let parsed
  try {
    parsed = JSON.parse(await readFile(path, 'utf8'))
  } catch (_error) {
    return undefined
  }
  const profile = parsed !== null && typeof parsed === 'object' && parsed.profiles !== undefined ? parsed.profiles.prod : undefined
  return profile !== null && typeof profile === 'object' ? profile : undefined
}

/** One AMR account read. Accepts a direct object or a `data` envelope. */
async function amrJson(path, key, workspaceId) {
  const headers = { authorization: `Bearer ${key}`, accept: 'application/json' }
  if (workspaceId !== undefined) headers['x-vela-workspace-id'] = workspaceId
  let response
  try {
    response = await fetch(AMR_API + path, {
      headers,
      redirect: 'error',
      signal: AbortSignal.timeout(HTTP_TIMEOUT_MS)
    })
  } catch (_error) {
    return { ok: false, status: 0 }
  }
  if (!response.ok) return { ok: false, status: response.status }
  let body
  try {
    body = await response.json()
  } catch (_error) {
    return { ok: false, status: response.status }
  }
  if (body === null || typeof body !== 'object' || body.error !== undefined || body.success === false) {
    return { ok: false, status: response.status }
  }
  return { ok: true, body: body.data !== null && typeof body.data === 'object' ? body.data : body }
}

/**
 * OpenDesign (AMR) account reading. The inference key that `/v1/models` accepts
 * is refused here, so this needs the control-plane key: an explicit
 * `OPEN_DESIGN_CONTROL_KEY` reference, or the one the desktop app saved in
 * `~/.amr/config.json`.
 */
async function readOpenDesignWorkspace(provider, key, workspaceId) {
  if (!validWorkspaceId(workspaceId)) return { status: 'unavailable', reason: 'invalid-workspace-id' }
  const prefix = `/workspaces/${workspaceId}`
  const [wallet, billing] = await Promise.all([
    amrJson(`${prefix}/wallet/balance`, key, workspaceId),
    amrJson(`${prefix}/billing/preflight`, key, workspaceId)
  ])
  const label = provider === 'open-design-2' ? 'OD 2' : 'OD'
  const windows = billing.ok ? QUOTA_MATH.openDesignWindows(billing.body, workspaceId) : []
  const mapped = QUOTA_MATH.opendesignAmounts(wallet.ok ? wallet.body : null, null)
  if (windows.length > 0) {
    return { ...QUOTA_MATH.reading(label, windows, mapped.detail), amounts: mapped.amounts }
  }
  if (!wallet.ok) {
    const unauthorized = wallet.status === 401 || wallet.status === 403
    return { status: 'unavailable', reason: unauthorized ? 'unauthorized' : 'unreachable' }
  }
  if (mapped.amounts.length === 0) return { status: 'unavailable', reason: 'no-fields' }
  return QUOTA_MATH.amountsReading(label, mapped.amounts, { ...mapped.detail,
    windowStatus: billing.ok ? 'no-windows' : 'unavailable' })
}

async function readOpenDesign(ctx, provider = 'open-design') {
  const ref = openDesignBindingRef(provider)
  if (ref === null) return { status: 'unavailable', reason: 'no-adapter' }
  const saved = (await ctx.credentials.resolve(ref))?.value
  if (typeof saved === 'string' && saved.length > 0) {
    let binding
    try { binding = JSON.parse(saved) } catch { /* A malformed binding must not fall back. */ }
    if (!validControlKey(binding?.controlKey) || !validWorkspaceId(binding?.workspaceId)) {
      return { status: 'unavailable', reason: 'invalid-binding' }
    }
    return readOpenDesignWorkspace(provider, binding.controlKey, binding.workspaceId)
  }
  if (provider === 'open-design-2') {
    const key = await resolveReference(ctx, ['OPEN_DESIGN_2_CONTROL_KEY'])
    if (key === undefined) return { status: 'unavailable', reason: 'no-control-key' }
    const workspaceId = await resolveReference(ctx, ['OPEN_DESIGN_2_WORKSPACE_ID'])
    if (workspaceId === undefined) return { status: 'unavailable', reason: 'no-workspace-id' }
    return readOpenDesignWorkspace(provider, key, workspaceId)
  }
  const explicit = await resolveReference(ctx, ['OPEN_DESIGN_CONTROL_KEY', 'OPENDESIGN_CONTROL_KEY'])
  let controlKey = explicit
  if (controlKey === undefined) {
    const profile = await readAmrProfile()
    if (profile !== undefined && typeof profile.controlKey === 'string' && profile.controlKey.trim() !== '') {
      controlKey = profile.controlKey.trim()
    }
  }
  if (controlKey === undefined) return { status: 'unavailable', reason: 'no-control-key' }
  const [wallet, billing] = await Promise.all([amrJson('/wallet/balance', controlKey), amrJson('/billing/summary', controlKey)])
  if (!wallet.ok && !billing.ok) {
    const unauthorized = wallet.status === 401 || billing.status === 401
    return { status: 'unavailable', reason: unauthorized ? 'unauthorized' : 'unreachable' }
  }
  const mapped = QUOTA_MATH.opendesignAmounts(wallet.ok ? wallet.body : null, billing.ok ? billing.body : null)
  if (mapped.amounts.length === 0) return { status: 'unavailable', reason: 'no-fields' }
  return QUOTA_MATH.amountsReading('OD', mapped.amounts, mapped.detail)
}

async function readOpencodeGo(ctx) {
  const key = await resolveReference(ctx, ['OPENCODE_GO_API_KEY', 'OPENCODE_API_KEY'])
  if (key === undefined) return { status: 'unavailable', reason: 'no-credential' }
  let response
  try {
    response = await fetch(OPENCODE_USAGE_URL, {
      headers: { authorization: `Bearer ${key}`, accept: 'application/json' },
      signal: AbortSignal.timeout(HTTP_TIMEOUT_MS)
    })
  } catch (_error) {
    return { status: 'unavailable', reason: 'network' }
  }
  if (!response.ok) return { status: 'unavailable', reason: `http-${String(response.status)}` }
  let body
  try {
    body = await response.json()
  } catch (_error) {
    return { status: 'unavailable', reason: 'bad-json' }
  }
  const windows = QUOTA_MATH.opencodeWindows(body)
  if (windows.length === 0) return { status: 'unavailable', reason: 'no-windows' }
  return QUOTA_MATH.reading('Go', windows, {})
}

/** One Command Code account read; the account API is slow, so each call gets a wide bound. */
async function commandCodeJson(base, path, key, timeoutMs) {
  let response
  try {
    response = await fetch(base.replace(/\/+$/, '') + path, {
      headers: { authorization: `Bearer ${key}`, accept: 'application/json' },
      signal: AbortSignal.timeout(timeoutMs)
    })
  } catch (_error) {
    return { ok: false, status: 0 }
  }
  if (!response.ok) return { ok: false, status: response.status }
  let body
  try {
    body = await response.json()
  } catch (_error) {
    return { ok: false, status: response.status }
  }
  if (body === null || typeof body !== 'object') return { ok: false, status: response.status }
  return { ok: true, body }
}

/**
 * Command Code plan usage: rolling 5-hour, weekly and monthly windows. The three
 * endpoints are independent, so a partial failure still yields the windows that
 * did answer; all three failing reports the reason instead of a zero.
 */
async function readCommandCode(ctx) {
  const key = await resolveReference(ctx, ['COMMANDCODE_API_KEY', 'COMMANDCODE2_API_KEY', 'COMMAND_CODE_API_KEY'])
  if (key === undefined) return { status: 'unavailable', reason: 'no-credential' }
  const base =
    typeof process.env.COMMANDCODE_BASE_URL === 'string' && process.env.COMMANDCODE_BASE_URL.length > 0
      ? process.env.COMMANDCODE_BASE_URL
      : COMMANDCODE_BASE
  const [credits, usage, subscriptions] = await Promise.all([
    commandCodeJson(base, '/alpha/billing/credits', key, COMMANDCODE_TIMEOUT_MS),
    commandCodeJson(base, '/alpha/usage/summary', key, COMMANDCODE_TIMEOUT_MS),
    commandCodeJson(base, '/alpha/billing/subscriptions', key, COMMANDCODE_TIMEOUT_MS)
  ])
  if (!credits.ok && !usage.ok && !subscriptions.ok) {
    const unauthorized = credits.status === 401 || usage.status === 401 || subscriptions.status === 401
    return { status: 'unavailable', reason: unauthorized ? 'unauthorized' : 'unreachable' }
  }
  const windows = QUOTA_MATH.commandCodeWindows(
    credits.ok ? credits.body : null,
    usage.ok ? usage.body : null,
    subscriptions.ok ? subscriptions.body : null
  )
  if (windows.length === 0) return { status: 'unavailable', reason: 'no-windows' }
  return QUOTA_MATH.reading('CC', windows, {})
}

/** Candidate invocations for `codex app-server`, most specific first. */
function codexInvocations() {
  const candidates = []
  if (typeof process.env.CODEX_CLI === 'string' && process.env.CODEX_CLI.length > 0) {
    candidates.push({ command: process.env.CODEX_CLI, args: [], env: {} })
  }
  const npmRoot = typeof process.env.APPDATA === 'string' ? `${process.env.APPDATA}\\npm` : undefined
  if (npmRoot !== undefined) {
    const launcher = `${npmRoot}\\node_modules\\@openai\\codex\\bin\\codex.js`
    candidates.push({
      command: process.execPath,
      args: [launcher],
      env: { ELECTRON_RUN_AS_NODE: '1' }
    })
    candidates.push({ command: `${npmRoot}\\codex.cmd`, args: [], env: {}, shell: true })
  }
  candidates.push({ command: 'codex', args: [], env: {}, shell: true })
  return candidates
}

/** Run one JSON-RPC session against `codex app-server` and read the rate-limit snapshot. */
async function codexAttempt(invocation, timeoutMs) {
  const baseEnv = { ...process.env, ...invocation.env }
  if (typeof process.env.CODEX_HOME === 'string' && process.env.CODEX_HOME.length > 0) baseEnv.CODEX_HOME = process.env.CODEX_HOME
  let child
  try {
    child = spawn(invocation.command, [...invocation.args, 'app-server', '--listen', 'stdio://'], {
      env: baseEnv,
      stdio: ['pipe', 'pipe', 'ignore'],
      windowsHide: true,
      shell: invocation.shell === true
    })
  } catch (_error) {
    return { status: 'unavailable', reason: 'codex-spawn' }
  }

  return await new Promise((resolve) => {
    let settled = false
    let buffer = ''
    const finish = (value) => {
      if (settled) return
      settled = true
      try {
        child.kill()
      } catch (_error) {
        // An already-dead child needs no kill.
      }
      resolve(value)
    }
    const timer = setTimeout(() => finish({ status: 'unavailable', reason: 'codex-timeout' }), timeoutMs)
    child.on('error', () => {
      clearTimeout(timer)
      finish({ status: 'unavailable', reason: 'codex-spawn' })
    })
    child.on('close', () => {
      clearTimeout(timer)
      // A close without our answer means the CLI refused or is not logged in.
      finish({ status: 'unavailable', reason: 'codex-closed' })
    })
    child.stdout.on('data', (chunk) => {
      buffer += chunk.toString('utf8')
      let newline
      while ((newline = buffer.indexOf('\n')) >= 0) {
        const line = buffer.slice(0, newline).trim()
        buffer = buffer.slice(newline + 1)
        if (line === '') continue
        let message
        try {
          message = JSON.parse(line)
        } catch (_error) {
          continue
        }
        if (message.id === 2) {
          clearTimeout(timer)
          const snapshot = message.result === undefined || message.result.rateLimits === undefined ? undefined : message.result.rateLimits
          const windows = QUOTA_MATH.codexWindows(snapshot)
          if (windows.length === 0) {
            finish({ status: 'unavailable', reason: message.result === undefined ? 'codex-error' : 'no-windows' })
            return
          }
          finish(
            QUOTA_MATH.reading('Codex', windows, {
              planType: typeof snapshot.planType === 'string' ? snapshot.planType : null,
              credits:
                snapshot.credits !== undefined && snapshot.credits !== null && typeof snapshot.credits.balance === 'string'
                  ? snapshot.credits.balance
                  : null,
              unlimited: snapshot.credits !== undefined && snapshot.credits !== null ? snapshot.credits.unlimited === true : false
            })
          )
        }
      }
    })
    const write = (payload) => {
      try {
        child.stdin.write(`${JSON.stringify(payload)}\n`)
      } catch (_error) {
        clearTimeout(timer)
        finish({ status: 'unavailable', reason: 'codex-stdin' })
      }
    }
    write({ jsonrpc: '2.0', id: 1, method: 'initialize', params: { clientInfo: { name: 'dsh-tps-window', title: 'DSH TPS Window', version: '0.3.0' } } })
    write({ jsonrpc: '2.0', method: 'initialized' })
    setTimeout(() => write({ jsonrpc: '2.0', id: 2, method: 'account/rateLimits/read', params: {} }), 250)
  })
}

/**
 * Try every candidate launcher until one answers. A machine may have the CLI as
 * an npm shim, a vendored binary, or on PATH; the first candidate that is missing
 * or not logged in must not hide the next one.
 */
async function codexRateLimits() {
  const deadline = Date.now() + CODEX_TIMEOUT_MS
  let last = { status: 'unavailable', reason: 'codex-spawn' }
  for (const invocation of codexInvocations()) {
    const remaining = deadline - Date.now()
    if (remaining <= 500) break
    const result = await codexAttempt(invocation, Math.min(remaining, CODEX_TIMEOUT_MS))
    if (result.status === 'ok') return result
    last = result
  }
  return last
}

/**
 * One quota service: fixed-provider adapters behind a small cache, so a page
 * that asks every few seconds costs one upstream read per TTL at most.
 */
class QuotaService {
  constructor(ctx, log) {
    this.ctx = ctx
    this.log = log
    this.entries = new Map()
    this.inflight = new Map()
  }

  ttlOf(provider, value) {
    if (provider === 'openai-codex') return CODEX_TTL_MS
    if (openDesignBindingRef(provider) !== null) {
      if (value.reason === 'unreachable' || value.reason === 'error' ||
          value.detail?.windowStatus === 'unavailable') return OPENDESIGN_RETRY_MS
      return OPENDESIGN_TTL_MS
    }
    if (provider === 'command-code' || provider === 'commandcode') return COMMANDCODE_TTL_MS
    return OPENCODE_TTL_MS
  }

  async read(provider) {
    if (provider === 'opencode-go') return readOpencodeGo(this.ctx)
    if (provider === 'openai-codex') return codexRateLimits()
    if (provider === 'open-design' || provider === 'open-design-2') {
      return readOpenDesign(this.ctx, provider)
    }
    // The provider id is whatever the profile named it; Command Code is not a
    // shipped pi-ai provider, so both spellings a user may choose are accepted.
    if (provider === 'command-code' || provider === 'commandcode') return readCommandCode(this.ctx)
    // Providers without a readable limit API answer with the reason, so a future
    // adapter is a one-line addition here rather than a silent wrong number.
    return { status: 'unavailable', reason: 'no-adapter' }
  }

  /** Cached reading for one provider, or an "unavailable" answer. */
  async quotaFor(provider) {
    const cached = this.entries.get(provider)
    if (cached !== undefined && Date.now() - cached.at < this.ttlOf(provider, cached.value)) {
      return cached.value
    }
    const inFlight = this.inflight.get(provider)
    if (inFlight !== undefined) return await inFlight
    const run = (async () => {
      let value
      try {
        value = await this.read(provider)
      } catch (error) {
        this.log?.warn?.(`tps-window: quota read failed for ${provider}: ${String(error && error.message)}`)
        value = { status: 'unavailable', reason: 'error' }
      }
      if (this.inflight.get(provider) === run) {
        this.entries.set(provider, { at: Date.now(), value })
        this.inflight.delete(provider)
      }
      return value
    })()
    this.inflight.set(provider, run)
    return await run
  }

  async handle(request) {
    const url = new URL(request.url, 'http://localhost')
    if (routePath(ROUTE_PREFIX) !== url.pathname) {
      return new Response(JSON.stringify({ status: 'unavailable', reason: 'unknown-path' }), {
        status: 404,
        headers: { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' }
      })
    }
    const provider = url.searchParams.get('provider') ?? ''
    const payload = { provider, ...(await this.quotaFor(provider)) }
    return new Response(JSON.stringify(payload), {
      status: 200,
      headers: { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' }
    })
  }
}

export async function apply(ctx) {
  const service = new QuotaService(ctx, ctx.logger)
  const login = new OpenDesignLogin(ctx.credentials, provider => {
    service.entries.delete(provider)
    service.inflight.delete(provider)
  })
  ctx.effect(() => () => login.close())
  const speed = new SpeedCollector(ctx)
  ctx.on('agent/assistant-stream', ({ agent, frame }) => {
    speed.safely(() => speed.frame(agent, frame), agent.session)
  }, { global: true })
  ctx.on('session/event', (session, event) => {
    speed.safely(() => speed.event(session, event), session)
  }, { global: true })
  ctx.effect(() => () => speed.close())
  const timer = setInterval(() => speed.gc(), 1000)
  timer.unref?.()
  ctx.effect(() => () => clearInterval(timer))
  ctx.effect(() => ctx.webServer.register({
    kind: 'exact',
    path: ROUTE_PREFIX + '/open-design-login',
    // DSH Web resources only admit GET/HEAD. This handler owns the POST.
    handler: async (req, res) => {
      const url = new URL(req.url, 'http://localhost')
      const headers = new Headers()
      for (const [name, value] of Object.entries(req.headers)) {
        for (const part of typeof value === 'string' ? [value] : value ?? []) headers.append(name, part)
      }
      const auth = ctx.connection.requestRejection(new Request(url, { method: 'POST', headers }))
      const rejection = auth ?? (req.method !== 'POST' ? 405 :
        headers.get('x-dsh-tps-window') !== 'device-login' ? 403 :
        headers.has('transfer-encoding') || Number(headers.get('content-length')) !== 0 ? 400 : null)
      if (rejection !== null) {
        res.writeHead(rejection, { 'cache-control': 'no-store' })
        res.end()
        return
      }
      const value = await login.handle(url.searchParams.get('provider'),
        url.searchParams.get('action'), url.searchParams.get('id'))
      res.writeHead(200, { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' })
      res.end(JSON.stringify(value))
    }
  }), 'tps-window: OpenDesign login route')
  ctx.effect(
    () =>
      ctx.webServer.register({
        kind: 'prefix',
        path: ROUTE_PREFIX,
        resource: (request) => {
          const rejection = ctx.connection.requestRejection(request)
          if (rejection !== undefined) return new Response(null, { status: rejection })
          if (request.method !== 'GET') return new Response(null, { status: 405 })
          const url = new URL(request.url, 'http://localhost')
          if (url.pathname !== ROUTE_PREFIX + '/speed') return service.handle(request)
          const id = url.searchParams.get('sessionId')
          if (!id || id.length > 256) return new Response(null, { status: 400 })
          return Response.json(speed.snapshot(id), { headers: { 'cache-control': 'no-store' } })
        }
      }),
    'tps-window: quota route'
  )
  await speed.seed()
}

export const QUOTA = QUOTA_MATH

/**
 * Named seams for the read-only smoke test. The Host composition only consumes
 * `name`, `inject`, and `apply`; these exist so the adapters can be exercised
 * against a live provider without booting a whole profile.
 */
export const seams = { readOpencodeGo, readOpenDesign, readCommandCode, codexRateLimits, QuotaService }
