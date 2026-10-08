// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFileSync } from 'node:child_process';
import {
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { normalizeAndroidPluginRegistry } from './android-plugin-registry.mjs';

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repositoryRoot = resolve(projectRoot, '../..');
const androidRoot = join(projectRoot, 'android');
const ndkVersion = '29.0.14206865';
const buildToolsVersion = '36.0.0';
const archivePath = join(
  androidRoot,
  'app',
  'build',
  'outputs',
  'apks',
  'release',
  'app-release.apks'
);
const runtimeSonames = [
  'libc++_shared.so',
  'libcardio.so',
  'libmuon_android_rpc.so',
  'libmuon_javascript_runtime.so',
];
const forbiddenDependencies = ['libcef', 'libgtk', 'libgio', 'muon-executor'];
const allowedDependencies = new Set([
  ...runtimeSonames,
  'libandroid.so',
  'libc.so',
  'libdl.so',
  'liblog.so',
  'libm.so',
]);
const abis = [
  { abi: 'x86_64', machine: 'Advanced Micro Devices X86-64' },
  { abi: 'arm64-v8a', machine: 'AArch64' },
];

const execute = (command, args, options = {}) =>
  execFileSync(command, args, {
    cwd: repositoryRoot,
    encoding: 'utf8',
    maxBuffer: 256 * 1024 * 1024,
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

const inspectElf = (readelf, filePath, entry, soname, plugin) => {
  const header = execute(readelf, ['-hW', filePath]);
  expectCondition(
    header.includes(`Machine:                           ${entry.machine}`),
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
  const needed = [...dynamic.matchAll(/Shared library: \[([^\]]+)]/g)].map(
    (match) => match[1]
  );
  expectCondition(
    needed.every((dependency) => allowedDependencies.has(dependency)),
    `${filePath}: unexpected DT_NEEDED (${needed.join(', ')})`
  );
  for (const dependency of forbiddenDependencies) {
    expectCondition(
      !dynamic.includes(dependency),
      `${filePath}: forbidden desktop dependency ${dependency}`
    );
  }
  if (soname === 'libcardio.so') {
    expectCondition(
      needed.includes('libc++_shared.so') && needed.includes('libandroid.so'),
      `${filePath}: cardio DT_NEEDED mismatch`
    );
  } else if (soname === 'libmuon_android_rpc.so') {
    expectCondition(
      needed.includes('libcardio.so') && needed.includes('libc++_shared.so'),
      `${filePath}: muon runtime DT_NEEDED mismatch`
    );
  } else if (soname === 'libmuon_javascript_runtime.so') {
    expectCondition(
      needed.includes('libc++_shared.so') && needed.includes('liblog.so'),
      `${filePath}: JavaScript runtime DT_NEEDED mismatch`
    );
  }

  if (plugin) {
    const symbols = execute(readelf, ['--dyn-syms', '--wide', filePath]);
    expectCondition(
      /\bmuon_init_plugin\b/.test(symbols),
      `${filePath}: muon_init_plugin is not exported`
    );
  }
};

const registry = normalizeAndroidPluginRegistry(
  JSON.parse(readFileSync(join(projectRoot, 'android-plugins.json'), 'utf8'))
);
const pluginSonames = registry.plugins.map((plugin) => plugin.soname);
const expectedSonames = [...runtimeSonames, ...pluginSonames].sort();
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
const zipalign = join(androidSdk, 'build-tools', buildToolsVersion, 'zipalign');
const aapt2 = join(androidSdk, 'build-tools', buildToolsVersion, 'aapt2');

const temporaryRoot = mkdtempSync(join(tmpdir(), 'muon-release-apks-'));
try {
  const archiveEntries = execute('unzip', ['-Z1', archivePath]).split('\n');
  expectCondition(
    archiveEntries.includes('toc.pb'),
    `${archivePath}: bundletool table of contents is missing`
  );
  execute('unzip', ['-q', archivePath, '-d', temporaryRoot]);
  const apkPaths = readdirSync(temporaryRoot, { recursive: true })
    .filter((relativePath) => relativePath.endsWith('.apk'))
    .map((relativePath) => join(temporaryRoot, relativePath));
  expectCondition(
    apkPaths.length > 0,
    `${archivePath}: no APKs were generated`
  );

  const entriesByApk = new Map();
  for (const apkPath of apkPaths) {
    execute(zipalign, ['-c', '-P', '16', '4', apkPath]);
    entriesByApk.set(apkPath, execute('unzip', ['-Z1', apkPath]).split('\n'));
  }
  expectCondition(
    [...entriesByApk.values()].some((entries) =>
      entries.includes('assets/third-party/quickjs-LICENSE')
    ),
    `${archivePath}: QuickJS license asset is missing`
  );

  const masterVariantSuffixes = apkPaths
    .map((apkPath) => /^base-master(_\d+)?\.apk$/.exec(basename(apkPath)))
    .filter((match) => match != null)
    .map((match) => match[1] ?? '')
    .sort();
  expectCondition(
    masterVariantSuffixes.length > 0 &&
      new Set(masterVariantSuffixes).size === masterVariantSuffixes.length,
    `${archivePath}: base master variants are missing or duplicated`
  );

  const masterApks = apkPaths.filter((apkPath) =>
    /^base-master(_\d+)?\.apk$/.test(basename(apkPath))
  );
  for (const masterApk of masterApks) {
    const manifest = execute(aapt2, [
      'dump',
      'xmltree',
      masterApk,
      '--file',
      'AndroidManifest.xml',
    ]);
    expectCondition(
      manifest.includes('"android.permission.INTERNET"'),
      `${masterApk}: INTERNET permission is missing`
    );
    expectCondition(
      manifest.includes('"android.permission.ACCESS_LOCAL_NETWORK"'),
      `${masterApk}: ACCESS_LOCAL_NETWORK permission is missing`
    );
    expectCondition(
      manifest.includes('android:usesCleartextTraffic(0x010104ec)=false'),
      `${masterApk}: release cleartext traffic must be disabled`
    );
    expectCondition(
      manifest.includes('android:networkSecurityConfig(0x01010527)='),
      `${masterApk}: network security config is missing`
    );

    const resources = execute(aapt2, ['dump', 'resources', masterApk]);
    const securityConfigPath =
      /resource 0x[0-9a-f]+ xml\/network_security_config\n\s+\(\) \(file\) ([^ ]+) type=XML/.exec(
        resources
      )?.[1];
    expectCondition(
      securityConfigPath !== undefined,
      `${masterApk}: network security config resource is missing`
    );
    const securityConfig = execute(aapt2, [
      'dump',
      'xmltree',
      masterApk,
      '--file',
      securityConfigPath,
    ]);
    expectCondition(
      /E: base-config[^]*A: cleartextTrafficPermitted=false/.test(
        securityConfig
      ),
      `${masterApk}: base cleartext traffic must be disabled`
    );
    expectCondition(
      /E: domain-config[^]*A: cleartextTrafficPermitted=true[^]*T: 'localhost'/.test(
        securityConfig
      ),
      `${masterApk}: localhost cleartext exception is missing`
    );
  }

  for (const entry of abis) {
    const nativePrefix = `lib/${entry.abi}/`;
    const nativeApks = apkPaths.filter((apkPath) =>
      entriesByApk
        .get(apkPath)
        .includes(`${nativePrefix}libmuon_android_rpc.so`)
    );
    const bundletoolAbi = entry.abi.replaceAll('-', '_');
    const expectedNativeApkNames = masterVariantSuffixes
      .map((suffix) => `base-${bundletoolAbi}${suffix}.apk`)
      .sort();
    const actualNativeApkNames = nativeApks
      .map((apkPath) => basename(apkPath))
      .sort();
    expectCondition(
      JSON.stringify(actualNativeApkNames) ===
        JSON.stringify(expectedNativeApkNames),
      `${archivePath}: ${entry.abi} native split variants mismatch ` +
        `(expected ${expectedNativeApkNames.join(', ')}, ` +
        `got ${actualNativeApkNames.join(', ')})`
    );

    for (const nativeApk of nativeApks) {
      const apkEntries = entriesByApk.get(nativeApk);
      const actualSonames = apkEntries
        .filter((packageEntry) => packageEntry.startsWith(nativePrefix))
        .map((packageEntry) => packageEntry.slice(nativePrefix.length))
        .sort();
      expectCondition(
        JSON.stringify(actualSonames) === JSON.stringify(expectedSonames),
        `${nativeApk}: ${entry.abi} native libraries mismatch ` +
          `(expected ${expectedSonames.join(', ')}, got ${actualSonames.join(', ')})`
      );
      const packagedAbis = new Set(
        apkEntries
          .filter((packageEntry) => packageEntry.startsWith('lib/'))
          .map((packageEntry) => packageEntry.split('/')[1])
      );
      expectCondition(
        packagedAbis.size === 1 && packagedAbis.has(entry.abi),
        `${nativeApk}: ABI split contains ${[...packagedAbis].join(', ')}`
      );

      for (const soname of expectedSonames) {
        const packagedLibrary = `${nativePrefix}${soname}`;
        const destination = join(
          temporaryRoot,
          `${basename(nativeApk, '.apk')}-${entry.abi}-${soname}`
        );
        const contents = execFileSync(
          'unzip',
          ['-p', nativeApk, packagedLibrary],
          {
            cwd: repositoryRoot,
            maxBuffer: 64 * 1024 * 1024,
          }
        );
        writeFileSync(destination, contents);
        inspectElf(
          readelf,
          destination,
          entry,
          soname,
          pluginSonames.includes(soname)
        );
      }
    }
  }
} finally {
  rmSync(temporaryRoot, { force: true, recursive: true });
}

console.log('muon_android_release_apks_test: PASS');
