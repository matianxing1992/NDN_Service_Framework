import { spawn } from 'node:child_process';
import { access, readFile, readdir, stat } from 'node:fs/promises';
import { constants } from 'node:fs';
import { homedir } from 'node:os';
import { delimiter, join, isAbsolute } from 'node:path';
import { AuditError, DEFAULT_MODELS } from './audit-manager.js';
import { codexHomePath, readCodexSelection } from './model-observation.js';
import { readProviderConfig } from './provider-config.js';

const LIMIT = 8 * 1024 * 1024;
export async function readAuditModels(root = codexHomePath()) {
  try {
    const file = join(root, 'models_cache.json');
    if ((await stat(file)).size > LIMIT) return DEFAULT_MODELS;
    const { models } = JSON.parse(await readFile(file, 'utf8'));
    if (!Array.isArray(models)) return DEFAULT_MODELS;
    const options = new Map();
    for (const model of models) {
      if (model?.visibility !== 'list' || typeof model.slug !== 'string' ||
          !/^[a-z0-9][a-z0-9._-]{0,99}$/i.test(model.slug)) continue;
      const levels = Array.isArray(model.supported_reasoning_levels) ? model.supported_reasoning_levels : [];
      const efforts = [...new Set(levels.map(level => level?.effort).filter(level =>
        ['none', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max', 'ultra'].includes(level)))];
      if (efforts.length) options.set(model.slug, { id: model.slug, efforts,
        defaultEffort: efforts.includes(model.default_reasoning_level) ? model.default_reasoning_level : efforts[0] });
    }
    return options.size ? [...options.values()] : DEFAULT_MODELS;
  } catch {
    return DEFAULT_MODELS;
  }
}

export function classifyFailure(message) {
  if (/insufficient_quota|billing|payment.required/i.test(message)) {
    return new AuditError('CODEX_QUOTA', 'Codex 账户额度或计费状态不允许请求。',
      '检查账户额度和计费；修复后重新开启定时审计。');
  }
  if (/401|403|unauthorized|forbidden|not logged in|authentication|invalid.*key/i.test(message)) {
    return new AuditError('CODEX_AUTH', 'Codex 认证失败。', '在 DSH Host 所在机器登录 Codex。');
  }
  if (/model.*(not found|not supported|unavailable)|unsupported.*\b(model|reasoning|xhigh|ultra)\b|(?:reasoning|effort).*(unsupported|invalid|not supported)/i.test(message)) {
    return new AuditError('CODEX_MODEL', '指定模型或思考等级不可用。',
      '检查账户权限是否支持所选模型和思考强度。');
  }
  const transient = /429|rate.limit|timeout|timed out|503|502|connection|stream.*(disconnect|closed)/i.test(message);
  return new AuditError(transient ? 'CODEX_TRANSIENT' : 'CODEX_EXECUTION',
    message.slice(0, 2000), transient ? '稍后有限重试；检查服务状态。' : '检查 Codex 配置及执行记录。', transient);
}

// No shell interpolation; the child owns its process group, deadline and output bounds.
export function execute(binary, args, { cwd, input = '', signal, timeoutMs = 3600000,
  processName = 'Codex', env } = {}) {
  return new Promise((resolve, reject) => {
    let child;
    let stdout = '';
    let stderr = '';
    let failure;
    let killTimer;
    const stop = error => {
      failure ??= error;
      if (!child?.pid) return;
      const kill = kind => {
        try { process.kill(process.platform === 'win32' ? child.pid : -child.pid, kind); }
        catch (error) { if (error.code !== 'ESRCH') failure ??= error; }
      };
      kill('SIGTERM');
      killTimer ??= setTimeout(() => kill('SIGKILL'), 2000);
    };
    const abort = () => stop(signal.reason ?? new Error('Audit cancelled'));
    if (signal?.aborted) return reject(signal.reason);
    try {
      child = spawn(binary, args, { cwd, env, detached: process.platform !== 'win32',
        stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
    } catch (error) { reject(error); return; }
    signal?.addEventListener('abort', abort, { once: true });
    const deadline = setTimeout(() => stop(new AuditError(
      processName === 'Git' ? 'GIT_TIMEOUT' : 'CODEX_TIMEOUT',
      `${processName} 操作超过时限。`, '检查运行记录后重试。', true)), timeoutMs);
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', value => {
      stdout += value;
      if (Buffer.byteLength(stdout) > LIMIT) stop(new Error(`${processName} output exceeds 8 MiB`));
      if (stdout.length > LIMIT) stdout = stdout.slice(0, LIMIT);
    });
    child.stderr.on('data', value => { stderr = (stderr + value).slice(-65536); });
    child.stdin.on('error', error => { if (error.code !== 'EPIPE') stop(error); });
    child.on('error', error => { failure ??= error; });
    child.on('close', (code, killedBy) => {
      clearTimeout(deadline);
      clearTimeout(killTimer);
      signal?.removeEventListener('abort', abort);
      if (failure) reject(failure);
      else if (code !== 0) {
        const message = (stderr || stdout || `${processName} exit ${code ?? killedBy}`).trim();
        reject(processName === 'Git' ? new AuditError('GIT_EXECUTION', message.slice(0, 2000),
          '检查 DSH Host 上 Git 工作区的可读性和配置。') : classifyFailure(message));
      }
      else resolve({ stdout, stderr });
    });
    child.stdin.end(input);
  });
}

async function isExecutable(file) {
  try {
    if (!(await stat(file)).isFile()) return false;
    await access(file, constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

export async function resolveCodex(configured) {
  if (configured) {
    if (!isAbsolute(configured)) throw new AuditError('CODEX_NOT_FOUND',
      'Codex 路径必须是绝对路径。', '配置 DSH Host 上的绝对可执行文件路径。');
    if (await isExecutable(configured)) return configured;
    throw new AuditError('CODEX_NOT_FOUND', `Codex 无法执行：${configured}`,
      '在 GUI 配置 DSH Host 上 Codex 可执行文件的绝对路径。');
  }
  for (const dir of (process.env.PATH ?? '').split(delimiter)) {
    if (!isAbsolute(dir)) continue;
    const candidate = join(dir, process.platform === 'win32' ? 'codex.exe' : 'codex');
    if (await isExecutable(candidate)) return candidate;
  }
  const platform = { linux: 'linux', darwin: 'macos', win32: 'win32' }[process.platform];
  const arch = { arm64: 'aarch64', x64: 'x86_64' }[process.arch];
  for (const extensions of ['.vscode', '.vscode-server']) {
    if (!platform || !arch) break;
    const base = join(homedir(), extensions, 'extensions');
    const entries = await readdir(base).catch(() => []);
    for (const name of entries.filter(name => name.startsWith('openai.chatgpt-')).sort().reverse()) {
      const candidate = join(base, name, 'bin', `${platform}-${arch}`,
        process.platform === 'win32' ? 'codex.exe' : 'codex');
      if (await isExecutable(candidate)) return candidate;
    }
  }
  throw new AuditError('CODEX_NOT_FOUND', 'DSH Host 上未找到 Codex CLI。',
    '将 Codex CLI 加入 DSH Host 的 PATH，或在 GUI 设置其绝对路径。');
}

export async function checkCodex(configured, { signal, cwd } = {}) {
  const binary = await resolveCodex(configured);
  const codexHome = codexHomePath();
  const provider = await readProviderConfig(codexHome);
  let help;
  try {
    const result = await execute(binary, ['exec', '--help'], { signal, cwd, timeoutMs: 10000,
      env: { ...process.env, CODEX_HOME: codexHome } });
    help = result.stdout;
  } catch (error) {
    if (signal?.aborted) throw error;
    throw new AuditError('CODEX_CLI_UNSUPPORTED',
      `DSH Host 无法运行 Codex CLI 帮助（${error.code ?? error.message}）：${binary}`,
      '确认宿主操作系统和架构匹配，并配置支持 exec 的 Codex 可执行文件。Windows 上使用 codex.exe。');
  }
  const missing = ['--ignore-user-config', '--ignore-rules', '--json', '--sandbox',
    '--skip-git-repo-check', '--output-schema'].filter(flag => !help.includes(flag));
  if (missing.length) throw new AuditError('CODEX_CLI_UNSUPPORTED',
    `Codex CLI 缺少审计所需选项：${missing.join('、')}`,
    '升级 DSH Host 上的 Codex CLI，或在面板指定支持这些选项的可执行文件。');
  return { binary, codexHome, provider };
}

export function parseResult(stdout) {
  let completed = false;
  let report;
  let threadId;
  let usage;
  for (const line of stdout.split('\n').filter(Boolean)) {
    let event;
    try { event = JSON.parse(line); } catch { throw new Error('Invalid Codex JSONL event'); }
    if (event.type === 'thread.started') threadId = event.thread_id;
    if (event.type === 'turn.failed') throw classifyFailure(JSON.stringify(event.error));
    if (event.type === 'item.completed' && event.item?.type === 'agent_message') report = event.item.text;
    if (event.type === 'turn.completed') { completed = true; usage = event.usage; }
  }
  if (!completed || !report) throw new Error('Codex exited without a completed turn and report');
  let result;
  try { result = JSON.parse(report); } catch { throw new Error('Codex final report violates JSON schema'); }
  if (typeof result.report !== 'string' || !Array.isArray(result.documentChanges)) {
    throw new Error('Codex report missing required fields');
  }
  return { ...result, threadId, usage };
}

export async function runCodex({ record, settings, snapshot, signal, onAttempt = async () => {} }) {
  const { binary, codexHome, provider } = await checkCodex(settings.codexPath,
    { signal, cwd: snapshot.path });
  const args = ['exec', '--ignore-user-config', '--ignore-rules', '--json',
    '--skip-git-repo-check', '--sandbox', 'read-only', '-c', 'approval_policy="never"',
    '-m', settings.model, '-c', `model_reasoning_effort="${settings.effort}"`,
    ...provider.args, '--output-schema', snapshot.schemaPath, '-C', snapshot.path, '-'];
  for (let attempt = 1; attempt <= 3; attempt++) {
    signal.throwIfAborted();
    record.attempts = attempt;
    await onAttempt();
    try {
      const output = await execute(binary, args, { cwd: snapshot.path, signal,
        env: { ...process.env, CODEX_HOME: codexHome },
        input: snapshot.prompt, timeoutMs: settings.timeoutMs ?? 3600000 });
      const result = parseResult(output.stdout);
      result.modelObservation = await readCodexSelection({ threadId: result.threadId,
        startedAt: record.startedAt, codexHome });
      record.modelObservation = result.modelObservation;
      record.codexThreadId = result.threadId;
      const observed = result.modelObservation;
      if (observed.status === 'mixed' || observed.status === 'confirmed' &&
          (observed.model !== settings.model || observed.effort !== settings.effort)) {
        throw new AuditError('CODEX_SELECTION_MISMATCH', 'Codex 运行记录与指定模型/思考等级不一致。',
          '检查运行记录；插件未将本轮报告当作指定配置的成功结果。');
      }
      if (observed.status === 'confirmed') {
        result.effectiveModel = observed.model;
        result.effectiveEffort = observed.effort;
      }
      return result;
    } catch (error) {
      if (!error.transient || attempt === 3 || signal.aborted) throw error;
      await new Promise((resolve, reject) => {
        const abort = () => { clearTimeout(timer); reject(signal.reason); };
        const timer = setTimeout(() => { signal.removeEventListener('abort', abort); resolve(); },
          attempt * 5000);
        signal.addEventListener('abort', abort, { once: true });
        if (signal.aborted) abort();
      });
    }
  }
}
