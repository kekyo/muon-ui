// Native builds belong to the shared workspace; this fixture owns its cache.
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';

const projectRoot = resolve(
  process.argv[2] ?? fileURLToPath(new URL('..', import.meta.url))
);
execFileSync(
  process.execPath,
  [
    fileURLToPath(
      new URL(
        '../../android/scripts/build-native-dependencies.mjs',
        import.meta.url
      )
    ),
  ],
  {
    stdio: 'inherit',
    env: {
      ...process.env,
      MUON_ANDROID_DEPENDENCY_ROOT: resolve(
        projectRoot,
        'android/.native-dependencies'
      ),
    },
  }
);
