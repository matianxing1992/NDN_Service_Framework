import { expandAssistantStream } from '@deepseek-ai/dsh-llm'

const WINDOW_MS = 60000
const MAX_SESSIONS = 128
const MAX_STEPS = 256
const MAX_BUCKETS = 512
const MAX_SEED_EVENTS = 4096

function charsOf(chunk) {
  if (!['text-delta', 'reasoning-delta', 'tool-call-delta'].includes(chunk?.type)) return 0
  return Object.entries(chunk).reduce((sum, [key, value]) => sum +
    (typeof value === 'string' && !['type', 'id', 'callId', 'blockType'].includes(key) ? value.length : 0), 0)
}

// Counts are owned by the generating session, never by the caller's async scope.
export class SpeedCollector {
  constructor(ctx, now = Date.now) {
    this.ctx = ctx
    this.now = now
    this.nodes = new Map()
    this.startedAt = now()
    this.overflowUntil = 0
  }

  gc() {
    const cutoff = this.now() - WINDOW_MS
    for (const node of this.nodes.values()) {
      for (const [key, step] of node.steps) {
        for (const [bucket, sample] of step.samples) if (sample.at <= cutoff) step.samples.delete(bucket)
        if (step.lastAt <= cutoff) node.steps.delete(key)
      }
    }
    // Keep a quiet ancestor only while a retained descendant still needs its edge.
    const retained = new Set()
    for (const node of this.nodes.values()) if (node.steps.size || node.errorUntil > this.now()) {
      let current = node
      while (current && !retained.has(current.id)) {
        retained.add(current.id)
        current = this.nodes.get(current.parentId)
      }
    }
    for (const id of this.nodes.keys()) if (!retained.has(id) && !this.seeding) this.nodes.delete(id)
  }

  node(session) {
    let node = this.nodes.get(session.id)
    if (node) return node
    if (this.nodes.size >= MAX_SESSIONS) this.gc()
    if (this.nodes.size >= MAX_SESSIONS) {
      this.overflowUntil = this.now() + WINDOW_MS
      return null
    }
    const parentId = session.header?.parentSession
    const parent = parentId ? this.ctx.agents.get(parentId) : null
    const owned = parent && this.ctx.agents.isOwnedBy(session.id, parent)
    node = { id: session.id, parentId: owned ? parentId : null, steps: new Map(), errorUntil: 0 }
    this.nodes.set(node.id, node)
    if (owned) this.node(parent.session)
    return node
  }

  step(node, key, turn, index) {
    let step = node.steps.get(key)
    if (step) return step
    if (node.steps.size >= MAX_STEPS) {
      this.gc()
      // GC may have removed this empty node. Restore its identity before adding work.
      this.nodes.set(node.id, node)
    }
    if (node.steps.size >= MAX_STEPS) {
      node.errorUntil = this.now() + WINDOW_MS
      return null
    }
    step = { key, turn, index, chars: 0, samples: new Map(), lastAt: this.now(),
      lastIndex: -1, tokens: null, settled: false, seq: null }
    node.steps.set(key, step)
    return step
  }

  sample(node, step, time, chunk) {
    if (!Number.isFinite(time) || time > this.now() + 1000) return
    const chars = charsOf(chunk)
    step.chars += chars
    step.lastAt = Math.max(step.lastAt, time)
    if (!chars || time <= this.now() - WINDOW_MS) return
    const bucket = Math.floor(time / 250)
    let sample = step.samples.get(bucket)
    if (!sample) {
      if (step.samples.size >= MAX_BUCKETS) {
        node.errorUntil = this.now() + WINDOW_MS
        return
      }
      sample = { at: time, chars: 0 }
      step.samples.set(bucket, sample)
    }
    sample.chars += chars
    // Never move old characters forward merely because the same bucket gets another chunk.
    sample.at = Math.min(sample.at, time)
  }

  frame(agent, frame) {
    const node = this.node(agent.session)
    if (!node || !frame?.attemptId) return
    const key = `attempt:${frame.attemptId}`
    if (frame.type === 'start') {
      this.step(node, key, frame.turn, frame.step)
      node.active = key
      return
    }
    const step = node.steps.get(key) ?? (frame.type === 'chunk' ? this.step(node, key) : null)
    if (!step || step.settled) return
    if (frame.type === 'chunk') {
      if (!Number.isSafeInteger(frame.index) || frame.index <= step.lastIndex) return
      step.lastIndex = frame.index
      node.active = key
      this.sample(node, step, frame.time, frame.chunk)
    } else if (frame.type === 'end' && (frame.outcome?.kind === 'abandoned' ||
      frame.outcome?.eventType === 'assistant/attempt')) {
      node.steps.delete(key)
    }
  }

  event(session, event) {
    // Fork seeds and their inherited usage are never generated output of this child.
    if (!session.isOwnSeq(event.seq) || !['assistant/message', 'assistant/attempt'].includes(event.type)) return
    const data = event.data
    const node = this.node(session)
    if (!node || !data) return
    if ([...node.steps.values()].some(step => step.seq === event.seq)) return
    let step = node.steps.get(node.active)
    if (step && (step.settled || step.turn !== undefined &&
      (step.turn !== data.turn || step.index !== data.step))) step = null
    if (event.type === 'assistant/attempt') {
      if (step) node.steps.delete(step.key)
      return
    }
    step ??= this.step(node, `event:${event.seq}`, data.turn, data.step)
    if (!step) return
    // The compact durable stream is authoritative and repairs a mid-stream attach.
    // Validate/expand before replacing any good live samples.
    const chunks = expandAssistantStream(data.stream ?? [])
    step.samples.clear()
    step.chars = 0
    for (const chunk of chunks) this.sample(node, step, chunk.time, chunk.chunk)
    const tokens = data.usage?.outputTokens
    step.tokens = Number.isFinite(tokens) && tokens >= 0 ? tokens : null
    step.seq = event.seq
    step.settled = true
    step.lastAt = Math.max(step.lastAt, event.time)
    if (!step.chars && step.tokens > 0 && event.time > this.now() - WINDOW_MS) {
      step.samples.set('usage', { at: event.time, tokens: step.tokens, chars: 0 })
    }
  }

  safely(operation, session) {
    try { operation() } catch {
      const node = this.node(session)
      if (node) node.errorUntil = this.now() + WINDOW_MS
      else this.overflowUntil = this.now() + WINDOW_MS
    }
  }

  async seed() {
    const agents = this.ctx.agents.list()
    if (agents.length > MAX_SESSIONS) this.overflowUntil = this.now() + WINDOW_MS
    this.seeding = true
    // Capture live ownership before the first await: a child may finish while
    // its final history observation is being acquired.
    for (const agent of agents.slice(0, MAX_SESSIONS)) this.node(agent.session)
    const signal = AbortSignal.timeout(5000)
    try {
    for (const agent of agents.slice(0, MAX_SESSIONS)) {
      let cut
      try {
        cut = await this.ctx.sessionQuery.observeSession(agent.id, { projectionMode: 'none', signal })
        const cutoff = this.now() - WINDOW_MS
        let scanned = 0
        const recent = []
        for (let i = cut.events.length - 1; i >= 0; i--) {
          const event = cut.events[i]
          if (event.time <= cutoff) break
          if (++scanned > MAX_SEED_EVENTS) {
            const node = this.node(agent.session)
            if (node) node.errorUntil = this.now() + WINDOW_MS
            break
          }
          recent.push(event)
        }
        for (const event of recent.reverse()) this.safely(() => this.event(agent.session, event), agent.session)
      } catch {
        const node = this.node(agent.session)
        if (node) node.errorUntil = this.now() + WINDOW_MS
      } finally { cut?.[Symbol.dispose]() }
    }
    } finally { this.seeding = false }
  }

  snapshot(sessionId) {
    this.gc()
    const session = this.ctx.agents.get(sessionId)?.session
    const root = session ? this.node(session) : this.nodes.get(sessionId)
    const aggregate = !root?.parentId
    const members = [...this.nodes.values()].filter(node => {
      if (node.id === sessionId) return true
      if (!aggregate) return false
      const seen = new Set()
      let parentId = node.parentId
      while (parentId && !seen.has(parentId)) {
        if (parentId === sessionId) return true
        seen.add(parentId)
        parentId = this.nodes.get(parentId)?.parentId
      }
      return false
    })
    if (this.overflowUntil > this.now() || members.some(node => node.errorUntil > this.now())) {
      return { status: 'unavailable', reason: '统计容量或事件读取异常，不能宣称完整速度。' }
    }
    let tokens = 0
    let oldest = this.now()
    let newest = 0
    let estimated = false
    for (const node of members) for (const step of node.steps.values()) {
      for (const sample of step.samples.values()) {
        oldest = Math.min(oldest, sample.at)
        newest = Math.max(newest, sample.at)
        const weight = step.tokens !== null && step.chars > 0 ? step.tokens / step.chars : 0.32
        tokens += sample.tokens ?? sample.chars * weight
        if (!step.settled || step.tokens === null) estimated = true
      }
    }
    const spanMs = newest ? Math.min(WINDOW_MS, Math.max(2000, this.now() - oldest)) : WINDOW_MS
    return { status: 'ok', sessionId, scope: aggregate ? 'family' : 'self',
      reading: tokens / (spanMs / 1000), tokens, spanMs, estimated,
      fresh: newest > this.now() - 15000, members: members.length,
      observedSince: this.startedAt, capturedAt: this.now() }
  }

  close() { this.nodes.clear() }
}
