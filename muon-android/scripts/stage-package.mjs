// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { access, chmod, copyFile, cp, mkdir, rm } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { prepareAndroid } from '../dist/lib/index.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repository = resolve(root, '..');
if (process.argv[2] === undefined)
  throw new Error('The package destination is required.');
const destination = resolve(process.argv[2]);
for (const artifact of [
  'dist/maven/dev/muon/runtime/0.1.0/runtime-0.1.0.aar',
  'dist/renderer/renderer.js',
  'dist/lib/index.mjs',
]) {
  await access(join(root, artifact));
}
await rm(destination, { recursive: true, force: true });
await mkdir(destination, { recursive: true });
for (const directory of ['maven', 'renderer', 'lib']) {
  await cp(join(root, 'dist', directory), join(destination, directory), {
    recursive: true,
  });
}
await cp(join(root, 'templates'), join(destination, 'templates'), {
  recursive: true,
});
for (const entry of ['gradlew', 'gradlew.bat', 'gradle']) {
  await cp(join(root, entry), join(destination, 'templates', entry), {
    recursive: true,
  });
}
await chmod(join(destination, 'templates/gradlew'), 0o755);
await copyFile(
  join(root, 'toolchain.json'),
  join(destination, 'toolchain.json')
);
await mkdir(join(destination, 'licenses'));
for (const [name, source] of [
  ['muon', 'LICENSE'],
  ['cardio', 'deps/cardio/LICENSE'],
  ['tra-ffic', 'deps/tra-ffic/LICENSE'],
  ['libffi', 'deps/tra-ffic/deps/libffi/LICENSE'],
  ['sha2', 'deps/sha2/sha2.h'],
]) {
  await copyFile(
    join(repository, source),
    join(destination, 'licenses', `${name}.txt`)
  );
}
const tools = await prepareAndroid({
  componentsDirectory: root,
  sdkPath: undefined,
  environment: process.env,
  prepareGradle: false,
});
await copyFile(
  join(tools.sdkPath, 'ndk/29.0.14206865/NOTICE.toolchain'),
  join(destination, 'licenses/libcxx.txt')
);
console.log(`Staged Muon Android runtime at ${destination}`);
