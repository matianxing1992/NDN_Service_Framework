// Scope is an evidence boundary, not a second execution scheduler.
const HOUR = 3600000;

export function sourceChanges(baseline, hashes, cwd) {
  if (!baseline || baseline.basis.cwd !== cwd) return null;
  const changes = [];
  for (const path of new Set([...Object.keys(baseline.hashes), ...Object.keys(hashes)])) {
    const beforeHash = baseline.hashes[path] ?? null;
    const afterHash = hashes[path] ?? null;
    if (beforeHash !== afterHash) changes.push({ path, beforeHash, afterHash,
      kind: beforeHash === null ? 'added' : afterHash === null ? 'deleted' : 'modified' });
  }
  return changes;
}

export function auditScope({ period, trigger, events, basis, hashes, intervalHours }) {
  const baseline = period?.checkpoint ?? period?.baseline;
  const changes = sourceChanges(baseline, hashes, basis.cwd);
  const fromSeq = baseline?.basis.seq;
  const hasCursor = Number.isSafeInteger(fromSeq) && Number.isSafeInteger(basis.seq) &&
    fromSeq <= basis.seq && baseline.basis.cwd === basis.cwd;
  const pending = hasCursor && events.some(event => event.seq > fromSeq && event.seq <= basis.seq);
  const fallback = !hasCursor || (trigger === 'manual' && !pending && changes?.length === 0);
  const kind = trigger === 'goal-blocked' ? 'blocked' : fallback ? 'lookback' :
    period.checkpoint ? 'since-audit' : 'since-enable';
  const startAt = fallback ? basis.capturedAt - intervalHours * HOUR : baseline.basis.capturedAt;
  // A blocked review may trace earlier causality; its window is never full coverage.
  const selected = events.filter(event => event.seq <= basis.seq && (fallback ?
    (typeof event.time === 'number' ? event.time : Date.parse(event.time)) >= startAt : event.seq > fromSeq));
  return { scope: { version: 1, periodId: period?.id ?? null, kind, trigger,
    startAt, endAt: basis.capturedAt, fromSeqExclusive: fallback ? null : fromSeq,
    toSeqInclusive: basis.seq, focusedEvents: selected.length,
    sourceBaseline: baseline ? { capturedAt: baseline.basis.capturedAt,
      seq: baseline.basis.seq, head: baseline.basis.head,
      manifestHash: baseline.basis.manifestHash } : null,
    sourceChangeCount: fallback ? null : changes?.length ?? null,
    sourceChangedPaths: fallback ? [] : changes?.slice(0, 100).map(change => change.path) ?? [],
    preexistingChangeCount: baseline?.basis.workingTreeChanges?.length ?? null,
    sourceLimitation: fallback ? '回看起点没有文件快照，不能据此确定期间代码差异。' :
      changes === null ? '工作区或基线不可比，不能确定期间代码差异。' :
        '文件摘要只标识变化路径；未提供旧文件正文，不能把整个文件视为本期新增。',
    background: '窗口外历史仅作按需追溯；不声明已全面审计。',
    coverage: trigger === 'goal-blocked' ? '阻塞专项，不推进周期覆盖点。' :
      fallback ? '最近一个配置周期回看，不填补关闭期间覆盖。' :
        '本次开启内的重点审计窗口；关闭期间不自动补审。' },
    selected, changes: fallback ? null : changes };
}

export function describeScope(scope) {
  const label = { blocked: '当前阻塞专项', lookback: '最近一个配置周期回看',
    'since-audit': '上次成功快照之后', 'since-enable': '本次开启之后' }[scope.kind];
  return `重点范围：${label}，${new Date(scope.startAt).toISOString()} 至 ` +
    `${new Date(scope.endAt).toISOString()}；消息游标 (${scope.fromSeqExclusive ?? '按时间回看'}, ` +
    `${scope.toSeqInclusive}]。${scope.coverage}${scope.background}${scope.sourceLimitation}`;
}
