// Stage link inputs from the runtimes built by the distribution pipeline.
import { execFileSync } from 'node:child_process';
import { copyFile, mkdir, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import {
  getSdkDirectory,
  getPluginTarget,
  muonPluginTargets,
  stageSdkTarget,
} from '../dist/index.mjs';

const sdk = getSdkDirectory();
const repository = resolve(sdk, '..');
const selected = process.argv.slice(2);
for (const target of selected.length ? selected : muonPluginTargets) {
  const descriptor = getPluginTarget(target);
  let runtimeDirectory = join(repository, 'core/cef/dist-' + target);
  if (descriptor.os === 'android') {
    runtimeDirectory = join(sdk, '.build/packaged', target);
    await mkdir(runtimeDirectory, { recursive: true });
    const archive = join(
      repository,
      'core/android/dist/maven/dev/muon/runtime/0.1.0/runtime-0.1.0.aar'
    );
    await writeFile(
      join(runtimeDirectory, descriptor.cardioLibrary),
      execFileSync('unzip', [
        '-p',
        archive,
        'jni/' + target.slice(8) + '/' + descriptor.cardioLibrary,
      ])
    );
  }
  await stageSdkTarget({
    target,
    runtimeDirectory,
    cardioDirectory: join(repository, 'deps/cardio'),
    linkDirectory: join(
      repository,
      'core/cef/.build/dist',
      target,
      'release/cardio'
    ),
  });
}
await copyFile(join(repository, 'LICENSE'), join(sdk, 'LICENSE'));
console.log('Staged SDK from distribution runtimes.');
