import { Message } from './chatTypes';
import { chatHistoryMessage, ChatWireMessage } from './chatWireMessage';

export type ChatTurn = Record<string, any>;

export function hasCompletedChatHistory(messages: Message[]): boolean {
  return messages.some(message => message.wireHistory !== undefined
    ? message.wireHistory.length > 0
    : Boolean(message.wireMessage && !message.wireExcluded));
}

export function conversationWire(messages: Message[]): ChatTurn[] {
  if (messages.some(message => message.wireBlocked)) throw new Error('Start a new chat after an invalid tool response');
  return messages.flatMap(message => message.wireHistory ??
    (message.wireExcluded ? [] : [chatHistoryMessage(message)]));
}

export async function runChatToolLoop(
  history: ChatTurn[],
  names: string[],
  send: (history: ChatTurn[]) => Promise<ChatWireMessage>,
  execute: (call: any) => Promise<string>,
  publish: (turns: ChatTurn[]) => void,
  signal?: AbortSignal,
): Promise<void> {
  const turns: ChatTurn[] = [];
  let executedTools = 0;
  for (let step = 0; step < 5; step++) {
    signal?.throwIfAborted();
    const assistant = await send([...history, ...turns]);
    turns.push(assistant);
    publish(structuredClone(turns));
    const calls = assistant.tool_calls;
    if (!Array.isArray(calls) || calls.length === 0) return;
    const ids = new Set<string>();
    for (const call of calls) {
      if (!call || typeof call.id !== 'string' || !call.id || ids.has(call.id)) {
        const error = new Error('Invalid or duplicate tool identity; start a new chat');
        error.name = 'ChatHistoryError';
        throw error;
      }
      ids.add(call.id);
    }

    let stopped: unknown;
    for (const call of calls) {
      let content: string;
      try {
        if (stopped) throw stopped;
        signal?.throwIfAborted();
        if (!call || typeof call.id !== 'string' || !call.id ||
            call.type !== 'function' || !names.includes(call.function?.name)) {
          throw new Error('Unknown or malformed tool call');
        }
        const args = JSON.parse(call.function.arguments);
        if (!args || typeof args !== 'object' || Array.isArray(args)) throw new Error('Invalid tool arguments');
        if (executedTools >= 5) throw new Error('Tool execution limit reached');
        executedTools++;
        content = await execute(call);
      } catch (error) {
        stopped = error;
        content = `Tool did not complete: ${error instanceof Error ? error.message : String(error)}`;
      }
      turns.push({ role: 'tool', tool_call_id: call?.id, content });
      publish(structuredClone(turns));
    }
    if (stopped) throw stopped;
  }
  throw new Error('Tool continuation limit reached');
}
