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
const { prepareModelSelection } = load('modelSelection');
const { isModelEffectivelyDownloaded, isModelEffectivelyLoaded, getCollectionPrimaryChatModel } = load('collectionModels');
const models = {
  cloud: { recipe: 'cloud', downloaded: true },
  local: { recipe: 'llamacpp', downloaded: false },
  router: { recipe: 'collection.router', components: ['cloud'] },
  mixed: { recipe: 'collection.router', components: ['cloud', 'local'] },
  omni: { recipe: 'collection.omni', components: ['cloud', 'local'] },
};
module.exports = { tests: [
  { name: 'cloud router is downloadable-ready without inventing resident loaded state', run() {
    assert.equal(isModelEffectivelyDownloaded('router', models.router, models), true);
    assert.equal(isModelEffectivelyDownloaded('mixed', models.mixed, models), false);
    assert.equal(isModelEffectivelyLoaded('router', models.router, models, new Set(['cloud', 'router'])), false);
    assert.equal(isModelEffectivelyLoaded('omni', models.omni, models, new Set(['cloud', 'local'])), true);
    assert.equal(isModelEffectivelyLoaded('omni', models.omni, models, new Set(['cloud'])), false);
    assert.equal(getCollectionPrimaryChatModel('router', models), 'router');
  } },
  { name: 'router selects once while Omni and ordinary models retain their readiness calls', async run() {
    for (const [name, expected] of [['router', [['select', 'router']]], ['omni', [['ready', 'cloud'], ['ready', 'local']]], ['cloud', [['ready', 'cloud']]]]) {
      const calls = [];
      await prepareModelSelection(name, models, {
        selectRouter: async value => { calls.push(['select', value]); },
        ensureReady: async value => { calls.push(['ready', value]); },
      });
      assert.deepEqual(calls, expected);
    }
  } },
  { name: 'router admission failure propagates without falling back to loading candidates', async run() {
    let ready = 0;
    await assert.rejects(prepareModelSelection('router', models, {
      selectRouter: async () => { throw new Error('router refused'); },
      ensureReady: async () => { ready++; },
    }), /router refused/);
    assert.equal(ready, 0);
  } },
] };
