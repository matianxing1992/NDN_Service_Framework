import { spawn } from 'node:child_process';
import { access, readFile } from 'node:fs/promises';
import { constants } from 'node:fs';
import { homedir } from 'node:os';
import { join, isAbsolute } from 'node:path';
import { AuditError, AUDIT_MODEL, AUDIT_EFFORT } from './audit-manager.js';
import { readCodexSelection } from './model-observation.js';

const LIMIT = 8 * 1024 * 1024;

export function classifyFailure(message) {
  if (/insufficient_quota|billing|payment.required/i.test(message)) {
    return new AuditError('CODEX_QUOTA', 'Codex 账户额度或计费状态不允许请求。',
      '检查账户额度和计费；修复后重新开启定时审计。');
  }
  if (/401|403|unauthorized|forbidden|not logged in|authentication|invalid.*key/i.test(message)) {
    return new AuditError('CODEX_AUTH', 'Codex 认证失败。', '在 DSH Host 所在机器登录 Codex。');
  }
  if (/model.*(not found|not supported|unavailable)|unsupported.*(model|reasoning|xhigh)|(?:reasoning|effort).*(unsupported|invalid|not supported)/i.test(message)) {
    return new AuditError('CODEX_MODEL', '指定模型或思考等级不可用。',
      `检查 ${AUDIT_MODEL} / ${AUDIT_EFFORT} 的账户权限；插件不会降级。`);
  }
  const transient = /429|rate.limit|timeout|timed out|503|502|connection|stream.*(disconnect|closed)/i.test(message);
  return new AuditError(transient ? 'CODEX_TRANSIENT' : 'CODEX_EXECUTION',
    message.slice(0, 2000), transient ? '稍后有限重试；检查服务状态。' : '检查 Codex 配置及执行记录。', transient);
}

// No shell interpolation; the child owns its process group, deadline and output bounds.
export function execute(binary, args, { cwd, input = '', signal, timeoutMs = 3600000 } = {}) {
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
      child = spawn(binary, args, { cwd, detached: process.platform !== 'win32',
        stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
    } catch (error) { reject(error); return; }
    signal?.addEventListener('abort', abort, { once: true });
    const deadline = setTimeout(() => stop(new AuditError('CODEX_TIMEOUT',
      'Codex 审计超过时限。', '检查运行记录后重试。', true)), timeoutMs);
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', value => {
      stdout += value;
      if (Buffer.byteLength(stdout) > LIMIT) stop(new Error('Codex output exceeds 8 MiB'));
      if (stdout.length > LIMIT) stdout = stdout.slice(0, LIMIT);
    });
    child.stderr.on('data', value => { stderr = (stderr + value).slice(-65536); });
    child.stdin.on('error', error => { if (error.code !== 'EPIPE') stop(error); });
    child.on('error', error => { failure ??= error; });
    child.on('close', code => {
      clearTimeout(deadline);
      clearTimeout(killTimer);
      signal?.removeEventListener('abort', abort);
      if (failure) reject(failure);
      else if (code !== 0) reject(classifyFailure(stderr || stdout || `Codex exit ${code}`));
      else resolve({ stdout, stderr });
    });
    child.stdin.end(input);
  });
}

export async function resolveCodex(configured) {
  if (configured) {
    if (!isAbsolute(configured)) throw new AuditError('CODEX_NOT_FOUND',
      'Codex 路径必须是绝对路径。', '配置 DSH Host 上的绝对可执行文件路径。');
    try { await access(configured, constants.X_OK); return configured; }
    catch { throw new AuditError('CODEX_NOT_FOUND', `Codex 无法执行：${configured}`,
      '在 GUI 配置 DSH Host 上 Codex 可执行文件的绝对路径。'); }
  }
  const { readdir } = await import('node:fs/promises');
  for (const base of [join(homedir(), '.vscode/extensions'), join(homedir(), '.vscode-server/extensions')]) {
    const entries = await readdir(base).catch(() => []);
    for (const name of entries.filter(name => name.startsWith('openai.chatgpt-')).sort().reverse()) {
      const platform = process.platform === 'linux' ? 'linux' : 'macos';
      const arch = process.arch === 'arm64' ? 'aarch64' : 'x86_64';
      const candidate = join(base, name, 'bin', `${platform}-${arch}`, 'codex');
      try { await access(candidate, constants.X_OK); return candidate; } catch { /* next */ }
    }
  }
  throw new AuditError('CODEX_NOT_FOUND', 'DSH Host 上未找到 VSCode Codex 后端。',
    '安装 Codex 扩展或在 GUI 设置可执行文件路径；远程窗口与本机可能不同。');
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
  const binary = await resolveCodex(settings.codexPath);
  // The audit must not inherit MCP servers, hooks or writable tools. Authentication still
  // comes from CODEX_HOME. Custom provider routes require an explicitly isolated profile.
  const config = await readFile(join(process.env.CODEX_HOME || join(homedir(), '.codex'),
    'config.toml'), 'utf8').catch(error => { if (error.code === 'ENOENT') return ''; throw error; });
  if (/^model_provider\s*=/m.test(config)) throw new AuditError('CUSTOM_PROVIDER_UNSUPPORTED',
    '检测到自定义 Codex provider；尚未验证隔离审计配置。',
    '保留现有配置，需要为该 provider 验证独立只读配置，不能静默切换账户路由。');
  const args = ['exec', '--ignore-user-config', '--ignore-rules', '--json',
    '--skip-git-repo-check', '--sandbox', 'read-only', '-c', 'approval_policy="never"',
    '-m', settings.model, '-c', `model_reasoning_effort="${settings.effort}"`,
    '--output-schema', snapshot.schemaPath, '-C', snapshot.path, '-'];
  for (let attempt = 1; attempt <= 3; attempt++) {
    signal.throwIfAborted();
    record.attempts = attempt;
    await onAttempt();
    try {
      const output = await execute(binary, args, { cwd: snapshot.path, signal,
        input: snapshot.prompt, timeoutMs: settings.timeoutMs ?? 3600000 });
      const result = parseResult(output.stdout);
      result.modelObservation = await readCodexSelection({ threadId: result.threadId,
        startedAt: record.startedAt });
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
