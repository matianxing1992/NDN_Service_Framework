import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

test('guard panel stays collapsed by default and exposes blocked progress and exact-goal controls', async () => {
  const data = { settings: { enabled: true, intervalHours: 3,
    auditWindow: { enabledAt: 100, lastCoveredAt: 150 } }, records: [{
    id: 'a', trigger: 'manual', status: 'completed', delivery: 'delivered', replies: [],
    startedAt: 100, finishedAt: 200, attempts: 1, model: 'gpt-6.2-sol', effort: 'xhigh',
    basis: { scope: { kind: 'since-enable', startAt: 100, endAt: 200,
      focusedEvents: 3, sourceChangeCount: 1, coverage: '关闭期间不自动补审。' } },
  }], total: 1,
    guard: { enabled: true, status: 'blocked', message: '目标阻塞', history: [],
      goal: { id: 'g', revision: 7, phase: 'blocked', activation: 'disarmed',
        roundsStarted: 19, maxGoalRounds: 256 },
      blocked: { status: 'waiting', summary: 'Requires a private credential', steps: [] } } };
  let open = false;
  let stateIndex = 0;
  let Panel;
  let plugin;
  const calls = [];
  const h = (type, props, ...children) => ({ type, props: props ?? {}, children });
  const React = { createElement: h, Fragment: 'fragment', useEffect() {},
    useCallback: fn => fn, useRef: initial => ({ current: initial }),
    useState(initial) {
      const slot = stateIndex++;
      return [slot === 0 ? (open ? 's' : null) : slot === 1 ? data : initial, () => {}];
    } };
  const require = name => name === 'react' ? React : name === 'react-dom' ?
    { createPortal: child => child } : { MarkdownText: 'markdown' };
  vm.runInNewContext(await readFile(new URL('../lib/client.js', import.meta.url), 'utf8'), {
    window: { __ModuleLoader__: { load: module => { plugin = module.factory(require); } } },
    document: { body: {} }, Date, Set, Map, Object, Number, String, Math,
  });
  plugin.apply({ slots: { inject(name, factory) {
    if (name === 'conversation.composer.dock') [...factory()];
  },
    register(options, component) { Panel = component; } },
    connection: { rpc: { async call(channel, method, payload) {
      calls.push({ method, request: payload.args.request }); return { ok: true, value: data };
    } } }, sessions: {} });
  const nodes = tree => {
    if (!tree || typeof tree !== 'object') return [];
    if (Array.isArray(tree)) return tree.flatMap(nodes);
    return [tree, ...tree.children.flatMap(nodes)];
  };
  const props = { sessionId: 's', rpc: async (method, request) => {
    calls.push({ method, request }); return data;
  } };
  const closed = nodes(Panel(props));
  assert.equal(closed.some(n => n.props.role === 'dialog'), false);
  open = true; stateIndex = 0;
  const rendered = nodes(Panel(props));
  assert.ok(rendered.some(n => n.children.includes('阻塞审计与续跑守护')));
  assert.ok(rendered.some(n => n.children.includes('手动审计 · gpt-6.2-sol · xhigh')));
  assert.ok(rendered.some(n => n.children.some(child => typeof child === 'string' &&
    child.includes('立即审计会从本次开始时间重新计算自动审计周期'))));
  const retry = rendered.find(n => n.type === 'button' && n.children.includes('重新审计阻塞'));
  assert.ok(rendered.some(n => n.children.some(child => typeof child === 'string' &&
    child.includes('重点范围：本次开启后'))));
  assert.ok(rendered.some(n => n.children.some(child => typeof child === 'string' &&
    child.includes('本次开启：'))));
  assert.equal(retry.props.disabled, false);
  await retry.props.onClick();
  assert.deepEqual(JSON.parse(JSON.stringify(calls[0])), { method: 'control', request: {
    sessionId: 's', action: 'review-blocked', goalRef: { id: 'g', revision: 7 },
  } });
  const resume = rendered.find(n => n.type === 'button' && n.children.includes('恢复当前目标'));
  assert.equal(resume.props.disabled, true, 'blocked goals require their audit route');
});

test('a manual audit prompts for this run only, and clearing it is not an empty instruction', async () => {
  const data = { settings: { enabled: true, intervalHours: 3, prompt: '默认提示词' },
    records: [], total: 0, guard: null };
  let Panel;
  let plugin;
  const calls = [];
  const h = (type, props, ...children) => ({ type, props: props ?? {}, children });
  // Slot 9 is `promptOpen`; starting it true renders the dialog without simulating a click.
  const React = { createElement: h, Fragment: 'fragment', useEffect() {},
    useCallback: fn => fn, useRef: initial => ({ current: initial }),
    useState(initial) {
      // Slot 0 is `openSession`: returning this sessionId renders the open panel. Slots 1 and 7 are
      // `data` and `details`; 8 and 9 are the draft prompt and the dialog switch.
      const slot = stateIndex++;
      if (slot === 0) return ['s', () => {}];
      if (slot === 1) return [data, () => {}];
      if (slot === 8) return ['改过的提示词', () => {}];
      if (slot === 9) return [true, () => {}];
      return [initial, () => {}];
    } };
  let stateIndex = 0;
  const require = name => name === 'react' ? React : name === 'react-dom' ?
    { createPortal: child => child } : { MarkdownText: 'markdown' };
  vm.runInNewContext(await readFile(new URL('../lib/client.js', import.meta.url), 'utf8'), {
    window: { __ModuleLoader__: { load: module => { plugin = module.factory(require); } } },
    document: { body: {} }, Date, Set, Map, Object, Number, String, Math,
  });
  plugin.apply({ slots: { inject(name, factory) {
    if (name === 'conversation.composer.dock') [...factory()];
  },
    register(options, component) { Panel = component; } },
    connection: { rpc: { async call() { return { ok: true, value: data }; } } }, sessions: {} });
  const nodes = tree => {
    if (!tree || typeof tree !== 'object') return [];
    if (Array.isArray(tree)) return tree.flatMap(nodes);
    return [tree, ...tree.children.flatMap(nodes)];
  };
  const props = { sessionId: 's', rpc: async (method, request) => {
    calls.push({ method, request }); return data;
  } };
  const rendered = nodes(Panel(props));
  // The panel itself is a role="dialog" too, so the prompt dialog is selected by its own label;
  // matching on the role alone picked the panel and then looked for a textarea inside it.
  const dialog = rendered.find(n => n.props['aria-label'] === '本次审计提示词');
  assert.ok(dialog, 'the prompt dialog is shown');
  const area = nodes(dialog).find(n => n.type === 'textarea');
  assert.equal(area.props.value, '改过的提示词', 'the box starts from the default and is editable');
  const labels = nodes(dialog).filter(n => n.type === 'button')
    .map(n => n.children.filter(c => typeof c === 'string').join(''));
  assert.ok(labels.includes('清除'), 'there is a clear button');
  assert.ok(labels.includes('开始审计'), 'there is a start button');
});
