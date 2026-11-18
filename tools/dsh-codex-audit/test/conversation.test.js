import test from 'node:test';
import assert from 'node:assert/strict';
import { serializeConversation, conversationParts } from '../lib/conversation.js';

test('complete transcript retains direct user payload and tool calls without repeated streams', () => {
  const events = [
    { seq: 1, time: 1, type: 'user/message', data: { id: 'u', content: 'question' } },
    { seq: 2, time: 2, type: 'assistant/message', data: { message: { id: 'a', content: 'answer' },
      stream: ['redundant-stream'] } },
    { seq: 3, time: 3, type: 'tool/call', data: { name: 'bash', arguments: 'literal command' } },
  ];
  const text = serializeConversation(events);
  assert.ok(text.includes('question') && text.includes('answer') && text.includes('literal command'));
  assert.ok(!text.includes('redundant-stream'));
  assert.equal(conversationParts(text, 100).join('\n'), text);
});
