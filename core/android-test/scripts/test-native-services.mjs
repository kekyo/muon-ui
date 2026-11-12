// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFileSync } from 'node:child_process';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const serial = process.env.ANDROID_SERIAL;
if (!serial) throw new Error('ANDROID_SERIAL must identify the test device.');
const adb = (args) =>
  execFileSync('adb', ['-s', serial, ...args], {
    encoding: 'utf8',
    maxBuffer: 16 * 1024 * 1024,
  });
const abi = adb(['shell', 'getprop', 'ro.product.cpu.abi']).trim();
const pageSize = adb(['shell', 'getconf', 'PAGESIZE']).trim();
const api = adb(['shell', 'getprop', 'ro.build.version.sdk']).trim();
if (
  api !== '37' ||
  !(
    (abi === 'arm64-v8a' && pageSize === '4096') ||
    (abi === 'x86_64' && pageSize === '16384')
  )
) {
  throw new Error(
    `Expected Android API 37 arm64/4K or x86_64/16K; received ${api}/${abi}/${pageSize}.`
  );
}
execFileSync(
  './gradlew',
  [
    ':serviceDriver:assembleDebug',
    ':serviceDriver:assembleDebugAndroidTest',
    ':observer:assembleDebug',
    ':observer:assembleDebugAndroidTest',
  ],
  { cwd: join(root, 'android'), stdio: 'inherit' }
);
for (const apk of [
  'service-driver/build/outputs/apk/debug/serviceDriver-debug.apk',
  'service-driver/build/outputs/apk/androidTest/debug/serviceDriver-debug-androidTest.apk',
  'observer/build/outputs/apk/debug/observer-debug.apk',
  'observer/build/outputs/apk/androidTest/debug/observer-debug-androidTest.apk',
]) {
  const result = adb(['install', '-r', join(root, apk)]);
  if (!result.includes('Success')) throw new Error(result);
}
// The timeout assertion alters only the disposable emulator. Keep a second
// restore boundary outside instrumentation in case its process terminates.
const timeoutKey = ['activity_manager', 'data_sync_fgs_timeout_duration'];
const emulator = adb(['shell', 'getprop', 'ro.kernel.qemu']).trim() === '1';
const originalTimeout = emulator
  ? adb(['shell', 'device_config', 'get', ...timeoutKey]).trim()
  : undefined;
try {
  const result = adb([
    'shell',
    'am',
    'instrument',
    '-w',
    '-r',
    '-e',
    'class',
    'dev.muon.e2e.observer.NativeServiceProcessTest,dev.muon.e2e.observer.ServiceConstraintsTest',
    'dev.muon.e2e.observer.test/androidx.test.runner.AndroidJUnitRunner',
  ]);
  process.stdout.write(result);
  if (
    !/OK \(\d+ tests?\)/u.test(result) ||
    /FAILURES|INSTRUMENTATION_FAILED/u.test(result)
  ) {
    throw new Error('Native service observer assertions failed.');
  }
} finally {
  if (originalTimeout !== undefined) {
    adb([
      'shell',
      'device_config',
      originalTimeout === 'null' ? 'delete' : 'put',
      ...timeoutKey,
      ...(originalTimeout === 'null' ? [] : [originalTimeout]),
    ]);
    if (
      adb(['shell', 'device_config', 'get', ...timeoutKey]).trim() !==
      originalTimeout
    ) {
      throw new Error(
        'The emulator FGS timeout configuration was not restored.'
      );
    }
  }
}

const dispatcherResult = adb([
  'shell',
  'am',
  'instrument',
  '-w',
  '-r',
  '-e',
  'class',
  'dev.muon.runtime.MuonServiceDispatcherTest,dev.muon.runtime.MuonServiceSourceTest',
  'dev.muon.servicefixture.test/androidx.test.runner.AndroidJUnitRunner',
]);
process.stdout.write(dispatcherResult);
if (
  !/OK \(\d+ tests?\)/u.test(dispatcherResult) ||
  /FAILURES|INSTRUMENTATION_FAILED/u.test(dispatcherResult)
) {
  throw new Error('Service source or shared dispatcher assertions failed.');
}
execFileSync('npm', ['exec', '--', 'vitest', 'run', 'service-rpc.test.ts'], {
  cwd: root,
  stdio: 'inherit',
  env: { ...process.env, MUON_ANDROID_SERVICE_RPC_TEST: '1' },
});
