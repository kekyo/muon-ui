// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFileSync, spawn } from 'node:child_process';
import { once } from 'node:events';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const projectRoot = resolve(
  process.argv[2] ?? resolve(dirname(fileURLToPath(import.meta.url)), '..')
);
const includeQuickJs = process.argv.includes('--quickjs');
const androidRoot = join(projectRoot, 'android');
const releaseApk = join(
  androidRoot,
  'app',
  'build',
  'outputs',
  'apk',
  'release',
  'app-release.apk'
);
const packageName = includeQuickJs ? 'dev.muon.prototype' : 'dev.muon.testhost';
const activityName = `${packageName}/dev.muon.runtime.MuonActivity`;
const pageReadyMarker =
  'Muon page ready: https://main.asset.muon.invalid/index.html';
const serial = process.env.ANDROID_SERIAL;
const profileName = process.env.MUON_ANDROID_TEST_PROFILE;

const targetProfiles = {
  'vm-x86_64-16k': {
    serialPattern: /^emulator-\d+$/,
    serialDescription: 'one local Android emulator',
    properties: [
      ['ro.product.cpu.abi', 'x86_64', 'ABI'],
      ['ro.build.version.sdk', '37', 'Android SDK'],
    ],
    pageSize: '16384',
    resultName: 'vm',
  },
  'pixel6-arm64-4k': {
    serialPattern: /^(?!emulator-\d+$).+$/,
    serialDescription: 'one physical Pixel 6',
    properties: [
      ['ro.product.model', 'Pixel 6', 'model'],
      ['ro.product.device', 'oriole', 'device'],
      ['ro.product.cpu.abi', 'arm64-v8a', 'ABI'],
      ['ro.build.version.sdk', '37', 'Android SDK'],
    ],
    pageSize: '4096',
    resultName: 'pixel6',
  },
};

const expectCondition = (condition, message) => {
  if (!condition) {
    throw new Error(message);
  }
};

const execute = (command, args, options = {}) =>
  execFileSync(command, args, {
    cwd: projectRoot,
    encoding: 'utf8',
    maxBuffer: 64 * 1024 * 1024,
    ...options,
  }).trim();

expectCondition(
  profileName === 'vm-x86_64-16k' || profileName === 'pixel6-arm64-4k',
  'MUON_ANDROID_TEST_PROFILE must identify a supported test target'
);
const targetProfile = targetProfiles[profileName];
expectCondition(
  typeof serial === 'string' && targetProfile.serialPattern.test(serial),
  `ANDROID_SERIAL must identify ${targetProfile.serialDescription}`
);

const adb = (args) => execute('adb', ['-s', serial, ...args]);

expectCondition(
  adb(['get-state']) === 'device',
  `${serial}: device is offline`
);
for (const [property, expected, label] of targetProfile.properties) {
  expectCondition(
    adb(['shell', 'getprop', property]) === expected,
    `${serial}: expected ${label} ${expected}`
  );
}
expectCondition(
  adb(['shell', 'getconf', 'PAGESIZE']) === targetProfile.pageSize,
  `${serial}: expected page size ${targetProfile.pageSize}`
);

const launchAndAwaitPage = async (label) => {
  adb(['logcat', '-c']);
  const logcat = spawn(
    'adb',
    ['-s', serial, 'logcat', '-v', 'brief', 'MuonActivity:I', '*:S'],
    {
      cwd: projectRoot,
      signal: AbortSignal.timeout(45_000),
      stdio: ['ignore', 'pipe', 'pipe'],
    }
  );
  const exit = once(logcat, 'exit');
  let output = '';
  let processError;
  let pageReady = false;
  logcat.once('error', (error) => {
    processError = error;
  });

  await once(logcat, 'spawn');
  adb(['shell', 'am', 'force-stop', packageName]);
  adb(['shell', 'am', 'start', '-W', '-n', activityName]);

  try {
    for await (const chunk of logcat.stdout) {
      output = `${output}${chunk}`.slice(-64 * 1024);
      if (output.includes(pageReadyMarker)) {
        pageReady = true;
        break;
      }
    }
  } catch (error) {
    processError ??= error;
  } finally {
    if (logcat.exitCode === null) {
      logcat.kill();
    }
    await exit;
  }

  expectCondition(
    pageReady,
    `${label}: page-ready event was not observed` +
      `${processError == null ? '' : ` (${processError.message})`}\n${output}`
  );
};

adb(['install', '-r', releaseApk]);
await launchAndAwaitPage('signed release APK');

execFileSync(
  includeQuickJs ? resolve(projectRoot, '../android/gradlew') : join(androidRoot, 'gradlew'),
  ['-p', androidRoot, 'installReleaseBundleApks'], {
  cwd: androidRoot,
  env: { ...process.env, ANDROID_SERIAL: serial },
  stdio: 'inherit',
});
await launchAndAwaitPage('release APK set');

console.log(
  `muon_android_release_package_${targetProfile.resultName}_test: PASS`
);
