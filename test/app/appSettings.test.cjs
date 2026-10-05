for (const key of Object.keys(process.env)) {
  if (key.startsWith('npm_') || key === 'INIT_CWD') {
    delete process.env[key];
  }
}

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const appRoot = path.resolve(__dirname, '..', '..', 'src', 'app');
let ts;
try {
  ts = require(path.join(appRoot, 'node_modules', 'typescript'));
} catch (_) {
  ts = require('typescript');
}

const originalTsLoader = require.extensions['.ts'];

require.extensions['.ts'] = function loadTypeScript(module, filename) {
  const source = fs.readFileSync(filename, 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: {
      esModuleInterop: true,
      module: ts.ModuleKind.CommonJS,
      moduleResolution: ts.ModuleResolutionKind.NodeJs,
      target: ts.ScriptTarget.ES2020,
    },
    fileName: filename,
  }).outputText;
  module._compile(output, filename);
};

const appSettingsPath = path.join(appRoot, 'src', 'renderer', 'utils', 'appSettings.ts');
const { mergeWithDefaultSettings, createDefaultSettings, cloneSettings, buildChatRequestOverrides, clampNumericSettingValue } = require(appSettingsPath);

if (originalTsLoader) {
  require.extensions['.ts'] = originalTsLoader;
} else {
  delete require.extensions['.ts'];
}

const tests = [];

function defineTest(name, fn) {
  tests.push({ name, fn });
}

defineTest('mergeWithDefaultSettings preserves light theme', () => {
  const settings = mergeWithDefaultSettings({
    layout: {
      theme: 'light',
    },
  });

  assert.equal(settings.layout.theme, 'light');
});

defineTest('mergeWithDefaultSettings rejects invalid theme', () => {
  const settings = mergeWithDefaultSettings({
    layout: {
      theme: 'neon-pink',
    },
  });

  assert.equal(settings.layout.theme, 'dark');
});

defineTest('mergeWithDefaultSettings rejects removed prompt-debugger leftPanelView', () => {
  // Prompt Debugger was removed as a standalone view (folded into the Router
  // Builder's Test Prompt tab instead) - a stale settings file still naming
  // it must fall back to the default, not persist a now-nonexistent view.
  const settings = mergeWithDefaultSettings({
    layout: {
      leftPanelView: 'prompt-debugger',
    },
  });

  assert.equal(settings.layout.leftPanelView, 'models');
});

defineTest('output cap defaults remain omitted for new and older settings', () => {
  assert.equal(buildChatRequestOverrides(createDefaultSettings()).max_completion_tokens, undefined);
  assert.equal(buildChatRequestOverrides(mergeWithDefaultSettings({ temperature: { value: 0.5, useDefault: false } })).max_completion_tokens, undefined);
});

defineTest('explicit output cap survives persistence and clone without changing thinking', () => {
  const settings = mergeWithDefaultSettings({ maxOutputTokens: { value: 512, useDefault: false } });
  const restored = mergeWithDefaultSettings(JSON.parse(JSON.stringify(settings)));
  const copied = cloneSettings(restored);
  assert.deepEqual(buildChatRequestOverrides(copied), { max_completion_tokens: 512 });
  copied.maxOutputTokens.value = 128;
  assert.equal(restored.maxOutputTokens.value, 512);
  copied.maxOutputTokens.useDefault = true;
  assert.equal(buildChatRequestOverrides(copied).max_completion_tokens, undefined);
});

defineTest('output cap is always a bounded positive integer', () => {
  for (const value of [0, -1, 1.4, 511.8, Infinity, NaN, 2 ** 40]) {
    const n = clampNumericSettingValue('maxOutputTokens', value);
    assert.ok(Number.isInteger(n) && n >= 1 && n <= 1048576);
    const settings = mergeWithDefaultSettings({ maxOutputTokens: { value, useDefault: false } });
    assert.equal(buildChatRequestOverrides(settings).max_completion_tokens, n);
  }
});

let failures = 0;

for (const { name, fn } of tests) {
  try {
    fn();
    console.log(`ok - ${name}`);
  } catch (error) {
    failures += 1;
    console.error(`not ok - ${name}`);
    console.error(error);
  }
}

if (failures > 0) {
  process.exitCode = 1;
}
