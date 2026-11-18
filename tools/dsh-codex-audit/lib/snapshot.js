import { createHash } from 'node:crypto';
import { mkdtemp, mkdir, readFile, writeFile, rm, lstat, realpath, statfs } from 'node:fs/promises';
import { join, dirname, relative, isAbsolute, extname } from 'node:path';
import { execute } from './codex-runner.js';
import { AuditError } from './audit-manager.js';
import { conversationParts } from './conversation.js';
import { auditScope, describeScope } from './audit-scope.js';

const MAX_BYTES = 256 * 1024 * 1024;
const MAX_FILES = 15000;
const SOURCE_EXTENSIONS = new Set(['.cpp', '.hpp', '.h', '.c', '.cc', '.cxx', '.py', '.js',
  '.ts', '.tsx', '.jsx', '.md', '.txt', '.json', '.yaml', '.yml', '.toml', '.sh', '.cmake']);
const EXCLUDED = /(^|\/)(\.git|\.codex|\.aws|node_modules|\.venv|build[^/]*|credentials?|secrets?)(\/|$)|(^|\/)(\.env[^/]*|auth\.json|[^/]*\.(pem|key|p12|pfx))$/i;
export const digest = value => createHash('sha256').update(value).digest('hex');

export function describeBasis(basis) {
  if (basis.sourceMode !== 'working-tree') {
    return `历史审计依据 HEAD=${basis.head}；旧记录未保存工作区改动清单，不能据此判断是否包含未提交改动。`;
  }
  return `审计依据：${new Date(basis.capturedAt).toISOString()} 的最新工作区快照；` +
    `HEAD=${basis.head} 仅为基线，另含 ${basis.workingTreeChanges.length} 项暂存、未暂存或未跟踪路径变化。` +
    '审计以快照文件实际内容为准，包含删除；不是仅审计该 commit。';
}

export async function checkSnapshotCurrent(snapshot) {
  const changedFiles = [];
  let changedCount = 0;
  for (const [name, expected] of Object.entries(snapshot.hashes)) {
    let current;
    try { current = digest(await readFile(join(snapshot.originalCwd, name))); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    if (current !== expected) {
      changedCount++;
      if (changedFiles.length < 100) changedFiles.push(name);
    }
  }
  const list = async args => (await execute('git', ['-C', snapshot.originalCwd, ...args],
    { timeoutMs: 15000 })).stdout.split('\0').filter(Boolean);
  const names = await list(['ls-files', '-z', '--cached', '--others', '--exclude-standard']);
  if (snapshot.basis.feature) names.push(...await list(['ls-files', '-z', '--others',
    '--ignored', '--exclude-standard', '--', snapshot.basis.feature]));
  for (const name of new Set(names)) {
    if (snapshot.hashes[name] || !safeRelative(name) || EXCLUDED.test(name) ||
        !(SOURCE_EXTENSIONS.has(extname(name)) || name === 'wscript' || name.endsWith('/wscript'))) continue;
    const stat = await lstat(join(snapshot.originalCwd, name)).catch(error => {
      if (error.code === 'ENOENT') return null;
      throw error;
    });
    if (!stat?.isFile() || stat.isSymbolicLink()) continue;
    changedCount++;
    if (changedFiles.length < 100) changedFiles.push(name);
  }
  const currentHead = (await execute('git', ['-C', snapshot.originalCwd, 'rev-parse', 'HEAD'],
    { timeoutMs: 15000 })).stdout.trim();
  return { checkedAt: Date.now(), currentHead, changedCount, changedFiles,
    stale: changedCount > 0 || currentHead !== snapshot.basis.head };
}

export function safeRelative(path) {
  return typeof path === 'string' && path.length > 0 && !isAbsolute(path) &&
    !path.includes('\\') && !path.split('/').some(p => p === '..' || p === '' || p === '.');
}

export const REPORT_SCHEMA = {
  type: 'object', additionalProperties: false,
  properties: {
    report: { type: 'string' },
    blockedRecovery: { type: 'object', additionalProperties: false,
      properties: {
        decision: { type: 'string', enum: ['actionable', 'needs-user', 'external-wait', 'not-applicable'] },
        summary: { type: 'string' }, steps: { type: 'array', items: { type: 'string' } },
      }, required: ['decision', 'summary', 'steps'] },
    documentChanges: { type: 'array', items: { type: 'object', additionalProperties: false,
      properties: { path: { type: 'string' }, baseHash: { type: 'string' },
        content: { type: 'string' }, reason: { type: 'string' },
        changesContract: { type: 'boolean' } },
      required: ['path', 'baseHash', 'content', 'reason', 'changesContract'] } },
  }, required: ['report', 'documentChanges', 'blockedRecovery'],
};

// Capture the working tree, not HEAD alone. Ignored active Spec documents are included explicitly.
export async function createSnapshot({ cwd, conversation = '', seq, prompt, signal,
  baselineOnly = false, period = null, trigger = 'manual', intervalHours = 3 }) {
  if (Buffer.byteLength(conversation) > 64 * 1024 * 1024) {
    throw new AuditError('CONVERSATION_CAPACITY', '完整消息记录超过 64 MiB 审计输入上限。',
      '请为该超大会话提供明确的审计范围；插件没有静默截断对话。');
  }
  cwd = await realpath(cwd).catch(() => { throw new AuditError('WORKSPACE_NOT_FOUND',
    `DSH Host 无法访问工作目录：${cwd}`, '检查 DSH 与 VSCode 是否在同一主机及工作目录。'); });
  const fs = baselineOnly ? null : await statfs('/tmp');
  if (fs && (Number(fs.type) === 0x01021994 || Number(fs.bavail) * Number(fs.bsize) < MAX_BYTES * 2)) {
    throw new AuditError('SCRATCH_UNAVAILABLE', '/tmp 为内存盘或可用空间不足。',
      '为审计提供至少 512 MiB 可用的磁盘 /tmp。');
  }
  const git = async args => (await execute('git', ['-C', cwd, ...args],
    { signal, timeoutMs: 15000 })).stdout;
  const head = (await git(['rev-parse', 'HEAD'])).trim();
  const names = (await git(['ls-files', '-z', '--cached', '--others', '--exclude-standard']))
    .split('\0').filter(Boolean);
  let feature = null;
  try {
    feature = JSON.parse(await readFile(join(cwd, '.specify/feature.json'), 'utf8')).feature_directory;
    if (!safeRelative(feature)) throw new Error('Invalid active feature path');
    // Git can enumerate ignored files in the selected feature without traversing other caches.
    const ignored = await git(['ls-files', '-z', '--others', '--ignored', '--exclude-standard', '--', feature]);
    names.push(...ignored.split('\0').filter(Boolean));
    names.push('.specify/feature.json');
  } catch (error) { if (error.code !== 'ENOENT') throw error; }
  const selected = [...new Set(names)].filter(path => safeRelative(path) && !EXCLUDED.test(path) &&
    (SOURCE_EXTENSIONS.has(extname(path)) || path.endsWith('/wscript') || path === 'wscript'));
  const selectedNames = new Set(selected);
  const workingTreeStatus = async () => (await git(['status', '--porcelain=v1', '-z',
    '--untracked-files=all', '--no-renames'])).split('\0').filter(Boolean)
    .map(row => ({ path: row.slice(3), index: row[0], worktree: row[1] }))
    .filter(row => selectedNames.has(row.path));
  const workingTreeChanges = await workingTreeStatus();
  if (selected.length > MAX_FILES) throw new Error('Snapshot exceeds 15000 source files');
  const path = baselineOnly ? null : await mkdtemp('/tmp/dsh-codex-audit-');
  const hashes = {};
  let bytes = 0;
  try {
    for (const name of selected.sort()) {
      signal?.throwIfAborted();
      const original = join(cwd, name);
      let stat;
      try { stat = await lstat(original); } catch (error) { if (error.code === 'ENOENT') continue; throw error; }
      if (!stat.isFile() || stat.isSymbolicLink()) continue;
      const resolved = await realpath(original);
      if (relative(cwd, resolved).startsWith('..')) throw new Error(`Source escapes workspace: ${name}`);
      bytes += stat.size;
      if (bytes > MAX_BYTES || stat.size > 8 * 1024 * 1024) throw new Error('Snapshot source size limit exceeded');
      const content = await readFile(original);
      hashes[name] = digest(content);
      if (!baselineOnly) {
        await mkdir(dirname(join(path, name)), { recursive: true });
        await writeFile(join(path, name), content, { mode: 0o600 });
      }
    }
    for (const [name, hash] of Object.entries(hashes)) {
      if (digest(await readFile(join(cwd, name))) !== hash) throw new AuditError('SNAPSHOT_CHANGED',
        '复制期间源码发生变化，本轮未使用不一致快照。', '在下一个稳定步骤重新审计。', true);
    }
    if ((await git(['rev-parse', 'HEAD'])).trim() !== head) throw new Error('HEAD changed during snapshot');
    if (JSON.stringify(await workingTreeStatus()) !== JSON.stringify(workingTreeChanges)) {
      throw new AuditError('SNAPSHOT_CHANGED', '复制期间工作区改动清单发生变化。',
        '在下一个稳定步骤重新审计。', true);
    }
    const basis = { cwd, head, seq, feature, sourceMode: 'working-tree',
      capturedAt: Date.now(), workingTreeChanges, files: Object.keys(hashes).length,
      bytes, manifestHash: digest(JSON.stringify(hashes)) };
    const admission = await checkSnapshotCurrent({ originalCwd: cwd, hashes, basis });
    if (admission.stale) throw new AuditError('SNAPSHOT_CHANGED',
      '复制期间源码或文件清单发生变化。', '在下一个稳定步骤重新审计。', true);
    if (baselineOnly) return { basis: { cwd, head, seq, capturedAt: basis.capturedAt,
      manifestHash: basis.manifestHash, workingTreeChanges }, hashes };
    const events = conversation.split('\n').filter(Boolean).flatMap(line => {
      try { return [JSON.parse(line)]; } catch { return []; }
    });
    const window = auditScope({ period, trigger, events, basis, hashes, intervalHours });
    basis.scope = window.scope;
    const inputPath = join(path, '__audit_input__');
    if (hashes.__audit_input__) throw new Error('Reserved snapshot path collision');
    await mkdir(inputPath);
    await writeFile(join(inputPath, 'scope.json'), JSON.stringify(window.scope), { mode: 0o600 });
    await writeFile(join(inputPath, 'source-changes.json'), JSON.stringify({
      comparable: window.changes !== null, changes: window.changes }), { mode: 0o600 });
    const focusIndex = [];
    for (const [i, part] of conversationParts(window.selected.map(event => JSON.stringify(event)).join('\n')).entries()) {
      const file = `focus-${String(i).padStart(4, '0')}.jsonl`;
      await writeFile(join(inputPath, file), part, { mode: 0o600 });
      focusIndex.push({ file, bytes: Buffer.byteLength(part), sha256: digest(part) });
    }
    await writeFile(join(inputPath, 'focus.txt'), describeScope(window.scope) + '\n' +
      JSON.stringify(focusIndex, null, 2), { mode: 0o600 });
    const index = [];
    for (const [i, part] of conversationParts(conversation).entries()) {
      const file = `conversation-${String(i).padStart(4, '0')}.jsonl`;
      await writeFile(join(inputPath, file), part, { mode: 0o600 });
      const lines = part.split('\n');
      const parseSeq = line => { try { return JSON.parse(line).seq ?? null; } catch { return null; } };
      index.push({ file, fromSeq: parseSeq(lines[0]), toSeq: parseSeq(lines.at(-1)),
        bytes: Buffer.byteLength(part), sha256: digest(part) });
    }
    basis.conversationBytes = Buffer.byteLength(conversation);
    basis.conversationParts = index.length;
    await writeFile(join(inputPath, 'conversation.txt'),
      '完整对话按时间顺序保存在下列 JSONL 文件中，没有截断消息。' +
      '先读 focus.txt 的本期工作，再按需回溯此处的历史背景；窗口外不声明全面覆盖。\n' + JSON.stringify(index, null, 2), { mode: 0o600 });
    await writeFile(join(inputPath, 'manifest.json'), JSON.stringify({ basis, hashes }), { mode: 0o600 });
    const schemaPath = join(inputPath, 'report-schema.json');
    await writeFile(schemaPath, JSON.stringify(REPORT_SCHEMA), { mode: 0o600 });
    return { path, schemaPath, basis, hashes, originalCwd: cwd,
      prompt: `${prompt}\n\n只读审计快照，禁止构建、实验、提交及修改源码。\n` +
        describeBasis(basis) + '\n' +
        describeScope(window.scope) + '\n' +
        '先读 __audit_input__/focus.txt、scope.json、source-changes.json；' +
        '完整历史索引在 conversation.txt，仅按需回溯。旧文件正文未保存时不要臆测差异行。' +
        '快照仅含选定源码类型与活动 Spec，未提供的历史证据不等于从未执行或不存在。\n' +
        '必须审计快照中的当前源码、未提交改动及当前 Spec，并沿调用链检查；' +
        '对话中的旧 commit、旧审计结论和旧代码只能作为历史线索，不能替代当前文件。' +
        '不能因为 HEAD 未变而忽略未提交修改，也不要建议先提交再审计。\n' +
        'blockedRecovery 用于解阻判断：对现有目标和项目约定内能自行解决、能合理决定的问题，' +
        '审计应直接作出选择并给出可执行步骤，填 actionable。公开资源下载、数据库选型及配置、' +
        '兼容依赖安装、替代资源和实现路线选择均可自行决定；优先复用现有路径，说明获取方式和验证办法，' +
        '不得仅因需要下载或需要选择数据库就停下问用户。只有缺少无法从已授权来源取得的私有凭据、' +
        '付费授权或仅用户掌握的必要信息才填 needs-user；外部条件确实不可获得且无可行替代才填 external-wait。' +
        '普通审计填 not-applicable。审计负责判断和方案，DSH 负责实际下载、安装、修复与验证；' +
        '遵守现有权限和明确资源上限，不得将修复计划当成已经解决或验证通过。\n' +
        `对话见 __audit_input__/conversation.txt；版本及文件 SHA256 见 __audit_input__/manifest.json。\n` +
        `当前 Spec：${feature ?? '未选择；按当前用户目标及已有设计审计，不强制要求 Spec'}。\n` +
        '报告须区分确认缺陷、条件风险和未验证事项，并给出调用链及文件位置。\n' +
        '文档修正以 documentChanges 返回完整替换内容及原始 baseHash，不直接写入。\n' +
        '仅修正有证据的事实及措辞；目标、依赖、验收标准的变更必须 changesContract=true，留作建议。',
      dispose: () => rm(path, { recursive: true, force: true }),
    };
  } catch (error) {
    if (path) await rm(path, { recursive: true, force: true });
    throw error;
  }
}
