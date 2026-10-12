// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import assert from "node:assert/strict";
import { createHash, randomBytes } from "node:crypto";
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import {
  chmod,
  copyFile,
  mkdtemp,
  mkdir,
  readFile,
  readdir,
  symlink,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const execute = promisify(execFile);
const archive = process.argv[2];
const sdk = process.env.ANDROID_HOME ?? process.env.ANDROID_SDK_ROOT;
const serial = process.env.ANDROID_SERIAL;
if (!archive || !sdk || !serial)
  throw new Error("Specify the muon-ui tgz, ANDROID_HOME and ANDROID_SERIAL.");
const root = await mkdtemp(join(tmpdir(), "muon-android-consumer-"));
console.log(`Consumer project: ${root}`);
const repository = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const versionOf = async (name) =>
  JSON.parse(
    await readFile(
      join(repository, "node_modules", name, "package.json"),
      "utf8",
    ),
  ).version;
await writeFile(
  join(root, "package.json"),
  JSON.stringify({
    name: "muon-android-consumer",
    version: "1.0.0",
    private: true,
    type: "module",
    scripts: { dependencies: "reskill" },
    devDependencies: {
      vite: await versionOf("vite"),
      typescript: await versionOf("typescript"),
      "prettier-max": await versionOf("prettier-max"),
      "resolved-killer": await versionOf("resolved-killer"),
    },
  }),
);
await execute(
  "npm",
  ["install", "--ignore-scripts", "--no-audit", "--no-fund", resolve(archive)],
  { cwd: root, maxBuffer: 16 * 1024 * 1024 },
);
const minimalSdk = join(root, "sdk");
await mkdir(minimalSdk);
// Standard consumers cannot see the NDK and cannot invoke native compilers.
for (const name of ["platforms", "build-tools", "licenses"])
  await symlink(join(sdk, name), join(minimalSdk, name), "dir");
const guards = join(root, "native-tools-disabled");
await mkdir(guards);
for (const name of ["cmake", "ninja", "clang", "clang++", "gcc", "g++"]) {
  const guard = join(guards, name);
  await writeFile(
    guard,
    '#!/bin/sh\necho "Native compilation is forbidden in a consumer build" >&2\nexit 99\n',
  );
  await chmod(guard, 0o755);
}
const environment = {
  ...process.env,
  PATH: guards + ":" + process.env.PATH,
  ANDROID_HOME: minimalSdk,
  ANDROID_SDK_ROOT: minimalSdk,
  ANDROID_NDK_HOME: "",
  ANDROID_NDK_ROOT: "",
};
const run = async (command, args) => {
  const result = await execute(command, args, {
    cwd: root,
    env: environment,
    maxBuffer: 32 * 1024 * 1024,
  });
  return result.stdout;
};
const cli = join(root, "node_modules/muon-ui/dist/cli.cjs");
const muon = async (args) =>
  JSON.parse(await run(process.execPath, [cli, ...args, "--json"]));
const applicationId = "dev.muon.e2e.publicconsumer";
const includePlugin = process.argv.includes("--plugins");
const validate = process.argv.includes("--validate");
const inspect = async (path, variant, code, version) => {
  const { stdout } = await execute(process.execPath, [
    join(repository, "ui/android-test/verify-consumer-apk.mjs"),
    path,
    variant,
    String(code),
    version,
    ...(includePlugin ? ["--plugins"] : []),
  ]);
  console.log(stdout.trim());
};
const config = {
  android: {
    applicationId,
    label: "Packaged Muon Notes",
    versionCode: 1,
    icon: "icon.png",
    permissions: ["android.permission.INTERNET"],
  },
  config: { channel: "package-consumer" },
  plugin: {
    mode: "simple",
    plugins: [
      {
        name: "internal",
        allow: [
          "muon.browser.reload",
          "muon.environments.getRuntimeInfo",
          "muon.environments.getConfigValues",
          "muon.fs.exists",
          "muon.fs.readTextFile",
          "muon.fs.writeTextFile",
        ],
      },
    ],
  },
};
if (includePlugin) {
  config.android.plugins = [];
  const fixtures = [
    {
      stem: "alpha",
      namespace: "alpha",
      functions: ["alphaName", "alphaAdd", "alphaConfig"],
      allow: ["alphaAdd", "alphaConfig"],
      config: { "alpha.config": "consumer-registry" },
    },
    {
      stem: "types",
      namespace: "types",
      functions: [
        "echoBool",
        "echoI8",
        "echoU8",
        "echoI16",
        "echoU16",
        "echoI32",
        "echoU32",
        "echoI64",
        "echoU64",
        "echoF32",
        "echoF64",
        "echoPointer",
        "echoString",
        "returnNullString",
        "returnNullPointer",
        "stringNullCallbackRoundtrip",
        "i64CallbackRoundtrip",
        "u64CallbackRoundtrip",
        "pointerCallbackRoundtrip",
        "bufferChecksum",
        "transformBuffer",
        "mutateBufferCopy",
        "returnNormalBuffer",
        "returnSharedBuffer",
        "bufferCallbackRoundtrip",
        "returnBufferFunction",
        "pointerBitSize",
        "returnVoid",
        "rejectValue",
        "resolveAsync",
        "resolveTwice",
      ],
      allow: ["buffer*", "returnBufferFunction", "echoI64"],
      config: {},
    },
    {
      stem: "recursive_functions",
      namespace: "recursiveFunctions",
      functions: [
        "recursiveInvoke",
        "recursiveReturnFunction",
        "recursiveFunctionArgRoundtrip",
        "recursiveBufferReturnFunction",
      ],
      allow: ["*"],
      config: {},
    },
  ];
  for (const fixture of fixtures) {
    const libraries = {};
    const sha256 = {};
    const soname = "libmuon_test_plugin_" + fixture.stem + ".so";
    for (const abi of ["arm64-v8a", "x86_64"]) {
      const destination = join(root, "plugins", abi);
      await mkdir(destination, { recursive: true });
      const library = join(destination, soname);
      await copyFile(
        join(repository, "core/android-test/.build/plugins", abi, soname),
        library,
      );
      libraries[abi] = library;
      sha256[abi] = createHash("sha256")
        .update(await readFile(library))
        .digest("hex");
    }
    const metadata = join(root, "plugins", fixture.stem + ".json");
    await writeFile(
      metadata,
      JSON.stringify({
        schemaVersion: 1,
        functions: fixture.functions.map(
          (name) => "muon.test." + fixture.namespace + "." + name,
        ),
        sha256,
      }),
    );
    config.android.plugins.push({
      name: "consumer_" + fixture.stem,
      soname,
      libraries,
      // Exercise the optional catalog and cross-plugin allow rules in simple mode.
      ...(validate || fixture.stem !== "alpha" ? { metadata } : {}),
    });
    config.plugin.plugins.push({
      name: "consumer_" + fixture.stem,
      allow: [
        ...fixture.allow.map(
          (name) => "muon.test." + fixture.namespace + "." + name,
        ),
        ...(!validate && fixture.stem === "alpha" ? ["muon.fs.unlink"] : []),
      ],
      config: fixture.config,
    });
  }
}
if (validate) {
  config.plugin.mode = "validate";
  for (const plugin of config.plugin.plugins) {
    plugin.imports =
      plugin.name === "internal"
        ? ["browser", "environments", "fs"].map((namespace) => ({
            sources: ["main.ts"],
            allow: plugin.allow.filter((path) =>
              path.startsWith("muon." + namespace + "."),
            ),
          }))
        : [{ sources: ["main.ts"], allow: plugin.allow }];
    delete plugin.allow;
  }
}
await copyFile(join(repository, "images/muon-256.png"), join(root, "icon.png"));
await writeFile(join(root, "muon.json"), JSON.stringify(config));
await writeFile(
  join(root, "tsconfig.json"),
  JSON.stringify({
    compilerOptions: {
      target: "ES2022",
      module: "ESNext",
      moduleResolution: "Bundler",
      strict: true,
      noEmit: true,
      lib: ["ES2022", "DOM"],
      skipLibCheck: true,
    },
    include: ["main.ts", "vite.config.ts", "plugins.d.ts"],
  }),
);
await writeFile(
  join(root, "plugins.d.ts"),
  `
declare module 'muon:test.alpha' {
  export const alphaAdd: (a: number, b: number) => Promise<number>;
  export const alphaConfig: () => Promise<string>;
}
declare module 'muon:test.types' {
  export const echoI64: (value: number) => Promise<number>;
  export const bufferChecksum: (value: ArrayBuffer) => Promise<number>;
  export const bufferCallbackRoundtrip: (callback: (buffer: ArrayBuffer) => ArrayBuffer) => Promise<number>;
  export const returnBufferFunction: () => Promise<MuonPluginFunctionProxy<[ArrayBuffer], ArrayBuffer>>;
}
declare module 'muon:test.recursiveFunctions' {
  export const recursiveFunctionArgRoundtrip: (callback: (proxy: MuonPluginFunctionProxy<[number], number>) => MuonPluginFunctionProxy<[number], number>) => Promise<number>;
  export const recursiveInvoke: (callback: (value: number) => number | Promise<number>) => Promise<number>;
  export const recursiveBufferReturnFunction: (callback: (outer: ArrayBuffer) => (inner: ArrayBuffer) => ArrayBuffer) => Promise<ArrayBuffer>;
}
`,
);
await writeFile(
  join(root, "vite.config.ts"),
  `import { defineConfig } from 'vite';\nimport muon from 'muon-ui/vite';\nimport prettierMax from 'prettier-max';\nexport default defineConfig({ base: '/notes/', plugins: [prettierMax(), muon({ ${validate ? "" : "pluginAccess: false,"} build: { targets: ['android'] } })], build: { target: 'es2022' } });\n`,
);
await writeFile(
  join(root, "index.html"),
  `<!doctype html><html><head><meta name="viewport" content="width=device-width, initial-scale=1"><title>Muon Notes</title><style>body{font-family:sans-serif;margin:48px 20px;background:#f3f6fb;color:#102338}h1{font-size:28px}button{font-size:20px;display:block;margin:20px 0;padding:16px}output{display:block;margin:20px 0;font-size:18px}</style></head><body><h1>Packaged Muon Notes</h1><output id="status">Starting</output><output id="saved">Reading</output><output id="generation"></output><output id="plugin"></output><output id="policy"></output><output id="version"></output><button id="save">Save note</button><button id="reload">Reload page</button><script type="module" src="/main.ts"></script></body></html>`,
);
if (validate) {
  const htmlPath = join(root, "index.html");
  await writeFile(
    htmlPath,
    (await readFile(htmlPath, "utf8")).replace(
      "</head>",
      `<script>
    const originalCall = globalThis.__muon_plugin_call;
    globalThis.__muon_plugin_call = async (id, path, args) => {
      if (path === 'muon.environments.getRuntimeInfo') globalThis.observedCapability = id;
      return await originalCall(id, path, args);
    };
    globalThis.delegatedCall = async (id, path, args) => await originalCall(id, path, args);
  </script></head>`,
    ),
  );
}
await writeFile(
  join(root, "main.ts"),
  `import type {} from 'muon-ui';
${
  includePlugin && validate
    ? `import * as alpha from 'muon:test.alpha';
import * as types from 'muon:test.types';
import * as recursive from 'muon:test.recursiveFunctions';`
    : `const test = Reflect.get(globalThis, 'muon')?.test;
const alpha = test?.alpha as typeof import('muon:test.alpha');
const types = test?.types as typeof import('muon:test.types');
const recursive = test?.recursiveFunctions as typeof import('muon:test.recursiveFunctions');`
}
${
  validate
    ? `import * as browser from 'muon:browser';
import * as environments from 'muon:environments';
import * as fs from 'muon:fs';
const api = { browser, environments, fs };
if (Reflect.get(globalThis, 'muon') !== undefined) throw new Error('validate exposed simple globals');`
    : "const api = window.muon;"
}
const status = document.querySelector<HTMLOutputElement>('#status')!;
const saved = document.querySelector<HTMLOutputElement>('#saved')!;
const path = 'note.txt';
const generation = Number(sessionStorage.getItem('generation') ?? '0') + 1;
sessionStorage.setItem('generation', String(generation));
document.querySelector<HTMLOutputElement>('#generation')!.textContent = 'Page loads: ' + generation;
try {
  const runtime = await api.environments.getRuntimeInfo();
  if (runtime.backend !== 'android-webview') throw new Error('Expected Android backend');
  document.querySelector<HTMLOutputElement>('#version')!.textContent = 'Version: ' + runtime.applicationVersion;
  if (${includePlugin}) {
    if (Reflect.get(alpha, 'alphaName') !== undefined) throw new Error('A denied plugin function was exposed');
    if (await types.echoI64(9007199254740991) !== 9007199254740991) throw new Error('i64 changed');
    if (await types.bufferChecksum(Uint8Array.from([1, 2, 3, 4]).buffer) !== 10) throw new Error('buffer argument changed');
    if (await types.bufferCallbackRoundtrip((bytes) => {
      if (Array.from(new Uint8Array(bytes)).join(',') !== '7,8,9,10') throw new Error('callback buffer changed');
      return Uint8Array.from([101, 102, 103, 104]).buffer;
    }) !== 1) throw new Error('callback result changed');
    const bufferProxy = await types.returnBufferFunction();
    if (Array.from(new Uint8Array(await bufferProxy(Uint8Array.from([1, 2, 3, 4, 5, 6, 7, 8]).buffer))).join(',') !== '18,19,20,21,22,23,24,25') throw new Error('proxy buffer changed');
    bufferProxy.release();
    bufferProxy.release();
    let released = false;
    try { await bufferProxy(new ArrayBuffer(4)); } catch (error) { if (!/released/i.test(String(error))) throw error; released = true; }
    if (!released) throw new Error('released proxy executed');
    let callbackProxy: MuonPluginFunctionProxy<[number], number> | undefined;
    await recursive.recursiveFunctionArgRoundtrip((proxy) => { callbackProxy = proxy; return proxy; });
    if (callbackProxy === undefined || await callbackProxy(41) !== 42) throw new Error('function callback failed');
    callbackProxy.release();
    const recursiveBuffer = await recursive.recursiveBufferReturnFunction((outer) => {
      if (Array.from(new Uint8Array(outer)).join(',') !== '12,13,14,15') throw new Error('outer callback buffer changed');
      return (inner) => {
        if (Array.from(new Uint8Array(inner)).join(',') !== '21,22,23,24') throw new Error('inner callback buffer changed');
        return Uint8Array.from([201, 202, 203, 204]).buffer;
      };
    });
    if (Array.from(new Uint8Array(recursiveBuffer)).join(',') !== '31,32,33,34') throw new Error('recursive result changed');
    document.querySelector<HTMLOutputElement>('#plugin')!.textContent = 'Plugin: ' + await alpha.alphaAdd(3, 4) + ':' + await alpha.alphaConfig() + ':blocked';
  }
  if (Reflect.get(api.fs, 'unlink') !== undefined) throw new Error('A denied built-in was exposed');
  const rawCall = Reflect.get(globalThis, '__muon_plugin_call') as (id: string, path: string, args: unknown[]) => Promise<unknown>;
  let denied = false;
  try { await rawCall('fs-capability', 'muon.fs.unlink', [path]); }
  catch (error) { if (!/not allowed|capability|Unknown muon plugin function/i.test(String(error))) throw error; denied = true; }
  if (!denied) throw new Error('The native policy accepted a denied built-in');
  if (${includePlugin && !validate}) {
    const probe = 'policy-probe.txt';
    await api.fs.writeTextFile(probe, 'preserved', 'utf8');
    let blocked = false;
    try { await rawCall('consumer_alpha', 'muon.fs.unlink', [probe]); }
    catch (error) { if (!/not allowed|Unknown muon plugin function/i.test(String(error))) throw error; blocked = true; }
    if (!blocked || !await api.fs.exists(probe)) throw new Error('An external capability bypassed the internal allow policy');
  }
  if (${validate}) {
    const id = Reflect.get(globalThis, 'observedCapability') as string;
    const delegated = Reflect.get(globalThis, 'delegatedCall') as typeof rawCall;
    const shared = await delegated(id, 'muon.environments.getConfigValues', []) as Record<string, string>;
    if (shared.channel !== 'package-consumer') throw new Error('Same-page capability delegation failed');
    let refusedPath = false;
    try { await delegated(id, 'muon.fs.exists', [path]); }
    catch (error) { if (!/not allowed/i.test(String(error))) throw error; refusedPath = true; }
    if (!refusedPath) throw new Error('Native policy accepted a path outside the valid capability');
    if (${includePlugin}) {
      let refusedPlugin = false;
      try { await rawCall('consumer_alpha', 'muon.test.alpha.alphaAdd', [1, 2]); }
      catch (error) { if (!/capability/i.test(String(error))) throw error; refusedPlugin = true; }
      if (!refusedPlugin) throw new Error('Native policy accepted the simple plugin ID');
    }
    for (const id of ['', 'unknown-id', 'environment-capability', 'browser-capability']) {
      let refused = false;
      try { await rawCall(id, 'muon.environments.getConfigValues', []); }
      catch (error) { if (!/capability/i.test(String(error))) throw error; refused = true; }
      if (!refused) throw new Error('Native policy accepted the invalid ID: ' + id);
    }
  }
  document.querySelector<HTMLOutputElement>('#policy')!.textContent = 'Policy: blocked';
  const config = await api.environments.getConfigValues();
  const data = await api.fs.exists(path) ? await api.fs.readTextFile(path, 'utf8') : 'empty';
  saved.textContent = 'Stored: ' + data;
  status.textContent = 'ready:' + runtime.backend + ':' + config.channel;
} catch (error) { status.textContent = 'failed:' + String(error); }
document.querySelector<HTMLButtonElement>('#save')!.onclick = async () => {
  try { await api.fs.writeTextFile(path, 'saved-on-device', 'utf8'); saved.textContent = 'Stored: ' + await api.fs.readTextFile(path, 'utf8'); }
  catch (error) { status.textContent = 'failed:' + String(error); }
};
document.querySelector<HTMLButtonElement>('#reload')!.onclick = async () => { await api.browser.reload(); };
`,
);
const prepared = await muon(["prepare", "--target", "android"]);
assert.equal(prepared.target, "android");
assert.equal(prepared.sdkPath, minimalSdk);
await run(process.execPath, [
  join(root, "node_modules/typescript/bin/tsc"),
  "--noEmit",
]);
const build = await muon(["build", "--target", "android"]);
const result = build.targets[0];
assert.equal(result.target, "android");
assert.equal(result.signing, "debug");
assert.equal(result.applicationId, applicationId);
const firstBytes = await readFile(result.packagePath);
await inspect(result.packagePath, "debug", 1, "1.0.0");
console.log("Public CLI debug APK built");
await run(process.execPath, [
  join(root, "node_modules/vite/bin/vite.js"),
  "build",
]);
if (!validate)
  assert.deepEqual(
    await readFile(result.packagePath),
    firstBytes,
    "CLI and direct Vite build must generate the same APK",
  );
const adb = async (args) =>
  (
    await execute(join(sdk, "platform-tools/adb"), ["-s", serial, ...args], {
      maxBuffer: 16 * 1024 * 1024,
    })
  ).stdout;
const start = async () => {
  await adb(["shell", "am", "force-stop", applicationId]);
  await adb([
    "shell",
    "am",
    "start",
    "-W",
    "-n",
    `${applicationId}/dev.muon.runtime.MuonAppActivity`,
  ]);
};
// Keep one accessibility connection alive while waiting for actual app state.
// The observer APK never adds a test bridge or library to the consumer APK.
await execute(
  join(repository, "core/android-test/android/gradlew"),
  [":observer:assembleDebug", ":observer:assembleDebugAndroidTest"],
  {
    cwd: join(repository, "core/android-test/android"),
    maxBuffer: 16 * 1024 * 1024,
  },
);
for (const apk of [
  "apk/debug/observer-debug.apk",
  "apk/androidTest/debug/observer-debug-androidTest.apk",
]) {
  await adb([
    "install",
    "-r",
    join(repository, "core/android-test/observer/build/outputs", apk),
  ]);
}
let observationCount = 0;
const observe = async (mode, version) => {
  const output = await adb([
    "shell",
    "am",
    "instrument",
    "-w",
    "-r",
    "-e",
    "mode",
    mode,
    "-e",
    "plugin",
    String(includePlugin),
    "-e",
    "version",
    version,
    "dev.muon.e2e.observer.test/androidx.test.runner.AndroidJUnitRunner",
  ]);
  await writeFile(
    join(
      root,
      "instrumentation-" +
        ++observationCount +
        "-" +
        version +
        "-" +
        mode +
        ".log",
    ),
    output,
  );
  assert.match(output, /OK \(1 test\)/u, output);
  assert.doesNotMatch(output, /FAILURES|INSTRUMENTATION_FAILED/u);
};
if (
  (await adb(["shell", "pm", "list", "packages", applicationId]))
    .split(/\r?\n/u)
    .includes("package:" + applicationId)
)
  await adb(["uninstall", applicationId]);
await adb(["install", result.packagePath]);
await start();
await observe("operate", "1.0.0");
await start();
await observe("verify", "1.0.0");
await assert.rejects(
  async () => await muon(["pack", "--target", "android", "--type", "apk"]),
  /android.signing/u,
);
const keyDirectory = await mkdtemp(join(tmpdir(), "muon-android-test-key-"));
const keystore = join(keyDirectory, "application.p12");
environment.MUON_ANDROID_TEST_STORE_PASSWORD = randomBytes(24).toString("hex");
await run("keytool", [
  "-genkeypair",
  "-keystore",
  keystore,
  "-alias",
  "release",
  "-keyalg",
  "RSA",
  "-keysize",
  "2048",
  "-validity",
  "3650",
  "-dname",
  "CN=Muon Android E2E",
  "-storepass:env",
  "MUON_ANDROID_TEST_STORE_PASSWORD",
  "-keypass:env",
  "MUON_ANDROID_TEST_STORE_PASSWORD",
]);
await chmod(keystore, 0o600);
config.android.signing = {
  keystore,
  keyAlias: "release",
  storePasswordEnv: "MUON_ANDROID_TEST_STORE_PASSWORD",
};
config.android.versionCode = 2;
await writeFile(join(root, "muon.json"), JSON.stringify(config));
const release = await muon(["pack", "--target", "android", "--type", "apk"]);
assert.equal(release.targets[0].signing, "release");
assert.match(release.targets[0].certificateSha256, /^[a-f0-9]{64}$/u);
const releasePath = release.artifacts[0].path;
await inspect(releasePath, "release", 2, "1.0.0");
console.log("Signed release APK built and verified");
const certificate = await run(join(sdk, "build-tools/36.0.0/apksigner"), [
  "verify",
  "--print-certs",
  releasePath,
]);
assert.ok(certificate.includes(release.targets[0].certificateSha256));
const entries = await run("unzip", ["-Z1", releasePath]);
assert.doesNotMatch(
  entries,
  /\.(?:jks|keystore|p12)$|MuonJavaScriptRuntime|quickjs/imu,
);
const metadata = await readFile(releasePath + ".json", "utf8");
assert.ok(
  !metadata.includes(keystore) &&
    !metadata.includes(environment.MUON_ANDROID_TEST_STORE_PASSWORD),
);
// The previous installation is this driver's debug fixture, which has a different key.
await adb(["uninstall", applicationId]);
await adb(["install", releasePath]);
await start();
await observe("operate", "1.0.0");
await start();
await observe("verify", "1.0.0");
config.android.versionCode = 3;
config.android.versionName = "1.0.1";
await writeFile(join(root, "muon.json"), JSON.stringify(config));
const update = await muon(["pack", "--target", "android", "--type", "apk"]);
assert.equal(
  update.targets[0].certificateSha256,
  release.targets[0].certificateSha256,
);
assert.equal(update.targets[0].versionCode, 3);
await inspect(update.artifacts[0].path, "release", 3, "1.0.1");
await adb(["install", "-r", update.artifacts[0].path]);
await start();
await observe("verify", "1.0.1");
const installedComponents = await readdir(minimalSdk);
assert.ok(
  !installedComponents.includes("ndk") &&
    !installedComponents.includes("cmake"),
  "Consumer must not install native toolchains",
);
console.log(
  "Release installation, operation, restart and data-preserving update passed",
);
const sha256 = async (path) =>
  createHash("sha256")
    .update(await readFile(path))
    .digest("hex");
await writeFile(
  join(root, "release-result.json"),
  JSON.stringify(
    {
      serial,
      release,
      update,
      releaseSha256: await sha256(releasePath),
      updateSha256: await sha256(update.artifacts[0].path),
    },
    null,
    2,
  ),
);
const screenshot = await execute(
  join(sdk, "platform-tools/adb"),
  ["-s", serial, "exec-out", "screencap", "-p"],
  { encoding: "buffer", maxBuffer: 16 * 1024 * 1024 },
);
await writeFile(join(root, "screen.png"), screenshot.stdout);
await writeFile(
  join(root, "result.json"),
  JSON.stringify({ ...result, serial, prepare: prepared }, null, 2),
);
if (validate && includePlugin) {
  const goodApk = join(root, "valid-release.apk");
  await copyFile(update.artifacts[0].path, goodApk);
  const catalogPath = config.android.plugins[0].metadata;
  const originalCatalog = await readFile(catalogPath, "utf8");
  const originalConfig = JSON.stringify(config);
  const catalog = JSON.parse(originalCatalog);
  catalog.functions.push("muon.test.alpha.ghost");
  config.plugin.plugins
    .find((entry) => entry.name === "consumer_alpha")
    .imports[0].allow.push("muon.test.alpha.ghost");
  await writeFile(catalogPath, JSON.stringify(catalog));
  await writeFile(join(root, "muon.json"), JSON.stringify(config));
  try {
    const invalid = await muon([
      "pack",
      "--target",
      "android",
      "--type",
      "apk",
    ]);
    await copyFile(
      invalid.artifacts[0].path,
      join(root, "invalid-catalog.apk"),
    );
    await adb(["install", "-r", invalid.artifacts[0].path]);
    await start();
    await observe("catalog-mismatch", "1.0.1");
  } finally {
    await writeFile(catalogPath, originalCatalog);
    await writeFile(join(root, "muon.json"), originalConfig);
    await copyFile(goodApk, update.artifacts[0].path);
    await adb(["install", "-r", goodApk]);
  }
  await start();
  await observe("verify", "1.0.1");
  console.log("Native catalog mismatch rejected; valid APK restored");
}
console.log(
  `Packaged Android application: PASS (${serial}, ${update.artifacts[0].path})`,
);
