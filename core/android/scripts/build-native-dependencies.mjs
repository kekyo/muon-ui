// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  accessSync,
  constants,
  copyFileSync,
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { availableParallelism } from 'node:os';
import { basename, dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptPath = fileURLToPath(import.meta.url);
const projectRoot = resolve(dirname(scriptPath), '..');
const repositoryRoot = resolve(projectRoot, '../..');
const dependencyRoot = resolve(
  process.env.MUON_ANDROID_DEPENDENCY_ROOT ??
    join(projectRoot, '.native-dependencies')
);
const libffiSourceRoot = join(
  repositoryRoot,
  'deps',
  'tra-ffic',
  'deps',
  'libffi'
);
const cardioSourceRoot = join(repositoryRoot, 'deps', 'cardio');
const ndkVersion = '29.0.14206865';
const androidApi = 24;
const libffiReleaseTag = 'v3.8.0';
// Pin the release commit as well as the archive: shallow CI checkouts omit tags.
const libffiReleaseCommit = '12ffd1f9dc56fcea79d2f742f424301ae668d663';
const libffiSourceArchiveName = 'libffi-3.8.0.tar.gz';
const libffiSourceArchiveUrl =
  'https://github.com/libffi/libffi/releases/download/v3.8.0/libffi-3.8.0.tar.gz';
const libffiSourceArchiveSha256 =
  '7da3e2d9a171eb0a038f592ecad3ff2bb2550f3496d87b3b29ad0cf4430c0db4';
const cflags = '-O3 -g -fPIC';
const ldflags = '-Wl,-z,max-page-size=16384';
const cardioDefinitions = [
  'CARDIO_BUILD_SHARED_LIB=1',
  'CARDIO_HAS_POSIX_FD=1',
  'CARDIO_SHARED_LIB=1',
  'CARDIO_WITH_LINUX_IO_URING=0',
];
const patchFiles = [
  'patches/libffi/0001-android-x86_64-16k-static-trampoline.patch',
];
const abis = [
  {
    abi: 'x86_64',
    compilerPrefix: 'x86_64-linux-android',
    host: 'x86_64-linux-android',
  },
  {
    abi: 'arm64-v8a',
    compilerPrefix: 'aarch64-linux-android',
    host: 'aarch64-linux-android',
  },
];

const capture = (command, args, options = {}) =>
  execFileSync(command, args, {
    cwd: repositoryRoot,
    encoding: 'utf8',
    maxBuffer: 256 * 1024 * 1024,
    ...options,
  }).trim();

const execute = (command, args, options = {}) => {
  execFileSync(command, args, {
    cwd: repositoryRoot,
    stdio: 'inherit',
    ...options,
  });
};

const sha256Buffer = (contents) =>
  createHash('sha256').update(contents).digest('hex');

const sha256File = (filePath) => sha256Buffer(readFileSync(filePath));

const canonicalJson = (value) => `${JSON.stringify(value)}\n`;

const patches = patchFiles.map((path) => ({
  path,
  sha256: sha256File(join(projectRoot, path)),
}));

const requireExecutable = (filePath) => {
  accessSync(filePath, constants.X_OK);
  return filePath;
};

const findAndroidSdk = () => {
  const configured = process.env.ANDROID_SDK_ROOT ?? process.env.ANDROID_HOME;
  if (configured) {
    return resolve(configured);
  }
  const adb = capture('which', ['adb']);
  return resolve(dirname(adb), '..');
};

const extractOfficialSource = (archive, destination) => {
  mkdirSync(destination, { recursive: true });
  const result = spawnSync(
    'tar',
    ['-xzf', '-', '--strip-components=1', '-C', destination],
    {
      cwd: repositoryRoot,
      input: archive,
      stdio: ['pipe', 'inherit', 'inherit'],
    }
  );
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    throw new Error(`tar failed with exit status ${result.status}`);
  }
};

const applyLibffiPatches = (sourceRoot) => {
  const sourceDirectory = relative(repositoryRoot, sourceRoot);
  for (const patch of patches) {
    const patchPath = join(projectRoot, patch.path);
    execute(
      'git',
      ['apply', '--check', `--directory=${sourceDirectory}`, patchPath],
      { cwd: repositoryRoot }
    );
    execute('git', ['apply', `--directory=${sourceDirectory}`, patchPath], {
      cwd: repositoryRoot,
    });
  }
};

const loadOfficialSourceArchive = async (archivePath) => {
  if (existsSync(archivePath)) {
    const cached = readFileSync(archivePath);
    if (sha256Buffer(cached) === libffiSourceArchiveSha256) {
      return cached;
    }
    rmSync(archivePath, { force: true });
  }

  console.log(`Downloading ${libffiSourceArchiveUrl}...`);
  const response = await fetch(libffiSourceArchiveUrl);
  if (!response.ok) {
    throw new Error(
      `Unable to download libffi source archive: HTTP ${response.status}`
    );
  }
  const downloaded = Buffer.from(await response.arrayBuffer());
  const downloadedSha256 = sha256Buffer(downloaded);
  if (downloadedSha256 !== libffiSourceArchiveSha256) {
    throw new Error(`libffi source archive hash mismatch: ${downloadedSha256}`);
  }
  writeFileSync(archivePath, downloaded);
  return downloaded;
};

const configureArgumentsFor = (entry) => [
  `--host=${entry.host}`,
  '--disable-shared',
  '--enable-static',
  '--disable-docs',
  '--with-pic',
];

const findGeneratedConfig = (sourceRoot) => {
  const candidates = readdirSync(sourceRoot, { recursive: true })
    .filter(
      (relativePath) =>
        basename(relativePath) === 'fficonfig.h' &&
        !relativePath.startsWith(
          `msvc_build${process.platform === 'win32' ? '\\' : '/'}`
        )
    )
    .map((relativePath) => join(sourceRoot, relativePath));
  if (candidates.length !== 1) {
    throw new Error(
      `Expected one generated fficonfig.h, found ${candidates.length}`
    );
  }
  return candidates[0];
};

const isCurrentBuild = (abiRoot, buildInputSha256) => {
  const manifestPath = join(abiRoot, 'manifest.json');
  const archivePath = join(abiRoot, 'install', 'lib', 'libffi.a');
  const configPath = join(abiRoot, 'fficonfig.h');
  if (
    !existsSync(manifestPath) ||
    !existsSync(archivePath) ||
    !existsSync(configPath)
  ) {
    return false;
  }
  try {
    const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'));
    return (
      manifest.buildInputSha256 === buildInputSha256 &&
      manifest.libffi.archiveSha256 === sha256File(archivePath) &&
      manifest.libffi.fficonfigSha256 === sha256File(configPath)
    );
  } catch {
    return false;
  }
};

const argumentsSet = new Set(process.argv.slice(2));
for (const argument of argumentsSet) {
  if (argument !== '--clean') {
    throw new Error(`Unknown argument: ${argument}`);
  }
}

const androidSdk = findAndroidSdk();
const ndkRoot = join(androidSdk, 'ndk', ndkVersion);
const toolchainRoot = join(
  ndkRoot,
  'toolchains',
  'llvm',
  'prebuilt',
  'linux-x86_64',
  'bin'
);
const ar = requireExecutable(join(toolchainRoot, 'llvm-ar'));
const ranlib = requireExecutable(join(toolchainRoot, 'llvm-ranlib'));
const strip = requireExecutable(join(toolchainRoot, 'llvm-strip'));
const libffiCommit = capture('git', [
  '-C',
  libffiSourceRoot,
  'rev-parse',
  'HEAD',
]);
if (libffiReleaseCommit !== libffiCommit) {
  throw new Error(
    `libffi submodule ${libffiCommit} does not match ${libffiReleaseTag} (${libffiReleaseCommit})`
  );
}
const cardioCommit = capture('git', [
  '-C',
  cardioSourceRoot,
  'rev-parse',
  'HEAD',
]);
const libffiRepository = capture('git', [
  '-C',
  libffiSourceRoot,
  'remote',
  'get-url',
  'origin',
]);
const cardioRepository = capture('git', [
  '-C',
  cardioSourceRoot,
  'remote',
  'get-url',
  'origin',
]);
const recipeSha256 = sha256File(scriptPath);
const patchQueueSha256 = sha256Buffer(canonicalJson(patches));

if (argumentsSet.has('--clean')) {
  rmSync(dependencyRoot, { force: true, recursive: true });
}
mkdirSync(dependencyRoot, { recursive: true });
const sourceArchivePath = join(dependencyRoot, libffiSourceArchiveName);
const sourceArchive = await loadOfficialSourceArchive(sourceArchivePath);

for (const entry of abis) {
  const abiRoot = join(dependencyRoot, entry.abi);
  const sourceRoot = join(abiRoot, 'source');
  const installRoot = join(abiRoot, 'install');
  const configureArguments = configureArgumentsFor(entry);
  const buildInput = {
    androidApi,
    cardioCommit,
    cardioDefinitions,
    libffiCommit,
    ndkVersion,
    patchQueueSha256,
    recipeSha256,
    sourceArchiveSha256: libffiSourceArchiveSha256,
    sourceArchiveUrl: libffiSourceArchiveUrl,
    sourceReleaseTag: libffiReleaseTag,
    ...entry,
    cflags,
    configureArguments,
    ldflags,
  };
  const buildInputSha256 = sha256Buffer(canonicalJson(buildInput));
  if (isCurrentBuild(abiRoot, buildInputSha256)) {
    console.log(`Android native dependencies are current for ${entry.abi}.`);
    continue;
  }

  console.log(
    `Building libffi ${libffiCommit.slice(0, 12)} for ${entry.abi}...`
  );
  rmSync(abiRoot, { force: true, recursive: true });
  extractOfficialSource(sourceArchive, sourceRoot);
  applyLibffiPatches(sourceRoot);

  const cc = requireExecutable(
    join(toolchainRoot, `${entry.compilerPrefix}${androidApi}-clang`)
  );
  const cxx = requireExecutable(
    join(toolchainRoot, `${entry.compilerPrefix}${androidApi}-clang++`)
  );
  const buildEnvironment = {
    ...process.env,
    AR: ar,
    CC: cc,
    CFLAGS: cflags,
    CXX: cxx,
    LDFLAGS: ldflags,
    RANLIB: ranlib,
    STRIP: strip,
  };
  execute('./configure', [`--prefix=${installRoot}`, ...configureArguments], {
    cwd: sourceRoot,
    env: buildEnvironment,
  });
  execute('make', [`-j${availableParallelism()}`], {
    cwd: sourceRoot,
    env: buildEnvironment,
  });
  execute('make', ['install'], { cwd: sourceRoot, env: buildEnvironment });

  const generatedConfigPath = findGeneratedConfig(sourceRoot);
  const configPath = join(abiRoot, 'fficonfig.h');
  const archivePath = join(installRoot, 'lib', 'libffi.a');
  copyFileSync(generatedConfigPath, configPath);
  const manifest = {
    schemaVersion: 1,
    abi: entry.abi,
    androidApi,
    buildInputSha256,
    host: entry.host,
    ndkVersion,
    cardio: {
      commit: cardioCommit,
      definitions: cardioDefinitions,
      repository: cardioRepository,
    },
    libffi: {
      archiveSha256: sha256File(archivePath),
      cflags,
      commit: libffiCommit,
      configureArguments,
      fficonfigSha256: sha256File(configPath),
      ldflags,
      patches,
      patchQueueSha256,
      recipeSha256,
      repository: libffiRepository,
      sourceArchiveSha256: libffiSourceArchiveSha256,
      sourceArchiveUrl: libffiSourceArchiveUrl,
      sourceReleaseTag: libffiReleaseTag,
    },
  };
  writeFileSync(join(abiRoot, 'manifest.json'), canonicalJson(manifest));
}

console.log('Android native dependency build: PASS');
