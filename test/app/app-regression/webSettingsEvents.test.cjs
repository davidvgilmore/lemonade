const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function webApi() {
  const source = fs.readFileSync(path.join(__dirname, '../../../src/cpp/server/server.cpp'), 'utf8');
  const match = source.match(/std::string mock_api = R"\(\s*<script>([\s\S]*?)<\/script>\s*\)";/);
  assert.ok(match, 'actual server-injected web API script');
  const stored = new Map();
  const storage = {
    getItem: (key) => stored.get(key) ?? null,
    setItem: (key, value) => stored.set(key, value),
  };
  const window = new EventTarget();
  window.location = { origin: 'http://127.0.0.1:1234', port: '1234' };
  vm.runInNewContext(match[1], {
    window, localStorage: storage, navigator: { platform: 'test' },
    CustomEvent: class extends Event {
      constructor(type, options) { super(type); this.detail = options.detail; }
    },
  });
  return { api: window.api, storage };
}

exports.tests = [
  {
    name: 'saving web settings updates an existing subscriber after persistence',
    async run() {
      const { api } = webApi();
      const settings = { maxOutputTokens: { value: 512, useDefault: false }, theme: 'dark' };
      let observed;
      let persisted;
      api.onSettingsUpdated((value) => { observed = value; persisted = api.getSettings(); });
      assert.equal(await api.saveSettings(settings), settings);
      assert.equal(observed, settings);
      assert.deepEqual(JSON.parse(JSON.stringify(await persisted)), settings);
      assert.deepEqual(JSON.parse(JSON.stringify(await api.getSettings())), settings);
    },
  },
  {
    name: 'unsubscribing removes only that mounted settings listener',
    async run() {
      const { api } = webApi();
      let first = 0;
      let second = 0;
      const unsubscribe = api.onSettingsUpdated(() => { first += 1; });
      api.onSettingsUpdated(() => { second += 1; });
      await api.saveSettings({ theme: 'light' });
      unsubscribe();
      unsubscribe();
      await api.saveSettings({ theme: 'dark' });
      assert.equal(first, 1);
      assert.equal(second, 2);
    },
  },
  {
    name: 'failed web persistence never announces saved settings',
    async run() {
      const { api, storage } = webApi();
      let notified = false;
      api.onSettingsUpdated(() => { notified = true; });
      storage.setItem = () => { throw new Error('storage full'); };
      await assert.rejects(api.saveSettings({ theme: 'light' }), /storage full/);
      assert.equal(notified, false);
    },
  },
];
