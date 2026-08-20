// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFileSync, spawn } from 'node:child_process';
import { once } from 'node:events';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
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
const packageName = 'dev.muon.prototype';
const activityName = `${packageName}/.MuonActivity`;
const pageReadyMarker =
  'Muon page ready: https://main.asset.muon.invalid/index.html';
const serial = process.env.ANDROID_SERIAL;

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
  typeof serial === 'string' && /^emulator-\d+$/.test(serial),
  'ANDROID_SERIAL must identify one local Android emulator'
);

const adb = (args) => execute('adb', ['-s', serial, ...args]);

expectCondition(
  adb(['get-state']) === 'device',
  `${serial}: device is offline`
);
expectCondition(
  adb(['shell', 'getprop', 'ro.product.cpu.abi']) === 'x86_64',
  `${serial}: expected x86_64 ABI`
);
expectCondition(
  adb(['shell', 'getprop', 'ro.build.version.sdk']) === '37',
  `${serial}: expected Android SDK 37`
);
expectCondition(
  adb(['shell', 'getconf', 'PAGESIZE']) === '16384',
  `${serial}: expected a 16 KiB page size`
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

execFileSync(join(androidRoot, 'gradlew'), ['installReleaseBundleApks'], {
  cwd: androidRoot,
  env: { ...process.env, ANDROID_SERIAL: serial },
  stdio: 'inherit',
});
await launchAndAwaitPage('release APK set');

console.log('muon_android_release_package_vm_test: PASS');
