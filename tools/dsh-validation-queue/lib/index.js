import { TypertRemoteService, Remote } from '@deepseek-ai/dsh-typert-protocol';
import { defineDomain, domainTable } from '@deepseek-ai/dsh-storage-domain';
import { defineTool } from '@deepseek-ai/dsh-tools';
import { z } from 'zod';
import { realpath } from 'node:fs/promises';
import { ValidationQueue } from './queue.js';
import { assess, message } from './worker.js';
import { identity, git } from './repository.js';
import { handoffChildren } from './handoff.js';
import { discoverSetup, setupProblem } from './setup.js';

export const name = 'dsh-validation-queue';
const initializers = [];
const domain = defineDomain({ name: 'validation_queue', version: 1,
  tables: { state: domainTable(z.object({ version: z.literal(1), config: z.unknown(), jobs: z.array(z.unknown()) })),
    handoff: domainTable(z.unknown()) } });

export default class ValidationQueuePlugin extends TypertRemoteService {
  static inject = ['agents', 'sessionQuery', 'storageDomain', 'typertGateway', 'tools'];
  constructor(ctx) {
    super(ctx, 'validationQueue', { namespace: 'validationQueue' });
    for (const initializer of initializers) initializer.call(this);
    this.workers = new Set();
    this.ready = this.initialize().catch(error => { this.bootError = error.message; ctx.logger.error(error); });
    ctx.effect(() => async () => {
      await this.ready;
      clearInterval(this.deliveryTimer);
      await this.queue?.close(); await this.deliveryWork; await this.domain?.close();
    });
  }
  async initialize() {
    this.domain = await this.ctx.storageDomain.open(domain);
    const table = this.domain.table('state');
    this.queue = new ValidationQueue({ load: () => table.get('queue'), save: x => table.put('queue', x),
      available: async ({ validation }) => {
        for (const agent of this.ctx.agents.list()) {
          if (!this.workers.has(agent.id) && agent.session.header.cwd &&
              await realpath(agent.session.header.cwd).catch(() => '') === validation.root)
            throw new Error('VALIDATION_WORKSPACE_HAS_RESIDENT_AGENT: start coding in author worktree and unload its old session first');
        }
      },
      review: async (job, phase, signal) => assess(this.ctx, this.ctx.agents.get(job.sessionId),
        job, phase, this.queue.state.config.validationRoot, signal, this.workers),
      deliver: job => this.deliver(job) });
    await this.queue.open();
    this.deliveryTimer = setInterval(() => {
      if (this.deliveryWork) return;
      this.deliveryWork = (async () => {
        for (const job of this.queue.state.jobs) {
          const agent = this.ctx.agents.get(job.sessionId);
          if (!agent || job.delivery !== 'pending' || !job.finishedAt || (job.deliveryAttempts ?? 0) >= 5) continue;
          const goal = this.ctx.get('goals')?.get(agent);
          if (!goal || goal.phase === 'active') await this.queue.retryDelivery(job.id);
        }
      })().catch(error => { this.queue.lastError = error.message; }).finally(() => { this.deliveryWork = null; });
    }, 10000);
    this.registerTool('validation_submit',
      'Freeze your complete current author worktree and enqueue one approved validation profile. ' +
      'Returns immediately after capture. Continue only independent work; do not mark the task DONE before the report. ' +
      'At most one running and one waiting candidate. Build/run commands belong to configured profiles, not this tool.',
      { profile: { type: 'string', required: true }, title: { type: 'string', required: true },
        dependsOn: { type: 'string' } }, async (args, agent, signal) => {
        const cwd = await realpath(agent.session.header.cwd);
        const id = await this.queue.enqueue({ ...args, sessionId: agent.id, cwd, signal });
        return { id, status: 'queued', instruction: 'Continue independent work; wait for this candidate report before task completion.' };
      });
    this.registerTool('validation_status', 'Read your validation queue, versions and results without running checks.',
      {}, async (_args, agent) => this.view({ sessionId: agent.id }));
    this.registerTool('validation_cancel', 'Cancel your queued/running validation job and drain its owned processes.',
      { id: { type: 'string', required: true } }, async ({ id }, agent) => {
        if (this.queue.state.jobs.find(x => x.id === id)?.sessionId !== agent.id)
          throw new Error('JOB_NOT_OWNED_BY_SESSION');
        await this.queue.cancel(id); return { cancelled: id };
      });
  }
  registerTool(name, description, parameters, execute) {
    this.ctx.tools.register(defineTool({ name, description, parameters,
      output: { schema: { type: 'string' }, render: (_args, value) => [{ type: 'text', text: value }] },
      execute: async (args, exec) => {
        await this.ready;
        if (!exec.agent || this.workers.has(exec.agent.id)) throw new Error('AUTHOR_AGENT_REQUIRED');
        return JSON.stringify(await execute(args, exec.agent, exec.signal));
      } }));
  }
  async deliver(job) {
    const agent = this.ctx.agents.get(job.sessionId);
    if (!agent) throw new Error('AUTHOR_SESSION_NOT_LOADED');
    const id = `validation-result-${job.id}`;
    if ([...agent.inbox.nextStep, ...agent.inbox.nextTurn].some(x => x.id === id)) return;
    const cut = await this.ctx.sessionQuery.observeSession(job.sessionId, { projectionMode: 'none' });
    try {
      if (cut.events.some(e => e.type === 'user/message' && (e.data.message ?? e.data).id === id)) return;
    } finally { cut[Symbol.dispose](); }
    const goal = this.ctx.get('goals')?.get(agent);
    if (goal && goal.phase !== 'active') throw new Error(`AUTHOR_GOAL_${goal.phase.toUpperCase()}: report retained; do not wake it`);
    agent.steer(message(id, `[验证队列 ${job.id}] ${job.status}\n` +
      `候选=${job.candidate.commit} tree=${job.candidate.tree}\n` +
      `适用性=${job.applicability}；${job.applicability === 'historical-only' ? '源码已推进，本结果不能将最新版本标PASS。' : '仅对所列候选和检查范围有效。'}\n` +
      `任务=${job.title}\n首个失败=${job.error ?? '无'}\n` +
      `${job.analysis?.summary ?? job.review?.summary ?? ''}\n` +
      [...(job.review?.findings ?? []), ...(job.analysis?.findings ?? [])].join('\n') +
      '\n保持原验收标准；有依赖失败先修复，不因单项通过宣布整个goal完成。'));
  }
  async view(request) {
    const { sessionId } = request;
    await this.ready;
    if (this.bootError) return { error: this.bootError };
    const state = this.queue.state;
    const last = await this.domain.table('handoff').get('latest');
    return { config: state.config, error: this.queue.lastError,
      readiness: !state.config ? { status: 'needs-setup', ...setupProblem(new Error('CONFIGURATION_REQUIRED')) } :
        { status: state.config.enabled ? 'enabled' : 'disabled' },
      handoff: last?.parentId === sessionId ? { id: last.id, status: last.status,
        finishedAt: last.finishedAt, children: last.children.map(x => ({ id: x.id, label: x.label,
          loaded: x.loaded, remaining: x.remaining })) } : undefined,
      counts: { queued: state.jobs.filter(x => x.status === 'queued').length,
        running: state.jobs.filter(x => ['materializing','reviewing','build','test','run','metadata','analyzing'].includes(x.status)).length },
      jobs: state.jobs.filter(x => x.sessionId === sessionId).map(x => ({ ...x,
        commands: x.commands.map(c => ({ ...c, stdout: c.stdout.slice(-6000), stderr: c.stderr.slice(-6000) })) })).reverse() };
  }
  async control(request) {
    await this.ready;
    if (this.bootError) throw new Error(this.bootError);
    const { sessionId, action } = request;
    const agent = this.ctx.agents.get(sessionId);
    if (!agent) throw new Error('SESSION_NOT_LOADED');
    if (action === 'setup') {
      const setup = await discoverSetup(agent.session.header.cwd, this.ctx.agents.list()
        .filter(x => !this.workers.has(x.id)).map(x => ({ id: x.id, status: x.status, cwd: x.session.header.cwd })));
      return { ...(await this.view({ sessionId })), setup };
    } else if (action === 'children') {
      const subagents = this.ctx.get('subagents');
      if (!subagents) throw new Error('SUBAGENTS_UNAVAILABLE');
      return { children: await subagents.listChildren(agent.id) };
    } else if (action === 'handoff') {
      if (request.confirmed !== true || !Array.isArray(request.childIds)) throw new Error('EXPLICIT_HANDOFF_REQUIRED');
      const record = await handoffChildren(this.ctx, agent, request.childIds,
        value => this.domain.table('handoff').put('latest', value), request.remainingByChild);
      return { ...(await this.view({ sessionId })), handoff: record };
    } else if (action === 'probe') {
      if (request.model === true) {
        if (this.queue.running || this.workers.size) throw new Error('WORKER_BUSY');
        const cwd = agent.session.header.cwd;
        const head = await git(cwd, ['rev-parse', 'HEAD']);
        const tree = await git(cwd, ['rev-parse', 'HEAD^{tree}']);
        const result = await assess(this.ctx, agent, { id: 'model-contract-probe',
          title: 'Check the small arithmetic fixture only; do not inspect or change project code.',
          candidate: { commit: head, head, tree }, diff: '(no source changes; transport probe only)',
          approvedProfile: { acceptance: 'Confirm 6 * 7 = 42 from the fixture. This is only a model/tool transport probe, not code/build/inference validation.' },
          commands: [{ file: 'internal-fixture', args: [], code: 0, stdout: '6 * 7 = 42', stderr: '' }] },
        'analysis', cwd, new AbortController().signal, this.workers);
        return { ...(await this.view({ sessionId })), modelProbe: result };
      }
      const probe = await assess(this.ctx, agent, { id: 'host-contract-probe' }, 'probe',
        agent.session.header.cwd, new AbortController().signal, this.workers, true);
      return { ...(await this.view({ sessionId })), check: JSON.stringify(probe) };
    } else if (action === 'save' || action === 'save-and-enable') {
      // Configuration is a human GUI action. Model-facing tools cannot supply executable commands.
      try {
        const base = await identity(agent.session.header.cwd);
        const roots = await identity(request.config.validationRoot);
        if (base.common !== roots.common) throw new Error('CONFIGURATION_OUTSIDE_SESSION_REPOSITORY');
        await this.queue.configure({ ...request.config, enabled: false });
      } catch (error) { return { ...(await this.view({ sessionId })), readiness: { status: 'blocked', ...setupProblem(error) } }; }
      if (action === 'save-and-enable') {
        try { await this.queue.enable(true); }
        catch (error) { return { ...(await this.view({ sessionId })), readiness: { status: 'blocked', ...setupProblem(error) } }; }
      }
    } else if (action === 'enable') {
      try { await this.queue.enable(request.enabled === true); }
      catch (error) { return { ...(await this.view({ sessionId })), readiness: { status: 'blocked', ...setupProblem(error) } }; }
    }
    else if (action === 'check') {
      try { await this.queue.check(); }
      catch (error) { return { ...(await this.view({ sessionId })), readiness: { status: 'blocked', ...setupProblem(error) } }; }
      return { ...(await this.view({ sessionId })), check: '自检通过（未发起构建或模型请求）' };
    } else if (action === 'cancel' || action === 'deliver') {
      const job = this.queue.state.jobs.find(x => x.id === request.id);
      if (!job || job.sessionId !== sessionId) throw new Error('JOB_NOT_OWNED_BY_SESSION');
      if (action === 'cancel') await this.queue.cancel(job.id); else await this.queue.retryDelivery(job.id);
    } else throw new Error('UNKNOWN_ACTION');
    return this.view({ sessionId });
  }
}
for (const method of ['view', 'control']) {
  Remote({ resources: { session: request => request.sessionId } })(ValidationQueuePlugin.prototype[method], {
    name: method, kind: 'method', private: false, static: false,
    addInitializer: x => initializers.push(x),
  });
}
