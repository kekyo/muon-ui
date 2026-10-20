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
const scenario = process.argv[3] ?? 'fullscreen';
const tests = {
  fullscreen:
    'dev.muon.runtime.MuonActivityTest#animatesExistingFullscreenSystemBars',
  ime: 'dev.muon.runtime.MuonInsetsTest#reportsImeSeparatelyFromTheSafeArea',
  transient:
    'dev.muon.runtime.MuonTransientBarsTest#preservesFullscreenThroughTransientSwipeBars',
};
assert.ok(
  Object.hasOwn(tests, scenario),
  'The video scenario must be fullscreen, ime, or transient.'
);
const output = resolve(process.argv[2] ?? join(root, `.run/${scenario}-video`));
const videoPath = join(output, `${scenario}.mp4`);
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

const remote = `/sdcard/muon-${scenario}-${process.pid}.mp4`;
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
    tests[scenario],
    'dev.muon.testhost.test/androidx.test.runner.AndroidJUnitRunner'
  );
  await writeFile(join(output, 'instrumentation.log'), result);
} finally {
  await adb('shell', 'kill', '-2', pid);
  await finished;
  await adb('pull', remote, videoPath);
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
      '-select_streams',
      'v:0',
      '-show_entries',
      'stream=width,height,nb_read_frames',
      '-of',
      'json',
      videoPath,
    ])
  ).stdout
);
assert.equal(video.streams[0].width, 540);
// Insets assertions verify the transitions; screenrecord uses a variable frame rate.
assert.ok(
  Number(video.streams[0].nb_read_frames) > 0,
  'The insets verification must produce a readable video.'
);
const decodeCrop = async (filter) => {
  const { stdout, stderr } = await execute(
    'ffmpeg',
    [
      '-v',
      'error',
      '-i',
      videoPath,
      '-vf',
      filter,
      '-fps_mode',
      'passthrough',
      '-enc_time_base',
      '1:1000000',
      '-f',
      'rawvideo',
      '-pix_fmt',
      'rgb24',
      'pipe:1',
    ],
    { encoding: 'buffer', maxBuffer: 128 * 1024 * 1024 }
  );
  assert.equal(
    stderr.toString().trim(),
    '',
    'Video decoding must complete without errors.'
  );
  return stdout;
};
let frameMetrics;
if (scenario !== 'fullscreen') {
  const [markers, keyboard] = await Promise.all([
    decodeCrop('crop=200:200:(iw-200)/2:100'),
    decodeCrop(
      scenario === 'ime'
        ? 'crop=iw:ih/4:0:ih*3/4,scale=90:50'
        : 'crop=iw:100:0:ih-100,scale=270:50'
    ),
  ]);
  const markerFrameSize = 200 * 200 * 3;
  const keyboardFrameSize = (scenario === 'ime' ? 90 : 270) * 50 * 3;
  assert.equal(
    markers.length / markerFrameSize,
    keyboard.length / keyboardFrameSize
  );
  const states = [];
  let shownFrame;
  let hiddenFrame;
  const shownFrames = [];
  const hiddenFrames = [];
  for (let offset = 0; offset < markers.length; offset += markerFrameSize) {
    let green = 0;
    let magenta = 0;
    for (let i = offset; i < offset + markerFrameSize; i += 3) {
      if (markers[i] < 40 && markers[i + 1] > 200 && markers[i + 2] < 40)
        green++;
      if (markers[i] > 200 && markers[i + 1] < 40 && markers[i + 2] > 200)
        magenta++;
    }
    const state = magenta > 300 ? 'shown' : green > 300 ? 'hidden' : undefined;
    if (state === 'shown') {
      shownFrame = offset / markerFrameSize;
      shownFrames.push(shownFrame);
    }
    if (state === 'hidden' && shownFrame !== undefined) {
      hiddenFrame = offset / markerFrameSize;
      hiddenFrames.push(hiddenFrame);
    }
    if (state !== undefined && states.at(-1) !== state) states.push(state);
  }
  assert.deepEqual(
    states.slice(-2),
    ['shown', 'hidden'],
    'The rendered marker must reflect both visibility transitions.'
  );
  assert.ok(shownFrame !== undefined && hiddenFrame > shownFrame);
  // Native checks hold each completed state for 60 display frames. Use the
  // middle recorded frame of each marked interval, excluding entry/exit motion
  // and the Activity teardown that can begin while its last color is still visible.
  shownFrame = shownFrames[Math.floor(shownFrames.length / 2)];
  hiddenFrame = hiddenFrames[Math.floor(hiddenFrames.length / 2)];
  if (scenario === 'ime') {
    let changedPixels = 0;
    for (let pixel = 0; pixel < keyboardFrameSize; pixel += 3) {
      const shown = shownFrame * keyboardFrameSize + pixel;
      const hidden = hiddenFrame * keyboardFrameSize + pixel;
      if (
        Math.max(
          ...[0, 1, 2].map((channel) =>
            Math.abs(keyboard[shown + channel] - keyboard[hidden + channel])
          )
        ) > 20
      )
        changedPixels++;
    }
    const changedFraction = changedPixels / (keyboardFrameSize / 3);
    assert.ok(
      changedFraction > 0.2,
      'Keyboard pixels must change along with the IME state marker.'
    );
    frameMetrics = { states, shownFrame, hiddenFrame, changedFraction };
  } else {
    const whitePixels = (frame) => {
      let count = 0;
      for (
        let i = frame * keyboardFrameSize;
        i < (frame + 1) * keyboardFrameSize;
        i += 3
      ) {
        if (keyboard[i] > 180 && keyboard[i + 1] > 180 && keyboard[i + 2] > 180)
          count++;
      }
      return count;
    };
    const shownWhitePixels = Math.max(...shownFrames.map(whitePixels));
    const hiddenWhitePixels = whitePixels(hiddenFrame);
    assert.ok(
      shownWhitePixels > 10,
      'The transient frame must contain the physical navigation buttons.'
    );
    assert.ok(
      hiddenWhitePixels <= 2,
      'Navigation buttons must disappear when the transient overlay ends.'
    );
    frameMetrics = {
      states,
      shownFrame,
      hiddenFrame,
      shownWhitePixels,
      hiddenWhitePixels,
    };
  }
} else {
  // The driver's blue corner control must reach the physical bottom when the
  // navigation bar is hidden and move above it when shown. Inspect actual video
  // frames, in addition to the native animation and insets assertions.
  const cropWidth = 200;
  const cropHeight = 200;
  const pixels = await decodeCrop(
    `crop=${cropWidth}:${cropHeight}:iw-${cropWidth}:ih-${cropHeight}`
  );
  const frameSize = cropWidth * cropHeight * 3;
  assert.equal(
    pixels.length % frameSize,
    0,
    'Every decoded frame must be complete.'
  );
  const positions = [];
  for (let offset = 0; offset < pixels.length; offset += frameSize) {
    let bottom = -1;
    for (let y = 0; y < cropHeight; y++) {
      let matches = 0;
      for (let x = 0; x < cropWidth; x++) {
        const i = offset + (y * cropWidth + x) * 3;
        // Allow the recording's YUV conversion and compression around #0067a3.
        if (
          pixels[i] < 35 &&
          Math.abs(pixels[i + 1] - 103) < 35 &&
          Math.abs(pixels[i + 2] - 163) < 35
        )
          matches++;
      }
      if (matches >= 30) bottom = y;
    }
    positions.push(bottom < 0 ? null : cropHeight - 1 - bottom);
  }
  const settled = positions.filter((value) => value !== null && value > 8);
  assert.ok(
    settled.length > 0,
    'The video must show the control above the navigation bar.'
  );
  // The settled position recurs while each animation has finished. A larger
  // one-frame clearance during initial WebView layout is not a settled position.
  const counts = new Map();
  for (const clearance of settled)
    counts.set(clearance, (counts.get(clearance) ?? 0) + 1);
  const visibleClearance = [...counts].sort((a, b) => b[1] - a[1])[0][0];
  const states = [];
  for (const clearance of positions) {
    if (clearance === null) continue;
    const state =
      clearance <= 2
        ? 'hidden'
        : Math.abs(clearance - visibleClearance) <= 2
          ? 'shown'
          : undefined;
    if (state !== undefined && states.at(-1) !== state) states.push(state);
  }
  const firstHidden = states.indexOf('hidden');
  assert.deepEqual(
    states.slice(firstHidden, firstHidden + 4),
    ['hidden', 'shown', 'hidden', 'shown'],
    'The recorded control must follow enter, exit, and both toggle transitions.'
  );
  frameMetrics = { visibleClearance, states, positions };
}
await writeFile(
  join(output, 'video.json'),
  JSON.stringify({ serial, scenario, ...video, ...frameMetrics }, null, 2)
);
console.log(`Android ${scenario} animation and insets: PASS (${output})`);
