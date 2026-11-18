import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { readCodexSelection } from '../lib/model-observation.js';

const id = '12345678-1234-1234-1234-123456789abc';
const startedAt = Date.UTC(2026, 10, 15, 12);
async function fixture(rows, check) {
  const codexHome = await mkdtemp('/tmp/dsh-model-observation-test-');
  try {
    const directory = join(codexHome, 'sessions', '2026', '11', '15');
    await mkdir(directory, { recursive: true });
    await writeFile(join(directory, `rollout-test-${id}.jsonl`), rows.map(r => JSON.stringify(r)).join('\n'));
    await writeFile(join(codexHome, 'config.toml'), 'model = "not-the-runtime-model"');
    await check({ threadId: id, startedAt, codexHome });
  } finally { await rm(codexHome, { recursive: true, force: true }); }
}

test('confirms actual matching-thread metadata independently from requested config', async () => {
  await fixture([
    { type: 'session_meta', payload: { id } },
    { type: 'response_item', payload: { privateText: 'never exported' } },
    { type: 'turn_context', payload: { model: 'gpt-6.2-sol', effort: 'xhigh' } },
  ], async args => {
    const result = await readCodexSelection(args);
    assert.equal(result.status, 'confirmed');
    assert.equal(result.model, 'gpt-6.2-sol');
    assert.equal(result.effort, 'xhigh');
    assert.equal(result.source, 'codex-turn-context');
    assert.ok(!JSON.stringify(result).includes('never exported'));
  });
});

test('wrong thread identity never confirms a similarly named log', async () => {
  await fixture([
    { type: 'session_meta', payload: { id: 'other-thread' } },
    { type: 'turn_context', payload: { model: 'gpt-6.2-sol', effort: 'xhigh' } },
  ], async args => assert.equal((await readCodexSelection(args)).status, 'unavailable'));
});

test('detects multiple runtime selections instead of silently picking one', async () => {
  await fixture([
    { type: 'session_meta', payload: { id } },
    { type: 'turn_context', payload: { model: 'gpt-6.2-sol', effort: 'xhigh' } },
    { type: 'turn_context', payload: { model: 'different', effort: 'low' } },
  ], async args => assert.equal((await readCodexSelection(args)).status, 'mixed'));
});

test('does not infer missing effort from a model name or local configuration', async () => {
  await fixture([
    { type: 'session_meta', payload: { id } },
    { type: 'turn_context', payload: { model: 'gpt-6.2-sol' } },
  ], async args => assert.equal((await readCodexSelection(args)).status, 'unavailable'));
});

test('rejects path-like thread ids without reading arbitrary files', async () => {
  assert.equal((await readCodexSelection({ threadId: '../../auth.json', startedAt })).status, 'unavailable');
});

test('later thread resumes do not change the recorded selection of a finished audit', async () => {
  await fixture([
    { type: 'session_meta', payload: { id } },
    { type: 'turn_context', timestamp: '2026-11-15T12:00:00Z',
      payload: { model: 'gpt-6.2-sol', effort: 'xhigh' } },
    { type: 'turn_context', timestamp: '2026-11-16T12:00:00Z',
      payload: { model: 'later-model', effort: 'low' } },
  ], async args => {
    const result = await readCodexSelection({ ...args, finishedAt: Date.UTC(2026, 10, 15, 13) });
    assert.equal(result.status, 'confirmed');
    assert.equal(result.model, 'gpt-6.2-sol');
  });
});
