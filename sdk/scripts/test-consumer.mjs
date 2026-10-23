// Exercise published packages outside the repository; browser calls use Playwright MCP.
import assert from 'node:assert/strict';
import { execFile, spawn } from 'node:child_process';
import {
  access,
  chmod,
  cp,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { setTimeout as delay } from 'node:timers/promises';
import { createConnection } from '@playwright/mcp';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';

const repository = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const execute = promisify(execFile);
const root = await mkdtemp(join(tmpdir(), 'muon-sdk-consumer-'));
console.log('Independent package test: ' + root);
const run = async (command, args, cwd = root, env = process.env) => {
  try {
    return (
      await execute(command, args, { cwd, env, maxBuffer: 32 * 1024 * 1024 })
    ).stdout;
  } catch (error) {
    throw new Error(
      `${error.message}\n${error.stdout ?? ''}\n${error.stderr ?? ''}`
    );
  }
};
const pack = async (directory, ignoreScripts) =>
  join(
    root,
    Object.values(
      JSON.parse(
        await run(
          'npm',
          [
            'pack',
            '--json',
            ...(ignoreScripts ? ['--ignore-scripts'] : []),
            '--pack-destination',
            root,
          ],
          directory
        )
      )
    )[0].filename
  );
// CI supplies a tested runtime. Only distribution runtime files enter the UI tarball.
const runtime = process.argv[2];
if (!runtime)
  throw new Error('Specify a built Linux amd64 muon runtime directory.');
const staged = join(repository, 'ui/dist/runtime/linux-amd64');
await mkdir(staged, { recursive: true });
for (const file of [
  'muon-core',
  'muon-executor-supervisor',
  'libmuon-ui.so',
  'libcardio.so',
  'plugins/node.so',
  'plugins/node-bridge.mjs',
  'CREDITS.md',
]) {
  await mkdir(dirname(join(staged, file)), { recursive: true });
  await cp(join(resolve(runtime), file), join(staged, file));
}
// This fixture may contain only the CI target subset; release prepack checks all seven.
const sdkTarball = await pack(join(repository, 'sdk'), true);
const uiTarball = await pack(join(repository, 'ui'), false);
const author = join(root, 'author');
await mkdir(author);
await writeFile(
  join(author, 'package.json'),
  JSON.stringify({ name: 'independent-author', private: true })
);
await run(
  'npm',
  ['install', '--ignore-scripts', '--no-audit', '--no-fund', sdkTarball],
  author
);
const sdkCli = join(author, 'node_modules/muon-plugin-sdk/dist/cli.mjs');
await run(
  process.execPath,
  [sdkCli, 'init', 'source', '--name', 'independent'],
  author
);
const sourceConfig = join(author, 'source/muon-plugin.build.json');
const buildConfig = JSON.parse(await readFile(sourceConfig, 'utf8'));
buildConfig.headers = ['independent.h'];
await writeFile(
  join(author, 'source/independent.h'),
  '/** Example public native value used by plugin clients. */\ntypedef int independent_value;\n'
);
await writeFile(sourceConfig, JSON.stringify(buildConfig));
const androidSdk = process.env.ANDROID_HOME ?? process.env.ANDROID_SDK_ROOT;
const serials = (
  process.env.MUON_TEST_ANDROID_SERIALS ??
  process.env.ANDROID_SERIAL ??
  ''
)
  .split(',')
  .filter(Boolean);
const targets = [
  'linux-amd64',
  ...(serials.length ? ['android-arm64-v8a', 'android-x86_64'] : []),
];
await writeFile(
  join(author, 'source/vendor.c'),
  'int vendor_increment(int value) { return value + 1; }\n'
);
const pluginSource = join(author, 'source/plugin.cpp');
await writeFile(
  pluginSource,
  'extern "C" int vendor_increment(int value);\n' +
    (await readFile(pluginSource, 'utf8')).replace(
      'value + 1',
      'vendor_increment(value)'
    )
);
for (const target of targets) {
  const nativeEnvironment = JSON.parse(
    await run(process.execPath, [sdkCli, 'env', '--target', target], author)
  );
  const directory = join(author, 'vendor-build', target);
  await mkdir(directory, { recursive: true });
  const dependency = join(directory, 'libindependent_vendor.so');
  await run(
    nativeEnvironment.CC,
    [
      '-std=c99',
      '-shared',
      '-fPIC',
      join(author, 'source/vendor.c'),
      '-Wl,-soname,libindependent_vendor.so',
      '-Wl,-z,max-page-size=16384',
      '-o',
      dependency,
    ],
    author,
    { ...process.env, ...nativeEnvironment }
  );
  buildConfig.build.args = ['LDLIBS=' + dependency];
  buildConfig.dependencies = { [target]: [dependency] };
  await writeFile(sourceConfig, JSON.stringify(buildConfig));
  await run(
    process.execPath,
    [sdkCli, 'build', sourceConfig, '--target', target, '--out', 'package'],
    author
  );
}
await writeFile(
  join(author, 'package/package.json'),
  JSON.stringify({
    name: 'independent-native-plugin',
    version: '1.0.0',
    types: './types/index.d.ts',
    exports: {
      '.': { types: './types/index.d.ts' },
      './muon-plugin.json': './muon-plugin.json',
    },
    files: ['muon-plugin.json', 'native', 'include', 'types', 'licenses'],
  })
);
const pluginTarball = await pack(join(author, 'package'), false);
// The consumer cannot recover sources or the authoring SDK from the producer.
await rm(author, { recursive: true, force: true });
const app = join(root, 'application');
await mkdir(app);
await writeFile(
  join(app, 'package.json'),
  JSON.stringify({
    name: 'sdk-consumer',
    version: '1.0.0',
    private: true,
    type: 'module',
    scripts: { dependencies: 'reskill' },
  })
);
const versionOf = async (name) =>
  JSON.parse(
    await readFile(
      join(repository, 'node_modules', name, 'package.json'),
      'utf8'
    )
  ).version;
await run(
  'npm',
  [
    'install',
    '--ignore-scripts',
    '--no-audit',
    '--no-fund',
    uiTarball,
    pluginTarball,
    'vite@' + (await versionOf('vite')),
    'typescript@' + (await versionOf('typescript')),
    'prettier-max@' + (await versionOf('prettier-max')),
    'resolved-killer@' + (await versionOf('resolved-killer')),
  ],
  app
);
await assert.rejects(access(join(app, 'node_modules/muon-plugin-sdk')));
await access(
  join(app, 'node_modules/independent-native-plugin/include/independent.h')
);
const guards = join(root, 'native-tools-disabled');
await mkdir(guards);
for (const tool of [
  'cc',
  'c++',
  'gcc',
  'g++',
  'clang',
  'clang++',
  'cmake',
  'make',
  'ninja',
  'pkg-config',
]) {
  await writeFile(
    join(guards, tool),
    '#!/bin/sh\necho "A prebuilt plugin consumer must not run a native compiler" >&2\nexit 99\n'
  );
  await chmod(join(guards, tool), 0o755);
}
const env = {
  ...process.env,
  PATH: guards + ':' + process.env.PATH,
  ANDROID_NDK_HOME: '',
  ANDROID_NDK_ROOT: '',
};
if (serials.length) {
  if (!androidSdk)
    throw new Error('Set ANDROID_HOME when selecting Android devices.');
  const minimalSdk = join(root, 'android-sdk-without-ndk');
  await mkdir(minimalSdk);
  for (const entry of ['platforms', 'build-tools', 'licenses'])
    await symlink(join(androidSdk, entry), join(minimalSdk, entry));
  env.ANDROID_HOME = env.ANDROID_SDK_ROOT = minimalSdk;
}
await mkdir(join(app, 'src'));
await writeFile(
  join(app, 'index.html'),
  '<!doctype html><html><head><title>Independent plugin</title></head><body><p id="result">Loading plugin</p><script type="module" src="/src/main.ts"></script></body></html>'
);
await writeFile(
  join(app, 'src/plugin.d.ts'),
  'declare module "muon:independent" { export const increment: import("independent-native-plugin").independentPlugin["increment"]; }\n'
);
await writeFile(
  join(app, 'src/main.ts'),
  'import { increment } from "muon:independent";\nObject.assign(window, { callPlugin: increment });\nconst start = async () => { document.querySelector("#result")!.textContent = "Result: " + await increment(41); };\nawait start();\n'
);
await writeFile(
  join(app, 'tsconfig.json'),
  JSON.stringify({
    compilerOptions: {
      target: 'ES2022',
      module: 'ESNext',
      moduleResolution: 'Bundler',
      strict: true,
      skipLibCheck: true,
      noEmit: true,
    },
    include: ['src'],
  })
);
const config = {
  plugin: {
    mode: 'validate',
    plugins: [
      {
        name: 'independent',
        native: { package: 'independent-native-plugin' },
        imports: [{ sources: ['src/**'], allow: ['muon.independent.*'] }],
      },
    ],
  },
  cdp: { enable: true, port: 9589 },
  android: {
    applicationId: 'dev.muon.e2e.sdkconsumer',
    label: 'Independent plugin',
    versionCode: 1,
  },
};
const configPath = join(app, 'muon.json');
await writeFile(configPath, JSON.stringify(config));
await writeFile(
  join(app, 'vite.config.ts'),
  'import { defineConfig } from "vite";\nimport muon from "muon-ui/vite";\nexport default defineConfig({ plugins: [muon({ open: false })], build: { target: "es2022" } });\n'
);
await run(
  process.execPath,
  [join(app, 'node_modules/typescript/bin/tsc'), '--noEmit'],
  app,
  env
);
const cli = join(app, 'node_modules/muon-ui/dist/cli.cjs');
const build = async (target) =>
  JSON.parse(
    await run(
      process.execPath,
      [cli, 'build', '--target', target, '--json'],
      app,
      env
    )
  );
const inspectBrowser = async (endpoint) => {
  const server = await createConnection({
    browser: { cdpEndpoint: endpoint, cdpTimeout: 120000 },
    outputDir: join(root, 'playwright'),
  });
  const client = new Client({ name: 'muon-sdk-test', version: '1.0.0' });
  const [clientTransport, serverTransport] =
    InMemoryTransport.createLinkedPair();
  await server.connect(serverTransport);
  await client.connect(clientTransport);
  try {
    const call = async (name, args) => {
      const result = await client.callTool(
        { name, arguments: args },
        undefined,
        { timeout: 120000 }
      );
      if (result.isError) throw new Error(JSON.stringify(result));
      return result.content
        .filter((entry) => entry.type === 'text')
        .map((entry) => entry.text)
        .join('\n');
    };
    // Initialize the MCP browser context before using its assertion-only wait tool.
    const tabs = await call('browser_tabs', { action: 'list' });
    const index = tabs.match(/(?:^|\n)- (\d+):[^\n]*asset:\/\/main\//u)?.[1];
    if (index !== undefined)
      await call('browser_tabs', { action: 'select', index: Number(index) });
    await call('browser_wait_for', { text: 'Result: 42' });
    const result = await call('browser_evaluate', {
      function: 'async () => await window.callPlugin(99)',
    });
    assert.match(result, /\b100\b/u);
  } finally {
    await client.close();
    await server.close();
  }
};
const waitFor = async (probe, description) => {
  const deadline = Date.now() + 120000;
  while (Date.now() < deadline) {
    const value = await probe();
    if (value) return value;
    await delay(200);
  }
  throw new Error('Timed out waiting for ' + description);
};
const desktop = (await build('linux-amd64')).targets[0];
console.log('Compiler-free CEF application built: ' + desktop.launcherPath);
const child = spawn(
  desktop.launcherPath,
  ['--ozone-platform=x11', '--disable-gpu'],
  {
    cwd: dirname(desktop.launcherPath),
    env: { ...process.env, XDG_STATE_HOME: join(root, 'state') },
    stdio: ['ignore', 'pipe', 'pipe'],
    detached: true,
  }
);
let log = '';
child.stdout.on('data', (value) => {
  log += value;
});
child.stderr.on('data', (value) => {
  log += value;
});
try {
  await waitFor(async () => {
    if (child.exitCode !== null) throw new Error('CEF exited: ' + log);
    try {
      return (
        await (await fetch('http://127.0.0.1:9589/json/list')).json()
      ).some(
        (target) =>
          target.type === 'page' && target.url.startsWith('asset://main/')
      );
    } catch {
      return false;
    }
  }, 'CEF application page');
  await inspectBrowser('http://127.0.0.1:9589');
  console.log('CEF package plugin invoked through Playwright MCP.');
} finally {
  try {
    process.kill(-child.pid, 'SIGTERM');
  } catch {}
  await writeFile(join(root, 'cef.log'), log);
}
const manifestPath = join(
  app,
  'node_modules/independent-native-plugin/muon-plugin.json'
);
const validManifest = await readFile(manifestPath, 'utf8');
const invalidManifest = JSON.parse(validManifest);
invalidManifest.functions.push('muon.independent.nonexistent');
await writeFile(manifestPath, JSON.stringify(invalidManifest));
try {
  const invalidDesktop = (await build('linux-amd64')).targets[0];
  const rejected = spawn(
    invalidDesktop.launcherPath,
    ['--ozone-platform=x11', '--disable-gpu'],
    {
      cwd: dirname(invalidDesktop.launcherPath),
      env: { ...process.env, XDG_STATE_HOME: join(root, 'invalid-state') },
      stdio: ['ignore', 'pipe', 'pipe'],
      detached: true,
    }
  );
  let diagnostic = '';
  rejected.stdout.on('data', (value) => {
    diagnostic += value;
  });
  rejected.stderr.on('data', (value) => {
    diagnostic += value;
  });
  try {
    await waitFor(
      async () => diagnostic.includes('Plugin catalog mismatch'),
      'CEF catalog rejection'
    );
  } finally {
    try {
      process.kill(-rejected.pid, 'SIGTERM');
    } catch {}
    await writeFile(join(root, 'cef-catalog-rejection.log'), diagnostic);
  }
  console.log('CEF rejected the mismatched producer catalog.');
} finally {
  await writeFile(manifestPath, validManifest);
}
if (serials.length) {
  delete config.cdp;
  await writeFile(configPath, JSON.stringify(config));
  const apk = (await build('android')).targets[0].packagePath;
  console.log('Compiler-free two-ABI APK built: ' + apk);
  for (const serial of serials) {
    const adb = async (args) =>
      await run(join(androidSdk, 'platform-tools/adb'), [
        '-s',
        serial,
        ...args,
      ]);
    await adb(['install', '-r', apk]);
    await adb(['shell', 'am', 'force-stop', config.android.applicationId]);
    await adb([
      'shell',
      'am',
      'start',
      '-W',
      '-n',
      config.android.applicationId + '/dev.muon.runtime.MuonAppActivity',
    ]);
    const pid = (
      await adb(['shell', 'pidof', config.android.applicationId])
    ).trim();
    const socket = await waitFor(
      async () =>
        (await adb(['shell', 'cat', '/proc/net/unix']))
          .split(/\r?\n/u)
          .map((line) => line.split(' ').at(-1))
          .find((name) => name === '@webview_devtools_remote_' + pid),
      'Android WebView CDP socket'
    );
    const port = (
      await adb(['forward', 'tcp:0', 'localabstract:' + socket.slice(1)])
    ).trim();
    try {
      await inspectBrowser('http://127.0.0.1:' + port);
    } finally {
      await adb(['forward', '--remove', 'tcp:' + port]);
      await adb(['shell', 'am', 'force-stop', config.android.applicationId]);
    }
    console.log(
      'Android package plugin invoked through Playwright MCP: ' + serial
    );
  }
  await writeFile(manifestPath, JSON.stringify(invalidManifest));
  try {
    const invalidApk = (await build('android')).targets[0].packagePath;
    for (const serial of serials) {
      const adb = async (args) =>
        await run(join(androidSdk, 'platform-tools/adb'), [
          '-s',
          serial,
          ...args,
        ]);
      await adb(['install', '-r', invalidApk]);
      await adb([
        'shell',
        'am',
        'start',
        '-W',
        '-n',
        config.android.applicationId + '/dev.muon.runtime.MuonAppActivity',
      ]);
      const pid = (
        await adb(['shell', 'pidof', config.android.applicationId])
      ).trim();
      await waitFor(
        async () =>
          (
            await adb(['logcat', '-d', '--pid=' + pid, '-s', 'MuonActivity:E'])
          ).includes('Plugin catalog mismatch'),
        'Android catalog rejection'
      );
      await adb(['shell', 'am', 'force-stop', config.android.applicationId]);
      console.log(
        'Android rejected the mismatched producer catalog: ' + serial
      );
    }
  } finally {
    await writeFile(manifestPath, validManifest);
  }
}
await writeFile(
  join(root, 'result.json'),
  JSON.stringify({ targets, serials, desktop, compilerFree: true }, null, 2)
);
console.log('Independent SDK → plugin NPM → application test passed.');
