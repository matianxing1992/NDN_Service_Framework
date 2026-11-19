import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

test('立即审计 opens a real prompt dialog, prefilled and clearable, and sends only that run', async () => {
  const data = { settings: { enabled: true, intervalHours: 3, prompt: '默认提示词',
    model: 'gpt-6.2-sol', effort: 'xhigh' }, efforts: ['medium', 'high', 'xhigh', 'ultra'],
    models: [{ id: 'gpt-6.2-sol' }, { id: 'gpt-6.2-astra' }],
    records: [], total: 0, running: false, guard: null };
  let Panel, plugin;
  const states = [];
  let cursor = 0;
  const calls = [];
  const h = (type, props, ...children) => ({ type, props: props ?? {}, children });
  let rerender = () => {};
  const React = {
    createElement: h, Fragment: 'fragment', useEffect() {},
    useCallback: fn => fn, useRef: initial => ({ current: initial }),
    useState(initial) {
      const slot = cursor++;
      if (!(slot in states)) states[slot] = typeof initial === 'function' ? initial() : initial;
      const set = value => {
        states[slot] = typeof value === 'function' ? value(states[slot]) : value;
        rerender();
      };
      return [states[slot], set];
    },
  };
  const require = name => name === 'react' ? React : name === 'react-dom'
    // React's createPortal puts the child under the given container, NOT where the expression sits.
    // Returning the child directly (as this harness first did) made an inline card and a real
    // overlay indistinguishable, which is why a wrong layout passed the 0.1.15 test.
    ? { createPortal: (child, container) =>
        ({ type: '__portal__', portalTo: container, props: {}, children: [child] }) }
    : { MarkdownText: 'markdown' };
  vm.runInNewContext(await readFile(new URL('../lib/client.js', import.meta.url), 'utf8'), {
    window: { __ModuleLoader__: { load: m => { plugin = m.factory(require); } } },
    document: { body: {} }, Date, Set, Map, Object, Number, String, Math,
  });
  plugin.apply({ slots: { inject(name, factory) { [...factory()]; },
    register(options, component) { Panel = component; } },
    connection: { rpc: { async call() { return { ok: true, value: data }; } } }, sessions: {} });

  const props = { sessionId: 's', rpc: async (method, request) => {
    calls.push({ method, request });
    if (request.action === 'configure' && request.effort) data.settings.effort = request.effort;
    if (request.model) {
      data.settings.model = request.model;
      data.settings.effort = 'high';
      data.efforts = ['medium', 'high'];
    }
    return data;
  } };
  const nodes = tree => !tree || typeof tree !== 'object' ? []
    : Array.isArray(tree) ? tree.flatMap(nodes) : [tree, ...tree.children.flatMap(nodes)];
  const label = n => n.children.filter(c => typeof c === 'string').join('');
  // The panel itself is also a role=dialog, so the prompt dialog is identified by its own label.
  const promptDialog = tree => nodes(tree)
    .find(n => n.props?.['aria-label'] === '本次审计提示词');

  // Open the panel: slot 0 is openSession, slot 1 is the view payload the poll fills in.
  states[0] = 's';
  states[1] = data;

  let tree;
  const render = () => { cursor = 0; tree = Panel(props); };
  render();
  assert.ok(nodes(tree).some(n => n.type === 'button' && label(n) === '立即审计'),
    '立即审计 is offered');
  assert.equal(Boolean(promptDialog(tree)), false, 'no prompt dialog before clicking');

  rerender = render;
  const effort = nodes(tree).find(n => n.type === 'select' && n.props.id === 'ca-effort');
  assert.equal(effort.props.value, 'xhigh');
  assert.deepEqual(nodes(effort).filter(n => n.type === 'option').map(n => [n.props.value, label(n)]),
    [['medium', '中等'], ['high', '高'], ['xhigh', '极高（Extra high）'], ['ultra', 'Ultra']]);
  await effort.props.onChange({ target: { value: 'ultra' } });
  assert.equal(nodes(tree).find(n => n.type === 'select' && n.props.id === 'ca-effort').props.value, 'ultra');
  assert.equal(calls[0].request.effort, 'ultra');
  nodes(tree).find(n => n.props?.id === 'ca-model').props.onChange({ target: { value: 'gpt-6.2-astra' } });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(calls.find(call => call.request.model)?.request.model, 'gpt-6.2-astra');
  assert.equal(nodes(tree).find(n => n.props?.id === 'ca-effort').props.value, 'high');
  const runButton = nodes(tree).find(n => n.type === 'button' && label(n) === '立即审计');
  runButton.props.onClick();
  const dialog = promptDialog(tree);
  assert.ok(dialog, 'clicking 立即审计 opens the prompt dialog');
  // The failure this guards: the dialog rendered inside the scrolling body, below the guard card,
  // so the click looked like it did nothing. It must not be a descendant of ca-body at all.
  const body = nodes(tree).find(n => n.props?.className === 'ca-body');
  assert.ok(body, 'the panel body exists');
  assert.equal(nodes(body).some(n => n.props?.['aria-label'] === '本次审计提示词'), false,
    'the prompt dialog is not inside the scrolling panel body');
  const portals = (tree.children ?? []).filter(n => n && n.type === '__portal__');
  assert.ok(portals.some(p => nodes(p).some(n => n.props?.['aria-label'] === '本次审计提示词')),
    'the prompt dialog is its own portal at the top of the component');
  const area = nodes(dialog).find(n => n.type === 'textarea');
  assert.equal(area?.props.value, '默认提示词', 'the box starts from the configured default');
  const labels = nodes(dialog).filter(n => n.type === 'button').map(label);
  assert.deepEqual(labels, ['清除', '恢复默认', '取消', '开始审计']);
  area.props.onChange({ target: { value: '只看内存' } });
  const start = nodes(tree).find(n => n.type === 'button' && label(n) === '开始审计');
  start.props.onClick();
  const sent = calls.filter(c => c.method === 'control' && c.request.action === 'run').map(c => c.request);
  // Compared as JSON with sorted keys: the object is assembled from a spread, so key order is an
  // accident of construction and asserting it would be asserting nothing about behaviour.
  assert.equal(JSON.stringify(sent.map(entry => Object.fromEntries(
    Object.entries(entry).sort(([a], [b]) => a.localeCompare(b))))),
  JSON.stringify([{ action: 'run', prompt: '只看内存', sessionId: 's' }]),
  'only this run is told about the typed prompt');
  assert.equal(Boolean(promptDialog(tree)), false, 'the dialog closes after starting');
});
