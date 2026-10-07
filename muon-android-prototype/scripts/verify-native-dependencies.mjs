// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  mkdtempSync,
  readFileSync,
  rmSync,
  statSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repositoryRoot = resolve(projectRoot, '..');
const androidRoot = join(projectRoot, 'android');
const dependencyRoot = join(androidRoot, '.native-dependencies');
const expectedNdkVersion = '29.0.14206865';
const expectedBuildToolsVersion = '36.0.0';
const expectedApi = 24;
const expectedLibffiSourceArchive = {
  releaseTag: 'v3.8.0',
  sha256: '7da3e2d9a171eb0a038f592ecad3ff2bb2550f3496d87b3b29ad0cf4430c0db4',
  url: 'https://github.com/libffi/libffi/releases/download/v3.8.0/libffi-3.8.0.tar.gz',
};
const expectedDefinitions = [
  'CARDIO_BUILD_SHARED_LIB=1',
  'CARDIO_HAS_POSIX_FD=1',
  'CARDIO_SHARED_LIB=1',
  'CARDIO_WITH_LINUX_IO_URING=0',
];
const expectedPatchFiles = [
  'patches/libffi/0001-android-x86_64-16k-static-trampoline.patch',
];
const abis = [
  {
    abi: 'x86_64',
    host: 'x86_64-linux-android',
    machine: 'Advanced Micro Devices X86-64',
  },
  {
    abi: 'arm64-v8a',
    host: 'aarch64-linux-android',
    machine: 'AArch64',
  },
];

const execute = (command, args, options = {}) =>
  execFileSync(command, args, {
    cwd: repositoryRoot,
    encoding: 'utf8',
    maxBuffer: 64 * 1024 * 1024,
    ...options,
  }).trim();

const expectCondition = (condition, message) => {
  if (!condition) {
    throw new Error(message);
  }
};

const sha256 = (filePath) =>
  createHash('sha256').update(readFileSync(filePath)).digest('hex');

const sha256Contents = (contents) =>
  createHash('sha256').update(contents).digest('hex');

const findAndroidSdk = () => {
  const configured = process.env.ANDROID_SDK_ROOT ?? process.env.ANDROID_HOME;
  if (configured) {
    return configured;
  }
  const adb = execute('bash', ['-lc', 'command -v adb']);
  return resolve(dirname(adb), '..');
};

const verifyManifest = (entry, commits, expectedHashes) => {
  const abiRoot = join(dependencyRoot, entry.abi);
  const manifestPath = join(abiRoot, 'manifest.json');
  const archivePath = join(abiRoot, 'install', 'lib', 'libffi.a');
  const ffiHeaderPath = join(abiRoot, 'install', 'include', 'ffi.h');
  const ffiConfigPath = join(abiRoot, 'fficonfig.h');
  const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'));

  expectCondition(
    manifest.schemaVersion === 1,
    `${entry.abi}: schema mismatch`
  );
  expectCondition(manifest.abi === entry.abi, `${entry.abi}: ABI mismatch`);
  expectCondition(
    manifest.androidApi === expectedApi,
    `${entry.abi}: API mismatch`
  );
  expectCondition(
    manifest.ndkVersion === expectedNdkVersion,
    `${entry.abi}: NDK mismatch`
  );
  expectCondition(manifest.host === entry.host, `${entry.abi}: host mismatch`);
  expectCondition(
    manifest.libffi.commit === commits.libffi,
    `${entry.abi}: libffi commit mismatch`
  );
  expectCondition(
    manifest.cardio.commit === commits.cardio,
    `${entry.abi}: cardio commit mismatch`
  );
  expectCondition(
    JSON.stringify(manifest.libffi.configureArguments) ===
      JSON.stringify([
        `--host=${entry.host}`,
        '--disable-shared',
        '--enable-static',
        '--disable-docs',
        '--with-pic',
      ]),
    `${entry.abi}: configure arguments mismatch`
  );
  expectCondition(
    manifest.libffi.cflags === '-O3 -g -fPIC',
    `${entry.abi}: CFLAGS mismatch`
  );
  expectCondition(
    manifest.libffi.ldflags === '-Wl,-z,max-page-size=16384',
    `${entry.abi}: LDFLAGS mismatch`
  );
  expectCondition(
    JSON.stringify(manifest.libffi.patches) === JSON.stringify(expectedPatches),
    `${entry.abi}: patch list mismatch`
  );
  expectCondition(
    manifest.libffi.patchQueueSha256 === expectedHashes.patchQueue,
    `${entry.abi}: patch queue hash mismatch`
  );
  expectCondition(
    manifest.libffi.recipeSha256 === expectedHashes.recipe,
    `${entry.abi}: build recipe hash mismatch`
  );
  expectCondition(
    manifest.libffi.sourceArchiveSha256 === expectedHashes.sourceArchive,
    `${entry.abi}: source archive hash mismatch`
  );
  expectCondition(
    manifest.libffi.sourceArchiveUrl === expectedLibffiSourceArchive.url,
    `${entry.abi}: source archive URL mismatch`
  );
  expectCondition(
    manifest.libffi.sourceReleaseTag === expectedLibffiSourceArchive.releaseTag,
    `${entry.abi}: source release tag mismatch`
  );
  expectCondition(
    JSON.stringify(manifest.cardio.definitions) ===
      JSON.stringify(expectedDefinitions),
    `${entry.abi}: cardio definitions mismatch`
  );
  expectCondition(
    statSync(archivePath).size > 0,
    `${entry.abi}: libffi.a is empty`
  );
  expectCondition(
    statSync(ffiHeaderPath).size > 0,
    `${entry.abi}: ffi.h is empty`
  );
  expectCondition(
    manifest.libffi.archiveSha256 === sha256(archivePath),
    `${entry.abi}: libffi.a hash mismatch`
  );
  expectCondition(
    manifest.libffi.fficonfigSha256 === sha256(ffiConfigPath),
    `${entry.abi}: fficonfig.h hash mismatch`
  );
  const ffiConfig = readFileSync(ffiConfigPath, 'utf8');
  expectCondition(
    /^#define FFI_EXEC_STATIC_TRAMP 1$/m.test(ffiConfig),
    `${entry.abi}: static trampolines are disabled`
  );
  expectCondition(
    /^#define FFI_MMAP_EXEC_WRIT 1$/m.test(ffiConfig),
    `${entry.abi}: mmap-based closure support is disabled`
  );
  expectCondition(
    !/^#define FFI_EXEC_TRAMPOLINE_TABLE 1$/m.test(ffiConfig),
    `${entry.abi}: trampoline table must be disabled`
  );
};

const verifyElf = (readelf, filePath, entry, required, forbidden) => {
  const elfHeader = execute(readelf, ['-hW', filePath]);
  expectCondition(
    elfHeader.includes(`Machine:                           ${entry.machine}`),
    `${filePath}: ELF machine mismatch`
  );
  const programHeaders = execute(readelf, ['-lW', filePath]);
  const alignments = programHeaders
    .split('\n')
    .filter((line) => /^\s*LOAD\s/.test(line))
    .map((line) => line.trim().split(/\s+/).at(-1));
  expectCondition(alignments.length > 0, `${filePath}: no LOAD segments`);
  expectCondition(
    alignments.every((alignment) => alignment === '0x4000'),
    `${filePath}: LOAD alignment mismatch (${alignments.join(', ')})`
  );
  const dynamic = execute(readelf, ['-dW', filePath]);
  for (const dependency of required) {
    expectCondition(
      dynamic.includes(`Shared library: [${dependency}]`),
      `${filePath}: missing DT_NEEDED ${dependency}`
    );
  }
  for (const dependency of forbidden) {
    expectCondition(
      !dynamic.includes(dependency),
      `${filePath}: forbidden desktop dependency ${dependency}`
    );
  }
};

const verifyApk = (apkPath, readelf, zipalign, temporaryRoot) => {
  execute(zipalign, ['-c', '-P', '16', '4', apkPath]);
  const entries = execute('unzip', ['-Z1', apkPath]).split('\n');
  for (const entry of abis) {
    const libraryEntries = {
      cardio: `lib/${entry.abi}/libcardio.so`,
      cpp: `lib/${entry.abi}/libc++_shared.so`,
      javascript: `lib/${entry.abi}/libmuon_javascript_runtime.so`,
      runtime: `lib/${entry.abi}/libmuon_android_rpc.so`,
    };
    for (const libraryEntry of Object.values(libraryEntries)) {
      expectCondition(
        entries.includes(libraryEntry),
        `${apkPath}: missing ${libraryEntry}`
      );
    }
    const extracted = {};
    for (const [name, libraryEntry] of Object.entries(libraryEntries)) {
      const destination = join(temporaryRoot, `${entry.abi}-${name}.so`);
      const contents = execFileSync('unzip', ['-p', apkPath, libraryEntry], {
        cwd: repositoryRoot,
        maxBuffer: 64 * 1024 * 1024,
      });
      writeFileSync(destination, contents);
      extracted[name] = destination;
    }
    const forbidden = ['libcef', 'libgtk', 'libgio', 'muon-executor'];
    verifyElf(
      readelf,
      extracted.cardio,
      entry,
      ['libc++_shared.so', 'libandroid.so'],
      forbidden
    );
    verifyElf(readelf, extracted.cpp, entry, [], forbidden);
    verifyElf(
      readelf,
      extracted.javascript,
      entry,
      ['libc++_shared.so', 'liblog.so'],
      forbidden
    );
    verifyElf(
      readelf,
      extracted.runtime,
      entry,
      ['libcardio.so', 'libc++_shared.so'],
      forbidden
    );
  }
  expectCondition(
    entries.includes('assets/third-party/quickjs-LICENSE'),
    `${apkPath}: missing the QuickJS license asset`
  );
};

const verifySubmodulesAreClean = () => {
  for (const relativePath of [
    'deps/cardio',
    'deps/tra-ffic',
    'deps/tra-ffic/deps/libffi',
  ]) {
    const status = execute('git', [
      '-C',
      relativePath,
      'status',
      '--porcelain',
    ]);
    expectCondition(status === '', `${relativePath}: submodule is dirty`);
  }
};

const androidSdk = findAndroidSdk();
const toolchain = join(
  androidSdk,
  'ndk',
  expectedNdkVersion,
  'toolchains',
  'llvm',
  'prebuilt',
  'linux-x86_64',
  'bin'
);
const readelf = join(toolchain, 'llvm-readelf');
const zipalign = join(
  androidSdk,
  'build-tools',
  expectedBuildToolsVersion,
  'zipalign'
);
const commits = {
  cardio: execute('git', ['-C', 'deps/cardio', 'rev-parse', 'HEAD']),
  libffi: execute('git', [
    '-C',
    'deps/tra-ffic/deps/libffi',
    'rev-parse',
    'HEAD',
  ]),
};
const expectedPatches = expectedPatchFiles.map((path) => ({
  path,
  sha256: sha256(join(repositoryRoot, 'muon-android', path)),
}));
const expectedHashes = {
  patchQueue: sha256Contents(`${JSON.stringify(expectedPatches)}\n`),
  recipe: sha256(
    join(
      repositoryRoot,
      'muon-android',
      'scripts',
      'build-native-dependencies.mjs'
    )
  ),
  sourceArchive: expectedLibffiSourceArchive.sha256,
};

for (const entry of abis) {
  verifyManifest(entry, commits, expectedHashes);
}
verifySubmodulesAreClean();

const temporaryRoot = mkdtempSync(join(tmpdir(), 'muon-native-deps-'));
try {
  verifyApk(
    join(
      androidRoot,
      'app',
      'build',
      'outputs',
      'apk',
      'debug',
      'app-debug.apk'
    ),
    readelf,
    zipalign,
    temporaryRoot
  );
  verifyApk(
    join(
      androidRoot,
      'app',
      'build',
      'outputs',
      'apk',
      'release',
      'app-release.apk'
    ),
    readelf,
    zipalign,
    temporaryRoot
  );
} finally {
  rmSync(temporaryRoot, { force: true, recursive: true });
}

console.log('muon_android_native_dependencies_test: PASS');
