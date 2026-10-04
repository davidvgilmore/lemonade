const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const appRoot = path.resolve(__dirname, '../../../src/app');
const ts = require(path.join(appRoot, 'node_modules/typescript'));
const filename = path.join(appRoot, 'src/renderer/utils/serverConfig.ts');
const source = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 },
}).outputText;

async function initialized(options = {}) {
  let ready;
  const tauriReady = new Promise(resolve => { ready = resolve; });
  const callbacks = {};
  const api = {
    getServerAPIKey: async () => 'synthetic-key',
    getServerBaseUrl: async () => options.url || null,
    getServerPort: async () => 18376,
    onServerPortUpdated: callback => { callbacks.port = callback; },
    onConnectionSettingsUpdated: callback => { callbacks.connection = callback; },
    ...options.api,
  };
  const exports = {};
  const window = { location: { origin: 'https://browser.example/' } };
  vm.runInNewContext(source, {
    exports, window, console: { log() {}, warn() {}, error() {} },
    require: name => {
      assert.equal(name, '../tauriShim');
      return { tauriReady };
    },
  }, { filename });
  const changes = [];
  exports.serverConfig.onUrlChange((url, key) => changes.push([url, key]));
  assert.equal(changes.length, 0);
  window.api = api;
  ready();
  await exports.serverConfig.waitForInit();
  return { config: exports.serverConfig, changes, callbacks };
}

module.exports.tests = [
  {
    name: 'explicit URL resolves early subscribers and retains native updates',
    async run() {
      const { config, changes, callbacks } = await initialized({ url: 'http://localhost:18376' });
      assert.deepEqual(changes, [['http://localhost:18376', 'synthetic-key']]);
      callbacks.port(19999);
      assert.equal(changes.length, 1);
      callbacks.connection('https://next.example', 'next-key');
      assert.equal(config.getServerBaseUrl(), 'https://next.example');
      assert.equal(config.getAPIKey(), 'next-key');
      assert.deepEqual(changes.at(-1), ['https://next.example', 'next-key']);
      callbacks.connection('', 'next-key');
      callbacks.port(19999);
      assert.deepEqual(changes.at(-1), ['http://localhost:19999', 'next-key']);
    },
  },
  {
    name: 'web origin emits resolved initial URL without native URL discovery',
    async run() {
      const { changes } = await initialized({ api: {
        isWebApp: true,
        getServerBaseUrl: async () => { throw new Error('must not discover'); },
      } });
      assert.deepEqual(changes, [['https://browser.example', 'synthetic-key']]);
    },
  },
  {
    name: 'local port and initialization failure both notify early subscribers',
    async run() {
      const local = await initialized();
      assert.deepEqual(local.changes, [['http://localhost:18376', 'synthetic-key']]);
      local.callbacks.port(18377);
      assert.deepEqual(local.changes.at(-1), ['http://localhost:18377', 'synthetic-key']);
      const failed = await initialized({ api: {
        getServerAPIKey: async () => { throw new Error('unavailable'); },
      } });
      assert.deepEqual(failed.changes, [['http://localhost:13305', '']]);
      assert.equal(typeof failed.callbacks.connection, 'function');
    },
  },
];
