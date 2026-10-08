// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import assert from 'node:assert/strict';
import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const execute = promisify(execFile);
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const sdk = process.env.ANDROID_HOME ?? process.env.ANDROID_SDK_ROOT;
const serial = process.env.ANDROID_SERIAL;
assert.ok(sdk && serial, 'Set ANDROID_HOME and ANDROID_SERIAL.');
const output = resolve(process.argv[2] ?? join(root, '.run/fullscreen-video'));
await mkdir(output, { recursive: true });
const adbPath = join(sdk, 'platform-tools/adb');
const adb = async (...args) =>
  (
    await execute(adbPath, ['-s', serial, ...args], {
      maxBuffer: 16 * 1024 * 1024,
    })
  ).stdout;
for (const apk of [
  'debug/app-debug.apk',
  'androidTest/debug/app-debug-androidTest.apk',
])
  await adb('install', '-r', join(root, 'android/app/build/outputs/apk', apk));

const remote = `/sdcard/muon-fullscreen-${process.pid}.mp4`;
const recording = spawn(
  adbPath,
  [
    '-s',
    serial,
    'shell',
    `screenrecord --size 540x1200 --time-limit 60 ${remote} >/dev/null 2>&1 & recording_pid=$!; echo "$recording_pid"; wait "$recording_pid"`,
  ],
  { stdio: ['ignore', 'pipe', 'inherit'] }
);
const finished = once(recording, 'exit');
const [pidData] = await once(recording.stdout, 'data');
const pid = String(pidData).trim();
assert.match(pid, /^\d+$/u);
let result;
try {
  result = await adb(
    'shell',
    'am',
    'instrument',
    '-w',
    '-r',
    '-e',
    'class',
    'dev.muon.runtime.MuonActivityTest#animatesExistingFullscreenSystemBars',
    'dev.muon.prototype.test/androidx.test.runner.AndroidJUnitRunner'
  );
  await writeFile(join(output, 'instrumentation.log'), result);
} finally {
  await adb('shell', 'kill', '-2', pid);
  await finished;
  await adb('pull', remote, join(output, 'fullscreen.mp4'));
  await adb('shell', 'rm', remote);
}
assert.match(result, /OK \(1 test\)/u);
assert.doesNotMatch(result, /FAILURES|INSTRUMENTATION_FAILED/u);
const video = JSON.parse(
  (
    await execute('ffprobe', [
      '-v',
      'error',
      '-count_frames',
      '-show_entries',
      'stream=width,height,nb_read_frames',
      '-of',
      'json',
      join(output, 'fullscreen.mp4'),
    ])
  ).stdout
);
assert.equal(video.streams[0].width, 540);
// Insets assertions verify the transitions; screenrecord uses a variable frame rate.
assert.ok(
  Number(video.streams[0].nb_read_frames) > 0,
  'The fullscreen verification must produce a readable video.'
);
await writeFile(
  join(output, 'video.json'),
  JSON.stringify({ serial, ...video }, null, 2)
);
console.log(`Fullscreen animation and system-bar visibility: PASS (${output})`);
