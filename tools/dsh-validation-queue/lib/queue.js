import { randomUUID } from 'node:crypto';
import * as repo from './repository.js';
import { run } from './process.js';
import { createReadStream } from 'node:fs';
import { stat, realpath } from 'node:fs/promises';
import { resolve } from 'node:path';
import { createHash } from 'node:crypto';

async function artifacts(root, paths = []) {
  const result = [];
  for (const path of paths) {
    const file = await realpath(resolve(root, path)), info = await stat(file);
    if (!info.isFile() || info.size > 512 * 1024 * 1024) throw new Error(`ARTIFACT_LIMIT: ${path}`);
    const digest = createHash('sha256');
    for await (const bytes of createReadStream(file)) digest.update(bytes);
    result.push({ path: file, size: info.size, sha256: digest.digest('hex') });
  }
  return result;
}

const terminal = new Set(['passed', 'failed', 'error', 'cancelled', 'blocked', 'interrupted']);
export function configuration(value) {
  const c = structuredClone(value);
  if (!c || typeof c.authorRoot !== 'string' || typeof c.validationRoot !== 'string')
    throw new Error('WORKSPACE_PATHS_REQUIRED');
  if (!c.profiles || !Object.keys(c.profiles).length) throw new Error('PROFILE_REQUIRED');
  for (const [name, profile] of Object.entries(c.profiles)) {
    if (!/^[a-zA-Z0-9_-]{1,64}$/.test(name) || !Array.isArray(profile.commands) ||
        profile.commands.length < 1 || profile.commands.length > 8) throw new Error('PROFILE_INVALID');
    for (const command of profile.commands) {
      if (typeof command.file !== 'string' || !command.file || !Array.isArray(command.args) ||
          command.args.some(x => typeof x !== 'string') ||
          !['build', 'test', 'run', 'metadata'].includes(command.kind))
        throw new Error('COMMAND_INVALID');
      command.timeoutMs ??= 7200000;
      if (!Number.isInteger(command.timeoutMs) || command.timeoutMs < 100 ||
          command.timeoutMs > 14400000) throw new Error('COMMAND_TIMEOUT_INVALID');
      if (command.env && Object.entries(command.env).some(([k, v]) =>
        !/^[A-Za-z_][A-Za-z0-9_]*$/.test(k) || typeof v !== 'string'))
        throw new Error('COMMAND_ENV_INVALID');
    }
    if (typeof profile.acceptance !== 'string' || !profile.acceptance.trim())
      throw new Error('ACCEPTANCE_REQUIRED');
    if (profile.commands.some(x => x.kind === 'build') && !profile.artifacts?.length)
      throw new Error('BUILD_ARTIFACT_PATHS_REQUIRED');
    if (profile.artifacts && (!Array.isArray(profile.artifacts) || profile.artifacts.length > 32 ||
        profile.artifacts.some(x => typeof x !== 'string' || !x))) throw new Error('ARTIFACT_PATH_INVALID');
    let ran = false;
    for (const command of profile.commands) {
      if (['test', 'run'].includes(command.kind)) ran = true;
      else if (ran && command.kind === 'build') throw new Error('BUILD_MUST_PRECEDE_RUN');
    }
  }
  c.enabled = c.enabled === true;
  return c;
}

export class ValidationQueue {
  constructor({ load, save, review, deliver, available, repository = repo, command = run }) {
    Object.assign(this, { load, save, review, deliver, available, repository, command });
    this.state = { version: 1, config: null, jobs: [] };
    this.serial = Promise.resolve();
    this.writes = Promise.resolve();
    this.running = null;
    this.closed = false;
  }
  transact(fn) {
    const work = this.serial.then(fn);
    this.serial = work.catch(() => {});
    return work;
  }
  async open() {
    const saved = await this.load();
    if (saved) this.state = saved;
    // Restart never replays an unknown command side effect or silently takes a build workspace.
    for (const job of this.state.jobs)
      if (!terminal.has(job.status) && job.status !== 'queued') {
        job.status = 'interrupted'; job.error = 'HOST_RESTARTED: inspect prior command effects before retry';
      }
    if (this.state.config) this.state.config.enabled = false;
    await this.persist();
  }
  async persist() {
    const copy = structuredClone(this.state);
    const write = this.writes.then(() => this.save(copy));
    this.writes = write.catch(() => {});
    await write;
  }
  async check(config = this.state.config) {
    if (!config) throw new Error('CONFIGURATION_REQUIRED');
    const paths = await this.repository.pair(config.authorRoot, config.validationRoot);
    await this.repository.clean(paths.validation.root);
    await this.available(paths);
    return paths;
  }
  async configure(value) {
    return this.transact(async () => {
      if (this.running || this.state.jobs.some(x => x.status === 'queued'))
        throw new Error('QUEUE_MUST_BE_DRAINED_BEFORE_RECONFIGURATION');
      const c = configuration(value);
      if (c.enabled) await this.check(c);
      this.state.config = c;
      await this.persist();
      this.wake();
    });
  }
  async enable(enabled) {
    return this.transact(async () => {
      if (enabled) await this.check();
      if (!this.state.config) throw new Error('CONFIGURATION_REQUIRED');
      this.state.config.enabled = enabled;
      if (enabled) this.lastError = null;
      await this.persist();
      this.wake();
    });
  }
  async enqueue({ sessionId, cwd, profile, title, dependsOn, signal }) {
    return this.transact(async () => {
      const c = this.state.config;
      if (this.closed || !c?.enabled) throw new Error('QUEUE_DISABLED');
      signal?.throwIfAborted();
      if (cwd !== c.authorRoot) throw new Error('SUBMIT_FROM_CONFIGURED_AUTHOR_WORKTREE');
      if (typeof title !== 'string' || !title.trim() || title.length > 300)
        throw new Error('TITLE_REQUIRED');
      if (!Object.hasOwn(c.profiles, profile)) throw new Error('UNKNOWN_APPROVED_PROFILE');
      if (this.state.jobs.some(x => x.status === 'queued'))
        throw new Error('BACKPRESSURE: one waiting candidate already exists; wait for its result');
      const dependency = dependsOn && this.state.jobs.find(x => x.id === dependsOn);
      if (dependsOn && !dependency) throw new Error('UNKNOWN_DEPENDENCY');
      if (dependency && terminal.has(dependency.status) && dependency.status !== 'passed')
        throw new Error('DEPENDENCY_NOT_PASSED');
      await this.check();
      const candidate = await this.repository.capture(c.authorRoot, { signal });
      const job = { id: randomUUID(), sessionId, title, profile, dependsOn,
        candidate, approvedProfile: structuredClone(c.profiles[profile]),
        configHash: repo.hash(JSON.stringify(c.profiles[profile])),
        status: 'queued', createdAt: Date.now(), commands: [], delivery: 'pending' };
      try {
        signal?.throwIfAborted();
        this.state.jobs.push(job);
        while (this.state.jobs.length > 22) {
          const first = this.state.jobs[0];
          if (!terminal.has(first.status) || this.state.jobs.some(x => !terminal.has(x.status) && x.dependsOn === first.id)) break;
          await this.repository.release(c.authorRoot, first.candidate);
          this.state.jobs.shift();
        }
        await this.persist();
      } catch (error) {
        this.state.jobs = this.state.jobs.filter(x => x !== job);
        await this.repository.release(c.authorRoot, candidate);
        throw error;
      }
      this.wake();
      return job.id;
    });
  }
  wake() {
    if (this.running || this.closed || !this.state.config?.enabled) return;
    this.running = this.serial.then(() => this.pump()).catch(error => {
      this.lastError = error.message;
      if (this.state.config) this.state.config.enabled = false;
    }).finally(() => {
      this.running = null;
      if (!this.lastError && this.state.jobs.some(x => x.status === 'queued')) this.wake();
    });
  }
  async pump() {
    while (!this.closed && this.state.config?.enabled) {
      const job = this.state.jobs.find(x => x.status === 'queued');
      if (!job) break;
      const c = this.state.config;
      const dep = job.dependsOn && this.state.jobs.find(x => x.id === job.dependsOn);
      if (dep && dep.status !== 'passed') {
        job.status = 'blocked'; job.error = `DEPENDENCY_FAILED: ${dep.id}`;
        await this.finish(job); continue;
      }
      this.controller = new AbortController();
      const signal = this.controller.signal;
      let unlock;
      try {
        await this.check();
        unlock = await this.repository.lock(c.validationRoot);
        // Recheck after acquiring the cross-process repository lease.
        await this.check();
        job.status = 'materializing'; job.startedAt = Date.now(); await this.persist();
        await this.repository.materialize(c.validationRoot, job.candidate);
        job.diff = (await this.repository.git(c.validationRoot,
          ['diff', '--stat', job.candidate.head, job.candidate.commit])).slice(0, 10000);
        job.status = 'reviewing'; await this.persist();
        job.review = await this.review(job, 'review', signal);
        if (job.review.decision !== 'accept') {
          job.status = 'failed'; job.error = 'STATIC_REVIEW_REJECTED';
        } else {
          for (const command of job.approvedProfile.commands) {
            signal.throwIfAborted();
            if (['test', 'run'].includes(command.kind) && !job.artifacts)
              job.artifacts = await artifacts(c.validationRoot, job.approvedProfile.artifacts);
            job.status = command.kind; await this.persist();
            const result = await this.command(command.file, command.args,
              { cwd: c.validationRoot, env: command.env, timeoutMs: command.timeoutMs, signal });
            job.commands.push({ ...result, stdout: result.stdout.slice(-24000), stderr: result.stderr.slice(-12000),
              environmentHash: repo.hash(JSON.stringify(Object.fromEntries(
                ['PATH', 'LD_LIBRARY_PATH', 'PYTHONPATH', 'CC', 'CXX'].map(k => [k, command.env?.[k] ?? process.env[k] ?? ''])))) });
            await this.persist();
            if (result.code !== 0) { job.error = `COMMAND_FAILED: ${command.kind}, exit=${result.code}`; break; }
          }
          // Never certify a build that consumed an externally mutated checkout.
          await this.repository.clean(c.validationRoot);
          const finalArtifacts = job.error ? job.artifacts ?? [] :
            await artifacts(c.validationRoot, job.approvedProfile.artifacts);
          if (job.artifacts && JSON.stringify(finalArtifacts) !== JSON.stringify(job.artifacts))
            throw new Error('ARTIFACT_CHANGED_DURING_RUN');
          job.artifacts = finalArtifacts;
          if (await this.repository.git(c.validationRoot, ['rev-parse', 'HEAD']) !== job.candidate.commit)
            throw new Error('VALIDATION_HEAD_CHANGED');
          job.status = 'analyzing'; await this.persist();
          job.analysis = await this.review(job, 'analysis', signal);
          job.status = !job.error && job.analysis.decision === 'accept' ? 'passed' : 'failed';
        }
      } catch (error) {
        job.status = signal.aborted ? 'cancelled' : 'error';
        job.error = error.message;
      } finally {
        if (unlock) await unlock().catch(e => { job.status = 'error'; job.error = e.message; });
        this.controller = null;
        await this.finish(job);
      }
    }
  }
  async finish(job) {
    job.finishedAt ??= Date.now(); await this.persist();
    job.deliveryAttempts = (job.deliveryAttempts ?? 0) + 1;
    try {
      const latest = await this.repository.capture(this.state.config.authorRoot, { retain: false });
      job.currentTree = latest.tree;
      job.applicability = latest.tree === job.candidate.tree ? 'exact-current-tree' : 'historical-only';
      await this.deliver(job);
      job.delivery = 'delivered';
    } catch (error) { job.delivery = 'pending'; job.deliveryError = error.message; }
    await this.persist();
  }
  async cancel(id) {
    return this.transact(async () => {
      const job = this.state.jobs.find(x => x.id === id);
      if (!job || terminal.has(job.status)) return;
      if (job.status === 'queued') { job.status = 'cancelled'; await this.finish(job); }
      else this.controller?.abort(new Error('USER_CANCELLED'));
    });
  }
  async retryDelivery(id) {
    const job = this.state.jobs.find(x => x.id === id);
    if (!job || !terminal.has(job.status)) throw new Error('FINISHED_JOB_REQUIRED');
    await this.finish(job);
  }
  async close() {
    this.closed = true;
    this.controller?.abort(new Error('PLUGIN_UNLOADED'));
    await this.serial;
    await this.running;
    await this.writes;
  }
}
