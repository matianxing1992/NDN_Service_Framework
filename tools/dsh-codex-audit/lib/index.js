import { TypertRemoteService, Remote } from '@deepseek-ai/dsh-typert-protocol';
import { defineDomain, domainTable } from '@deepseek-ai/dsh-storage-domain';
import { z } from 'zod';
import { AuditManager, AuditError } from './audit-manager.js';
import { runCodex, resolveCodex } from './codex-runner.js';
import { createSnapshot, checkSnapshotCurrent, describeBasis } from './snapshot.js';
import { reviewDocumentChanges, applyDocumentChange } from './document-changes.js';
import { serializeConversation } from './conversation.js';
import { readCodexSelection } from './model-observation.js';
import { GoalGuard } from './goal-guard.js';

export const name = 'dsh-codex-audit';
const spec = defineDomain({ name: 'codex_audit', version: 1,
  tables: { state: domainTable(z.object({ version: z.literal(1),
    sessions: z.record(z.string(), z.unknown()), records: z.array(z.unknown()) })) } });
const initializers = [];

export default class CodexAudit extends TypertRemoteService {
  static inject = ['agents', 'sessionQuery', 'storageDomain', 'typertGateway'];

  constructor(ctx) {
    super(ctx, 'codexAudit', { namespace: 'codexAudit' });
    for (const initializer of initializers) initializer.call(this);
    this.timer = null;
    this.closing = false;
    this.ready = this.initialize().catch(error => {
      this.bootError = { code: error.code ?? 'INITIALIZATION_FAILED',
        message: error.message, remedy: '检查 DSH 插件依赖、存储权限及版本。' };
      ctx.logger.error(error);
    });
    ctx.effect(() => async () => {
      this.closing = true;
      clearTimeout(this.timer);
      await this.ready;
      await Promise.all([this.guard?.close(), this.manager?.close()]);
      await this.blockedWork;
      await this.domain?.close();
    });
  }

  async initialize() {
    this.domain = await this.ctx.storageDomain.open(spec);
    const table = this.domain.table('state');
    this.manager = new AuditManager({
      load: async () => table.get('history'), save: value => table.put('history', value),
      active: id => this.auditEligible(id),
      captureBaseline: async id => {
        const cut = await this.ctx.sessionQuery.observeSession(id, { projectionMode: 'none' });
        try {
          return await createSnapshot({ cwd: cut.header.cwd, seq: cut.cursor, baselineOnly: true });
        } finally { cut[Symbol.dispose](); }
      },
      snapshot: async (id, signal, record, period, settings) => {
        const cut = await this.ctx.sessionQuery.observeSession(id, { signal, projectionMode: 'none' });
        try {
          settings ??= this.manager.settings(id);
          const blockedPrompt = record?.blockedGoal ? '\n\n本轮由 goal blocked 事件立即触发。' +
            `当前目标及阻塞原因：${JSON.stringify(record.blockedGoal)}。` +
            '优先沿调用链分析阻塞根因，并给出 DSH 可立即执行的最小解阻步骤、验证方式和停止条件。' +
            '只审计和提出文档修正，不修改产品代码；可自行解决或决定的事项请直接决定，' +
            '包括公开资源下载、数据库选型、依赖安装和配置；给出 DSH 可执行的步骤及验证。' +
            '只有缺少无法从已授权来源取得的私有凭据、付费授权、用户独有信息或真正无替代的外部条件时才等待，' +
            '不要建议绕过权限、安全检查或验收标准。用 blockedRecovery 返回解阻决策。' : '';
          return await createSnapshot({ cwd: cut.header.cwd, seq: cut.cursor,
            conversation: serializeConversation(cut.events), prompt: record.prompt + blockedPrompt, signal,
            period, trigger: record.trigger, intervalHours: settings.intervalHours });
        } finally { cut[Symbol.dispose](); }
      },
      run: async request => {
        const result = await runCodex(request);
        try { result.documentChanges = reviewDocumentChanges(result.documentChanges, request.snapshot); }
        catch (error) {
          result.documentChanges = [];
          result.documentError = { code: 'DOCUMENT_PROPOSAL_REJECTED', message: error.message,
            remedy: '报告已保留；无效文档补丁未应用，请根据报告重新提出修正。' };
        }
        try { result.sourceCheck = await checkSnapshotCurrent(request.snapshot); }
        catch (error) {
          result.sourceCheck = { stale: true, changedCount: null, currentHead: '未确认',
            error: { code: 'SOURCE_RECHECK_FAILED', message: error.message } };
        }
        return result;
      },
      deliver: record => this.deliver(record),
      notify: value => { this.lastError = value.error ?? {
        code: value.code ?? 'AUDIT_FAILED', message: value.message, remedy: value.remedy ?? '查看执行记录。' }; },
    });
    await this.manager.open();
    this.guard = new GoalGuard({ manager: this.manager,
      agent: id => this.ctx.agents.get(id), goals: () => this.ctx.get('goals'),
      observe: id => this.ctx.sessionQuery.observeSession(id, { projectionMode: 'none' }) });
    this.ctx.on('session/event', (session, event) => {
      if (['turn/end', 'goal/change', 'user/message'].includes(event.type)) {
        this.guard.invalidate(this.ctx.agents.get(session.id));
      }
    });
    this.ctx.on('goal/changed', ({ change }) => {
      if (change.operation === 'block') this.requestBlockedReview();
    });
    let observed = false;
    for (const record of this.manager.state.records) {
      if (record.status !== 'completed' || !record.codexThreadId || record.modelObservation?.status === 'confirmed') continue;
      const observation = await readCodexSelection({ threadId: record.codexThreadId,
        startedAt: record.startedAt, finishedAt: record.finishedAt });
      record.modelObservation = observation;
      if (observation.status === 'confirmed') {
        record.effectiveModel = observation.model;
        record.effectiveEffort = observation.effort;
        record.modelMismatch = observation.model !== record.model || observation.effort !== record.effort;
      }
      observed = true;
    }
    if (observed) await this.manager.persist();
    this.schedule();
  }

  schedule() {
    this.timer = setTimeout(async () => {
      try {
        await this.guard.tick();
        await this.reconcileDeliveries();
        await this.requestBlockedReview();
        await this.applyPendingDocuments();
        await this.collectReplies();
        await this.manager.tick();
        for (const record of this.manager.state.records) {
          if (record.status === 'completed' && record.delivery === 'pending' &&
              (record.deliveryAttempts ?? 0) < 5 &&
              this.ctx.agents.get(record.sessionId)) {
            try { await this.manager.retryDelivery(record.id, { automatic: true }); }
            catch (error) { this.lastError = { code: 'DELIVERY_FAILED', message: error.message }; }
          }
        }
      } catch (error) { this.lastError = { code: 'BACKGROUND_FAILED', message: error.message }; }
      if (!this.manager.disposed) this.schedule();
    }, 10000);
  }

  requestBlockedReview() {
    if (this.closing || !this.guard) return Promise.resolve();
    if (this.blockedWork) return this.blockedWork;
    this.blockedWork = this.reviewBlockedGoals().catch(error => {
      this.lastError = { code: 'BLOCKED_REVIEW_FAILED', message: error.message };
    }).finally(() => { this.blockedWork = null; });
    return this.blockedWork;
  }

  async reviewBlockedGoals() {
    for (const [id, settings] of Object.entries(this.manager.state.sessions)) {
      if (this.closing || !settings.guard?.enabled) continue;
      const guard = settings.guard;
      const agent = this.ctx.agents.get(id);
      const goal = agent && this.ctx.get('goals')?.get(agent);
      if (!goal || goal.id !== guard.goalId || goal.phase !== 'blocked') continue;
      if (!guard.blocked || guard.blocked.goalId !== goal.id || guard.blocked.revision !== goal.revision) {
        const sameReason = guard.blocked?.goalId === goal.id &&
          JSON.stringify(guard.blocked?.reason) === JSON.stringify(goal.blockedReason);
        const cycles = sameReason ? (guard.blocked?.cycles ?? 0) + 1 : 1;
        guard.blocked = { goalId: goal.id, revision: goal.revision, reason: goal.blockedReason, cycles,
          status: cycles > 3 ? 'limit' : 'queued' };
        this.guard.record(guard, { action: 'blocked', goalId: goal.id, revision: goal.revision,
          error: goal.blockedReason?.message });
        await this.manager.persist();
      }
      const blocked = guard.blocked;
      if (blocked.status === 'queued') {
        if (blocked.retryAt > Date.now()) continue;
        if (this.manager.running.size) continue;
        const current = this.ctx.get('goals')?.get(agent);
        if (!this.guard.settings(id).enabled || this.guard.settings(id) !== guard ||
            current?.id !== goal.id || current.revision !== goal.revision || current.phase !== 'blocked') continue;
        blocked.status = 'auditing';
        blocked.auditAttempts = (blocked.auditAttempts ?? 0) + 1;
        // Reserve through the existing audit manager; never await the model inside maintenance.
        const job = this.manager.start(id, 'goal-blocked',
          { id: goal.id, revision: goal.revision, reason: goal.blockedReason });
        blocked.auditId = this.manager.state.records.at(-1)?.id;
        job.then(() => this.requestBlockedReview()).catch(error => {
          this.lastError = { code: 'BLOCKED_AUDIT_FAILED', message: error.message };
        });
        await this.manager.persist();
        continue;
      }
      if (!['auditing', 'awaiting-delivery', 'resume-backoff'].includes(blocked.status)) continue;
      const record = this.manager.state.records.find(r => r.id === blocked.auditId);
      if (!record || record.status === 'running') continue;
      if (record.status === 'failed' && record.error?.code === 'SNAPSHOT_CHANGED' && blocked.auditAttempts < 3) {
        blocked.status = 'queued';
        blocked.retryAt = Date.now() + 20000 * 2 ** (blocked.auditAttempts - 1);
        blocked.error = record.error.message;
        await this.manager.persist();
        continue;
      }
      const status = record.status !== 'completed' ? 'audit-failed' :
        record.delivery === 'held' ? 'superseded' : await this.guard.resumeBlocked(id, record);
      if (blocked.status !== status) {
        blocked.status = status;
        blocked.summary = record.blockedRecovery?.summary;
        blocked.steps = record.blockedRecovery?.steps;
        blocked.error = record.error?.message ?? blocked.error;
        await this.manager.persist();
      }
    }
  }

  auditEligible(id) {
    const agent = this.ctx.agents.get(id);
    if (!agent) return false;
    if (agent.status === 'running') return true;
    const settings = this.manager.settings(id).guard;
    if (!settings?.enabled) return false;
    try {
      const goal = this.ctx.get('goals')?.get(agent);
      return goal?.phase === 'active' && goal.id === settings.goalId;
    } catch { return false; }
  }

  async deliver(record) {
    const agent = this.ctx.agents.get(record.sessionId);
    if (!agent) throw new Error('原 DSH 会话未加载；报告已保存，等待投递。');
    if (record.blockedGoal) {
      const guard = this.guard.settings(record.sessionId);
      const goal = this.ctx.get('goals')?.get(agent);
      if (!guard.enabled || guard.goalId !== record.blockedGoal.id || goal?.phase !== 'blocked' ||
          goal.id !== record.blockedGoal.id || goal.revision !== record.blockedGoal.revision) {
        return { held: true, reason: '审计期间目标被暂停、更换、恢复或修订，未自动投递继续指令。' };
      }
    }
    if (!agent.model?.current?.()) throw new AuditError('DSH_MODEL_UNAVAILABLE',
      '原 DSH 会话尚未配置 provider/model，暂不能读取报告。', '先为 DSH 会话选择模型；报告已保存。');
    const messageId = `codex-audit-${record.id}`;
    const pending = [...agent.inbox.nextStep, ...agent.inbox.nextTurn];
    if (pending.some(message => message.id === messageId)) return { messageId };
    const cut = await this.ctx.sessionQuery.observeSession(record.sessionId, { projectionMode: 'none' });
    try {
      if (cut.events.some(event => event.type === 'user/message' &&
          (event.data?.message ?? event.data)?.id === messageId)) return { messageId };
    } finally { cut[Symbol.dispose](); }
    const message = Object.freeze({ id: messageId, role: 'user',
      source: { kind: 'codex-audit' }, content: [{ type: 'text',
        text: `[独立 Codex 审计 ${record.id}]\n` +
          `对话截止 seq=${record.basis.seq}。${describeBasis(record.basis)}\n` +
          (record.sourceCheck?.error ? `源码复核失败：${record.sourceCheck.error.message}；本报告只对旧快照有效。\n` :
            record.sourceCheck?.stale ? `已检测到源码变化：${record.sourceCheck.changedCount} 个源码文件，` +
            `当前 HEAD=${record.sourceCheck.currentHead}。此报告基于旧快照，建议须先复核。\n` : '') +
          `${record.report}\n\n` +
          (record.blockedGoal ? record.blockedRecovery?.decision === 'actionable' ?
            `解阻计划：${record.blockedRecovery.summary}\n` +
            record.blockedRecovery.steps.map((step, index) => `${index + 1}. ${step}`).join('\n') +
            '\n请按计划继续修复并验证；如当前事实不支持计划，明确报告新阻塞。计划不是修复完成的证据。\n' :
            `仍需处理的阻塞：${record.blockedRecovery?.summary ?? '尚未获得可执行方案'}。` +
            '请说明需要的用户输入或外部条件并保留 blocked，不要擅自恢复目标。\n' : '') +
          `请明确回复“审计回应 ${record.id}”，说明接受、不同意、需要验证或暂缓处理及理由，` +
          (record.blockedGoal && record.blockedRecovery?.decision !== 'actionable' ?
            '等待所需输入，不执行新的修复轮次。' : '并继续原任务。') +
          '审计建议不能替代测试证据，也不能放宽验收标准。' }] });
    agent.steer(message);
    return { messageId };
  }

  async collectReplies() {
    const waiting = this.manager.state.records.filter(r => r.delivery === 'delivered' && !r.replies.length);
    const bySession = new Map();
    for (const record of waiting) {
      if (!bySession.has(record.sessionId)) bySession.set(record.sessionId, []);
      bySession.get(record.sessionId).push(record);
    }
    for (const [sessionId, records] of bySession) {
      if (!this.ctx.agents.get(sessionId)) continue;
      const cut = await this.ctx.sessionQuery.observeSession(sessionId, { projectionMode: 'none' });
      try {
        // Observations may materialize history on access. Read it once and index the
        // report admissions once, rather than searching the whole log for every reply.
        const events = cut.events;
        const messageIds = new Set(records.map(record => record.messageId));
        const admissions = new Map();
        for (const event of events) {
          if (event.type !== 'user/message') continue;
          const id = (event.data?.message ?? event.data)?.id;
          if (messageIds.has(id) && !admissions.has(id)) admissions.set(id, event.seq);
        }
        for (const event of events) {
          if (event.type !== 'assistant/message') continue;
          const message = event.data?.message;
          const text = message?.content?.filter(part => part.type === 'text').map(part => part.text).join('\n') ?? '';
          for (const record of records) {
            const admittedSeq = admissions.get(record.messageId);
            if (admittedSeq === undefined || event.seq <= admittedSeq ||
                !text.includes(`审计回应 ${record.id}`)) continue;
            await this.manager.acknowledge({ auditId: record.id, sessionId,
              messageId: message.id, text, at: event.time, seq: event.seq,
              turn: event.data.turn, step: event.data.step });
          }
        }
      } finally { cut[Symbol.dispose](); }
    }
  }

  async reconcileDeliveries() {
    for (const record of this.manager.state.records) {
      if (record.delivery !== 'delivered' || Number.isSafeInteger(record.admittedSeq)) continue;
      const agent = this.ctx.agents.get(record.sessionId);
      if (!agent) continue;
      const cut = await this.ctx.sessionQuery.observeSession(record.sessionId, { projectionMode: 'none' });
      try {
        const admitted = cut.events.find(event => event.type === 'user/message' &&
          (event.data?.message ?? event.data)?.id === record.messageId);
        if (admitted) {
          record.admittedSeq = admitted.seq;
          record.admittedAt = admitted.time;
          await this.manager.persist();
          continue;
        }
        const pending = [...agent.inbox.nextStep, ...agent.inbox.nextTurn];
        if (pending.some(message => message.id === record.messageId) || agent.status !== 'idle' ||
            Date.now() - record.deliveredAt < 10000) continue;
        record.delivery = 'pending';
        record.error = { code: 'DSH_REPORT_NOT_ADMITTED',
          message: 'DSH 在读取报告前结束了步骤；未发现对应的对话记录。',
          remedy: '报告已保留，将按原消息 ID 有限重投；请检查 DSH 模型配置及失败原因。' };
        await this.manager.persist();
      } finally { cut[Symbol.dispose](); }
    }
  }

  async applyPendingDocuments() {
    for (const record of this.manager.state.records) {
      // Parallel DSH work must never race an automatic document write. Stale
      // or unverified reports keep their proposals for explicit review instead.
      if (record.status !== 'completed' || record.delivery !== 'delivered' || record.sourceCheck?.stale !== false ||
          !Number.isSafeInteger(record.admittedSeq) ||
          this.ctx.agents.get(record.sessionId)?.status !== 'idle') continue;
      let changed = false;
      for (const proposal of record.documentChanges ?? []) {
        if (proposal.status !== 'pending') continue;
        try { applyDocumentChange(proposal); }
        catch (error) {
          proposal.status = 'error';
          proposal.error = { code: 'DOCUMENT_APPLY_FAILED', message: error.message };
        }
        changed = true;
      }
      if (changed) await this.manager.persist();
    }
  }

  async view(request) {
    await this.ready;
    if (this.bootError) return { error: this.bootError };
    const sessionId = request.sessionId;
    if (typeof sessionId !== 'string' || !sessionId) throw new Error('sessionId required');
    const all = this.manager.state.records.filter(r => r.sessionId === sessionId).slice().reverse();
    const offset = request.offset ?? 0;
    if (!Number.isInteger(offset) || offset < 0 || offset > 200) throw new Error('Invalid history offset');
    const records = request.auditId ? all.filter(r => r.id === request.auditId) :
      all.slice(offset, offset + 20).map(record => {
        const { report, documentChanges, prompt, dispatchedPrompt, replies, ...metadata } = record;
        return { ...metadata, summary: true, replies: replies.map(r => ({ messageId: r.messageId, at: r.at })) };
      });
    if (request.auditId && records.some(r => r.replies.some(reply => !Number.isSafeInteger(reply.seq)))) {
      const cut = await this.ctx.sessionQuery.observeSession(sessionId, { projectionMode: 'none' });
      try {
        let changed = false;
        for (const record of records) for (const reply of record.replies) {
          if (Number.isSafeInteger(reply.seq)) continue;
          const event = cut.events.find(e => e.type === 'assistant/message' && e.data.message.id === reply.messageId);
          if (event) { Object.assign(reply, { seq: event.seq, turn: event.data.turn, step: event.data.step }); changed = true; }
        }
        if (changed) await this.manager.persist();
      } finally { cut[Symbol.dispose](); }
    }
    return { settings: this.manager.publicSettings(sessionId),
      guard: await this.guard.view(sessionId),
      records, total: all.length, offset,
      running: this.manager.running.has(sessionId), error: this.lastError ?? null };
  }

  async control(request) {
    await this.ready;
    if (this.bootError) throw new Error(this.bootError.message);
    const { sessionId, action } = request;
    if (typeof sessionId !== 'string' || !this.ctx.agents.get(sessionId)) throw new Error('原 DSH 会话未加载。');
    if (action === 'configure-guard') {
      await this.guard.configure(sessionId, request.enabled, request.goalRef);
      await this.requestBlockedReview();
    } else if (action === 'review-blocked') {
      const guard = this.guard.settings(sessionId);
      const goal = this.ctx.get('goals')?.get(this.ctx.agents.get(sessionId));
      if (!guard.enabled || goal?.phase !== 'blocked' || goal.id !== request.goalRef?.id ||
          goal.revision !== request.goalRef.revision) throw new Error('阻塞目标已变化或守护未开启。');
      if (this.manager.running.size) throw new Error('已有审计正在运行，请等待完成。');
      guard.blocked = { goalId: goal.id, revision: goal.revision, reason: goal.blockedReason,
        cycles: 1, status: 'queued' };
      delete guard.pending;
      await this.manager.persist();
      await this.requestBlockedReview();
    } else if (action === 'resume-goal') {
      if (!request.goalRef) throw new Error('缺少当前目标引用，请刷新后重试。');
      await this.guard.resume(sessionId, request.goalRef);
    } else if (action === 'configure') {
      const patch = {};
      for (const key of ['enabled', 'intervalHours', 'codexPath']) {
        if (Object.hasOwn(request, key)) patch[key] = request[key];
      }
      if (patch.codexPath !== undefined && typeof patch.codexPath !== 'string') throw new Error('Invalid Codex path');
      if (patch.enabled) await resolveCodex(patch.codexPath ?? this.manager.settings(sessionId).codexPath);
      await this.manager.configure(sessionId, patch);
      this.lastError = null;
    } else if (action === 'check') {
      try { return { codexPath: await resolveCodex(this.manager.settings(sessionId).codexPath) }; }
      catch (error) { return { error: { code: error.code, message: error.message, remedy: error.remedy } }; }
    } else if (action === 'skip-next') {
      await this.manager.skipNext(sessionId);
    } else if (action === 'apply-document') {
      const record = this.manager.state.records.find(r => r.id === request.auditId && r.sessionId === sessionId);
      const change = record?.documentChanges?.[request.index];
      if (!change) throw new Error('Document change not found');
      if (this.ctx.agents.get(sessionId).status !== 'idle') {
        throw new Error('DSH 正在工作，已保留文档补丁；待空闲且哈希一致时可应用。');
      }
      applyDocumentChange(change);
      await this.manager.persist();
    } else if (action === 'run') {
      this.lastError = null;
      // The prompt a caller types for one run is passed straight through and is not stored in the
      // session settings; a later scheduled audit therefore still uses the configured prompt.
      const prompt = typeof request.prompt === 'string' ? request.prompt : null;
      this.manager.start(sessionId, 'manual', null, prompt)
        .catch(error => { this.lastError = { message: error.message }; });
    } else if (action === 'cancel') this.manager.cancel(sessionId);
    else if (action === 'retry-delivery') {
      const record = this.manager.state.records.find(r => r.id === request.auditId && r.sessionId === sessionId);
      if (!record) throw new Error('Audit not found in this session');
      await this.manager.retryDelivery(record.id);
    } else throw new Error('Unknown audit action');
    return this.view({ sessionId });
  }
}

// Stage-3 decorators applied explicitly keep this plugin plain JavaScript.
for (const method of ['view', 'control']) {
  Remote({ resources: { session: request => request.sessionId } })(CodexAudit.prototype[method], {
    name: method, kind: 'method', private: false, static: false,
    addInitializer: initializer => initializers.push(initializer),
  });
}
