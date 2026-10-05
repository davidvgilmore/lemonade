import type { Message } from './chatTypes';

export type ChatWireMessage = { role: 'assistant'; content: string; [key: string]: unknown };

export function appendChatDelta(message: ChatWireMessage, delta: Record<string, any>): void {
  for (const key of ['content', 'reasoning', 'reasoning_content', 'thinking', 'refusal']) {
    if (typeof delta[key] === 'string') message[key] = String(message[key] || '') + delta[key];
  }
  for (const key of ['reasoning_details', 'tool_calls']) {
    if (!Array.isArray(delta[key])) continue;
    const entries = (message[key] ||= []) as Record<string, any>[];
    for (const part of delta[key]) {
      const index = part.index ?? 0;
      const entry = (entries[index] ||= {});
      for (const [name, value] of Object.entries(part)) {
        if (name === 'index') {
          if (key === 'reasoning_details') entry.index = value;
        } else if (name === 'function' && value && typeof value === 'object') {
          const fn = (entry.function ||= {});
          for (const [field, text] of Object.entries(value)) fn[field] = (fn[field] || '') + text;
        } else if (['text', 'signature', 'data'].includes(name) && typeof value === 'string') {
          entry[name] = (entry[name] || '') + value;
        } else {
          entry[name] = value;
        }
      }
    }
  }
}

export function chatHistoryMessage(message: Message): Record<string, unknown> {
  // Provider-native state is separate from display text, which may hide thinking.
  return message.wireMessage || { role: message.role, content: message.content };
}
