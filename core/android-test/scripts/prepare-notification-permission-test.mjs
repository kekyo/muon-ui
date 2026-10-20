// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { join } from 'node:path';
import { promisify } from 'node:util';

const execute = promisify(execFile);
const serial = process.env.ANDROID_SERIAL;
assert.ok(
  serial,
  'ANDROID_SERIAL must identify the device used for connected Android tests.'
);
const sdk = process.env.ANDROID_SDK_ROOT ?? process.env.ANDROID_HOME;
const adbPath = sdk ? join(sdk, 'platform-tools', 'adb') : 'adb';
const adb = async (...args) =>
  (await execute(adbPath, ['-s', serial, ...args])).stdout.trim();
const api = Number(await adb('shell', 'getprop', 'ro.build.version.sdk'));
assert.ok(
  Number.isInteger(api) && api > 0,
  'The Android API level is unavailable.'
);
// A permission revocation can kill the application's UID, so it must happen
// before instrumentation starts. Never reset any package except this test host.
const applicationId = 'dev.muon.testhost';
const permission = 'android.permission.POST_NOTIFICATIONS';
if (api >= 33) {
  const installed = (
    await adb(
      'shell',
      'pm',
      'list',
      'packages',
      '--user',
      'current',
      applicationId
    )
  )
    .split('\n')
    .includes(`package:${applicationId}`);
  if (installed) {
    let declared = true;
    try {
      await adb('shell', 'pm', 'revoke', applicationId, permission);
    } catch (error) {
      // An older installed test host may predate notification support. The
      // connected test task will install the new APK with a fresh permission.
      if (!String(error.stderr).includes('has not requested permission'))
        throw error;
      declared = false;
    }
    if (declared) {
      for (const flag of ['user-set', 'user-fixed']) {
        await adb(
          'shell',
          'pm',
          'clear-permission-flags',
          applicationId,
          permission,
          flag
        );
      }
      console.log(
        `Reset ${applicationId} notification permission on ${serial}.`
      );
    }
  }
}
