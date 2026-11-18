// Retain complete model-facing messages, tool invocations and task/compaction
// boundaries. Streaming frames duplicate settled messages and are not transcripts.
export function serializeConversation(events) {
  const lines = [];
  for (const event of events) {
    let data;
    if (['user/message', 'system/message', 'developer/message', 'assistant/message',
      'tool/result'].includes(event.type)) {
      data = event.data?.message ?? event.data;
    } else if (['tool/call', 'goal/change', 'todo/write', 'compaction/prune',
      'turn/start', 'turn/end'].includes(event.type)) data = event.data;
    else continue;
    lines.push(JSON.stringify({ seq: event.seq, time: event.time, type: event.type, data }));
  }
  return lines.join('\n');
}

export function conversationParts(text, maxBytes = 256 * 1024) {
  const parts = [];
  let lines = [];
  let bytes = 0;
  for (const line of text.split('\n')) {
    const size = Buffer.byteLength(line) + 1;
    if (lines.length && bytes + size > maxBytes) {
      parts.push(lines.join('\n')); lines = []; bytes = 0;
    }
    lines.push(line); bytes += size;
  }
  if (lines.length) parts.push(lines.join('\n'));
  return parts;
}
