export const AMR_API = 'https://amr-api.open-design.ai/api/v1'
const DEVICE_PATH = '/cli/device-authorizations'
const MAX_AGE_MS = 10 * 60 * 1000
const REQUEST_TIMEOUT_MS = 8000

export function openDesignBindingRef(provider) {
  if (provider === 'open-design') return 'OPEN_DESIGN_QUOTA_BINDING'
  if (provider === 'open-design-2') return 'OPEN_DESIGN_2_QUOTA_BINDING'
  return null
}

export function validWorkspaceId(value) {
  return typeof value === 'string' && /^[A-Za-z0-9_-]{1,128}$/.test(value)
}

export function validControlKey(value) {
  return typeof value === 'string' && /^vela_ctrl_[A-Za-z0-9._~-]{1,512}$/.test(value)
}

function activationUrl(value) {
  if (typeof value !== 'string' || value.length > 2048) return null
  try {
    const url = new URL(value)
    if (url.origin === 'https://open-design.ai' && url.pathname === '/cloud/cli/activate' &&
        !url.username && !url.password) return url.href
  } catch { /* Invalid vendor response. */ }
  return null
}

async function request(path, signal, { body, controlKey } = {}) {
  const headers = { accept: 'application/json' }
  if (body !== undefined) headers['content-type'] = 'application/json'
  if (controlKey !== undefined) headers.authorization = `Bearer ${controlKey}`
  const response = await fetch(AMR_API + path, {
    method: body === undefined ? 'GET' : 'POST', headers, redirect: 'error',
    body: body === undefined ? undefined : JSON.stringify(body),
    signal: AbortSignal.any([signal, AbortSignal.timeout(REQUEST_TIMEOUT_MS)])
  })
  const data = await response.json().catch(() => null)
  return { code: response.status, ok: response.ok, data }
}

export class OpenDesignLogin {
  constructor(credentials, onSaved) {
    this.credentials = credentials
    this.onSaved = onSaved
    this.attempts = new Map()
    this.operations = new Set()
    this.closed = false
  }

  release(attempt) {
    if (this.attempts.get(attempt.provider) === attempt) this.attempts.delete(attempt.provider)
    clearTimeout(attempt.timer)
    attempt.controller.abort()
    attempt.deviceSecret = undefined
    attempt.controlKey = undefined
  }

  async close() {
    this.closed = true
    for (const attempt of this.attempts.values()) this.release(attempt)
    await Promise.allSettled(this.operations)
  }

  live(attempt) {
    return !this.closed && this.attempts.get(attempt.provider) === attempt &&
      Date.now() < attempt.expiresAt
  }

  async start(provider, id) {
    if (typeof id !== 'string' || !/^[0-9a-f-]{36}$/.test(id)) return { status: 'unavailable' }
    if (this.attempts.has(provider)) return { status: 'busy' }
    const attempt = { id, provider, controller: new AbortController(),
      expiresAt: Date.now() + MAX_AGE_MS, phase: 'starting' }
    this.attempts.set(provider, attempt)
    attempt.timer = setTimeout(() => this.release(attempt), MAX_AGE_MS)
    attempt.timer.unref?.()
    try {
      const { ok, data } = await request(DEVICE_PATH, attempt.controller.signal, {
        body: { cliVersion: '0.5.10', host: 'DSH TPS Window', profile: 'prod' }
      })
      const url = activationUrl(data?.activationUrl)
      if (!this.live(attempt)) return { status: 'expired' }
      if (!ok || !url || typeof data.deviceId !== 'string' || data.deviceId.length > 128 ||
          !data.deviceId || typeof data.deviceSecret !== 'string' || !data.deviceSecret ||
          data.deviceSecret.length > 512 || !/^[A-Za-z0-9-]{4,16}$/.test(data.userCode ?? '')) {
        this.release(attempt)
        return { status: 'error' }
      }
      const expiry = Date.parse(data.expiresAt)
      if (Number.isFinite(expiry)) attempt.expiresAt = Math.min(attempt.expiresAt, expiry)
      clearTimeout(attempt.timer)
      attempt.timer = setTimeout(() => this.release(attempt), Math.max(0, attempt.expiresAt - Date.now()))
      attempt.timer.unref?.()
      attempt.deviceId = data.deviceId
      attempt.deviceSecret = data.deviceSecret
      const interval = Number(data.pollIntervalSeconds)
      attempt.interval = Math.max(2, Math.min(30, Number.isFinite(interval) ? interval : 2)) * 1000
      attempt.nextPoll = Date.now() + attempt.interval
      attempt.phase = 'pending'
      return { status: 'pending', id: attempt.id, url, code: data.userCode,
        expiresAt: attempt.expiresAt, pollAfterMs: attempt.interval }
    } catch {
      this.release(attempt)
      return { status: 'error' }
    }
  }

  async poll(attempt) {
    if (attempt.phase === 'confirm') return attempt.confirmation
    const pending = { status: 'pending', pollAfterMs: attempt.interval }
    if (attempt.phase !== 'pending' || Date.now() < attempt.nextPoll) return pending
    attempt.phase = 'polling'
    try {
      const signal = attempt.controller.signal
      const { data, ok, code } = await request(`${DEVICE_PATH}/${encodeURIComponent(attempt.deviceId)}/token`,
        signal, { body: { deviceSecret: attempt.deviceSecret } })
      if (!this.live(attempt)) return { status: 'expired' }
      if (ok && validControlKey(data?.controlKey)) {
        const key = data.controlKey
        attempt.deviceSecret = undefined
        const account = typeof data.user?.email === 'string' ? data.user.email.trim() : ''
        const result = await request('/workspaces/current', signal, { controlKey: key })
        if (!this.live(attempt)) return { status: 'expired' }
        const workspaceId = result.data?.activeWorkspaceId ?? result.data?.workspaceId
        if (!result.ok || !validWorkspaceId(workspaceId) || !account || account.length > 254) {
          this.release(attempt)
          return { status: 'error' }
        }
        attempt.controlKey = key
        attempt.phase = 'confirm'
        attempt.confirmation = { status: 'confirm', account, workspaceId }
        return attempt.confirmation
      }
      const status = String(data?.status ?? '').toLowerCase()
      if (code === 401 || code === 403 || code === 404 || code === 410 ||
          ['expired', 'canceled', 'cancelled', 'denied', 'rejected'].includes(status)) {
        this.release(attempt)
        return { status: 'expired' }
      }
      if (code === 429) attempt.interval = Math.min(30000, attempt.interval + 5000)
    } catch {
      if (!this.live(attempt)) return { status: 'expired' }
      if (!attempt.deviceSecret) {
        this.release(attempt)
        return { status: 'error' }
      }
    }
    attempt.phase = 'pending'
    attempt.nextPoll = Date.now() + attempt.interval
    return { status: 'pending', pollAfterMs: attempt.interval }
  }

  handle(provider, action, id) {
    if (this.closed) return Promise.resolve({ status: 'unavailable' })
    const operation = this.dispatch(provider, action, id)
    this.operations.add(operation)
    return operation.finally(() => this.operations.delete(operation))
  }

  async dispatch(provider, action, id) {
    const ref = openDesignBindingRef(provider)
    if (this.closed || ref === null) return { status: 'unavailable' }
    if (action === 'start') return this.start(provider, id)
    const attempt = this.attempts.get(provider)
    if (!attempt || attempt.id !== id || !this.live(attempt)) return { status: 'expired' }
    if (action === 'cancel' && !attempt.saving) {
      this.release(attempt)
      return { status: 'cancelled' }
    }
    if (action === 'poll') return this.poll(attempt)
    if (action !== 'confirm' || attempt.phase !== 'confirm') return { status: 'unavailable' }
    attempt.phase = 'saving'
    clearTimeout(attempt.timer)
    const value = JSON.stringify({ controlKey: attempt.controlKey, workspaceId: attempt.confirmation.workspaceId })
    attempt.saving = Promise.resolve().then(() => this.credentials.set(ref, value))
    try {
      await attempt.saving
      if (!this.closed) this.onSaved(provider)
      return { status: 'saved' }
    } catch {
      return { status: 'error' }
    } finally {
      this.release(attempt)
    }
  }
}
