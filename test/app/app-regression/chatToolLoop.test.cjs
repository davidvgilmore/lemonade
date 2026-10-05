const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('../../../src/app/node_modules/typescript');
const root = path.resolve(__dirname, '../../..');
function load(name, mocks = {}) {
  if (name in mocks) return mocks[name];
  const filename = path.join(root, 'src/app/src/renderer/utils', name + '.ts');
  const code = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
  }).outputText;
  const module = { exports: {} };
  new Function('require', 'module', 'exports', code)(
    dep => dep.startsWith('.') ? load(dep.slice(2), mocks) : require(dep), module, module.exports);
  return module.exports;
}
const { runChatToolLoop, conversationWire, hasCompletedChatHistory } = load('chatToolLoop');
const { consumeChatStream } = load('chatStream');
const { appendChatDelta } = load('chatWireMessage');
const tool = (id = 'one', name = 'lemonade_list_models', args = '{}') => ({
  role: 'assistant', content: '', reasoning_content: 'Visible reasoning',
  tool_calls: [{ id, type: 'function', function: { name, arguments: args } }],
});
const tests = [
  { name: 'router edit guard distinguishes completed history from discarded partial output', run() {
    assert.equal(hasCompletedChatHistory([{ role: 'assistant', content: '', wireHistory: [], wireMessage: tool(), wireExcluded: true }]), false);
    assert.equal(hasCompletedChatHistory([{ role: 'assistant', content: 'Error', wireHistory: [tool()], wireExcluded: true }]), true);
    assert.equal(hasCompletedChatHistory([{ role: 'assistant', content: 'Old', wireMessage: tool() }]), true);
  } },
  { name: 'fragmented tool-only stream ends at DONE without waiting for EOF', async run() {
    const assistant = tool(); let cancelled = false; const actual = { role: 'assistant', content: '' };
    const frames = [
      { choices: [{ index: 0, delta: { role: 'assistant', tool_calls: assistant.tool_calls.map(x => ({ ...x, index: 0 })) }, finish_reason: null }] },
      { choices: [{ index: 0, delta: {}, finish_reason: 'tool_calls' }] },
    ].map(x => 'data: ' + JSON.stringify(x) + '\r\n\r\n').join('') + 'data: [DONE]\n\n';
    const reader = new ReadableStream({ start(controller) {
      const bytes = new TextEncoder().encode(frames); for (const byte of bytes) controller.enqueue(Uint8Array.of(byte));
    }, cancel() { cancelled = true; } }).getReader();
    await consumeChatStream(reader, delta => appendChatDelta(actual, delta));
    assert.equal(cancelled, true); assert.deepEqual(actual.tool_calls, assistant.tool_calls);
  } },
  { name: 'malformed or error frames before DONE refuse without tool execution', async run() {
    for (const raw of ['data: {bad}\n\ndata: [DONE]\n\n',
      'data: {"error":{"message":"failed"}}\n\ndata: [DONE]\n\n', 'data: [DONE]\n\n']) {
      let executed = 0; let published = 0;
      const reader = new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode(raw)); c.close(); } }).getReader();
      await assert.rejects(runChatToolLoop([], ['lemonade_list_models'], async () => {
        const value = { role: 'assistant', content: '' };
        await consumeChatStream(reader, delta => appendChatDelta(value, delta)); return value;
      }, async () => { executed++; return ''; }, () => { published++; }));
      assert.equal(executed, 0); assert.equal(published, 0);
    }
  } },
  { name: 'total tool execution cap preserves a result for every unexecuted call', async run() {
    const assistant = tool(); assistant.tool_calls = Array.from({ length: 7 }, (_, i) => tool(String(i)).tool_calls[0]);
    let count = 0; let saved;
    await assert.rejects(runChatToolLoop([], ['lemonade_list_models'], async () => assistant,
      async () => { count++; return 'ok'; }, turns => { saved = turns; }), /execution limit/);
    assert.equal(count, 5); assert.equal(saved.length, 8);
    assert.match(saved[6].content, /limit/); assert.match(saved[7].content, /limit/);
  } },
  { name: 'ordinary model-list tool uses authenticated server fetch without model execution', async run() {
    const requests = [];
    const { executeLemonadeTool, serverChatTools } = load('lemonadeTools', {
      serverConfig: { serverFetch: async (...args) => { requests.push(args); return { ok: true, json: async () => ({ data: [{ id: 'registered-model' }] }) }; } },
      modelLabels: {}, collectionModels: {}, collectionImageConfig: {}, 'toolDefinitions.json': {},
    });
    assert.equal(serverChatTools[0].function.name, 'lemonade_list_models');
    const result = await executeLemonadeTool(tool().tool_calls[0], '', {});
    assert.deepEqual(JSON.parse(result.text), { models: [{ id: 'registered-model' }] });
    assert.equal(requests.length, 1); assert.equal(requests[0][0], '/models');
    await assert.rejects(executeLemonadeTool(tool('x', 'lemonade_list_models', '{"download":true}').tool_calls[0], '', {}));
    assert.equal(requests.length, 1);
  } },
  { name: 'ordinary tool continuation preserves exact assistant and tool history', async run() {
    const user = { role: 'user', content: 'List models' }; const first = tool(); let saved; let calls = 0;
    await runChatToolLoop([user], ['lemonade_list_models'], async history => {
      if (calls++ === 0) return first;
      assert.deepEqual(history, [user, first, { role: 'tool', tool_call_id: 'one', content: 'model list' }]);
      return { role: 'assistant', content: 'Done' };
    }, async () => 'model list', turns => { saved = turns; });
    assert.deepEqual(conversationWire([user, { role: 'assistant', content: 'Display only', wireHistory: saved }]), [user, ...saved]);
  } },
  { name: 'partial failed generation never replaces committed tool transcript', async run() {
    let saved; let n = 0;
    await assert.rejects(runChatToolLoop([], ['lemonade_list_models'], async () => {
      if (n++ === 0) return tool(); throw new Error('stream incomplete');
    }, async () => 'done', turns => { saved = turns; }), /incomplete/);
    assert.equal(saved.length, 2);
    assert.deepEqual(conversationWire([{ role: 'assistant', content: 'partial', wireExcluded: true, wireHistory: saved }]), saved);
    assert.deepEqual(conversationWire([{ role: 'assistant', content: 'partial', wireExcluded: true }]), []);
  } },
  { name: 'unknown and malformed tools refuse without execution and retain error results', async run() {
    for (const first of [tool('x', 'unknown'), tool('x', 'lemonade_list_models', '[]')]) {
      let executions = 0; let saved;
      await assert.rejects(runChatToolLoop([], ['lemonade_list_models'], async () => first,
        async () => { executions++; return ''; }, turns => { saved = turns; }));
      assert.equal(executions, 0); assert.equal(saved[1].role, 'tool');
    }
  } },
  { name: 'abort preserves completed assistant and marks unexecuted tools without retry', async run() {
    const controller = new AbortController(); let saved; let calls = 0;
    await assert.rejects(runChatToolLoop([], ['lemonade_list_models'], async () => {
      controller.abort(); return tool();
    }, async () => { calls++; return ''; }, turns => { saved = turns; }, controller.signal));
    assert.equal(calls, 0); assert.equal(saved.length, 2); assert.match(saved[1].content, /did not complete/);
  } },
  { name: 'tool loop is bounded and invalid identities block future history', async run() {
    let calls = 0;
    await assert.rejects(runChatToolLoop([], ['lemonade_list_models'], async () => tool(String(calls++)), async () => 'ok', () => {}), /limit/);
    assert.equal(calls, 5);
    const bad = tool(); bad.tool_calls.push(bad.tool_calls[0]);
    await assert.rejects(runChatToolLoop([], ['lemonade_list_models'], async () => bad, async () => '', () => {}), { name: 'ChatHistoryError' });
    assert.throws(() => conversationWire([{ role: 'assistant', content: '', wireBlocked: true }]), /new chat/);
  } },
];
module.exports = { tests };
