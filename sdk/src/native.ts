import { copyFile, mkdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, delimiter, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { runScriptOnceToText, type FunCityLogEntry } from 'funcity';
import {
  getPluginTarget,
  muonPluginNdkVersion,
  type MuonPluginTarget,
} from './targets.js';

/**
 * Resolves the installed SDK independently of the application's working directory.
 * @returns Absolute SDK package directory.
 */
export const getSdkDirectory = (): string =>
  resolve(dirname(fileURLToPath(import.meta.url)), '..');

/** Inputs for staging link artifacts from an already built matching runtime. */
export interface StageSdkTargetOptions {
  /** Output target. */ target: MuonPluginTarget;
  /** Directory containing the matching cardio runtime. */ runtimeDirectory: string;
  /** Directory containing import libraries, defaults to runtimeDirectory. */ linkDirectory?: string;
  /** Cardio source checkout used by the runtime. */ cardioDirectory: string;
  /** SDK destination; defaults to this package. */ sdkDirectory?: string;
}

/**
 * Renders build files using the shared text processing engine.
 * @param template - Template source path.
 * @param variables - Named values supplied by the producer.
 * @returns Rendered text, or throws on a template diagnostic.
 */
export const renderSdkTemplate = async (
  template: string,
  variables: ReadonlyMap<string, unknown>
): Promise<string> => {
  const logs: FunCityLogEntry[] = [];
  const output = await runScriptOnceToText(await readFile(template, 'utf8'), {
    sourceId: template,
    variables,
    logs,
  });
  if (output === undefined || logs.some((entry) => entry.type === 'error'))
    throw new Error(`Cannot render ${template}: ${JSON.stringify(logs)}`);
  return output;
};

/**
 * Stages public link inputs and relocatable pkg-config files for one runtime.
 * @param options - Matching runtime binaries and dependency headers.
 * @returns Resolves once the target can be used by an independent build.
 * @remarks This producer operation does not compile the runtime or download tools.
 */
export const stageSdkTarget = async (
  options: StageSdkTargetOptions
): Promise<void> => {
  const sdk = options.sdkDirectory ?? getSdkDirectory();
  const target = getPluginTarget(options.target);
  const native = join(sdk, 'native', target.target);
  await mkdir(join(native, 'lib/pkgconfig'), { recursive: true });
  await mkdir(join(sdk, 'include'), { recursive: true });
  await mkdir(join(sdk, 'licenses'), { recursive: true });
  await copyFile(
    join(options.cardioDirectory, 'include/cardio.h'),
    join(sdk, 'include/cardio.h')
  );
  await copyFile(
    join(options.cardioDirectory, 'LICENSE'),
    join(sdk, 'licenses/cardio.txt')
  );
  await copyFile(
    join(options.runtimeDirectory, target.cardioLibrary),
    join(native, 'lib', target.cardioLibrary)
  );
  if (target.cardioLinkLibrary !== target.cardioLibrary)
    await copyFile(
      join(
        options.linkDirectory ?? options.runtimeDirectory,
        target.cardioLinkLibrary
      ),
      join(native, 'lib', target.cardioLinkLibrary)
    );
  for (const cardio of [false, true]) {
    const name = cardio ? 'muon-plugin-cardio' : 'muon-plugin';
    const variables = new Map<string, unknown>([
      ['name', name],
      ['requires', cardio ? target.packages.join(', ') : ''],
      [
        'cflags',
        [
          ...target.cflags,
          ...(cardio ? target.definitions.map((value) => '-D' + value) : []),
        ].join(' '),
      ],
      [
        'libs',
        [
          ...(cardio ? ['-L"${libdir}"', '-lcardio'] : []),
          ...target.ldflags,
        ].join(' '),
      ],
    ]);
    await writeFile(
      join(native, 'lib/pkgconfig', name + '.pc'),
      await renderSdkTemplate(join(sdk, 'pkgconfig/plugin.pc.fc'), variables)
    );
  }
  await writeFile(
    join(native, 'target.json'),
    JSON.stringify(target, null, 2) + '\n'
  );
};

/** Inputs for a native compiler environment. */
export interface NativeBuildEnvironmentOptions {
  /** Output ABI; never inferred from the host for cross compilation. */ target: MuonPluginTarget;
  /** Installed SDK root. */ sdkDirectory?: string;
  /** Android SDK location. */ androidSdkPath?: string;
  /** Target sysroot for cross compiler pkg-config lookups. */ sysroot?: string;
  /** Process environment, defaults to process.env. */ environment?: NodeJS.ProcessEnv;
}

/**
 * Creates a target-specific Make/pkg-config environment.
 * @param options - Target and optional toolchain locations.
 * @returns Environment variables for standard or custom native builds.
 * @remarks System pkg-config paths are disabled when using a cross target.
 */
export const getNativeBuildEnvironment = async (
  options: NativeBuildEnvironmentOptions
): Promise<NodeJS.ProcessEnv> => {
  const sdk = options.sdkDirectory ?? getSdkDirectory();
  const descriptor = getPluginTarget(options.target);
  const native = join(sdk, 'native', descriptor.target);
  const environment = { ...(options.environment ?? process.env) };
  let cc = descriptor.cc;
  let cxx = descriptor.cxx;
  if (descriptor.os === 'android') {
    const android =
      options.androidSdkPath ??
      environment.ANDROID_HOME ??
      environment.ANDROID_SDK_ROOT;
    if (!android)
      throw new Error(
        'Android NDK is required. Set ANDROID_HOME or androidSdkPath.'
      );
    const bin = join(
      android,
      'ndk',
      muonPluginNdkVersion,
      'toolchains/llvm/prebuilt/linux-x86_64/bin'
    );
    cc = join(bin, cc);
    cxx = join(bin, cxx);
  }
  const host =
    process.platform === 'win32'
      ? `windows-${process.arch === 'ia32' ? 'i686' : 'amd64'}`
      : `linux-${process.arch === 'x64' ? 'amd64' : process.arch === 'arm' ? 'armhf' : 'arm64'}`;
  const pc = join(native, 'lib/pkgconfig');
  return {
    ...environment,
    CC: cc,
    CXX: cxx,
    MUON_SDK: sdk,
    MUON_TARGET: descriptor.target,
    PKG_CONFIG_PATH: pc,
    ...(options.target === host
      ? {}
      : {
          PKG_CONFIG_LIBDIR: options.sysroot
            ? [
                join(options.sysroot, 'usr/lib/pkgconfig'),
                join(options.sysroot, 'usr/share/pkgconfig'),
              ].join(delimiter)
            : '',
        }),
    ...(options.sysroot === undefined
      ? {}
      : { PKG_CONFIG_SYSROOT_DIR: options.sysroot }),
  };
};
