import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

test('conversation card opens one saved report and updates independently of DSH admission', async () => {
  let plugin;
  let Card;
  let cursor;
  let effects;
  let timer;
  const state = [];
  const calls = [];
  const React = {
    createElement: (type, props, ...children) => ({ type, props, children }),
    useEffect: fn => effects.push(fn),
    useState(initial) {
      const index = cursor++;
      if (!(index in state)) state[index] = initial;
      return [state[index], value => { state[index] = value; }];
    },
  };
  vm.runInNewContext(await readFile(new URL('../lib/client.js', import.meta.url), 'utf8'), {
    window: { __ModuleLoader__: { load: m => { plugin = m.factory(name => name === 'react' ? React :
      name === 'react-dom' ? {} : { MarkdownText: 'markdown' }); } } },
    document: { hidden: false }, AbortController,
    setTimeout: callback => { timer = callback; }, clearTimeout() {},
  });
  plugin.apply({ slots: { inject(name, mount) { if (name === 'conversation.input.dock') mount(); },
    register(options, component) { Card = component; } }, sessions: {} });
  let metadata = { id: 'AUD-fixture', messageId: 'codex-audit-AUD-fixture',
    delivery: 'delivered', admitted: false, replied: false };
  const rpc = async (method, args, signal) => {
    calls.push({ method, args, signal });
    return { dismissible: true, card: args.auditId ? { ...metadata, report: 'fixture report' } : metadata };
  };
  const render = (sessionId = 'session-one') => {
    cursor = 0;
    effects = [];
    return Card({ sessionId, rpc });
  };
  const tick = () => new Promise(resolve => setImmediate(resolve));
  assert.equal(render().type, 'style');
  const cleanup = effects[0]();
  await tick();
  let tree = render();
  assert.equal(tree.props['data-codex-audit-id'], 'AUD-fixture');
  assert.match(JSON.stringify(tree), /等待 DSH 进入下一步/);
  assert.match(JSON.stringify(tree), /13:input-messagecodex-audit-AUD-/);
  assert.equal(calls.length, 1, 'collapsed card does not fetch the report body');
  tree.children[2].props.onToggle({ currentTarget: { open: true } });
  assert.equal(render().children[2].props.open, true);
  await effects[1]();
  await tick();
  assert.match(JSON.stringify(render()), /fixture report/);
  metadata = { ...metadata, admitted: true, replied: true };
  await timer();
  assert.doesNotMatch(JSON.stringify(render()), /等待 DSH 进入下一步/);
  assert.equal(render().children[2].children[0].children[2].children[2].props['data-reached'], true);
  await render().children[1].props.onClick();
  assert.ok(calls.some(call => call.args.action === 'dismiss-card' && call.args.auditId === 'AUD-fixture'));
  await timer();
  assert.equal(render().type, 'style', 'late refresh cannot bring back the dismissed card');
  metadata = { ...metadata, id: 'AUD-new', messageId: 'codex-audit-AUD-new' };
  await timer();
  assert.equal(render().props['data-codex-audit-id'], 'AUD-new');
  assert.equal(render().children[2].props.open, false);
  assert.equal(render('session-two').type, 'style');
  cleanup();
  assert.equal(calls[0].signal.aborted, true);
});
