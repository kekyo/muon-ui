// Native builds belong to the shared workspace; this fixture owns its cache.
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
execFileSync(
  process.execPath,
  [
    fileURLToPath(
      new URL(
        '../../android/scripts/build-native-dependencies.mjs',
        import.meta.url
      )
    ),
    ...process.argv.slice(2),
  ],
  {
    stdio: 'inherit',
    env: {
      ...process.env,
      MUON_ANDROID_DEPENDENCY_ROOT: fileURLToPath(
        new URL('../android/.native-dependencies', import.meta.url)
      ),
    },
  }
);
