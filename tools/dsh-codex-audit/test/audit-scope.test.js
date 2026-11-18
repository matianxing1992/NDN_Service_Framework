import test from 'node:test';
import assert from 'node:assert/strict';
import { auditScope } from '../lib/audit-scope.js';

const baseline = { basis: { cwd: '/project', seq: 10, capturedAt: 1000 },
  hashes: { 'existing-dirty.cpp': 'old', 'deleted.cpp': 'gone' } };
const period = { id: 'activation-1', baseline, checkpoint: null };
const basis = { cwd: '/project', seq: 13, capturedAt: 2000 };
const events = [{ seq: 9, time: 900 }, { seq: 10, time: 1000 },
  { seq: 11, time: 1100 }, { seq: 13, time: 1900 }, { seq: 14, time: 2001 }];

test('activation cursor excludes prior work and source hashes distinguish pre-existing changes', () => {
  const result = auditScope({ period, basis, events, trigger: 'scheduled', intervalHours: 3,
    hashes: { 'existing-dirty.cpp': 'old', 'added.cpp': 'new' } });
  assert.equal(result.scope.kind, 'since-enable');
  assert.deepEqual(result.selected.map(event => event.seq), [11, 13]);
  assert.deepEqual(result.changes.map(change => [change.path, change.kind]),
    [['deleted.cpp', 'deleted'], ['added.cpp', 'added']]);
  assert.equal(result.scope.fromSeqExclusive, 10);
});

test('subsequent review starts at the last captured cursor, not model completion time', () => {
  const result = auditScope({ period: { ...period, checkpoint: { ...baseline,
    basis: { ...baseline.basis, seq: 11, capturedAt: 1100 } } }, basis, events,
    hashes: baseline.hashes, trigger: 'manual', intervalHours: 3 });
  assert.equal(result.scope.kind, 'since-audit');
  assert.deepEqual(result.selected.map(event => event.seq), [13]);
});

test('disabled/manual no-new-work uses a labelled lookback and admits ISO event timestamps', () => {
  const opts = { basis: { ...basis, seq: 10, capturedAt: 7200000 },
    events: [{ seq: 4, time: new Date(1000).toISOString() },
      { seq: 10, time: new Date(7199999).toISOString() }], hashes: baseline.hashes,
    trigger: 'manual', intervalHours: 1 };
  for (const selectedPeriod of [null, { ...period, baseline: { ...baseline,
    basis: { ...baseline.basis, capturedAt: 7199999 } } }]) {
    const result = auditScope({ ...opts, period: selectedPeriod });
    assert.equal(result.scope.kind, 'lookback');
    assert.equal(result.scope.startAt, 3600000);
    assert.equal(result.scope.fromSeqExclusive, null);
    assert.equal(result.scope.sourceChangeCount, null, 'lookback has no historical file snapshot');
    assert.equal(result.changes, null);
    assert.deepEqual(result.selected.map(event => event.seq), [10]);
  }
});

test('blocked review is labelled partial; unrelated workspaces cannot claim comparable sources', () => {
  const result = auditScope({ period, basis: { ...basis, cwd: '/different' }, events,
    hashes: baseline.hashes, trigger: 'goal-blocked', intervalHours: 3 });
  assert.equal(result.scope.kind, 'blocked');
  assert.equal(result.scope.sourceChangeCount, null);
  assert.match(result.scope.coverage, /不推进/);
});
