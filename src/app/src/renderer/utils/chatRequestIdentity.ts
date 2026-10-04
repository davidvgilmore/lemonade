function browserId(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(16));
  return Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');
}

export function createChatRequestIdentity(newId: () => string = browserId) {
  const sessionId = newId();
  return () => ({
    'X-Client-Session-Id': sessionId,
    'X-Lemonade-Request-Id': newId(),
  });
}
