window.__ModuleLoader__.load({ id: 'dsh-codex-audit', factory: require => {
  const React = require('react');
  const h = React.createElement;
  const { createPortal } = require('react-dom');
  const { MarkdownText } = require('@deepseek-ai/dsh-client-ui-primitives');
  const EFFORT_LABEL = { none: '无', minimal: '最低', low: '低', medium: '中等',
    high: '高', xhigh: '极高（Extra high）', max: 'Max', ultra: 'Ultra' };
  const markdownLabels = { code: { copyLabel: '复制', copiedLabel: '已复制',
    toolbarLabels: { codeLabel: '代码', wrapLabel: '自动换行', unwrapLabel: '取消换行' } }, footnotes: '注释' };
  const css = `
    .ca-dock,.ca-panel{font-family:inherit;color:var(--dsw-alias-label-primary,#eceef3)}
    .ca-dock{display:inline-flex;align-items:center;gap:7px;border:1px solid var(--dsw-alias-border-l2,#41444e);border-radius:999px;padding:5px 10px;background:transparent;font-size:12px;cursor:pointer}
    .ca-dot{width:6px;height:6px;border-radius:50%;background:#8b91a0}.ca-dot.on{background:#4fc9a5}.ca-dot.error{background:#f58b82}
    .ca-overlay{position:fixed;inset:0;z-index:10000;background:#0005;display:flex;justify-content:flex-end;padding:16px;backdrop-filter:blur(3px)}
    .ca-panel{width:min(860px,100%);height:100%;box-sizing:border-box;display:flex;flex-direction:column;overflow:hidden;background:var(--dsw-alias-bg-base,#16181e);border:1px solid var(--dsw-alias-border-l2,#373b46);border-radius:18px;box-shadow:0 20px 80px #0006;font-size:13px;line-height:1.65}
    .ca-head{display:flex;align-items:flex-start;justify-content:space-between;padding:24px 26px 18px;border-bottom:1px solid var(--dsw-alias-border-l2,#373b46)}
    .ca-eyebrow{font-size:10px;letter-spacing:1.5px;color:var(--dsw-alias-label-caption,#969caa);font-weight:600}.ca-title{font-size:23px;line-height:1.4;letter-spacing:-.5px;margin:3px 0 5px;font-weight:650}
    .ca-muted{color:var(--dsw-alias-label-tertiary,#a3a9b7);font-size:12px}.ca-body{overflow:auto;padding:20px 26px 26px;flex:1;min-height:0}.ca-row{display:flex;align-items:center;gap:10px;flex-wrap:wrap}.ca-between{justify-content:space-between}.ca-stack{display:flex;flex-direction:column;gap:3px;min-width:0}
    .ca-card{border:1px solid var(--dsw-alias-border-l2,#373b46);background:var(--dsw-alias-bg-layer-1,#1d2028);border-radius:12px;padding:17px 18px;margin-bottom:16px}.ca-section-title{font-size:14px;font-weight:600;margin:0}
    .ca-panel button,.ca-panel select,.ca-panel input{font:inherit;color:inherit;box-sizing:border-box}.ca-btn,.ca-close{border:1px solid var(--dsw-alias-border-l2,#454a57);background:transparent;border-radius:8px;padding:7px 12px;cursor:pointer;transition:background .15s,border-color .15s}.ca-btn:hover,.ca-close:hover{background:var(--dsw-alias-interactive-bg-hover,#ffffff0b);border-color:#737b90}.ca-btn:disabled{opacity:.4;cursor:default}.ca-primary{background:#5868d9!important;border-color:#6c7be6!important;color:white!important}.ca-danger{color:#ed9b96!important}.ca-close{border:0;padding:3px 9px;font-size:22px;line-height:1.2}
    .ca-switch{display:flex;align-items:center;gap:8px;border:0;background:transparent;cursor:pointer;padding:4px 0}.ca-track{width:31px;height:18px;background:#565c6b;border-radius:12px;position:relative;transition:background .15s}.ca-track:after{content:'';position:absolute;top:3px;left:3px;width:12px;height:12px;border-radius:50%;background:white;transition:transform .15s}.ca-track.on{background:#6172e4}.ca-track.on:after{transform:translateX(13px)}
    .ca-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr)) minmax(0,1.4fr);gap:16px;margin:16px 0}.ca-field{display:flex;flex-direction:column;gap:7px}.ca-field label{font-size:11px;color:var(--dsw-alias-label-tertiary,#a3a9b7)}.ca-panel select,.ca-panel input{background:var(--dsw-alias-bg-base,#16181e);border:1px solid var(--dsw-alias-border-l2,#414754);border-radius:8px;padding:8px 10px;min-width:0}.ca-panel input{width:100%}.ca-next{font-size:14px;padding:7px 0;font-variant-numeric:tabular-nums}.ca-actions{border-top:1px solid var(--dsw-alias-border-l2,#373b46);padding-top:14px;margin-top:3px}
    .ca-alert{border:1px solid #f0957c55;background:#f0957c0c;color:var(--dsw-alias-label-primary,#f5c7ba);padding:12px 14px;border-radius:10px;overflow-wrap:anywhere;white-space:pre-wrap;margin-bottom:15px}.ca-settings{margin-bottom:23px}.ca-settings>summary{cursor:pointer;color:var(--dsw-alias-label-tertiary,#a3a9b7);font-size:12px}.ca-settings[open]>summary{margin-bottom:12px}.ca-history-head{margin-bottom:12px}.ca-count{border-radius:6px;padding:1px 6px;background:var(--dsw-alias-bg-layer-1,#272b35);font-size:11px;color:var(--dsw-alias-label-tertiary,#a3a9b7)}
    .ca-record{border:1px solid var(--dsw-alias-border-l2,#373b46);border-radius:11px;margin:10px 0;overflow:hidden}.ca-record>summary{display:flex;align-items:center;gap:12px;list-style:none;cursor:pointer;padding:15px}.ca-record>summary::-webkit-details-marker{display:none}.ca-chevron{color:var(--dsw-alias-label-caption,#8d95a5);transition:transform .15s}.ca-record[open]>summary>.ca-chevron{transform:rotate(90deg)}.ca-record[open]>summary{background:var(--dsw-alias-bg-layer-1,#20242c);border-bottom:1px solid var(--dsw-alias-border-l2,#373b46)}.ca-record>p,.ca-record>details,.ca-record>button{margin:12px 16px}.ca-record>p{font-size:12px;overflow-wrap:anywhere;color:var(--dsw-alias-label-tertiary,#abb1bf)}.ca-record details>summary{cursor:pointer;padding:5px 0;font-size:12px;font-weight:550}.ca-badges{display:flex;gap:5px;flex-wrap:wrap;justify-content:flex-end;margin-left:auto}.ca-badge{font-size:10px;white-space:nowrap;border-radius:5px;padding:2px 6px;background:var(--dsw-alias-bg-layer-1,#282d38);color:var(--dsw-alias-label-tertiary,#afb6c6)}.ca-badge.good{color:#62bca3;background:#35a78615}.ca-badge.bad{color:#e18e87;background:#de756c13}.ca-badge.live{color:#92a3ff;background:#6479e51c}
    .ca-markdown{font-size:13px;line-height:1.8;padding:8px 0 12px;overflow-wrap:anywhere}.ca-markdown table{display:block;max-width:100%;overflow-x:auto}.ca-empty{text-align:center;border:1px dashed var(--dsw-alias-border-l2,#424754);border-radius:12px;padding:30px 18px;color:var(--dsw-alias-label-tertiary,#9da6b7)}.ca-footer{border-top:1px solid var(--dsw-alias-border-l2,#373b46);padding:10px 26px;font-size:10px;color:var(--dsw-alias-label-caption,#8f97a6);overflow-wrap:anywhere}
    .ca-modal{position:fixed;inset:0;z-index:10002;display:flex;align-items:center;justify-content:center;padding:24px;background:#0007;backdrop-filter:blur(2px)}
    .ca-modal-card{width:min(720px,100%);max-height:calc(100vh - 48px);overflow:auto;box-sizing:border-box;background:var(--dsw-alias-bg-base,#16181e);border:1px solid var(--dsw-alias-border-l2,#414754);border-radius:14px;padding:20px 22px;box-shadow:0 24px 80px #0008;display:flex;flex-direction:column;gap:12px}
    .ca-modal-card .ca-actions{margin-top:0}
    .ca-prompt{width:100%;box-sizing:border-box;background:var(--dsw-alias-bg-base,#16181e);border:1px solid var(--dsw-alias-border-l2,#414754);border-radius:8px;padding:10px;font-family:inherit;font-size:12px;line-height:1.7;color:inherit;resize:vertical}
    .ca-panel :focus-visible,.ca-dock:focus-visible{outline:2px solid #8998f6;outline-offset:3px}@media(max-width:600px){.ca-overlay{padding:8px}.ca-panel{border-radius:12px}.ca-head{padding:18px}.ca-body{padding:16px}.ca-grid{grid-template-columns:1fr}.ca-badges{max-width:46%}.ca-record>summary{gap:8px;padding:12px}.ca-footer{padding:10px 16px}}@media(prefers-reduced-motion:reduce){.ca-panel *{transition:none!important}}
  `;
  const badge = (text, tone = '') => h('span', { className: `ca-badge ${tone}` }, text);
  const markdown = text => h('div', { className: 'ca-markdown' },
    h(MarkdownText, { text, labels: markdownLabels }));
  const date = value => value ? new Date(value).toLocaleString() : '—';
  const label = { running: '审计中', completed: '审计完成', failed: '审计失败',
    interrupted: '审计中断', cancelled: '已取消', pending: '待投递',
    delivered: '已投递', held: '报告已保留', 'not-ready': '未就绪' };

  function ConversationCard({ sessionId, rpc }) {
    const [latest, setLatest] = React.useState(null);
    const [expanded, setExpanded] = React.useState(null);
    const [body, setBody] = React.useState(null);
    const [dismissed, setDismissed] = React.useState(null);
    const [dismissError, setDismissError] = React.useState(null);
    const candidate = latest?.sessionId === sessionId ? latest.card : null;
    const card = dismissed?.sessionId === sessionId && dismissed.id === candidate?.id ? null : candidate;
    const id = card?.id;
    const isOpen = expanded?.sessionId === sessionId && expanded.id === id;

    React.useEffect(() => {
      const controller = new AbortController();
      let timer;
      const poll = async () => {
        if (controller.signal.aborted) return;
        if (!document.hidden) {
          try {
            const result = await rpc('view', { sessionId, conversationCard: true }, controller.signal);
            if (!controller.signal.aborted && !result.error) setLatest({ sessionId,
              card: result.card, dismissible: result.dismissible === true });
          } catch { /* Preserve the last receipt across a disconnected poll. */ }
        }
        if (!controller.signal.aborted) timer = setTimeout(poll, 2000);
      };
      poll();
      return () => { controller.abort(); clearTimeout(timer); };
    }, [sessionId, rpc]);

    React.useEffect(() => {
      if (!isOpen || !id || (body?.sessionId === sessionId && body.id === id && !body.error)) return;
      const controller = new AbortController();
      rpc('view', { sessionId, conversationCard: true, auditId: id }, controller.signal).then(result => {
        if (!controller.signal.aborted) setBody({ sessionId, id, report: result.card?.report,
          error: result.card ? null : '报告已不在审计历史中。' });
      }).catch(() => {
        if (!controller.signal.aborted) setBody({ sessionId, id, error: '暂时无法读取报告，请收起后重试。' });
      });
      return () => controller.abort();
    }, [isOpen, id, sessionId, rpc]);

    // Hide the plugin's native input rows even when there is no visible card.
    // They retain their original message IDs and remain in the model transcript.
    const nativeRows = /^[\w-]+$/.test(sessionId) ? h('style', null,
      `[data-conversation-session="${sessionId}"] [data-chat-node-key^="13:input-messagecodex-audit-AUD-"]{display:none}`) : null;
    if (!card) return nativeRows;
    const current = body?.sessionId === sessionId && body.id === id ? body : null;
    const wait = card.admitted || card.replied ? null : card.delivery === 'delivered' ?
      '已入队，等待 DSH 进入下一步' : card.delivery === 'held' ? '报告已保留' : '待投递';
    const close = async () => {
      try {
        await rpc('control', { sessionId, action: 'dismiss-card', auditId: id });
        setDismissed({ sessionId, id });
      } catch {
        setDismissError({ id, text: '关闭失败，请重试。' });
      }
    };
    return h('div', { className: 'ca-chat-card-wrap', 'data-codex-audit-id': id },
    nativeRows,
    latest.dismissible && h('button', { type: 'button', className: 'ca-chat-close', 'aria-label': '关闭审计报告',
      title: '关闭审计报告', onClick: close }, '×'),
    h('details', { className: 'ca-chat-card', open: isOpen,
      onToggle: event => {
        if (event.currentTarget.open !== isOpen) {
          setExpanded(event.currentTarget.open ? { sessionId, id } : null);
        }
      } },
    h('summary', null, h('strong', null, 'Codex 审计完成'), h('time', null, date(card.finishedAt)),
      h('span', { className: 'ca-chat-progress', 'aria-live': 'polite' },
        h('span', { 'data-reached': true }, '报告已完成'),
        h('span', { 'data-reached': card.admitted || card.replied }, 'DSH 已接收'),
        h('span', { 'data-reached': card.replied }, 'DSH 已回应'))),
    h('style', null, `
      .ca-chat-card-wrap{position:relative;width:100%}
      .ca-chat-close{position:absolute;inset-inline-end:8px;top:6px;z-index:1;border:0;border-radius:6px;background:transparent;color:inherit;font:inherit;cursor:pointer;padding:1px 7px}
      .ca-chat-close:hover{background:var(--dsw-alias-interactive-bg-hover,#78818f20)}
      .ca-chat-card{box-sizing:border-box;width:100%;margin:0 0 8px;padding:9px 14px;border:1px solid var(--dsw-alias-border-l2,#59616d);border-radius:12px;background:var(--dsw-alias-bg-layer-1,Canvas);color:var(--dsw-alias-label-primary,CanvasText)}
      .ca-chat-card>summary{cursor:pointer;line-height:1.7;padding-inline-end:24px}
      .ca-chat-card time,.ca-chat-progress{margin-inline-start:10px;font-size:.82em}
      .ca-chat-progress{display:inline-flex;gap:6px;flex-wrap:wrap}
      .ca-chat-progress span{padding:0 6px;border-radius:5px;background:var(--dsw-alias-bg-layer-2,#78818f20);opacity:.6}
      .ca-chat-progress [data-reached="true"]{opacity:1;background:#32a78024}
      .ca-chat-report{max-height:min(42vh,420px);overflow:auto;overflow-wrap:anywhere;padding:4px 0}
      .ca-chat-wait{margin:3px 0 0;font-size:.83em}
    `),
    dismissError?.id === id && h('p', { role: 'alert' }, dismissError.text),
    wait && h('p', { className: 'ca-chat-wait' }, wait, card.issue ? ` · ${card.issue}` : ''),
    isOpen && h('div', { className: 'ca-chat-report' },
      current?.report === undefined ? (current?.error ?? '正在读取报告…') : markdown(current.report))));
  }

  function Panel({ sessionId, rpc, revealReply }) {
    const [openSession, setOpenSession] = React.useState(null);
    const open = openSession === sessionId;
    const setOpen = value => setOpenSession(value ? sessionId : null);
    const [data, setData] = React.useState(null);
    const [error, setError] = React.useState('');
    const [busy, setBusy] = React.useState(false);
    const [path, setPath] = React.useState('');
    const [check, setCheck] = React.useState('');
    const [offset, setOffset] = React.useState(0);
    const [details, setDetails] = React.useState({});
    // A one-off prompt for the next manual audit. Kept in component state only: closing the panel or
    // reloading the page must not make it look like configuration, because it is not saved anywhere.
    const [runPrompt, setRunPrompt] = React.useState(null);
    const [promptOpen, setPromptOpen] = React.useState(false);
    const expanded = React.useRef(new Set());
    React.useEffect(() => {
      setOpen(false); setData(null); setDetails({}); setOffset(0); setCheck(''); setPath('');
      setRunPrompt(null); setPromptOpen(false);
      expanded.current.clear();
    }, [sessionId]);
    React.useEffect(() => {
      const hide = () => { if (document.hidden) setOpen(false); };
      const escape = event => { if (event.key === 'Escape') { event.stopPropagation(); setOpen(false); } };
      document.addEventListener('visibilitychange', hide);
      if (open) document.addEventListener('keydown', escape, true);
      return () => { document.removeEventListener('visibilitychange', hide);
        document.removeEventListener('keydown', escape, true); };
    }, [open]);
    const refresh = React.useCallback(async signal => {
      const value = await rpc('view', { sessionId, offset }, signal);
      setData(value);
      const full = await Promise.all([...expanded.current].slice(0, 20).map(async id => {
        const detail = await rpc('view', { sessionId, auditId: id }, signal);
        return [id, detail.records[0]];
      }));
      if (full.length) setDetails(previous => ({ ...previous,
        ...Object.fromEntries(full.filter(([, record]) => record)) }));
      setError('');
    }, [sessionId, rpc, offset]);
    React.useEffect(() => {
      const controller = new AbortController();
      let timer;
      let stopped = false;
      const poll = async () => {
        try { await refresh(controller.signal); }
        catch (error) { if (!stopped) setError(error.message); }
        if (!stopped) timer = setTimeout(poll, 10000);
      };
      poll();
      return () => { stopped = true; clearTimeout(timer); controller.abort(); };
    }, [refresh]);
    const act = async (action, values = {}) => {
      setBusy(true);
      try {
        const value = await rpc('control', { sessionId, action, ...values });
        if (action === 'check') setCheck(value.error ?
          `${value.error.code}: ${value.error.message} ${value.error.remedy}` :
          `Git 工作区：${value.cwd}\n临时目录可写 · Codex CLI 参数兼容：${value.codexPath}\n` +
          `Provider 配置通过：${value.provider}\n认证及模型调用待实跑确认。`);
        await refresh();
      } catch (error) { setError(error.message); }
      finally { setBusy(false); }
    };
    const settings = data?.settings;
    const reserved = Number.isFinite(settings?.reservationAt);
    const models = data?.models ?? [];
    const efforts = data?.efforts ?? [];
    const guard = data?.guard;
    const goalRef = guard?.goal ? { id: guard.goal.id, revision: guard.goal.revision } : null;
    const problem = error || (data?.error ?
      `${data.error.code ?? ''}: ${data.error.message} ${data.error.remedy ?? ''}` : '');
    const button = (text, onClick, disabled = false, variant = '') => h('button',
      { type: 'button', className: `ca-btn ${variant}`, onClick, disabled: disabled || busy }, text);
    const box = { whiteSpace: 'pre-wrap', overflowWrap: 'anywhere',
      background: 'var(--dsw-alias-bg-layer-1,#20242c)', padding: 12, borderRadius: 8,
      maxHeight: 400, overflow: 'auto', color: 'inherit', fontSize: 11, lineHeight: 1.7 };
    const records = (data?.records ?? []).map(summary => {
      const record = details[summary.id] ? { ...details[summary.id], ...summary,
        report: details[summary.id].report, documentChanges: details[summary.id].documentChanges,
        prompt: details[summary.id].prompt, dispatchedPrompt: details[summary.id].dispatchedPrompt,
        replies: details[summary.id].replies } : summary;
      return h('details', { key: record.id, className: 'ca-record',
      onToggle: async event => {
        if (event.target !== event.currentTarget) return;
        if (!event.currentTarget.open) { expanded.current.delete(record.id); return; }
        expanded.current.add(record.id);
        try {
          const value = await rpc('view', { sessionId, auditId: record.id });
          const full = value.records[0];
          if (full) setDetails(previous => ({ ...previous, [record.id]: full }));
        } catch (error) { setError(error.message); }
      },
      },
    h('summary', null, h('span', { className: 'ca-chevron', 'aria-hidden': true }, '›'),
      h('span', { className: 'ca-stack' }, h('strong', null, date(record.startedAt)),
        h('span', { className: 'ca-muted' },
          `${({ manual: '手动审计', scheduled: '定时审计', reservation: '预约审计',
            'goal-blocked': '阻塞审计' })[record.trigger] ?? '其他触发'} · ${record.model} · ${record.effort}`)),
      h('span', { className: 'ca-badges' },
        badge(label[record.status] ?? record.status, record.status === 'completed' ? 'good' : record.status === 'running' ? 'live' : 'bad'),
        badge(record.replies.length ? 'DSH 已回应' : record.delivery === 'delivered' &&
          !Number.isSafeInteger(record.admittedSeq) ? '已入队，待读取' : label[record.delivery] ?? record.delivery,
          record.replies.length ? 'good' : ''))),
    h('p', null, `${record.id}；完成：${date(record.finishedAt)}；耗时：` +
      `${Math.round(((record.finishedAt ?? Date.now()) - record.startedAt) / 1000)} 秒；尝试 ${record.attempts} 次`),
    h('p', null, record.modelObservation?.status === 'confirmed' ?
      `Codex 运行记录确认：${record.effectiveModel} · ${record.effectiveEffort === 'xhigh' ? 'Extra high（xhigh）' : record.effectiveEffort}` :
      record.attempts === 0 ? '本轮未发起模型请求。' :
        `请求配置：${record.model} · ${record.effort}；${record.modelObservation?.reason ?? '运行记录尚未提供确认信息。'}`),
    record.modelMismatch && h('p', { className: 'ca-alert' }, '历史运行配置与请求配置不同，请复核此报告。'),
    record.basis?.sourceMode === 'working-tree' && h('p', { className: 'ca-muted' },
      `工作区快照：${date(record.basis.capturedAt)}；HEAD 仅为基线，包含 ` +
      `${record.basis.workingTreeChanges.length} 项暂存、未暂存或未跟踪路径变化。`),
    record.basis?.scope && h('p', { className: 'ca-muted' },
      `重点范围：${({ 'since-enable': '本次开启后', 'since-audit': '上次成功快照后',
        lookback: '最近一个配置周期回看', blocked: '当前阻塞专项' })[record.basis.scope.kind]} · ` +
      `${date(record.basis.scope.startAt)} — ${date(record.basis.scope.endAt)}；` +
      `本期消息 ${record.basis.scope.focusedEvents} 条，变化文件 ${record.basis.scope.sourceChangeCount ?? '未比较'} 个。`),
    record.basis?.scope && h('p', { className: 'ca-muted' },
      record.basis.scope.coverage + '历史背景按需追溯，不声明全面覆盖。'),
    record.sourceCheck?.stale && h('p', { role: 'status', style: { color: '#ffd080' } },
      record.sourceCheck.error ? `源码复核失败：${record.sourceCheck.error.message}；本报告只对旧快照有效。` :
      `审计期间源码已变化（${record.sourceCheck.changedCount} 个源码文件，` +
      `当前 HEAD ${record.sourceCheck.currentHead}）；此报告须结合当前代码复核。`),
    h('details', null, h('summary', null, '完整请求与审计依据'),
      h('pre', { style: box }, (record.dispatchedPrompt ?? record.prompt) + '\n' + JSON.stringify(record.basis, null, 2))),
    record.error && h('p', { role: 'alert', style: { color: '#ffaaa0' } },
      `${record.error.code}: ${record.error.message}\n${record.error.remedy}`),
    record.documentError && h('p', { role: 'alert', style: { color: '#ffd080' } },
      `${record.documentError.code}: ${record.documentError.message} ${record.documentError.remedy}`),
    h('details', null, h('summary', null, '审计报告'), record.report ? markdown(record.report) : h('p', { className: 'ca-muted' }, '尚无完整报告')),
    h('details', null, h('summary', null, '文档修改'),
      ...(record.documentChanges ?? []).map((change, index) => h('div', { key: index },
        h('p', null, `${change.path} · ${change.status} · ${change.reason}`),
        h('details', null, h('summary', null, '修改前'), h('pre', { style: box }, change.before)),
        h('details', null, h('summary', null, '修改后'), h('pre', { style: box }, change.content)),
        change.status === 'pending' && button('核对并应用文档修正（DSH 空闲时）',
          () => act('apply-document', { auditId: record.id, index }))))),
    h('details', null, h('summary', null, '执行信息'),
      h('pre', { style: box }, JSON.stringify({ threadId: record.codexThreadId,
        usage: record.usage, deliveredAt: date(record.deliveredAt), messageId: record.messageId,
        admittedSeq: record.admittedSeq, admittedAt: date(record.admittedAt),
        modelObservation: record.modelObservation }, null, 2))),
    h('details', null, h('summary', null, 'DSH 回应'),
      record.replies.length ? record.replies.map(reply => h('div', { key: reply.messageId },
        h('p', { className: 'ca-muted' }, `${date(reply.at)} · 消息 ${reply.messageId}`),
        Number.isSafeInteger(reply.seq) && button('在对话中查看回应', async () => {
          try { if (await revealReply(reply)) setOpen(false); }
          catch (error) { setError(error.message); }
        }), markdown(reply.text ?? '正在加载回应…'))) :
        h('p', null, '尚未收到包含该审计编号的明确回应。')),
    record.delivery === 'pending' && button('重试投递', () => act('retry-delivery', { auditId: record.id })));
    });
    return h(React.Fragment, null,
      h('style', null, css),
      h('button', { type: 'button', className: 'ca-dock', 'aria-expanded': open,
        'aria-label': 'Codex 审计', onClick: () => setOpen(!open) },
        h('span', { className: `ca-dot ${problem ? 'error' : settings?.enabled ? 'on' : ''}` }),
        data?.running ? 'Codex 审计中' : 'Codex 审计',
        problem && h('span', null, '· 有错误')),
      open && createPortal(h('div', { className: 'ca-overlay', onClick: event => {
        if (event.target === event.currentTarget) setOpen(false);
      } }, h('section', { className: 'ca-panel', role: 'dialog', 'aria-modal': true,
        'aria-label': 'Codex 审计历史', 'data-audit-session': sessionId },
      h('header', { className: 'ca-head' }, h('div', null,
        h('div', { className: 'ca-eyebrow' }, 'CODEX AUDIT'),
        h('h2', { className: 'ca-title' }, '独立审计'),
        h('div', { className: 'ca-muted' }, '后台并行复核，DSH 继续当前任务。')),
        h('button', { className: 'ca-close', type: 'button', 'aria-label': '关闭审计面板',
          onClick: () => setOpen(false) }, '×')),
      h('div', { className: 'ca-body' },
      problem && h('div', { role: 'alert', className: 'ca-alert' }, problem),
      settings && h(React.Fragment, null, h('div', { className: 'ca-card' },
        h('div', { className: 'ca-row ca-between' },
          h('h3', { className: 'ca-section-title' }, '自动审计'),
          h('button', { type: 'button', className: 'ca-switch', role: 'switch',
            'aria-label': '自动审计开关', 'aria-checked': settings.enabled, disabled: busy,
            onClick: () => act('configure', { enabled: !settings.enabled }) },
            h('span', { className: 'ca-muted' }, settings.enabled ? '已开启' : '已暂停'),
            h('span', { className: `ca-track ${settings.enabled ? 'on' : ''}` }))),
        h('div', { className: 'ca-grid' },
        h('div', { className: 'ca-field' },
          h('label', { htmlFor: 'ca-model' }, '审计模型'),
          h('select', { id: 'ca-model', value: settings.model, disabled: busy || !models.length,
            onChange: event => act('configure', { model: event.target.value }) },
          !models.some(model => model.id === settings.model) &&
            h('option', { value: settings.model, disabled: true }, settings.model),
          models.map(model => h('option', { key: model.id, value: model.id }, model.id)))),
        h('div', { className: 'ca-field' },
          h('label', { htmlFor: 'ca-effort' }, '思考强度'),
          h('select', { id: 'ca-effort', value: settings.effort, disabled: busy || !efforts.length,
            onChange: event => act('configure', { effort: event.target.value }) },
          !efforts.includes(settings.effort) &&
            h('option', { value: settings.effort, disabled: true }, EFFORT_LABEL[settings.effort] ?? settings.effort),
          efforts.map(value => h('option', { key: value, value }, EFFORT_LABEL[value] ?? value)))),
        h('div', { className: 'ca-field' },
        h('label', { htmlFor: 'ca-interval' }, '调用周期'), h('select', { id: 'ca-interval',
          value: settings.intervalHours, disabled: busy,
          onChange: event => act('configure', { intervalHours: Number(event.target.value) }) },
        [1, 2, 3, 4, 5].map(value => h('option', { key: value, value }, `每 ${value} 小时`)))),
        h('div', { className: 'ca-field' }, h('label', null, '下次审计'),
          h('div', { className: 'ca-next' }, settings.enabled ? date(settings.nextDue) :
            reserved ? date(settings.reservationAt) : '未安排'))),
        h('div', { className: 'ca-row ca-actions' },
          button('立即审计', () => { setRunPrompt(settings.prompt ?? ''); setPromptOpen(true); },
            data.running, 'ca-primary'),
          button('跳过下一次', () => act('skip-next'), !settings.enabled),
          reserved ? button('取消预约', () => act('cancel-reservation')) :
            button('预约下次审计', () => act('reserve-next'), settings.enabled),
          data.running && button('取消当前审计', () => act('cancel'), false, 'ca-danger')),
        reserved ? h('p', { className: 'ca-muted' },
          `已预约一次 · ${date(settings.reservationAt)}${Date.now() >= settings.reservationAt ?
            '（已到期，等待执行）' : ''}`) : !settings.enabled &&
          h('p', { className: 'ca-muted' }, `预约将在 ${settings.intervalHours} 小时后执行一次。`),
        settings.enabled && h('p', { className: 'ca-muted' }, '立即审计会从本次开始时间重新计算自动审计周期。'),
        settings.enabled && settings.auditWindow && h('p', { className: 'ca-muted' },
          `本次开启：${date(settings.auditWindow.enabledAt)}；` +
          `下次重点从${settings.auditWindow.lastCoveredAt ? '上次成功快照 ' + date(settings.auditWindow.lastCoveredAt) : '本次开启'}开始。关闭期间不自动补审。`),
        h('p', { className: 'ca-muted' }, '立即审计没有新工作时回看最近一个配置周期；阻塞审计不推进整段工作覆盖点。'),
        settings.lastSkip && h('p', { className: 'ca-muted' },
          `最近跳过：${date(settings.lastSkip.skippedDue)}，顺延至 ${date(settings.lastSkip.nextDue)}。`)),
        guard && h('div', { className: 'ca-card' },
          h('div', { className: 'ca-row ca-between' },
            h('div', { className: 'ca-stack' },
              h('h3', { className: 'ca-section-title' }, '阻塞审计与续跑守护'),
              h('span', { className: 'ca-muted' }, '目标 blocked 时立即审计，按解阻方案继续')),
            h('button', { type: 'button', className: 'ca-switch', role: 'switch',
              'aria-label': '阻塞审计与续跑守护开关', 'aria-checked': guard.enabled,
              disabled: busy || (!guard.enabled && !['active', 'blocked'].includes(guard.goal?.phase)),
              onClick: () => act('configure-guard', { enabled: !guard.enabled, goalRef }) },
              h('span', { className: 'ca-muted' }, guard.enabled ? '已开启' : '已关闭'),
              h('span', { className: `ca-track ${guard.enabled ? 'on' : ''}` }))),
          h('p', { role: 'status' }, guard.message),
          guard.goal && h('p', { className: 'ca-muted' },
            `${guard.goal.objective ?? guard.goal.id} · ${guard.goal.phase} / ` +
            `${guard.goal.activation} · ${guard.goal.roundsStarted}/${guard.goal.maxGoalRounds} 轮`),
          guard.goal?.blockedReason && h('p', { className: 'ca-alert' },
            `${guard.goal.blockedReason.code}: ${guard.goal.blockedReason.message}`),
          guard.error && h('p', { className: 'ca-alert' }, guard.error),
          guard.blocked && h('div', null,
            h('p', { role: 'status' }, `解阻进度：${{
              queued: '排队等待审计', auditing: '正在分析阻塞根因',
              'awaiting-delivery': '报告待 DSH 读取', resumed: '已恢复目标，继续修复和验证',
              waiting: '需要用户输入或外部条件', stale: '代码已变化，请重新审计',
              superseded: '目标已变化，未继续', held: '检测到人工停止，未继续',
              limit: '同一原因已连续阻塞三次，请人工核查', budget: '目标轮数耗尽',
              uncertain: '上次恢复结果未确认，请人工核查', 'model-missing': 'DSH 模型不可用',
              'resume-backoff': '恢复失败，正在退避', 'resume-failed': '恢复失败三次，已停止重试',
              'audit-failed': '审计失败，请查看执行记录',
            }[guard.blocked.status] ?? guard.blocked.status}`),
            guard.blocked.summary && h('p', null, guard.blocked.summary),
            guard.blocked.steps?.length > 0 && h('ol', null,
              ...guard.blocked.steps.map((step, index) => h('li', { key: index }, step))),
            guard.blocked.error && h('p', { className: 'ca-alert' }, guard.blocked.error)),
          h('div', { className: 'ca-row ca-actions' },
            button('恢复当前目标', () => act('resume-goal', { goalRef }),
              guard.goal?.phase !== 'active' || guard.goal?.activation === 'armed' ||
              ['running', 'queued', 'model-missing', 'budget'].includes(guard.status))),
          guard.goal?.phase === 'blocked' && button('重新审计阻塞',
            () => act('review-blocked', { goalRef }), !guard.enabled || data.running),
          h('p', { className: 'ca-muted' },
            '只绑定开启时的目标；可执行方案会投递给 DSH 并恢复目标。需要用户或外部条件时保持阻塞。' +
            '公开下载、数据库和依赖选择由审计给出方案，DSH 执行与验证。' +
            '人工暂停和预算上限不会被自动绕过。' +
            '关闭守护不暂停已启用的目标；暂停请使用 DSH 原生目标控件。'),
          h('details', null, h('summary', null, `恢复历史 · ${guard.history?.length ?? 0}`),
            ...(guard.history ?? []).slice().reverse().map((entry, index) =>
              h('p', { key: index, className: 'ca-muted' },
                `${date(entry.at)} · ${{ enable: '开启守护', disable: '关闭守护',
                  'auto-resume': '重启恢复', 'manual-resume': '手动恢复', blocked: '目标阻塞',
                  'blocked-resume': '审计后解阻续跑', check: '状态检查' }[entry.action] ?? entry.action}` +
                `${entry.outcome ? ` · ${entry.outcome === 'resumed' ? '已恢复' : '失败'}` : ''}` +
                `${entry.error ? ` · ${entry.error}` : ''}`)))),
        h('details', { className: 'ca-settings' }, h('summary', null, '连接与运行环境'),
        h('div', { className: 'ca-stack' }, h('label', { className: 'ca-muted' }, 'Codex 可执行文件路径'),
        h('input', { value: path, placeholder: settings.codexPath || '自动查找 Codex CLI',
          'aria-label': 'Codex 可执行文件路径',
          onChange: event => setPath(event.target.value) }),
        h('div', { className: 'ca-row' }, button('保存路径', () => act('configure', { codexPath: path })),
        button('检查本机条件', () => act('check')))),
        check && h('p', { className: 'ca-muted' }, check))),
      h('div', { className: 'ca-row ca-between ca-history-head' },
        h('div', { className: 'ca-row' }, h('h3', { className: 'ca-section-title' }, '审计历史'),
          h('span', { className: 'ca-count' }, data?.total ?? records.length)),
        h('div', { className: 'ca-row' },
      button('上一页', () => { expanded.current.clear(); setDetails({}); setOffset(Math.max(0, offset - 20)); }, offset === 0),
      button('下一页', () => { expanded.current.clear(); setDetails({}); setOffset(offset + 20); }, offset + 20 >= (data?.total ?? 0)))),
      // A real overlay, not a card in the scrolling body: the first version inserted the prompt
      // panel after the guard card and the connection details, so clicking 立即审计 appeared to do
      // nothing until the operator scrolled down. Rendered here as a fixed, centred layer instead.
      ...(records.length ? records : [h('div', { className: 'ca-empty', key: 'empty' },
        h('strong', null, '还没有审计记录'), h('div', null, '可立即审计、预约一次或开启自动审计。'))])),
      h('footer', { className: 'ca-footer' }, `当前会话 · ${sessionId}`))), document.body),
      promptOpen && createPortal(h('div', { className: 'ca-modal', role: 'presentation',
        onClick: event => { if (event.target === event.currentTarget) setPromptOpen(false); } },
        h('section', { className: 'ca-modal-card', role: 'dialog', 'aria-modal': true,
          'aria-label': '本次审计提示词' },
          h('h3', { className: 'ca-section-title' }, '本次审计提示词'),
          h('p', { className: 'ca-muted' },
            '下面是默认提示词。可点“清除”清空后输入本次专用提示词；它只对这一次审计有效，' +
            '不会保存，也不改变自动审计的配置。'),
          h('textarea', { className: 'ca-prompt', 'aria-label': '本次提示词', rows: 10,
            autoFocus: true, value: runPrompt ?? '',
            onChange: event => setRunPrompt(event.target.value) }),
          h('div', { className: 'ca-row ca-actions' },
            button('清除', () => setRunPrompt(''), false),
            button('恢复默认', () => setRunPrompt(settings.prompt ?? ''), false),
            button('取消', () => setPromptOpen(false), false),
            button('开始审计', () => {
              const prompt = (runPrompt ?? '').trim();
              setPromptOpen(false); setRunPrompt(null);
              // An empty box means the operator cleared the default and changed their mind, not
              // "audit with no instructions"; the configured prompt still applies in that case.
              act('run', prompt ? { prompt } : {});
            }, false, 'ca-primary')))), document.body),
      );
  }

  return { name: 'dsh-codex-audit', inject: ['slots', 'connection', 'sessions'], apply(ctx) {
    const rpc = async (method, request, signal) => {
      const result = await ctx.connection.rpc.call('/api', `codexAudit/${method}`, { args: { request } }, signal);
      if (!result.ok) throw new Error(result.error?.message ?? '审计 RPC 调用失败');
      return result.value;
    };
    ctx.slots.inject('conversation.input.dock', () => ctx.slots.register({
      name: 'conversation.input.dock', id: 'codex-audit-report', order: 15,
      inject: sessionId => ({ sessionId, rpc }),
    }, ConversationCard));
    ctx.slots.inject('conversation.composer.dock', function* () {
      yield ctx.slots.register({ name: 'conversation.composer.dock', id: 'codex-audit', order: 2,
        inject: sessionId => ({ sessionId, rpc, revealReply: async reply => {
          const binding = ctx.sessions.binding(sessionId);
          if (!binding) throw new Error('原 DSH 会话当前未打开。');
          await binding.session.loadThrough(reply.seq);
          await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
          if (document.querySelector('.ca-panel')?.dataset.auditSession !== sessionId) return false;
          // DSH 0.2.2's Chat renderer addresses assistant-step rows by this
          // Conversation context key; the history loader is the maintained API.
          const kind = 'assistant-step';
          const key = `${kind.length}:${kind}${reply.turn}:${reply.step}`;
          const target = [...document.querySelectorAll('[data-chat-node-key]')]
            .find(element => element.getAttribute('data-chat-node-key') === key);
          if (!target || target.closest('[hidden]')) throw new Error('该回应尚未显示，请展开对应对话轮次后重试。');
          target.scrollIntoView({ block: 'center', behavior: 'auto' });
          return true;
        } }) }, Panel);
    });
  } };
} });
