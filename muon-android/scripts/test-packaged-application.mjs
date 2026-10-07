// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFileSync } from 'node:child_process';
import { chmod, mkdtemp, mkdir, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const archive = process.argv[2];
const sdk = process.env.ANDROID_HOME ?? process.env.ANDROID_SDK_ROOT;
const serial = process.env.ANDROID_SERIAL;
if (!archive || !sdk || !serial)
  throw new Error('Specify the muon-ui tgz, ANDROID_HOME and ANDROID_SERIAL.');
const root = await mkdtemp(join(tmpdir(), 'muon-android-consumer-'));
console.log(`Consumer project: ${root}`);
await writeFile(
  join(root, 'package.json'),
  JSON.stringify({
    name: 'muon-android-consumer',
    version: '1.0.0',
    private: true,
    type: 'module',
  })
);
execFileSync(
  'npm',
  ['install', '--ignore-scripts', '--no-audit', '--no-fund', resolve(archive)],
  { cwd: root, stdio: 'inherit' }
);
const minimalSdk = join(root, 'sdk');
await mkdir(minimalSdk);
// Deliberately expose only packages required by standard applications.
for (const name of ['platforms', 'build-tools', 'licenses']) {
  await symlink(join(sdk, name), join(minimalSdk, name), 'dir');
}
await mkdir(join(root, 'web'));
await writeFile(
  join(root, 'web/index.html'),
  `<!doctype html>
<html><head><meta name="viewport" content="width=device-width, initial-scale=1"></head>
<body><h1>Packaged Muon application</h1><output id="result">Starting</output>
<script type="module">
try {
  const runtime = await muon.environments.getRuntimeInfo();
  const config = await muon.environments.getConfigValues();
  document.querySelector('#result').textContent = 'ready:' + runtime.backend + ':' + config.channel;
} catch (error) { document.querySelector('#result').textContent = 'failed:' + error.message; }
</script></body></html>`
);
const componentsDirectory = join(root, 'node_modules/muon-ui/dist/android');
const api = await import(
  pathToFileURL(join(componentsDirectory, 'lib/index.mjs')).href
);
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
await api.prepareAndroid({
  componentsDirectory,
  sdkPath: minimalSdk,
  environment,
  prepareGradle: true,
});
const result = await api.buildAndroidApplication({
  componentsDirectory,
  assetsDirectory: join(root, 'web'),
  assetPath: '',
  startPage: 'https://main.asset.muon.invalid/index.html',
  applicationId: 'dev.muon.e2e.consumer',
  label: 'Muon consumer',
  versionCode: 1,
  versionName: '1.0.0',
  abis: ['arm64-v8a', 'x86_64'],
  permissions: [],
  values: { channel: 'package-consumer' },
  plugins: [],
  icon: undefined,
  projectDirectory: join(root, 'generated'),
  outputDirectory: join(root, 'output'),
  sdkPath: minimalSdk,
  variant: 'debug',
  environment,
  output: (text) => process.stdout.write(text),
});
const adb = (args) =>
  execFileSync(join(sdk, 'platform-tools/adb'), ['-s', serial, ...args], {
    encoding: 'utf8',
  });
adb(['install', '-r', result.packagePath]);
adb(['shell', 'am', 'force-stop', result.applicationId]);
adb([
  'shell',
  'am',
  'start',
  '-W',
  '-n',
  `${result.applicationId}/dev.muon.runtime.MuonAppActivity`,
]);
const deadline = Date.now() + 60000;
let hierarchy = '';
do {
  adb(['shell', 'uiautomator', 'dump', '/sdcard/muon-consumer-window.xml']);
  hierarchy = adb(['shell', 'cat', '/sdcard/muon-consumer-window.xml']);
  if (hierarchy.includes('ready:android-webview:package-consumer')) break;
  if (hierarchy.includes('startup failed:') || hierarchy.includes('failed:'))
    throw new Error(hierarchy);
} while (Date.now() < deadline);
if (!hierarchy.includes('ready:android-webview:package-consumer'))
  throw new Error(`Consumer did not respond: ${hierarchy}`);
await writeFile(
  join(root, 'result.json'),
  JSON.stringify({ ...result, serial }, null, 2)
);
console.log(
  `Packaged Android application: PASS (${serial}, ${result.packagePath})`
);
