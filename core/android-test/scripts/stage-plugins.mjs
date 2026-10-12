// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { copyFile, mkdir, readFile, rm } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { normalizeAndroidPluginRegistry } from './android-plugin-registry.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const registry = normalizeAndroidPluginRegistry(
  JSON.parse(await readFile(join(root, 'android-plugins.json'), 'utf8'))
);
const destination = join(root, '.build/plugins');
await rm(destination, { recursive: true, force: true });
for (const abi of ['arm64-v8a', 'x86_64']) {
  await mkdir(join(destination, abi), { recursive: true });
  for (const plugin of registry.plugins) {
    await copyFile(
      join(
        root,
        'android/app/build/intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib',
        abi,
        plugin.soname
      ),
      join(destination, abi, plugin.soname)
    );
  }
}
console.log(`Staged Android test plugins at ${destination}`);
