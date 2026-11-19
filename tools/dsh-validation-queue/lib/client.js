window.__ModuleLoader__.load({ id: 'dsh-validation-queue', factory: require => {
  const React = require('react'), h = React.createElement;
  const { createPortal } = require('react-dom');
  const example = {
    enabled: false, authorRoot: '/path/to/coding-worktree', validationRoot: '/path/to/original-checkout',
    profiles: { unit: { acceptance: '实际执行所选基础测试；缺前置、skip、零匹配不能记为通过。',
      artifacts: ['build/unit-tests'], commands: [
        { kind: 'build', file: './waf', args: ['build', '--targets=unit-tests', '-j6'] },
        { kind: 'test', file: 'python3', args: ['tests/run-checks.py', '--profile', 'unit'], timeoutMs: 3600000 }
      ] } }
  };
  const css = `
    .vq-chip{border:1px solid var(--dsw-alias-border-l2,#7775);border-radius:18px;padding:5px 10px;background:transparent;color:inherit;cursor:pointer;font:inherit;font-size:12px}
    .vq-overlay{position:fixed;inset:0;background:#0005;z-index:10000;display:flex;justify-content:flex-end;padding:18px;backdrop-filter:blur(3px)}
    .vq-panel{width:min(850px,100%);height:100%;box-sizing:border-box;background:var(--dsw-alias-bg-base,#17191f);color:var(--dsw-alias-label-primary,#e4e6ee);border:1px solid #8884;border-radius:18px;box-shadow:0 18px 80px #0006;display:flex;flex-direction:column;font:13px/1.65 system-ui}
    .vq-head{padding:22px 24px;border-bottom:1px solid #8883;display:flex;justify-content:space-between;align-items:center}.vq-head h2{font-size:23px;margin:0}.vq-head small{opacity:.65}.vq-body{padding:20px 24px;overflow:auto}.vq-row{display:flex;gap:9px;align-items:center;flex-wrap:wrap}.vq-card{border:1px solid #8883;border-radius:11px;padding:14px 16px;margin:12px 0}.vq-panel button{font:inherit;background:transparent;color:inherit;border:1px solid #8885;border-radius:8px;padding:6px 11px;cursor:pointer}.vq-panel button:disabled{opacity:.4;cursor:default}.vq-primary{background:#5969d8!important;color:white!important}.vq-panel textarea{width:100%;box-sizing:border-box;min-height:300px;resize:vertical;border:1px solid #8885;border-radius:8px;background:transparent;color:inherit;font:12px/1.6 monospace;padding:12px;margin:10px 0}.vq-panel input{font:inherit;color:inherit;background:transparent;border:1px solid #8885;border-radius:8px;padding:8px;box-sizing:border-box}.vq-error{color:#ec9b8f;background:#e77a1512;border:1px solid #e99b7444;padding:11px;border-radius:9px;white-space:pre-wrap;overflow-wrap:anywhere}.vq-panel summary{cursor:pointer}.vq-muted{opacity:.65;font-size:12px}.vq-panel pre{white-space:pre-wrap;overflow-wrap:anywhere;font:12px/1.6 monospace}.vq-tag{padding:2px 7px;border-radius:5px;background:#8882;font-size:11px}.vq-good{color:#68c7a7}.vq-bad{color:#ee9a91}.vq-title{font-weight:650}.vq-foot{padding:12px 24px;border-top:1px solid #8883;margin-top:auto;font-size:11px;opacity:.6}
  `;
  const labels = { queued: '排队', materializing: '加载候选', reviewing: '只读审查',
    build: '构建', test: '测试', run: '运行', metadata: '环境检查', analyzing: '分析结果',
    passed: '通过所选检查', failed: '失败', error: '执行错误', cancelled: '已取消',
    blocked: '依赖失败', interrupted: '重启中断' };
  function Panel({ sessionId, rpc }) {
    const [open, setOpen] = React.useState(false), [data, setData] = React.useState(null);
    const [draft, setDraft] = React.useState(null), [error, setError] = React.useState('');
    const [settingsOpen, setSettingsOpen] = React.useState(false);
    const [setup, setSetup] = React.useState(null), [problem, setProblem] = React.useState(null);
    const [busy, setBusy] = React.useState(false);
    const refresh = React.useCallback(async signal => {
      const value = await rpc('view', { sessionId }, signal);
      setData(value);
    }, [sessionId, rpc]);
    React.useEffect(() => { setOpen(false); setData(null); setDraft(null); setError('');
      setSetup(null); setProblem(null); setSettingsOpen(false); }, [sessionId]);
    React.useEffect(() => {
      if (!open) return;
      const controller = new AbortController();
      const poll = () => refresh(controller.signal).catch(e => { if (!controller.signal.aborted) setError(e.message); });
      poll(); const timer = setInterval(poll, 3000);
      return () => { controller.abort(); clearInterval(timer); };
    }, [open, refresh]);
    const action = async (name, extra = {}) => {
      setBusy(true); setError(''); setProblem(null);
      try {
        const value = await rpc('control', { sessionId, action: name, ...extra });
        setData(value);
        if (value.readiness?.status === 'blocked') { setProblem(value.readiness); setSettingsOpen(true); }
        if (value.setup) {
          setSetup(value.setup); setSettingsOpen(true);
          if (!value.config && draft === null) setDraft(JSON.stringify({ ...example,
            authorRoot: '', validationRoot: value.setup.currentRoot }, null, 2));
        }
        if ((name === 'save' || name === 'save-and-enable') && value.readiness?.status !== 'blocked')
          setDraft(null);
      } catch (e) { setError(e.message); }
      finally { setBusy(false); }
    };
    const config = data?.config;
    const startSetup = () => { setSettingsOpen(true); action('setup'); };
    let form;
    try { form = JSON.parse(draft ?? JSON.stringify(config ?? example)); } catch { form = null; }
    const field = (key, value) => {
      if (!form) { setError('高级配置不是有效 JSON，请先修正后再填写目录。'); return; }
      setDraft(JSON.stringify({ ...form, [key]: value }, null, 2));
    };
    const save = enable => {
      try {
        const value = JSON.parse(draft ?? JSON.stringify(config ?? example));
        if (!value.authorRoot || !value.validationRoot ||
            value.authorRoot.startsWith('/path/to/') || value.validationRoot.startsWith('/path/to/'))
          throw new Error('请选择真实的开发目录和验收目录，不能直接使用示例路径。');
        action(enable ? 'save-and-enable' : 'save', { config: value });
      } catch (e) { setError(e.message); setSettingsOpen(true); }
    };
    const active = data?.jobs?.find(x => !['passed','failed','error','cancelled','blocked','interrupted'].includes(x.status));
    return h(React.Fragment, null, h('style', null, css),
      h('button', { className: 'vq-chip', onClick: () => setOpen(true) },
        `验证队列${active ? ' · ' + labels[active.status] : ''}`),
      open && createPortal(h('div', { className: 'vq-overlay', onClick: () => setOpen(false) },
        h('section', { className: 'vq-panel', onClick: e => e.stopPropagation() },
          h('header', { className: 'vq-head' }, h('div', null,
            h('small', null, 'ASYNC VALIDATION · ONE WORKER'), h('h2', null, '验证队列')),
            h('button', { onClick: () => setOpen(false) }, '关闭')),
          h('div', { className: 'vq-body' },
            (error || data?.error) && h('p', { className: 'vq-error' }, error || data.error),
            !config && h('div', { className: 'vq-card' }, h('strong', null, '尚未完成首次设置'),
              h('p', null, '请先选择两个工作区和验证命令。插件不会自动迁移会话或清空源码。'),
              h('button', { className: 'vq-primary', disabled: busy || !data, onClick: startSetup }, '开始设置')),
            problem && h('div', { className: 'vq-error', role: 'alert' }, problem.message,
              h('details', null, h('summary', null, '具体原因'), h('pre', null, problem.detail))),
            h('div', { className: 'vq-card' }, h('div', { className: 'vq-row' },
              h('strong', null, !config ? '待设置' : config.enabled ? '已开启' : '已关闭'),
              h('button', { disabled: busy || !data, onClick: () => !config ? startSetup() :
                action('enable', { enabled: !config.enabled }) }, !config ? '设置并开启' : config.enabled ? '暂停接单与调度' : '开启'),
              h('button', { disabled: busy || !config, onClick: () => action('check') }, '环境自检')),
              h('button', { disabled: busy, onClick: () => action('probe') }, '验证宿主接口（不调用模型）'),
              data?.check && h('p', { className: 'vq-good' }, data.check),
              h('p', { className: 'vq-muted' }, '一个运行候选 + 一个等待候选。关闭开关不强杀当前任务；取消按钮会停止并回收该任务。'),
              h('p', { className: 'vq-muted' }, '开发 worktree 只编码；验收 checkout 独占原有构建树。配置和接管不会自动移动活动会话或恢复目标。')),
            h('details', { open: settingsOpen, onToggle: e => setSettingsOpen(e.currentTarget.open) },
              h('summary', null, '工作区与已授权命令配置'),
              h('button', { disabled: busy, onClick: startSetup }, '检测现有工作区（只读）'),
              h('p', null, '开发目录：主代理在这里编码，不创建编译树。'),
              h('input', { value: form?.authorRoot ?? '', placeholder: '选择用于继续编码的 worktree',
                list: 'vq-author-roots', style: { width: '100%' }, onChange: e => field('authorRoot', e.target.value) }),
              h('p', null, '验收目录：必须空闲、干净，并复用原有构建树。'),
              h('input', { value: form?.validationRoot ?? '', placeholder: '选择拥有现有构建树的 checkout',
                list: 'vq-author-roots', style: { width: '100%' }, onChange: e => field('validationRoot', e.target.value) }),
              h('datalist', { id: 'vq-author-roots' }, ...(setup?.worktrees ?? []).map(x =>
                h('option', { key: x.path, value: x.path }, x.branch))),
              setup && h('div', { className: 'vq-card' }, h('strong', null, '检测结果'),
                h('p', { className: 'vq-muted' }, setup.note),
                ...setup.worktrees.map(x => h('div', { key: x.path, style: { marginBottom: 12 } },
                  h('div', null, x.path), h('small', { className: 'vq-muted' },
                    `${x.head?.slice(0,8) ?? ''} · ${x.branch} · ${x.dirty ? '有未提交内容' : '干净'} · ` +
                    `${x.hasNormalBuild ? '有普通构建树' : '无普通构建树'} · ${(x.residents ?? []).length} 个驻留 DSH 会话`),
                  x.problem && h('p', { className: 'vq-error' }, x.problem.message)))),
              h('p', null, `验证组：${Object.keys(form?.profiles ?? {}).join('、') || '尚未配置'}。命令清单可在高级配置中修改。`),
              h('div', { className: 'vq-row' },
                h('button', { disabled: busy, onClick: () => save(false) }, '保存配置'),
                h('button', { className: 'vq-primary', disabled: busy, onClick: () => save(true) }, '保存并开启')),
              h('details', null, h('summary', null, '高级：验证命令与产物 JSON'),
              h('p', { className: 'vq-muted' }, '必须使用同仓库的两个独立 worktree。验收目录须干净且无驻留 DSH agent。保存的 file/args 清单由插件在本机执行；模型不能添加命令。示例路径须改成真实路径，artifact 须对应实际输出。'),
              h('textarea', { value: draft ?? JSON.stringify(config ?? example, null, 2),
                onChange: e => setDraft(e.target.value), spellCheck: false }))),
            h('p', { className: 'vq-muted' }, '主代理使用 validation_submit(profile, title, dependsOn?) 提交完整候选；不要轮询，结果会在步骤边界反馈。'),
            data?.handoff && h('details', { className: 'vq-card' }, h('summary', null, '现有子代理交接 · ' + data.handoff.status),
              ...data.handoff.children.map(x => h('div', { key: x.id }, h('strong', null, x.label),
                h('p', { className: 'vq-muted' }, x.id + (x.loaded === false ? ' · 已卸载，历史保留' : '')),
                h('p', null, x.remaining)))),
            h('h3', null, '验证历史'),
            !data?.jobs?.length && h('div', { className: 'vq-card vq-muted' }, '尚无验证任务。安装不会自动构建或调用模型。'),
            ...(data?.jobs ?? []).map(job => h('details', { className: 'vq-card', key: job.id },
              h('summary', null, h('span', { className: 'vq-title' }, job.title), ' ',
                h('span', { className: 'vq-tag ' + (job.status === 'passed' ? 'vq-good' : ['failed','error'].includes(job.status) ? 'vq-bad' : '') }, labels[job.status] ?? job.status)),
              h('p', { className: 'vq-muted' }, `${new Date(job.createdAt).toLocaleString()} · ${job.profile} · ${job.id}`),
              h('p', null, `候选 ${job.candidate.commit.slice(0,12)} · tree ${job.candidate.tree.slice(0,12)}`),
              job.applicability && h('p', { className: job.applicability === 'historical-only' ? 'vq-bad' : 'vq-muted' },
                job.applicability === 'historical-only' ? '源码已推进：此结果只属于历史候选，不能将当前代码标绿。' : '结果适用于相同源码树；检查范围以该 profile 为准。'),
              job.error && h('p', { className: 'vq-error' }, job.error),
              h('p', null, job.analysis?.summary ?? job.review?.summary ?? ''),
              h('details', null, h('summary', null, '审查、命令与产物'), h('pre', null,
                JSON.stringify({ review: job.review, analysis: job.analysis, commands: job.commands, artifacts: job.artifacts }, null, 2))),
              h('div', { className: 'vq-row' },
                !['passed','failed','error','cancelled','blocked','interrupted'].includes(job.status) &&
                  h('button', { disabled: busy, onClick: () => action('cancel', { id: job.id }) }, '取消并回收'),
                job.delivery === 'pending' && job.finishedAt && h('button', { disabled: busy,
                  onClick: () => action('deliver', { id: job.id }) }, '重试反馈'))))),
          h('footer', { className: 'vq-foot' }, '候选通过 ≠ 最新版本通过 ≠ 整个目标完成。'))), document.body));
  }
  return { name: 'dsh-validation-queue', inject: ['slots', 'connection'], apply(ctx) {
    const rpc = async (method, request, signal) => {
      const result = await ctx.connection.rpc.call('/api', `validationQueue/${method}`, { args: { request } }, signal);
      if (!result.ok) throw new Error(result.error?.message ?? '验证队列 RPC 失败');
      return result.value;
    };
    ctx.slots.inject('conversation.composer.dock', function* () {
      yield ctx.slots.register({ name: 'conversation.composer.dock', id: 'validation-queue',
        order: 3, inject: sessionId => ({ sessionId, rpc }) }, Panel);
    });
  } };
} });
