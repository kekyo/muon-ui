// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, join } from 'node:path';
import { promisify } from 'node:util';

const execute = promisify(execFile);
const [apk, variant, versionCode, versionName] = process.argv.slice(2);
const sdk = process.env.ANDROID_HOME ?? process.env.ANDROID_SDK_ROOT;
assert.ok(apk && sdk && ['debug', 'release'].includes(variant));
const includePlugin = process.argv.includes('--plugins');
const run = async (command, args) =>
  (await execute(command, args, { maxBuffer: 32 * 1024 * 1024 })).stdout;
const buildTools = join(sdk, 'build-tools/36.0.0');
const badging = await run(join(buildTools, 'aapt2'), ['dump', 'badging', apk]);
assert.ok(
  badging.includes(
    `package: name='dev.muon.e2e.publicconsumer' versionCode='${versionCode}' versionName='${versionName}'`
  )
);
assert.match(badging, /^application-label:'Packaged Muon Notes'$/mu);
assert.match(badging, /^application-icon-160:'[^']+\.png'$/mu);
assert.match(badging, /^minSdkVersion:'24'$/mu);
assert.match(badging, /^targetSdkVersion:'37'$/mu);
assert.match(badging, /^native-code: 'arm64-v8a' 'x86_64'$/mu);
assert.equal(/^application-debuggable$/mu.test(badging), variant === 'debug');
assert.deepEqual(
  [...badging.matchAll(/^uses-permission: name='([^']+)'/gmu)]
    .map((match) => match[1])
    .sort(),
  [
    'android.permission.INTERNET',
    'dev.muon.e2e.publicconsumer.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION',
  ]
);
const manifest = await run(join(buildTools, 'aapt2'), [
  'dump',
  'xmltree',
  apk,
  '--file',
  'AndroidManifest.xml',
]);
assert.doesNotMatch(
  manifest,
  /E: service|E: instrumentation|firebase|QuickJS|MuonActivity"/iu
);
await run(join(buildTools, 'zipalign'), ['-c', '-P', '16', '4', apk]);
await run(join(buildTools, 'apksigner'), ['verify', '--verbose', apk]);
const entries = (await run('unzip', ['-Z1', apk])).trim().split('\n');
assert.doesNotMatch(
  entries.join('\n'),
  /quickjs|backend\.mjs|google-services|\.(?:jks|p12|keystore)$/imu
);
const expectedLibraries = [
  'libc++_shared.so',
  'libcardio.so',
  'libmuon_android_rpc.so',
];
if (includePlugin)
  expectedLibraries.push(
    'libmuon_test_plugin_alpha.so',
    'libmuon_test_plugin_types.so',
    'libmuon_test_plugin_recursive_functions.so'
  );
assert.deepEqual(
  entries.filter((entry) => entry.startsWith('lib/')).sort(),
  ['arm64-v8a', 'x86_64']
    .flatMap((abi) => expectedLibraries.map((name) => `lib/${abi}/${name}`))
    .sort()
);
const directory = await mkdtemp(join(tmpdir(), 'muon-consumer-apk-'));
const libraries = [];
try {
  await run('unzip', ['-q', apk, '-d', directory]);
  for (const entry of entries.filter((name) =>
    /^classes\d*\.dex$/u.test(name)
  )) {
    const dex = (await readFile(join(directory, entry))).toString('latin1');
    assert.doesNotMatch(
      dex,
      /Lcom\/google\/firebase\/|Ldev\/muon\/runtime\/MuonJavaScriptRuntime|Landroidx\/test\/|Ldev\/muon\/runtime\/MuonActivity;/u
    );
  }
  const config = JSON.parse(
    await readFile(join(directory, 'assets/muon/config.json'), 'utf8')
  );
  assert.equal(
    config.startPage,
    'https://main.asset.muon.invalid/notes/index.html'
  );
  assert.equal(config.values.channel, 'package-consumer');
  const registry = JSON.parse(
    await readFile(join(directory, 'assets/muon/plugins.json'), 'utf8')
  );
  assert.equal(registry.plugins.length, includePlugin ? 3 : 0);
  for (const entry of entries.filter((name) => name.startsWith('lib/'))) {
    const path = join(directory, entry);
    const abi = entry.split('/')[1];
    const header = await run('readelf', ['-hW', path]);
    assert.match(
      header,
      abi === 'arm64-v8a'
        ? /Machine:\s+AArch64/u
        : /Machine:\s+Advanced Micro Devices X86-64/u
    );
    const segments = (await run('readelf', ['-lW', path]))
      .split('\n')
      .filter((line) => /^\s*LOAD\s/u.test(line))
      .map((line) => line.trim().split(/\s+/u));
    assert.ok(segments.length > 0);
    for (const segment of segments) {
      assert.ok(Number(segment.at(-1)) >= 16384, `${entry}: ELF alignment`);
      assert.equal(
        Number(segment[1]) % 16384,
        Number(segment[2]) % 16384,
        `${entry}: ELF load offset`
      );
    }
    const dynamic = await run('readelf', ['-dW', path]);
    const needed = [...dynamic.matchAll(/\(NEEDED\).*\[([^\]]+)\]/gu)].map(
      (match) => match[1]
    );
    for (const name of needed)
      assert.ok(
        [
          ...expectedLibraries,
          'libc.so',
          'libdl.so',
          'libm.so',
          'liblog.so',
          'libandroid.so',
        ].includes(name),
        `${entry}: unpackaged dependency ${name}`
      );
    const symbols = await run('readelf', ['--dyn-syms', '-W', path]);
    if (basename(entry) === 'libmuon_android_rpc.so') {
      assert.match(
        symbols,
        /Java_dev_muon_runtime_MuonRpcBridge_nativeCreateHost/u
      );
      assert.doesNotMatch(symbols, /JS_NewRuntime|muon_test_plugin|alphaAdd/u);
    }
    if (basename(entry) === 'libmuon_test_plugin_alpha.so')
      assert.match(symbols, /muon_init_plugin/u);
    libraries.push({
      entry,
      needed,
      alignments: segments.map((segment) => Number(segment.at(-1))),
    });
  }
} finally {
  await rm(directory, { recursive: true, force: true });
}
await writeFile(
  apk + '.inspection.json',
  JSON.stringify(
    {
      variant,
      versionCode,
      versionName,
      libraries,
      sha256: createHash('sha256')
        .update(await readFile(apk))
        .digest('hex'),
    },
    null,
    2
  )
);
console.log(
  `Consumer APK metadata, contents, ELF dependencies and 16 KiB alignment: PASS (${variant} ${versionCode})`
);
