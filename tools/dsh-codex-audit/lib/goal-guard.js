// Observe native goal ownership; this module never queues model input or creates a goal loop.
export function restartBoundary(events, goalId) {
  let boundary = { kind: 'none', restartSeq: null };
  for (const event of events) {
    if (event.type === 'goal/change') {
      boundary = { kind: 'none', restartSeq: null };
      if (event.data?.goal?.id !== goalId) continue;
    }
    if (event.type === 'user/message' &&
        (event.data?.message ?? event.data)?.source?.kind === 'user') {
      boundary = { kind: 'decision', restartSeq: null };
    }
    if (event.type !== 'turn/end') continue;
    const reason = event.data?.reason;
    if (reason?.kind === 'completed') continue;
    if (reason?.kind === 'aborted' && reason.reason?.kind === 'disposed') {
      if (!['decision', 'error'].includes(boundary.kind)) {
        boundary = { kind: 'restart', restartSeq: event.seq };
      }
    } else {
      boundary = { kind: reason?.kind === 'aborted' ? 'decision' : 'error', restartSeq: null };
    }
  }
  return boundary;
}

const MESSAGES = {
  off: '目标续跑守护未开启。', unloaded: '会话未加载；不会自动打开历史会话。',
  unsupported: '当前 DSH 缺少可用的原生 goals 服务；请检查版本。',
  'no-goal': '当前没有原生 goal；请先明确任务目标。',
  'goal-changed': '当前 goal 已更换；守护授权只适用于原目标，请重新开启。',
  paused: '目标已人工暂停，守护不会恢复。', blocked: '目标已阻塞，请先解决所列阻塞。',
  complete: '目标已完成。', budget: '目标轮数已耗尽；不会自动扩大上限。',
  running: 'DSH 正在执行任务。', queued: '有待处理输入，等待原生调度器。',
  'model-missing': 'DSH 尚未配置可用模型，不能恢复目标。',
  armed: '目标已启用，后续轮次由 DSH 原生驱动推进。',
  stalled: '目标已启用但空闲超过 60 秒；请检查原生驱动、子任务或持久化错误。',
  decision: '检测到人工或控制器停止；不会自动恢复。',
  error: '存在请求或执行失败；交由故障恢复插件处理，或检查后手动恢复。',
  disarmed: '目标尚未启用，且没有可验证的服务重启恢复记录。',
  recoverable: '目标因服务销毁中断，可恢复原生目标。',
  recovered: '本次重启已恢复过，不会重复恢复；请检查后手动处理。',
  uncertain: '上次恢复在持久化后中断，结果未确认；请核对后手动恢复。',
  backoff: '恢复失败，正在退避等待。',
  'recovery-failed': '恢复已失败三次；停止自动尝试，请检查错误后手动恢复。',
};

export class GoalGuard {
  constructor({ manager, agent, goals, observe, now = Date.now }) {
    Object.assign(this, { manager, agent, goals, observe, now });
    this.operations = new Map();
    this.boundaries = new WeakMap();
    this.idle = new WeakMap();
    this.epochs = new WeakMap();
    this.closed = false;
  }

  async close() {
    this.closed = true;
    await Promise.allSettled([...this.operations.values()]);
  }

  invalidate(agent) {
    if (!agent) return;
    this.boundaries.delete(agent);
    this.epochs.set(agent, (this.epochs.get(agent) ?? 0) + 1);
  }

  settings(id) {
    return this.manager.settings(id).guard ?? { enabled: false, goalId: null, history: [] };
  }

  exclusive(id, action) {
    const prior = this.operations.get(id) ?? Promise.resolve();
    const job = prior.catch(() => {}).then(action);
    this.operations.set(id, job);
    return job.finally(() => { if (this.operations.get(id) === job) this.operations.delete(id); });
  }

  async boundary(id, agent, goal) {
    const cached = this.boundaries.get(agent);
    if (cached?.revision === goal.revision && cached.goalId === goal.id) return cached.value;
    const cut = await this.observe(id);
    try {
      const value = restartBoundary(cut.events, goal.id);
      this.boundaries.set(agent, { goalId: goal.id, revision: goal.revision, value });
      return value;
    } finally { cut[Symbol.dispose](); }
  }

  async view(id) {
    const settings = this.settings(id);
    const agent = this.agent(id);
    let goal;
    let status;
    try { goal = agent && this.goals()?.get(agent); }
    catch (error) { return { enabled: settings.enabled, status: 'unsupported',
      message: MESSAGES.unsupported, error: error.message, history: settings.history }; }
    if (!agent) status = 'unloaded';
    else if (!this.goals()) status = 'unsupported';
    else if (!goal) status = 'no-goal';
    else if (!settings.enabled) status = 'off';
    else if (settings.goalId !== goal.id) status = 'goal-changed';
    else if (goal.phase !== 'active') status = goal.phase;
    else if (goal.roundsStarted >= goal.maxGoalRounds) status = 'budget';
    else if (agent.status !== 'idle') { status = 'running'; this.idle.delete(agent); }
    else if (agent.inbox.nextStep.length || agent.inbox.nextTurn.length) status = 'queued';
    else if (!agent.model?.current?.()) status = 'model-missing';
    else if (goal.activation === 'armed') {
      if (!this.idle.has(agent)) this.idle.set(agent, this.now());
      status = this.now() - this.idle.get(agent) >= 60000 ? 'stalled' : 'armed';
    } else if (settings.pending) status = 'uncertain';
    else {
      const boundary = await this.boundary(id, agent, goal);
      if (['decision', 'error'].includes(boundary.kind)) status = boundary.kind;
      else if (boundary.restartSeq === null) status = 'disarmed';
      else if (settings.lastRestartSeq === boundary.restartSeq) status = 'recovered';
      else if (settings.attempts >= 3) status = 'recovery-failed';
      else if (settings.retryAt > this.now()) status = 'backoff';
      else status = 'recoverable';
      return { enabled: settings.enabled, status, message: MESSAGES[status], goal,
        restartSeq: boundary.restartSeq, history: settings.history, error: settings.error };
    }
    return { enabled: settings.enabled, status, message: MESSAGES[status] ?? status,
      goal, history: settings.history, error: settings.error, blocked: settings.blocked };
  }

  record(settings, entry) {
    settings.history = [...(settings.history ?? []), { at: this.now(), ...entry }].slice(-30);
  }

  async configure(id, enabled, ref) {
    return this.exclusive(id, async () => {
      if (this.closed) throw new Error('目标守护已关闭。');
      if (typeof enabled !== 'boolean') throw new Error('守护开关必须为布尔值。');
      const goal = this.agent(id) && this.goals()?.get(this.agent(id));
      if (enabled && (!goal || !['active', 'blocked'].includes(goal.phase))) {
        throw new Error('需要 active 或 blocked 原生目标。');
      }
      if (enabled && (ref?.id !== goal.id || ref.revision !== goal.revision)) {
        throw new Error('目标已变化，请刷新后重试。');
      }
      const settings = { ...this.settings(id), enabled, ...(enabled ? { goalId: goal.id } : {}) };
      if (enabled) { settings.attempts = 0; settings.retryAt = 0; }
      this.record(settings, { action: enabled ? 'enable' : 'disable', goalId: settings.goalId });
      this.manager.state.sessions[id] = { ...this.manager.settings(id), guard: settings };
      await this.manager.persist();
      if (this.closed) throw new Error('目标守护已关闭。');
    });
  }

  async resume(id, ref, automatic = false) {
    return this.exclusive(id, async () => {
      if (this.closed) return;
      const before = await this.view(id);
      if (automatic && before.status !== 'recoverable') return;
      const agent = this.agent(id);
      const goals = this.goals();
      const goal = agent && goals?.get(agent);
      if (!goal || goal.phase !== 'active') throw new Error('只能恢复 active 目标；暂停和阻塞需先处理。');
      if (ref && (ref.id !== goal.id || ref.revision !== goal.revision)) {
        throw new Error('目标已变化，请刷新后重试。');
      }
      if (goal.activation === 'armed') return;
      if (agent.status !== 'idle' || agent.inbox.nextStep.length || agent.inbox.nextTurn.length) {
        throw new Error('DSH 正在执行或已有待处理输入。');
      }
      if (!agent.model?.current?.()) throw new Error(MESSAGES['model-missing']);
      if (goal.roundsStarted >= goal.maxGoalRounds) throw new Error(MESSAGES.budget);
      const settings = this.settings(id);
      this.manager.state.sessions[id] = { ...this.manager.settings(id), guard: settings };
      settings.pending = { goalId: goal.id, revision: goal.revision, restartSeq: before.restartSeq };
      const epoch = this.epochs.get(agent);
      await this.manager.persist();
      // No await between the final native state check and its revision-checked mutation.
      const current = goals.get(agent);
      try {
        if (current.id !== goal.id || current.revision !== goal.revision ||
            current.phase !== 'active' || current.activation === 'armed' || this.closed ||
            epoch !== this.epochs.get(agent)) {
          throw new Error('目标已变化，请刷新后重试。');
        }
        if (agent.status !== 'idle' || agent.inbox.nextStep.length || agent.inbox.nextTurn.length) {
          throw new Error('DSH 状态已变化，等待下次检查。');
        }
        goals.resume(agent, { id: goal.id, revision: goal.revision });
        settings.lastRestartSeq = before.restartSeq;
        settings.attempts = 0; settings.retryAt = 0; delete settings.error;
        this.record(settings, { action: automatic ? 'auto-resume' : 'manual-resume',
          outcome: 'resumed', goalId: goal.id, revision: goal.revision, restartSeq: before.restartSeq });
      } catch (error) {
        settings.attempts = (settings.attempts ?? 0) + 1;
        settings.retryAt = this.now() + 20000 * 2 ** (settings.attempts - 1);
        settings.error = error.message;
        this.record(settings, { action: automatic ? 'auto-resume' : 'manual-resume',
          outcome: 'failed', goalId: goal.id, error: error.message });
        if (!automatic) throw error;
      } finally {
        delete settings.pending;
        await this.manager.persist();
      }
    });
  }

  async resumeBlocked(id, record) {
    return this.exclusive(id, async () => {
      if (this.closed) return 'held';
      const settings = this.settings(id);
      const agent = this.agent(id);
      const goals = this.goals();
      const goal = agent && goals?.get(agent);
      if (!settings.enabled || settings.goalId !== record.blockedGoal?.id ||
          settings.blocked?.auditId !== record.id ||
          !goal || goal.id !== record.blockedGoal.id || goal.revision !== record.blockedGoal.revision ||
          goal.phase !== 'blocked') return 'superseded';
      if (record.status !== 'completed' || record.delivery !== 'delivered' ||
          !Number.isSafeInteger(record.admittedSeq)) return 'awaiting-delivery';
      if (record.blockedRecovery?.decision !== 'actionable' || !record.blockedRecovery.steps?.length) {
        return 'waiting';
      }
      if (record.sourceCheck?.stale !== false) return 'stale';
      if (settings.pending) return 'uncertain';
      const blocked = settings.blocked ?? {};
      if (blocked.resumeAttempts >= 3) return 'resume-failed';
      if (blocked.retryAt > this.now()) return 'resume-backoff';
      if (goal.roundsStarted >= goal.maxGoalRounds) return 'budget';
      if (!agent.model?.current?.()) return 'model-missing';
      const boundary = await this.boundary(id, agent, goal);
      if (['decision', 'error'].includes(boundary.kind)) return 'held';
      const epoch = this.epochs.get(agent);
      settings.pending = { goalId: goal.id, revision: goal.revision, auditId: record.id };
      await this.manager.persist();
      const current = goals.get(agent);
      try {
        if (this.closed || !settings.enabled || epoch !== this.epochs.get(agent) ||
            current.id !== goal.id || current.revision !== goal.revision || current.phase !== 'blocked') {
          return 'superseded';
        }
        goals.resume(agent, { id: goal.id, revision: goal.revision });
        this.record(settings, { action: 'blocked-resume', outcome: 'resumed',
          goalId: goal.id, revision: goal.revision, auditId: record.id });
        return 'resumed';
      } catch (error) {
        blocked.resumeAttempts = (blocked.resumeAttempts ?? 0) + 1;
        blocked.retryAt = this.now() + 20000 * 2 ** (blocked.resumeAttempts - 1);
        blocked.error = error.message;
        this.record(settings, { action: 'blocked-resume', outcome: 'failed',
          goalId: goal.id, auditId: record.id, error: error.message });
        return blocked.resumeAttempts >= 3 ? 'resume-failed' : 'resume-backoff';
      } finally {
        delete settings.pending;
        await this.manager.persist();
      }
    });
  }

  async tick() {
    if (this.closed) return;
    for (const [id, settings] of Object.entries(this.manager.state.sessions)) {
      if (!settings.guard?.enabled) continue;
      try { await this.resume(id, null, true); }
      catch (error) {
        const guard = this.settings(id);
        if (guard.error === error.message) continue;
        guard.error = error.message;
        this.record(guard, { action: 'check', outcome: 'failed', error: error.message });
        await this.manager.persist();
      }
    }
  }
}
