/** Preserve the server's actionable runtime error when a chat request is refused. */
export async function chatResponseError(response: Response): Promise<Error> {
  const fallback = `HTTP error! status: ${response.status}`;
  try {
    const data = await response.json();
    const detail = typeof data?.error === 'string' ? data.error : data?.error?.message;
    return new Error(typeof detail === 'string' && detail.trim() ? `${detail} (HTTP ${response.status})` : fallback);
  } catch {
    return new Error(fallback);
  }
}
