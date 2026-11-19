import { randomUUID, createHash } from 'node:crypto';
import { realpath, mkdtemp, rm, lstat, readFile, mkdir, writeFile, readlink } from 'node:fs/promises';
import { join, dirname, resolve, relative, isAbsolute } from 'node:path';
import { tmpdir } from 'node:os';
import { run } from './process.js';

const secret = /(^|\/)(\.env[^/]*|auth\.json|credentials?|secrets?|node_modules|build[^/]*|\.venv)(\/|$)|\.(pem|key|p12|pfx)$/i;
export const hash = value => createHash('sha256').update(value).digest('hex');
export async function git(cwd, args, options = {}) {
  const r = await run('git', ['-c', 'core.fsmonitor=false', '-C', cwd, ...args],
    { timeoutMs: 30000, ...options });
  if (r.code !== 0) throw new Error(`GIT_FAILED: ${args[0]}: ${r.stderr.slice(-1500)}`);
  return r.stdout.trimEnd();
}
export async function identity(root) {
  const path = await realpath(root);
  if (path !== await git(path, ['rev-parse', '--show-toplevel']))
    throw new Error('REPOSITORY_ROOT_REQUIRED');
  const common = await realpath(resolve(path, await git(path, ['rev-parse', '--git-common-dir'])));
  return { root: path, common };
}
export async function pair(authorRoot, validationRoot) {
  const author = await identity(authorRoot), validation = await identity(validationRoot);
  if (author.root === validation.root || author.common !== validation.common)
    throw new Error('DISTINCT_WORKTREES_OF_SAME_REPOSITORY_REQUIRED');
  return { author, validation };
}
export async function clean(root) {
  if (await git(root, ['status', '--porcelain=v1', '--untracked-files=all']))
    throw new Error('VALIDATION_WORKTREE_DIRTY: preserve/resolve existing changes before enabling');
}

// A private index captures staged + unstaged + safe untracked contents without changing the user index.
// Two tree scans and HEAD checks reject drift; the source lease must also be respected by writers.
export async function capture(root, { retain = true, signal } = {}) {
  const before = await git(root, ['rev-parse', 'HEAD']);
  const scratch = await mkdtemp(join(tmpdir(), 'dsh-validation-index-'));
  const env = { GIT_INDEX_FILE: join(scratch, 'index') };
  try {
    const scan = async () => {
      await git(root, ['read-tree', before], { env, signal });
      await git(root, ['add', '-u', '--', '.'], { env, signal });
      const names = (await git(root, ['ls-files', '-z', '--cached', '--others', '--exclude-standard'],
        { env, signal })).split('\0').filter(Boolean);
      let bytes = 0;
      if (names.length > 20000) throw new Error('SOURCE_FILE_LIMIT');
      const untracked = new Set((await git(root, ['ls-files', '-z', '--others', '--exclude-standard'],
        { env, signal })).split('\0').filter(Boolean));
      const add = [];
      for (const name of names) {
        signal?.throwIfAborted();
        if (untracked.has(name) && secret.test(name)) continue;
        const p = join(root, name), st = await lstat(p).catch(e => {
          if (e.code === 'ENOENT') return null; throw e;
        });
        if (!st) continue;
        if (st.isDirectory()) throw new Error(`SUBMODULE_REQUIRES_DECLARED_CLOSURE: ${name}`);
        bytes += st.size;
        if (bytes > 256 * 1024 * 1024) throw new Error('SOURCE_SIZE_LIMIT');
        if (st.isSymbolicLink()) {
          if (isAbsolute(await readlink(p))) throw new Error(`SOURCE_SYMLINK_ABSOLUTE: ${name}`);
          const dest = await realpath(p);
          if (relative(root, dest).startsWith('..') || isAbsolute(relative(root, dest)))
            throw new Error(`SOURCE_SYMLINK_ESCAPES: ${name}`);
          await git(root, ['ls-files', '--error-unmatch', '--', relative(root, dest)], { env, signal });
        }
        if (untracked.has(name)) add.push(name);
      }
      for (let i = 0; i < add.length; i += 100)
        await git(root, ['add', '--', ...add.slice(i, i + 100)], { env, signal });
      return { tree: await git(root, ['write-tree'], { env, signal }), bytes };
    };
    const first = await scan(), second = await scan();
    if (first.tree !== second.tree || before !== await git(root, ['rev-parse', 'HEAD']))
      throw new Error('SOURCE_CHANGED_DURING_CAPTURE');
    if (!retain) return { head: before, ...second };
    const id = randomUUID();
    const commit = await git(root, ['-c', 'user.name=DSH Validation',
      '-c', 'user.email=dsh-validation@localhost', 'commit-tree', second.tree, '-p', before,
      '-m', `Private validation candidate ${id}; not task completion`], { signal });
    const ref = `refs/dsh-validation/${id}`;
    await git(root, ['update-ref', ref, commit]);
    return { id, ref, commit, head: before, ...second, capturedAt: Date.now() };
  } finally { await rm(scratch, { recursive: true, force: true }); }
}
export async function release(root, candidate) {
  if (candidate?.ref?.startsWith('refs/dsh-validation/'))
    await git(root, ['update-ref', '-d', candidate.ref, candidate.commit]);
}
export async function lock(root) {
  const repo = await identity(root), path = join(repo.common, 'dsh-validation.lock');
  await mkdir(path).catch(e => { if (e.code === 'EEXIST') throw new Error('BUILD_LOCK_HELD'); throw e; });
  const token = randomUUID();
  try { await writeFile(join(path, 'owner.json'), JSON.stringify({ pid: process.pid, token })); }
  catch (e) { await rm(path, { recursive: true, force: true }); throw e; }
  return async () => {
    const owner = JSON.parse(await readFile(join(path, 'owner.json'), 'utf8'));
    if (owner.token !== token) throw new Error('BUILD_LOCK_OWNER_CHANGED');
    await rm(path, { recursive: true });
  };
}
export async function materialize(root, candidate) {
  await clean(root);
  await git(root, ['-c', 'advice.detachedHead=false', 'switch', '--detach', candidate.commit]);
  if (await git(root, ['rev-parse', 'HEAD^{tree}']) !== candidate.tree)
    throw new Error('CANDIDATE_TREE_MISMATCH');
}
export async function inspect(root, path) {
  if (typeof path !== 'string' || isAbsolute(path) || path.split(/[\\/]/).includes('..') || secret.test(path))
    throw new Error('READ_PATH_REJECTED');
  await git(root, ['ls-files', '--error-unmatch', '--', path]);
  const p = await realpath(join(root, path));
  if (relative(root, p).startsWith('..')) throw new Error('READ_PATH_ESCAPES');
  const st = await lstat(p);
  if (!st.isFile() || st.size > 256 * 1024) throw new Error('READ_FILE_LIMIT');
  return (await readFile(p, 'utf8')).slice(0, 40000);
}
