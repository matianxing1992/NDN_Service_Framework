import { createReadStream } from 'node:fs';
import { readdir, lstat } from 'node:fs/promises';
import { createInterface } from 'node:readline';
import { homedir } from 'node:os';
import { join, resolve } from 'node:path';

const MAX_LOG_BYTES = 32 * 1024 * 1024;
const unavailable = reason => ({ status: 'unavailable', source: 'codex-turn-context', reason });
export const codexHomePath = () => resolve(process.env.CODEX_HOME || join(homedir(), '.codex'));

// Read only metadata from the exact engine thread. Never infer actual selection
// from requested settings or return conversation/credential content to the UI.
export async function readCodexSelection({ threadId, startedAt, finishedAt = Date.now(),
  codexHome = codexHomePath() }) {
  if (!/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(threadId ?? '') ||
      !Number.isFinite(startedAt)) return unavailable('缺少有效的 Codex 会话标识或审计时间。');
  const directories = new Set();
  for (const delta of [-86400000, 0, 86400000]) {
    const date = new Date(startedAt + delta);
    for (const utc of [false, true]) {
      const year = utc ? date.getUTCFullYear() : date.getFullYear();
      const month = (utc ? date.getUTCMonth() : date.getMonth()) + 1;
      const day = utc ? date.getUTCDate() : date.getDate();
      directories.add(join(codexHome, 'sessions', String(year), String(month).padStart(2, '0'),
        String(day).padStart(2, '0')));
    }
  }
  try {
    for (const directory of directories) {
      const names = await readdir(directory).catch(error => {
        if (error.code === 'ENOENT') return [];
        throw error;
      });
      const name = names.find(name => name.toLowerCase().endsWith(`-${threadId.toLowerCase()}.jsonl`));
      if (!name) continue;
      const path = join(directory, name);
      const stat = await lstat(path);
      if (!stat.isFile() || stat.isSymbolicLink() || stat.size === 0 || stat.size > MAX_LOG_BYTES) {
        return unavailable('对应运行日志类型不支持或超过 32 MiB 读取上限。');
      }
      let sameThread = false;
      let contextCount = 0;
      const selections = new Map();
      const stream = createReadStream(path, { encoding: 'utf8', start: 0, end: stat.size - 1 });
      const reader = createInterface({ input: stream, crlfDelay: Infinity });
      try {
        for await (const line of reader) {
          if (!/"type"\s*:\s*"(?:session_meta|turn_context)"/.test(line)) continue;
          let row;
          try { row = JSON.parse(line); } catch { continue; }
          if (row.type === 'session_meta') sameThread ||= row.payload?.id === threadId;
          if (row.type !== 'turn_context') continue;
          // A user may later resume the audit thread with another model. That
          // later selection must not rewrite the configuration of this audit.
          if (typeof row.timestamp === 'string' && Date.parse(row.timestamp) > finishedAt) continue;
          const model = row.payload?.model;
          const effort = row.payload?.effort ?? row.payload?.reasoning_effort;
          if (typeof model !== 'string' || typeof effort !== 'string') continue;
          contextCount++;
          selections.set(JSON.stringify([model, effort]), { model, effort });
        }
      } finally { reader.close(); stream.destroy(); }
      if (!sameThread || !contextCount) return unavailable('对应日志未提供可核对的模型和思考等级。');
      if (selections.size !== 1) return { status: 'mixed', source: 'codex-turn-context',
        threadId, selections: [...selections.values()], observedAt: Date.now() };
      return { status: 'confirmed', source: 'codex-turn-context', threadId,
        ...selections.values().next().value, contextCount, observedAt: Date.now() };
    }
    return unavailable('未找到该审计对应的 Codex 运行日志。');
  } catch (error) {
    return unavailable(error.code === 'EACCES' ? '没有读取对应 Codex 运行日志的权限。' :
      `无法读取对应运行记录（${error.code ?? 'READ_FAILED'}）。`);
  }
}
