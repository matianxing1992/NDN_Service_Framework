import { randomUUID } from 'node:crypto';
import { message } from './worker.js';

function textOf(event) {
  const data = event.data.message ?? event.data;
  const content = data.content ?? [];
  return typeof content === 'string' ? content : content.filter(b => b.type === 'text')
    .map(b => b.text ?? '').join('\n');
}
// Release only the selected children. drainDescendants would forbid future child admission.
export async function handoffChildren(ctx, parent, childIds, save, remainingByChild) {
  const service = ctx.get('subagents');
  if (!service?.drainChildren) throw new Error('SELECTIVE_CHILD_DRAIN_UNAVAILABLE');
  const catalog = await service.listChildren(parent.id);
  const selected = [...new Set(childIds)];
  if (!selected.length || selected.some(id => !catalog.some(x => x.id === id && x.mode === 'continuable')))
    throw new Error('SELECT_CURRENT_DIRECT_CHILDREN_ONLY');
  if (selected.some(id => typeof remainingByChild?.[id] !== 'string' || !remainingByChild[id].trim() ||
      remainingByChild[id].length > 6000)) throw new Error('EXPLICIT_REMAINING_WORK_REQUIRED');
  const record = { id: randomUUID(), parentId: parent.id, createdAt: Date.now(), children: [] };
  const capture = async id => {
    const cut = await ctx.sessionQuery.observeSession(id, { projectionMode: 'none' });
    try {
      const own = cut.events.slice(cut.inheritedEventCount ?? 0);
      const users = own.filter(e => e.type === 'user/message' &&
        (e.data.message ?? e.data).source?.kind === 'user');
      const replies = own.filter(e => e.type === 'assistant/message' && textOf(e));
      return { id, label: catalog.find(x => x.id === id).label, cutoff: cut.cursor,
        remaining: remainingByChild[id],
        task: users.length ? textOf(users[0]).slice(0, 10000) : '(read original session task)',
        lastReplies: replies.slice(-2).map(e => ({ seq: e.seq, text: textOf(e).slice(0, 10000) })) };
    } finally { cut[Symbol.dispose](); }
  };
  for (const id of selected) record.children.push(await capture(id));
  record.status = 'captured'; await save(record);
  await service.drainChildren(parent, selected);
  for (let i = 0; i < selected.length; ++i) {
    const final = await capture(selected[i]);
    record.children[i] = { ...final, loaded: !!ctx.agents.get(selected[i]) };
  }
  if (record.children.some(x => x.loaded)) throw new Error('CHILD_STILL_LOADED_AFTER_DRAIN');
  record.status = 'released'; record.finishedAt = Date.now(); await save(record);
  parent.steer(message(`validation-handoff-${record.id}`,
    '用户明确要求：结束本次已有子代理，将尚未完成的工作交回你；以后由你按需管理子代理。\n' +
    '已通过原生 drainChildren 只释放下面这些现有子代理，保留源码改动和完整会话记录。' +
    '未对你调用 drainDescendants，因此以后仍能按需派发。原goal不变。\n' +
    '只接管尚未完成的项；已经交付/验证的成果引用原证据，不重做，也不因子代理结束而标DONE。' +
    '不要立即恢复旧子代理；先读各自记录与当前工作树，核对其最新结论、待验证边界和原失败。' +
    '后续有必要再派发有界子任务，统一管理构建树与资源。\n\n' +
    record.children.map(x => `子代理 ${x.id}（${x.label}），对话截止 ${x.cutoff}\n` +
      `只需接管的剩余工作：\n${x.remaining}\n` +
      '完整原任务与已完成成果仍在该子会话持久记录；不要把原任务整段重新执行。').join('\n\n') +
    '\n上述摘录不是新的验证结果；没有最终报告的子任务应从持久记录续查，不从零重做。'));
  record.status = 'delivered'; await save(record);
  return record;
}
