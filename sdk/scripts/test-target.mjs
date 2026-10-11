// Compile and invoke independent C/C++ projects with both supported build styles.
import { execFileSync } from 'node:child_process';
import {
  copyFile,
  mkdir,
  readFile,
  readdir,
  rm,
  writeFile,
} from 'node:fs/promises';
import { basename, dirname, join, resolve } from 'node:path';
import {
  buildPlugin,
  createPluginProject,
  getNativeBuildEnvironment,
  getPluginTarget,
  getSdkDirectory,
  readPluginArtifact,
} from '../dist/index.mjs';

const target = process.argv[2];
const descriptor = getPluginTarget(target);
const sdk = getSdkDirectory();
const root = resolve(sdk, '.build/target-tests', target);
await rm(root, { recursive: true, force: true });
await mkdir(root, { recursive: true });
const environment = await getNativeBuildEnvironment({ target });
const execute = (command, args, options = {}) =>
  execFileSync(command, args, {
    env: environment,
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
    ...options,
  });
const flags = execute('pkg-config', [
  '--cflags',
  '--libs',
  'muon-plugin-cardio',
])
  .trim()
  .split(/\s+/u);
const driver = join(
  root,
  descriptor.os === 'windows' ? 'driver.exe' : 'driver'
);
execute(environment.CXX, [
  '-std=c++20',
  join(sdk, 'test/fixtures/driver.cpp'),
  '-o',
  driver,
  ...flags,
  ...(descriptor.os === 'windows' ? [] : ['-ldl', '-Wl,-rpath,$ORIGIN']),
]);
await copyFile(
  join(sdk, 'native', target, 'lib', descriptor.cardioLibrary),
  join(root, descriptor.cardioLibrary)
);
if (descriptor.os === 'windows') {
  for (const name of [
    'libstdc++-6.dll',
    'libgcc_s_seh-1.dll',
    'libgcc_s_dw2-1.dll',
    'libgcc_s_sjlj-1.dll',
    'libwinpthread-1.dll',
  ]) {
    const path = execute(environment.CXX, ['-print-file-name=' + name]).trim();
    if (path !== name) await copyFile(path, join(root, name));
  }
}
if (descriptor.os === 'android') {
  const prebuilt = resolve(dirname(environment.CXX), '..');
  const triple =
    target === 'android-arm64-v8a'
      ? 'aarch64-linux-android'
      : 'x86_64-linux-android';
  await copyFile(
    join(prebuilt, 'sysroot/usr/lib', triple, 'libc++_shared.so'),
    join(root, 'libc++_shared.so')
  );
}
for (const language of ['c', 'c++'])
  for (const system of process.argv.includes('--make-only')
    ? ['make']
    : ['make', 'cmake']) {
    const name = `${language === 'c' ? 'plain' : 'cardio'}_${system}`;
    const directory = join(root, name);
    await createPluginProject({
      directory,
      name,
      language,
      buildSystem: system,
    });
    // Exercise a normal native dependency without a muon entry point on every OS.
    if (language === 'c' && system === 'make') {
      const dependency = join(
        directory,
        descriptor.os === 'windows' ? 'vendor.dll' : 'libvendor.so'
      );
      await writeFile(
        join(directory, 'vendor.c'),
        '__attribute__((visibility("default"))) int vendor_increment(int value) { return value + 1; }\n'
      );
      const importLibrary = join(directory, 'libvendor.dll.a');
      execute(environment.CC, [
        '-shared',
        ...descriptor.cflags,
        join(directory, 'vendor.c'),
        '-o',
        dependency,
        ...descriptor.ldflags,
        ...(descriptor.os === 'windows'
          ? ['-Wl,--out-implib,' + importLibrary]
          : ['-Wl,-soname,libvendor.so']),
      ]);
      const source = join(directory, 'plugin.c');
      await writeFile(
        source,
        (await readFile(source, 'utf8'))
          .replace(
            '#include <stddef.h>',
            '#include <stddef.h>\nextern int vendor_increment(int value);'
          )
          .replace('value + 1', 'vendor_increment(value)')
      );
      const configPath = join(directory, 'muon-plugin.build.json');
      const config = JSON.parse(await readFile(configPath, 'utf8'));
      config.build.args = [
        'LDLIBS=' + (descriptor.os === 'windows' ? importLibrary : dependency),
      ];
      config.dependencies = { [target]: [dependency] };
      await writeFile(configPath, JSON.stringify(config));
    }
    const manifest = await buildPlugin({
      configPath: join(directory, 'muon-plugin.build.json'),
      target,
    });
    const artifact = await readPluginArtifact(manifest, [target]);
    const library = artifact.targets[target].library;
    if (process.argv.includes('--compile-only')) {
      process.stdout.write(
        `${target} ${language} ${system}: compiled and verified\n`
      );
      continue;
    }
    let output;
    if (descriptor.os === 'windows') {
      output = execute('wine', [driver, 'Z:' + library.replaceAll('/', '\\')], {
        env: { ...environment, WINEDEBUG: '-all' },
        cwd: root,
      });
    } else if (descriptor.os === 'android') {
      if (!process.env.ANDROID_SERIAL)
        throw new Error(
          'Set ANDROID_SERIAL to a device matching ' +
            target +
            ' or pass --compile-only.'
        );
      const remote = '/data/local/tmp/muon-plugin-sdk-' + target;
      const adb = (args) =>
        execute('adb', ['-s', process.env.ANDROID_SERIAL, ...args]);
      const abi = adb(['shell', 'getprop', 'ro.product.cpu.abi']).trim();
      if ('android-' + abi !== target)
        throw new Error(`Device ABI ${abi} does not match ${target}.`);
      adb(['shell', 'mkdir', '-p', remote]);
      for (const name of await readdir(root))
        if (name === 'driver' || name.endsWith('.so'))
          adb(['push', join(root, name), remote + '/' + name]);
      adb(['push', library, remote + '/' + basename(library)]);
      for (const dependency of artifact.targets[target].dependencies)
        adb(['push', dependency, remote + '/' + basename(dependency)]);
      adb(['shell', 'chmod', '755', remote + '/driver']);
      output = adb([
        'shell',
        `LD_LIBRARY_PATH=${remote} ${remote}/driver ${remote}/${basename(library)}`,
      ]);
    } else
      output = execute(driver, [library], {
        env: { ...environment, LD_LIBRARY_PATH: root },
      });
    if (!output.includes('plugin function and shutdown passed'))
      throw new Error(`Native invocation failed: ${output}`);
    process.stdout.write(`${target} ${language} ${system}: ${output}`);
  }
