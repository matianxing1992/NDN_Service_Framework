import { randomUUID } from 'node:crypto';

export const LEGACY_PROMPT = '阅读 DSH 对话和当前 Spec 文档以及代码，沿着调用链条审计，修正文档并给出建议，不动代码。';
export const DEFAULT_PROMPT = '重点审计插件指定窗口内 DSH 围绕用户目标完成和尝试的工作。' +
  '先理解当前目标和必要历史，再核对本期对话、工具结果、代码及文档变化；相关设计或 Spec 存在时按其契约检查。' +
  '沿真实调用链检查输入输出、状态、所有权、错误处理与部署兼容性，优先指出影响目标的缺陷、无效重复和遗漏前置。' +
  '复用可靠证据，建议重复验证须说明原因；区分运行、候选和工作负载。' +
  '空提交或快照缺少材料不等于没有实现或验证，材料不足须明确指出缺什么。' +
  '允许回溯窗口外决定和证据，但区分本期新增、原有内容和来源不明变化，不声称全面审计历史。' +
  '输出本期进展、按影响排序的问题及证据、最小下一步与验证方法；无实质问题直接说明，不扩大目标。' +
  '只提出有据文档修正，不修改产品代码、不构建或实验；目标、范围和验收条件变更仅作建议。';
// One place names the audit selection. The previous form repeated the literal in the default, the
// guard and the error message, so updating the model meant finding all three and the GUI label still
// drifted. Raising it is a deliberate act: the guard exists so a silent downgrade cannot happen.
export const AUDIT_MODEL = 'gpt-6.2-sol';
export const AUDIT_EFFORT = 'xhigh';

const HOUR = 3600000;
const MAX_RECORDS = 200;
const MAX_RESULT_BYTES = 1024 * 1024;

export class AuditError extends Error {
  constructor(code, message, remedy, transient = false) {
    super(message);
    this.code = code;
    this.remedy = remedy;
    this.transient = transient;
  }
}

export function validateSettings(settings) {
  if (!Number.isInteger(settings.intervalHours) || settings.intervalHours < 1 ||
      settings.intervalHours > 5) {
    throw new AuditError('INVALID_INTERVAL', '审计周期必须为 1–5 小时的整数。', '请修改周期设置。');
  }
  if (typeof settings.enabled !== 'boolean') throw new Error('enabled must be boolean');
}

// The host supplies durable storage and DSH/Codex adapters. No agent callback awaits an audit.
export class AuditManager {
  constructor({ load, save, snapshot, captureBaseline, run, deliver, active, now = Date.now, notify = () => {} }) {
    Object.assign(this, { load, save, snapshot, captureBaseline, run, deliver, active, now, notify });
    this.state = { version: 1, sessions: {}, records: [] };
    this.running = new Map();
    this.configurationRequests = new Map();
    this.writes = Promise.resolve();
    this.disposed = false;
  }

  async open() {
    const stored = await this.load();
    if (stored) {
      if (stored.version !== 1 || !stored.sessions || !Array.isArray(stored.records)) {
        throw new AuditError('INVALID_HISTORY', '审计存储格式不兼容。', '保留原文件并检查插件版本。');
      }
      this.state = stored;
    }
    for (const settings of Object.values(this.state.sessions)) {
      if (settings.prompt === LEGACY_PROMPT) settings.prompt = DEFAULT_PROMPT;
      settings.model = AUDIT_MODEL;
      settings.effort = AUDIT_EFFORT;
    }
    for (const record of this.state.records) {
      if (record.status === 'running') {
        record.status = 'interrupted';
        record.finishedAt = this.now();
        record.error = { code: 'HOST_RESTARTED', message: '上次审计被 Host 退出中断。',
          remedy: '可手动重新审计；原记录已保留。' };
      }
    }
    await this.persist();
  }

  persist() {
    const value = structuredClone(this.state);
    const write = this.writes.catch(() => {}).then(() => this.save(value));
    this.writes = write;
    return write;
  }

  settings(sessionId) {
    return this.state.sessions[sessionId] ?? { enabled: false, intervalHours: 3,
      nextDue: null, model: AUDIT_MODEL, effort: AUDIT_EFFORT, prompt: DEFAULT_PROMPT };
  }

  publicSettings(sessionId) {
    const { auditPeriod, ...settings } = this.settings(sessionId);
    return { ...settings, auditWindow: auditPeriod ? { enabledAt: auditPeriod.startedAt,
      lastCoveredAt: auditPeriod.checkpoint?.basis.capturedAt ?? null,
      fromSeqExclusive: (auditPeriod.checkpoint ?? auditPeriod.baseline).basis.seq } : null };
  }

  async configure(sessionId, patch) {
    if (this.disposed) throw new Error('Audit manager disposed');
    if (!sessionId) throw new Error('sessionId required');
    const token = Symbol();
    this.configurationRequests.set(sessionId, token);
    const previous = this.settings(sessionId);
    const value = { ...previous, ...patch };
    validateSettings(value);
    if (value.model !== AUDIT_MODEL || value.effort !== AUDIT_EFFORT) {
      throw new AuditError('UNSUPPORTED_SELECTION', `审计固定使用 ${AUDIT_MODEL} / ${AUDIT_EFFORT}。`,
        '不可静默更换模型或降低思考等级。');
    }
    if (value.enabled && (!previous.enabled || !previous.auditPeriod)) {
      value.auditPeriod = await this.newPeriod(sessionId);
    }
    if (this.disposed) throw new Error('Audit manager disposed');
    if (this.configurationRequests.get(sessionId) !== token) throw new AuditError(
      'CONFIGURATION_SUPERSEDED', '设置已被后续操作替代。', '以面板当前显示的设置为准。');
    value.nextDue = value.enabled ? this.now() + value.intervalHours * HOUR : null;
    this.state.sessions[sessionId] = value;
    await this.persist();
    return structuredClone(this.publicSettings(sessionId));
  }

  async newPeriod(sessionId) {
    if (!this.captureBaseline) throw new AuditError('BASELINE_UNAVAILABLE',
      '无法捕获开启时的消息游标和工作区基线。', '检查会话及工作区访问权限后重新开启。');
    const baseline = await this.captureBaseline(sessionId);
    return { id: randomUUID(), startedAt: baseline.basis.capturedAt, baseline, checkpoint: null };
  }

  async tick() {
    if (this.disposed) return;
    for (const [sessionId, settings] of Object.entries(this.state.sessions)) {
      if (!settings.enabled || settings.nextDue > this.now() || !this.active(sessionId)) continue;
      // Advance the deadline once: missed intervals never become a backlog.
      settings.nextDue = this.now() + settings.intervalHours * HOUR;
      await this.persist();
      this.start(sessionId, 'scheduled').catch(error => this.safeNotify(error));
    }
  }

  async skipNext(sessionId) {
    const settings = this.state.sessions[sessionId];
    if (!settings?.enabled || !Number.isFinite(settings.nextDue)) {
      throw new AuditError('SCHEDULE_DISABLED', '定时审计尚未开启。', '开启后才能跳过下一次。');
    }
    const skippedDue = settings.nextDue;
    settings.nextDue = Math.max(this.now(), skippedDue) + settings.intervalHours * HOUR;
    settings.lastSkip = { at: this.now(), skippedDue, nextDue: settings.nextDue };
    await this.persist();
    return structuredClone(this.publicSettings(sessionId));
  }

  safeNotify(value) {
    try { this.notify(value); } catch { /* UI failure cannot interrupt DSH. */ }
  }

  // `promptOverride` is this audit's prompt only. It is never written into the session settings, so
  // typing a one-off instruction cannot silently become the standing configuration.
  start(sessionId, trigger = 'manual', blockedGoal = null, promptOverride = null) {
    if (this.disposed) return Promise.reject(new Error('Audit manager disposed'));
    if (this.running.has(sessionId)) return this.running.get(sessionId).promise;
    if (this.running.size) return Promise.reject(new AuditError('AUDIT_CAPACITY',
      '另一个会话的审计仍在运行。', '完成后重试；定时审计不会累积队列。'));
    const controller = new AbortController();
    const job = { controller, promise: null };
    // Reserve ownership before any asynchronous work starts.
    this.running.set(sessionId, job);
    // Every admitted audit consumes the current periodic slot. Joining an
    // existing job above must neither launch work nor postpone its deadline.
    const settings = this.state.sessions[sessionId];
    if (settings?.enabled) settings.nextDue = this.now() + settings.intervalHours * HOUR;
    job.promise = this.execute(sessionId, trigger, controller.signal, blockedGoal, promptOverride)
      .finally(() => this.running.delete(sessionId));
    return job.promise;
  }

  async execute(sessionId, trigger, signal, blockedGoal, promptOverride = null) {
    // Upgrade old enabled settings lazily: never invent the historical enable cursor.
    const current = this.state.sessions[sessionId];
    if (current?.enabled && !current.auditPeriod) current.auditPeriod = await this.newPeriod(sessionId);
    const settings = structuredClone(this.settings(sessionId));
    const record = { id: `AUD-${randomUUID()}`, sessionId, trigger,
      blockedGoal: blockedGoal ? structuredClone(blockedGoal) : null,
      model: settings.model, effort: settings.effort, effectiveModel: null,
      effectiveEffort: null,
      // A per-audit override is recorded on the record, which is exactly the scope it has; the
      // session settings keep whatever the operator configured.
      prompt: typeof promptOverride === 'string' && promptOverride.trim()
        ? promptOverride.trim() : settings.prompt,
      promptSource: typeof promptOverride === 'string' && promptOverride.trim() ? 'run' : 'settings',
      startedAt: this.now(),
      status: 'running', delivery: 'not-ready', replies: [], attempts: 0 };
    // Freeze the admitted period; toggling off/on while Codex runs cannot change it.
    const period = settings.enabled ? settings.auditPeriod : null;
    // Bound retained history, never evict running or undelivered completed work.
    while (this.state.records.length >= MAX_RECORDS) {
      const index = this.state.records.findIndex(r => r.status !== 'running' &&
        r.delivery !== 'pending' && r.delivery !== 'sending');
      if (index < 0) throw new Error('Audit history capacity reached');
      this.state.records.splice(index, 1);
    }
    this.state.records.push(record);
    let snapshot;
    try {
      await this.persist();
      signal.throwIfAborted();
      snapshot = await this.snapshot(sessionId, signal, record, period, settings);
      record.basis = snapshot.basis;
      record.dispatchedPrompt = snapshot.prompt;
      await this.persist();
      const result = await this.run({ record, settings, snapshot, signal,
        onAttempt: () => this.persist() });
      signal.throwIfAborted();
      if (!result.report?.trim()) throw new AuditError('EMPTY_REPORT',
        'Codex 没有返回完整审计报告。', '查看执行错误后重试。');
      if (Buffer.byteLength(JSON.stringify(result)) > MAX_RESULT_BYTES) {
        throw new AuditError('REPORT_CAPACITY', '审计报告及文档补丁超过 1 MiB 存储上限。',
          '请缩小文档修正范围；插件不会静默截断报告。');
      }
      record.report = result.report;
      const recovery = result.blockedRecovery;
      record.blockedRecovery = recovery &&
        ['actionable', 'needs-user', 'external-wait', 'not-applicable'].includes(recovery.decision) &&
        typeof recovery.summary === 'string' && Array.isArray(recovery.steps) &&
        recovery.steps.every(step => typeof step === 'string' && step.trim()) &&
        (recovery.decision !== 'actionable' || recovery.steps.length > 0 && recovery.summary.trim()) ?
        recovery : { decision: 'needs-user', summary: '审计未提供有效的结构化解阻方案，请人工核查。', steps: [] };
      record.documentChanges = result.documentChanges ?? [];
      record.effectiveModel = result.effectiveModel ?? null;
      record.effectiveEffort = result.effectiveEffort ?? null;
      record.modelObservation = result.modelObservation ?? null;
      record.codexThreadId = result.threadId ?? null;
      record.usage = result.usage ?? null;
      record.sourceCheck = result.sourceCheck ?? null;
      record.documentError = result.documentError ?? null;
      record.status = 'completed';
      record.finishedAt = this.now();
      record.delivery = 'pending';
      const live = this.state.sessions[sessionId];
      if (trigger !== 'goal-blocked' && live?.enabled &&
          period?.id === live.auditPeriod?.id &&
          ['since-enable', 'since-audit'].includes(snapshot.basis.scope?.kind)) {
        live.auditPeriod.checkpoint = { basis: { cwd: snapshot.basis.cwd,
          capturedAt: snapshot.basis.capturedAt, seq: snapshot.basis.seq,
          head: snapshot.basis.head, manifestHash: snapshot.basis.manifestHash,
          workingTreeChanges: snapshot.basis.workingTreeChanges }, hashes: snapshot.hashes };
      }
      await this.persist();
      await this.deliverRecord(record);
    } catch (error) {
      if (record.status !== 'completed') {
        record.status = signal.aborted ? 'cancelled' : 'failed';
        record.finishedAt = this.now();
      }
      record.error = { code: error.code ?? 'AUDIT_FAILED', message: error.message,
        remedy: error.remedy ?? '检查执行记录后重试。' };
      if (['CODEX_AUTH', 'CODEX_QUOTA', 'CODEX_MODEL', 'CODEX_SELECTION_MISMATCH', 'CODEX_NOT_FOUND', 'CUSTOM_PROVIDER_UNSUPPORTED'].includes(error.code)) {
        const current = this.state.sessions[sessionId];
        if (current) { current.enabled = false; current.nextDue = null; }
      }
      await this.persist();
      this.safeNotify(record);
    } finally {
      await snapshot?.dispose?.();
    }
    return structuredClone(record);
  }

  async deliverRecord(record) {
    if (record.delivery !== 'pending') return;
    record.deliveryAttempts = (record.deliveryAttempts ?? 0) + 1;
    // A previous save may have failed after computation. Never publish a report
    // unless this exact completed/pending state has reached durable storage.
    await this.persist();
    // Stable id must be deduplicated by the DSH adapter against inbox/history.
    const receipt = await this.deliver(record);
    if (receipt?.held) {
      record.delivery = 'held';
      record.error = { code: 'RECOVERY_SUPERSEDED', message: receipt.reason,
        remedy: '报告已保留；不会向已暂停、更换或修订的目标自动发起工作。' };
      await this.persist();
      return;
    }
    if (!receipt?.messageId) throw new Error('Missing DSH delivery receipt');
    record.delivery = 'delivered';
    record.deliveredAt = this.now();
    record.messageId = receipt.messageId;
    delete record.error;
    await this.persist();
  }

  async retryDelivery(id, { automatic = false } = {}) {
    const record = this.state.records.find(r => r.id === id);
    if (!record || record.status !== 'completed') throw new Error('Completed audit required');
    if (automatic && (record.deliveryAttempts ?? 0) >= 5) return;
    if (!automatic) record.deliveryAttempts = 0;
    try { await this.deliverRecord(record); }
    catch (error) {
      record.error = { code: 'DELIVERY_FAILED', message: error.message,
        remedy: '报告已保存；自动投递最多五次，可检查会话后手动重试。' };
      await this.persist();
      throw error;
    }
  }

  async acknowledge({ auditId, sessionId, messageId, text, at, seq, turn, step }) {
    const record = this.state.records.find(r => r.id === auditId && r.sessionId === sessionId);
    if (!record || record.delivery !== 'delivered' || !messageId || !text?.includes(auditId)) return false;
    if (record.replies.some(r => r.messageId === messageId)) return false;
    record.replies.push({ messageId, text, at,
      ...(Number.isSafeInteger(seq) ? { seq } : {}),
      ...(Number.isSafeInteger(turn) ? { turn } : {}),
      ...(Number.isSafeInteger(step) ? { step } : {}) });
    await this.persist();
    return true;
  }

  cancel(sessionId) {
    this.running.get(sessionId)?.controller.abort(new Error('用户取消审计'));
  }

  async close() {
    this.disposed = true;
    this.configurationRequests.clear();
    for (const job of this.running.values()) job.controller.abort(new Error('Host 正在退出'));
    await Promise.allSettled([...this.running.values()].map(job => job.promise));
    await this.writes;
  }
}
