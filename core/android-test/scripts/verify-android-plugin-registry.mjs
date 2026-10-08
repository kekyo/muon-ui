// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  normalizeAndroidPluginRegistry,
  validateAndroidPluginArtifacts,
} from './android-plugin-registry.mjs';

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repositoryRoot = resolve(projectRoot, '../..');
const androidRoot = join(projectRoot, 'android');
const ndkVersion = '29.0.14206865';
const abis = ['x86_64', 'arm64-v8a'];
const runtimeSonames = [
  'libc++_shared.so',
  'libcardio.so',
  'libmuon_android_rpc.so',
  'libmuon_javascript_runtime.so',
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

const findAndroidSdk = () => {
  const configured = process.env.ANDROID_SDK_ROOT ?? process.env.ANDROID_HOME;
  if (configured) {
    return resolve(configured);
  }
  const adb = execute('bash', ['-lc', 'command -v adb']);
  return resolve(dirname(adb), '..');
};

const inspectElf = (readelf, filePath) => {
  const header = execute(readelf, ['-hW', filePath]);
  const machine = /^\s*Machine:\s*(.+)$/m.exec(header)?.[1]?.trim();
  if (!machine) {
    throw new Error(`${filePath}: ELF machine is unavailable`);
  }

  const dynamic = execute(readelf, ['-dW', filePath]);
  const soname = /\(SONAME\)[^\n]*\[([^\]]+)]/.exec(dynamic)?.[1];
  if (!soname) {
    throw new Error(`${filePath}: DT_SONAME is unavailable`);
  }

  const symbols = execute(readelf, ['--dyn-syms', '--wide', filePath]);
  const exportedSymbols = symbols
    .split('\n')
    .map((line) => line.trim().split(/\s+/).at(-1))
    .filter((symbol) => symbol !== undefined);

  const programHeaders = execute(readelf, ['-lW', filePath]);
  const loadAlignments = programHeaders
    .split('\n')
    .filter((line) => /^\s*LOAD\s/.test(line))
    .map((line) => Number(line.trim().split(/\s+/).at(-1)));

  return { machine, soname, exportedSymbols, loadAlignments };
};

const extractPackageEntry = (packagePath, packageEntry, destination) => {
  const contents = execFileSync('unzip', ['-p', packagePath, packageEntry], {
    cwd: repositoryRoot,
    maxBuffer: 64 * 1024 * 1024,
  });
  expectCondition(
    contents.length > 0,
    `${packagePath}: ${packageEntry} is empty`
  );
  writeFileSync(destination, contents);
};

const verifyPackage = (
  packagePath,
  packagePrefix,
  registry,
  readelf,
  temporaryRoot
) => {
  const entries = execute('unzip', ['-Z1', packagePath]).split('\n');
  const inspections = new Map();
  for (const abi of abis) {
    const libraryPrefix = `${packagePrefix}lib/${abi}/`;
    const actualLibraries = entries
      .filter(
        (entry) => entry.startsWith(libraryPrefix) && entry.endsWith('.so')
      )
      .map((entry) => entry.slice(libraryPrefix.length))
      .sort();
    const registrySonames = registry.plugins.map((plugin) => plugin.soname);
    const expectedLibraries = [...runtimeSonames, ...registrySonames].sort();
    expectCondition(
      JSON.stringify(actualLibraries) === JSON.stringify(expectedLibraries),
      `${packagePath}: ${abi} native libraries differ from the registry ` +
        `(expected ${expectedLibraries.join(', ')}, got ${actualLibraries.join(', ')})`
    );

    for (const plugin of registry.plugins) {
      const artifactPath = plugin.artifacts[abi];
      const packageEntry = `${packagePrefix}${artifactPath}`;
      const destination = join(
        temporaryRoot,
        `${packagePrefix.replaceAll('/', '-')}${abi}-${plugin.soname}`
      );
      extractPackageEntry(packagePath, packageEntry, destination);
      inspections.set(
        `${abi}:${artifactPath}`,
        inspectElf(readelf, destination)
      );
    }
  }

  validateAndroidPluginArtifacts(registry, ({ abi, artifactPath }) => {
    const inspection = inspections.get(`${abi}:${artifactPath}`);
    if (!inspection) {
      throw new Error(`missing package entry: ${artifactPath}`);
    }
    return inspection;
  });
};

const manifestRegistry = normalizeAndroidPluginRegistry(
  JSON.parse(readFileSync(join(projectRoot, 'android-plugins.json'), 'utf8'))
);
const generatedRegistry = JSON.parse(
  readFileSync(
    join(
      androidRoot,
      '.generated',
      'plugin-registry',
      'android-plugin-registry.json'
    ),
    'utf8'
  )
);
expectCondition(
  JSON.stringify(generatedRegistry) === JSON.stringify(manifestRegistry),
  'Generated Android plugin registry is stale'
);

const androidSdk = findAndroidSdk();
const readelf = join(
  androidSdk,
  'ndk',
  ndkVersion,
  'toolchains',
  'llvm',
  'prebuilt',
  'linux-x86_64',
  'bin',
  'llvm-readelf'
);
const packages = [
  {
    path: join(
      androidRoot,
      'app',
      'build',
      'outputs',
      'apk',
      'debug',
      'app-debug.apk'
    ),
    prefix: '',
  },
  {
    path: join(
      androidRoot,
      'app',
      'build',
      'outputs',
      'apk',
      'release',
      'app-release.apk'
    ),
    prefix: '',
  },
  {
    path: join(
      androidRoot,
      'app',
      'build',
      'outputs',
      'bundle',
      'release',
      'app-release.aab'
    ),
    prefix: 'base/',
  },
];

const temporaryRoot = mkdtempSync(join(tmpdir(), 'muon-android-plugins-'));
try {
  for (const packageEntry of packages) {
    verifyPackage(
      packageEntry.path,
      packageEntry.prefix,
      manifestRegistry,
      readelf,
      temporaryRoot
    );
  }
} finally {
  rmSync(temporaryRoot, { force: true, recursive: true });
}

console.log('muon_android_plugin_registry_test: PASS');
