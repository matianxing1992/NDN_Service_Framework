import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, readFileSync, rmSync, symlinkSync } from 'node:fs';
import { join } from 'node:path';
import { reviewDocumentChanges, applyDocumentChange } from '../lib/document-changes.js';
import { digest, safeRelative } from '../lib/snapshot.js';

test('only admitted documents with exact snapshot identity can be revised', () => {
  const root = mkdtempSync('/tmp/dsh-audit-doc-test-');
  try {
    writeFileSync(join(root, 'README.md'), 'old text');
    const snapshot = { path: root, originalCwd: root, hashes: { 'README.md': digest('old text') } };
    const input = { path: 'README.md', content: 'new text', baseHash: digest('old text'),
      reason: 'correct factual statement', changesContract: false };
    for (const path of ['../README.md', '/etc/passwd', 'code.cpp']) {
      assert.throws(() => reviewDocumentChanges([{ ...input, path }], snapshot), /contract/);
    }
    const [change] = reviewDocumentChanges([input], snapshot);
    assert.equal(change.status, 'pending');
    applyDocumentChange(change);
    assert.equal(readFileSync(join(root, 'README.md'), 'utf8'), 'new text');
    assert.equal(change.status, 'applied');
  } finally { rmSync(root, { recursive: true }); }
});

test('concurrent edits become conflicts and are preserved', () => {
  const root = mkdtempSync('/tmp/dsh-audit-doc-test-');
  try {
    writeFileSync(join(root, 'README.md'), 'new DSH edit');
    const change = { originalCwd: root, path: 'README.md', status: 'pending',
      baseHash: digest('old'), content: 'audit revision' };
    applyDocumentChange(change);
    assert.equal(change.status, 'conflict');
    assert.equal(readFileSync(join(root, 'README.md'), 'utf8'), 'new DSH edit');
  } finally { rmSync(root, { recursive: true }); }
});

test('contract changes stay proposals and symlinks cannot write outside workspace', () => {
  const root = mkdtempSync('/tmp/dsh-audit-doc-test-');
  try {
    writeFileSync(join(root, 'spec.md'), 'contract');
    const snapshot = { path: root, originalCwd: root, hashes: { 'spec.md': digest('contract') } };
    const [change] = reviewDocumentChanges([{ path: 'spec.md', baseHash: digest('contract'),
      content: 'relaxed gate', reason: 'proposal', changesContract: false }], snapshot);
    assert.equal(change.status, 'proposal-only');
    assert.throws(() => applyDocumentChange(change), /pending/);
    symlinkSync('/etc/hosts', join(root, 'escape.md'));
    assert.throws(() => applyDocumentChange({ originalCwd: root, path: 'escape.md',
      status: 'pending', baseHash: 'irrelevant' }), /escaped/);
  } finally { rmSync(root, { recursive: true }); }
  assert.equal(safeRelative('a/../b'), false);
  assert.equal(safeRelative('C:\\secret.md'), false);
});
