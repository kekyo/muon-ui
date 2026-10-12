// Build SDK link inputs using the same public cardio settings as the runtime.
// This maintainer script is excluded from the published SDK.
import { execFileSync } from 'node:child_process';
import { mkdir } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  getPluginTarget,
  muonPluginNdkVersion,
  stageSdkTarget,
} from '../dist/index.mjs';

const sdk = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const host =
  process.platform === 'win32'
    ? `windows-${process.arch === 'ia32' ? 'i686' : 'amd64'}`
    : `linux-${process.arch === 'x64' ? 'amd64' : process.arch === 'arm' ? 'armhf' : 'arm64'}`;
const target = getPluginTarget(process.argv[2] ?? host);
const output = join(sdk, '.build', target.target);
const cardio = resolve(sdk, '../deps/cardio');
await mkdir(output, { recursive: true });
let compiler =
  target.os === 'linux' && target.target === host ? 'g++' : target.cxx;
if (target.os === 'android') {
  const android = process.env.ANDROID_HOME ?? process.env.ANDROID_SDK_ROOT;
  if (!android)
    throw new Error('Set ANDROID_HOME to build Android SDK link inputs.');
  compiler = join(
    android,
    'ndk',
    muonPluginNdkVersion,
    'toolchains/llvm/prebuilt/linux-x86_64/bin',
    compiler
  );
}
const systemFlags = target.packages.length
  ? execFileSync(
      'pkg-config',
      [
        '--cflags',
        '--libs',
        ...target.packages.map((value) => value.split(' ')[0]),
      ],
      { encoding: 'utf8' }
    )
      .trim()
      .split(/\s+/u)
  : [];
execFileSync(
  compiler,
  [
    '-std=c++20',
    '-O2',
    '-shared',
    ...target.cflags,
    '-DCARDIO_BUILD_SHARED_LIB=1',
    ...target.definitions.map((value) => '-D' + value),
    '-I' + join(cardio, 'include'),
    join(cardio, 'samples/libcardio/libcardio.cpp'),
    '-o',
    join(output, target.cardioLibrary),
    ...(target.os === 'windows'
      ? ['-Wl,--out-implib,' + join(output, target.cardioLinkLibrary)]
      : ['-Wl,-soname,' + target.cardioLibrary]),
    ...target.ldflags,
    ...systemFlags,
  ],
  { stdio: 'inherit' }
);
await stageSdkTarget({
  target: target.target,
  runtimeDirectory: output,
  cardioDirectory: cardio,
  sdkDirectory: sdk,
});
console.log(`Staged native SDK for ${target.target}`);
