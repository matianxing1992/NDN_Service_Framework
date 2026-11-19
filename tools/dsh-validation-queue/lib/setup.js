import { access, realpath } from 'node:fs/promises';
import { join } from 'node:path';
import { git, identity } from './repository.js';

export function setupProblem(error) {
  const detail = error.message ?? String(error);
  const code = detail.split(':', 1)[0];
  const messages = {
    CONFIGURATION_REQUIRED: '尚未完成首次设置。请先选择开发目录、验收目录和验证命令。',
    WORKSPACE_PATHS_REQUIRED: '请填写两个真实工作区的绝对路径。',
    DISTINCT_WORKTREES_OF_SAME_REPOSITORY_REQUIRED: '开发目录与验收目录必须是同一 Git 仓库的两个不同 worktree。',
    VALIDATION_WORKTREE_DIRTY: '验收目录还有未提交或未跟踪内容。请先保留并处理这些改动；插件不会替你清空目录。',
    VALIDATION_WORKSPACE_HAS_RESIDENT_AGENT: '验收目录仍有 DSH 会话驻留。请先完成或安全迁移该会话，避免构建期间仍有人改源码。',
    QUEUE_MUST_BE_DRAINED_BEFORE_RECONFIGURATION: '请先完成或取消队列中的任务，再修改工作区或命令。',
    PROFILE_REQUIRED: '至少配置一组验证命令及其验收标准。',
    BUILD_ARTIFACT_PATHS_REQUIRED: '构建命令需要填写实际输出文件，供运行前后核对摘要。',
    AUTHOR_MODEL_UNAVAILABLE: '当前主会话尚未选择可用模型。',
    BUILD_LOCK_HELD: '已有验证进程持有该仓库的构建锁，不能并发接管。',
  };
  return { code, message: messages[code] ??
    (error.code === 'ENOENT' ? '指定目录不存在，请选择现有 worktree；示例路径不能直接使用。' : detail), detail };
}

// Discovery is read-only. It never creates a worktree, changes HEAD or takes a build lease.
export async function discoverSetup(cwd, agents = []) {
  const current = await identity(cwd);
  // Older supported Git does not provide `worktree list -z`.
  const raw = await git(current.root, ['-c', 'core.quotePath=false', 'worktree', 'list', '--porcelain']);
  const entries = raw.split('\n\n').filter(Boolean).map(block => {
    const rows = block.split('\n');
    return { path: rows.find(x => x.startsWith('worktree '))?.slice(9),
      head: rows.find(x => x.startsWith('HEAD '))?.slice(5),
      branch: rows.find(x => x.startsWith('branch '))?.slice(7) ?? '(detached)' };
  }).filter(x => x.path).slice(0, 30);
  const exists = async p => access(p).then(() => true, () => false);
  const residents = await Promise.all(agents.filter(x => x.cwd).map(async x => ({
    ...x, cwd: await realpath(x.cwd).catch(() => x.cwd) })));
  for (const row of entries) {
    try {
      row.path = await realpath(row.path);
      row.dirty = !!await git(row.path, ['status', '--porcelain=v1', '--untracked-files=all']);
      row.hasNormalBuild = await exists(join(row.path, 'build/c4che/_cache.py'));
      row.hasSanitizerBuild = await exists(join(row.path, 'build-ASAN-UBSAN/c4che/_cache.py'));
      row.residents = residents.filter(x => x.cwd === row.path).map(({ id, status }) => ({ id, status }));
    } catch (error) { row.problem = setupProblem(error); }
  }
  return { currentRoot: current.root, worktrees: entries,
    proposedAuthorRoot: `${current.root}-coding`,
    note: '目录检测不会迁移当前会话、修改源码或构建。现有构建树应由验收工作区独占；不要为开启插件创建第三套编译树。' };
}
