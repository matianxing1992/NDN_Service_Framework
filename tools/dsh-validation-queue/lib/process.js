import { spawn } from 'node:child_process';

// The queue owns the entire process group, including timeout/cancellation and drain.
export function run(file, args, { cwd, env = {}, signal, timeoutMs = 7200000,
  limit = 2 * 1024 * 1024 } = {}) {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) return reject(signal.reason);
    let child, stopReason, killer, size = 0, stdout = '', stderr = '';
    const kill = kind => {
      try { process.kill(process.platform === 'win32' ? child.pid : -child.pid, kind); }
      catch (error) { if (error.code !== 'ESRCH') stopReason ??= error; }
    };
    const stop = error => {
      stopReason ??= error;
      if (!child?.pid) return;
      kill('SIGTERM');
      killer ??= setTimeout(() => kill('SIGKILL'), 1500);
    };
    const abort = () => stop(signal.reason ?? new Error('CANCELLED'));
    const startedAt = Date.now();
    const environment = { ...process.env, ...env };
    delete environment.NODE_TEST_CONTEXT;
    child = spawn(file, args, { cwd, env: environment,
      detached: process.platform !== 'win32', stdio: ['ignore', 'pipe', 'pipe'] });
    const deadline = setTimeout(() => stop(new Error('COMMAND_TIMEOUT')), timeoutMs);
    signal?.addEventListener('abort', abort, { once: true });
    if (signal?.aborted) abort();
    const consume = (key, bytes) => {
      size += bytes.length;
      if (size > limit) { stop(new Error('COMMAND_OUTPUT_LIMIT')); return; }
      if (key === 'stdout') stdout += bytes.toString('utf8');
      else stderr += bytes.toString('utf8');
    };
    child.stdout.on('data', b => consume('stdout', b));
    child.stderr.on('data', b => consume('stderr', b));
    child.on('error', error => { stopReason ??= error; });
    child.on('close', (code, exitSignal) => {
      // Even a command exiting normally must not leave a detached owned group behind.
      if (process.platform !== 'win32' && child.pid) kill('SIGKILL');
      clearTimeout(deadline); clearTimeout(killer);
      signal?.removeEventListener('abort', abort);
      if (stopReason) return reject(Object.assign(stopReason, { stdout, stderr }));
      resolve({ file, args, code, signal: exitSignal, stdout, stderr,
        startedAt, elapsedMs: Date.now() - startedAt });
    });
  });
}
