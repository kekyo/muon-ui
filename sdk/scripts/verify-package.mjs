// Verify SDK distribution inputs without running target binaries or compilers.
import { access, readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { join } from 'node:path';
import {
  getSdkDirectory,
  getPluginTarget,
  inspectPluginBinary,
  muonPluginTargets,
  muonPluginCardioHeaderSha256,
} from '../dist/index.mjs';

const sdk = getSdkDirectory();
const targets = process.argv.slice(2);
const selected = targets.length ? targets : muonPluginTargets;
for (const path of [
  'dist/index.mjs',
  'dist/index.cjs',
  'dist/artifact.mjs',
  'dist/artifact.cjs',
  'dist/cli.mjs',
  'dist/types/index.d.ts',
  'include/muon_plugin_api.h',
  'include/cardio.h',
  'make/plugin.mk',
  'cmake/MuonPluginConfig.cmake',
  'src/runtime-profile.json',
  'LICENSE',
  'licenses/cardio.txt',
])
  await access(join(sdk, path));
const hash = createHash('sha256')
  .update(await readFile(join(sdk, 'include/cardio.h')))
  .digest('hex');
if (hash !== muonPluginCardioHeaderSha256)
  throw new Error('Packaged cardio header does not match the runtime profile.');
for (const target of selected) {
  const descriptor = getPluginTarget(target);
  const root = join(sdk, 'native', target);
  const metadata = JSON.parse(
    await readFile(join(root, 'target.json'), 'utf8')
  );
  if (JSON.stringify(metadata) !== JSON.stringify(descriptor))
    throw new Error('Stale SDK target metadata: ' + target);
  for (const name of ['muon-plugin', 'muon-plugin-cardio'])
    await access(join(root, 'lib/pkgconfig', name + '.pc'));
  await access(join(root, 'lib', descriptor.cardioLinkLibrary));
  inspectPluginBinary(
    await readFile(join(root, 'lib', descriptor.cardioLibrary)),
    target
  );
}
console.log('Verified SDK distribution: ' + selected.join(', '));
