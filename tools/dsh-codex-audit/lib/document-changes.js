import { lstatSync, realpathSync, readFileSync, writeFileSync, renameSync, unlinkSync } from 'node:fs';
import { join, relative, dirname } from 'node:path';
import { randomUUID } from 'node:crypto';
import { digest, safeRelative } from './snapshot.js';

export function reviewDocumentChanges(changes, snapshot) {
  if (!Array.isArray(changes) || changes.length > 20) throw new Error('Too many document changes');
  return changes.map(change => {
    if (!safeRelative(change.path) || !/\.(md|tex|txt)$/.test(change.path) ||
        !snapshot.hashes[change.path] || typeof change.content !== 'string' ||
        Buffer.byteLength(change.content) > 128 * 1024 ||
        change.baseHash !== snapshot.hashes[change.path] || typeof change.reason !== 'string' ||
        typeof change.changesContract !== 'boolean') {
      throw new Error('Document change violates path, size, or snapshot hash contract');
    }
    const original = readFileSync(join(snapshot.path, change.path), 'utf8');
    const contract = /(^|\/)(spec|plan|tasks|data-model)\.md$|(^|\/)contracts\//.test(change.path);
    const instructions = /(^|\/)(AGENTS|CLAUDE|GEMINI|SKILL)\.md$|(^|\/)(skills|\.agents|\.codex)\//i.test(change.path);
    return { ...change, before: original, status: change.changesContract || contract || instructions ?
      'proposal-only' : 'pending', originalCwd: snapshot.originalCwd };
  });
}

// Synchronous final checks and rename keep another DSH step from starting between them.
// Caller requires an idle agent. External editors can still change files afterwards.
export function applyDocumentChange(change) {
  if (change.status !== 'pending') throw new Error('Only pending factual document corrections may be applied');
  if (!safeRelative(change.path) || !/\.(md|tex|txt)$/.test(change.path)) throw new Error('Invalid document path');
  const root = realpathSync(change.originalCwd);
  const target = join(root, change.path);
  const resolved = realpathSync(target);
  if (relative(root, resolved).startsWith('..') || resolved !== target || !lstatSync(target).isFile()) {
    throw new Error('Document path escaped workspace or contains a symbolic link');
  }
  const current = readFileSync(target);
  if (digest(current) === digest(change.content)) {
    change.status = 'already-current';
    return;
  }
  if (digest(current) !== change.baseHash) {
    change.status = 'conflict';
    return;
  }
  const temp = join(dirname(target), `.codex-audit-${randomUUID()}.tmp`);
  try {
    writeFileSync(temp, change.content, { mode: lstatSync(target).mode & 0o777, flag: 'wx' });
    if (digest(readFileSync(target)) !== change.baseHash || realpathSync(target) !== resolved) {
      change.status = 'conflict';
      return;
    }
    renameSync(temp, target);
    change.status = 'applied';
    change.appliedAt = Date.now();
  } finally {
    try { unlinkSync(temp); } catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
}
