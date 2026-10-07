// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import {
  chmod,
  copyFile,
  mkdtemp,
  mkdir,
  readFile,
  symlink,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const execute = promisify(execFile);
const archive = process.argv[2];
const sdk = process.env.ANDROID_HOME ?? process.env.ANDROID_SDK_ROOT;
const serial = process.env.ANDROID_SERIAL;
if (!archive || !sdk || !serial)
  throw new Error('Specify the muon-ui tgz, ANDROID_HOME and ANDROID_SERIAL.');
const root = await mkdtemp(join(tmpdir(), 'muon-android-consumer-'));
console.log(`Consumer project: ${root}`);
const repository = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const versionOf = async (name) =>
  JSON.parse(
    await readFile(
      join(repository, 'node_modules', name, 'package.json'),
      'utf8'
    )
  ).version;
await writeFile(
  join(root, 'package.json'),
  JSON.stringify({
    name: 'muon-android-consumer',
    version: '1.0.0',
    private: true,
    type: 'module',
    scripts: { dependencies: 'reskill' },
    devDependencies: {
      vite: await versionOf('vite'),
      typescript: await versionOf('typescript'),
      'prettier-max': await versionOf('prettier-max'),
      'resolved-killer': await versionOf('resolved-killer'),
    },
  })
);
await execute(
  'npm',
  ['install', '--ignore-scripts', '--no-audit', '--no-fund', resolve(archive)],
  { cwd: root, maxBuffer: 16 * 1024 * 1024 }
);
const minimalSdk = join(root, 'sdk');
await mkdir(minimalSdk);
// Standard consumers cannot see the NDK and cannot invoke native compilers.
for (const name of ['platforms', 'build-tools', 'licenses'])
  await symlink(join(sdk, name), join(minimalSdk, name), 'dir');
const guards = join(root, 'native-tools-disabled');
await mkdir(guards);
for (const name of ['cmake', 'ninja', 'clang', 'clang++', 'gcc', 'g++']) {
  const guard = join(guards, name);
  await writeFile(
    guard,
    '#!/bin/sh\necho "Native compilation is forbidden in a consumer build" >&2\nexit 99\n'
  );
  await chmod(guard, 0o755);
}
const environment = {
  ...process.env,
  PATH: guards + ':' + process.env.PATH,
  ANDROID_HOME: minimalSdk,
  ANDROID_SDK_ROOT: minimalSdk,
  ANDROID_NDK_HOME: '',
  ANDROID_NDK_ROOT: '',
};
const run = async (command, args) => {
  const result = await execute(command, args, {
    cwd: root,
    env: environment,
    maxBuffer: 32 * 1024 * 1024,
  });
  return result.stdout;
};
const cli = join(root, 'node_modules/muon-ui/dist/cli.cjs');
const muon = async (args) =>
  JSON.parse(await run(process.execPath, [cli, ...args, '--json']));
const applicationId = 'dev.muon.e2e.publicconsumer';
const includePlugin = process.argv.includes('--plugins');
const config = {
  android: {
    applicationId,
    label: 'Packaged Muon Notes',
    versionCode: 1,
    icon: 'icon.png',
    permissions: ['android.permission.INTERNET'],
  },
  config: { channel: 'package-consumer' },
};
if (includePlugin) {
  const libraries = {};
  for (const abi of ['arm64-v8a', 'x86_64']) {
    const destination = join(root, 'plugins', abi);
    await mkdir(destination, { recursive: true });
    const library = join(destination, 'libmuon_test_plugin_alpha.so');
    await copyFile(join(repository, 'muon-android-prototype/android/app/build/intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib', abi, 'libmuon_test_plugin_alpha.so'), library);
    libraries[abi] = library;
  }
  config.android.plugins = [{ name: 'consumer_alpha', soname: 'libmuon_test_plugin_alpha.so', libraries, allow: ['muon.test.alpha.alphaAdd', 'muon.test.alpha.alphaConfig'], config: { 'alpha.config': 'consumer-registry' } }];
}
await copyFile(join(repository, 'images/muon-256.png'), join(root, 'icon.png'));
await writeFile(join(root, 'muon.json'), JSON.stringify(config));
await writeFile(
  join(root, 'tsconfig.json'),
  JSON.stringify({
    compilerOptions: {
      target: 'ES2022',
      module: 'ESNext',
      moduleResolution: 'Bundler',
      strict: true,
      noEmit: true,
      lib: ['ES2022', 'DOM'],
      skipLibCheck: true,
    },
    include: ['main.ts', 'vite.config.ts'],
  })
);
await writeFile(
  join(root, 'vite.config.ts'),
  `import { defineConfig } from 'vite';\nimport muon from 'muon-ui/vite';\nimport prettierMax from 'prettier-max';\nexport default defineConfig({ base: '/notes/', plugins: [prettierMax(), muon({ pluginAccess: false, build: { targets: ['android'] } })], build: { target: 'es2022' } });\n`
);
await writeFile(
  join(root, 'index.html'),
  `<!doctype html><html><head><meta name="viewport" content="width=device-width, initial-scale=1"><title>Muon Notes</title><style>body{font-family:sans-serif;margin:48px 20px;background:#f3f6fb;color:#102338}h1{font-size:28px}button{font-size:20px;display:block;margin:20px 0;padding:16px}output{display:block;margin:20px 0;font-size:18px}</style></head><body><h1>Packaged Muon Notes</h1><output id="status">Starting</output><output id="saved">Reading</output><output id="generation"></output><output id="plugin"></output><button id="save">Save note</button><button id="reload">Reload page</button><script type="module" src="/main.ts"></script></body></html>`
);
await writeFile(
  join(root, 'main.ts'),
  `import type {} from 'muon-ui';
const status = document.querySelector<HTMLOutputElement>('#status')!;
const saved = document.querySelector<HTMLOutputElement>('#saved')!;
const path = 'note.txt';
const generation = Number(sessionStorage.getItem('generation') ?? '0') + 1;
sessionStorage.setItem('generation', String(generation));
document.querySelector<HTMLOutputElement>('#generation')!.textContent = 'Page loads: ' + generation;
try {
  const runtime = await window.muon.environments.getRuntimeInfo();
  if (${includePlugin}) {
    const alpha = (window.muon as MuonApi & { test: { alpha: { alphaAdd: (a: number, b: number) => Promise<number>; alphaConfig: () => Promise<string>; alphaName?: unknown } } }).test.alpha;
    if (alpha.alphaName !== undefined) throw new Error('A denied plugin function was exposed');
    document.querySelector<HTMLOutputElement>('#plugin')!.textContent = 'Plugin: ' + await alpha.alphaAdd(3, 4) + ':' + await alpha.alphaConfig() + ':blocked';
  }
  const config = await window.muon.environments.getConfigValues();
  const data = await window.muon.fs.exists(path) ? await window.muon.fs.readTextFile(path, 'utf8') : 'empty';
  saved.textContent = 'Stored: ' + data;
  status.textContent = 'ready:' + runtime.backend + ':' + config.channel;
} catch (error) { status.textContent = 'failed:' + String(error); }
document.querySelector<HTMLButtonElement>('#save')!.onclick = async () => {
  try { await window.muon.fs.writeTextFile(path, 'saved-on-device', 'utf8'); saved.textContent = 'Stored: ' + await window.muon.fs.readTextFile(path, 'utf8'); }
  catch (error) { status.textContent = 'failed:' + String(error); }
};
document.querySelector<HTMLButtonElement>('#reload')!.onclick = async () => { await window.muon.browser.reload(); };
`
);
const prepared = await muon(['prepare', '--target', 'android']);
assert.equal(prepared.target, 'android');
assert.equal(prepared.sdkPath, minimalSdk);
await run(process.execPath, [
  join(root, 'node_modules/typescript/bin/tsc'),
  '--noEmit',
]);
const build = await muon(['build', '--target', 'android']);
const result = build.targets[0];
assert.equal(result.target, 'android');
assert.equal(result.signing, 'debug');
assert.equal(result.applicationId, applicationId);
const firstBytes = await readFile(result.packagePath);
await run(process.execPath, [
  join(root, 'node_modules/vite/bin/vite.js'),
  'build',
]);
assert.deepEqual(
  await readFile(result.packagePath),
  firstBytes,
  'CLI and direct Vite build must generate the same APK'
);
const adb = async (args) =>
  (
    await execute(join(sdk, 'platform-tools/adb'), ['-s', serial, ...args], {
      maxBuffer: 16 * 1024 * 1024,
    })
  ).stdout;
const start = async () => {
  await adb(['shell', 'am', 'force-stop', applicationId]);
  await adb([
    'shell',
    'am',
    'start',
    '-W',
    '-n',
    `${applicationId}/dev.muon.runtime.MuonAppActivity`,
  ]);
};
// Keep one accessibility connection alive while waiting for actual app state.
// The observer APK never adds a test bridge or library to the consumer APK.
await execute(
  join(repository, 'muon-android/gradlew'),
  [':observer:assembleDebug', ':observer:assembleDebugAndroidTest'],
  { cwd: join(repository, 'muon-android'), maxBuffer: 16 * 1024 * 1024 }
);
for (const apk of [
  'apk/debug/observer-debug.apk',
  'apk/androidTest/debug/observer-debug-androidTest.apk',
]) {
  await adb([
    'install',
    '-r',
    join(repository, 'muon-android/observer/build/outputs', apk),
  ]);
}
const observe = async (mode) => {
  const output = await adb([
    'shell',
    'am',
    'instrument',
    '-w',
    '-r',
    '-e',
    'mode',
    mode,
    '-e',
    'plugin',
    String(includePlugin),
    'dev.muon.e2e.observer.test/androidx.test.runner.AndroidJUnitRunner',
  ]);
  await writeFile(join(root, 'instrumentation-' + mode + '.log'), output);
  assert.match(output, /OK \(1 test\)/u, output);
  assert.doesNotMatch(output, /FAILURES|INSTRUMENTATION_FAILED/u);
};
await adb(['install', '-r', result.packagePath]);
await start();
await observe('operate');
await start();
await observe('verify');
const screenshot = await execute(
  join(sdk, 'platform-tools/adb'),
  ['-s', serial, 'exec-out', 'screencap', '-p'],
  { encoding: 'buffer', maxBuffer: 16 * 1024 * 1024 }
);
await writeFile(join(root, 'screen.png'), screenshot.stdout);
await writeFile(
  join(root, 'result.json'),
  JSON.stringify({ ...result, serial, prepare: prepared }, null, 2)
);
console.log(
  `Packaged Android application: PASS (${serial}, ${result.packagePath})`
);
