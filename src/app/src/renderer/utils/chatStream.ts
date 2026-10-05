export async function consumeChatStream(
  reader: ReadableStreamDefaultReader<Uint8Array>,
  onDelta: (delta: Record<string, any>) => void,
): Promise<void> {
  const decoder = new TextDecoder();
  let buffer = '';
  let terminal = false;
  let finished = false;
  try {
    while (!terminal) {
      const { done, value } = await reader.read();
      if (done) throw new Error('Chat stream ended before its terminal marker');
      buffer += decoder.decode(value, { stream: true });
      let end: number;
      while ((end = buffer.indexOf('\n')) !== -1) {
        const line = buffer.slice(0, end).replace(/\r$/, '');
        buffer = buffer.slice(end + 1);
        if (!line.startsWith('data:')) continue;
        const data = line.slice(5).trim();
        if (data === '[DONE]') {
          if (!finished) throw new Error('Chat terminal missing finish reason');
          terminal = true;
          break;
        }
        if (!data) continue;
        const event = JSON.parse(data);
        if (event.error) throw new Error(event.error.message || 'Provider stream failed');
        if (!Array.isArray(event.choices)) throw new Error('Invalid Chat stream choices');
        if (event.choices.length === 0) continue;
        if (event.choices.length !== 1 || event.choices[0].index !== 0) throw new Error('One Chat choice required');
        const choice = event.choices[0];
        if (finished) throw new Error('Chat content after finish reason');
        if (choice.delta !== undefined) {
          if (!choice.delta || typeof choice.delta !== 'object' || Array.isArray(choice.delta)) throw new Error('Invalid Chat stream delta');
          onDelta(choice.delta);
        }
        if (choice.finish_reason != null) finished = true;
      }
    }
  } finally {
    // DONE is the protocol boundary; an upstream HTTP connection may stay open.
    try { await reader.cancel(); } catch { /* Cancellation cannot undo an accepted terminal. */ }
    reader.releaseLock();
  }
}
