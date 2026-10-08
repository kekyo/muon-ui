import { execFile } from 'node:child_process';
import { constants } from 'node:fs';
import { access } from 'node:fs/promises';
import { delimiter, isAbsolute, join } from 'node:path';
import { promisify } from 'node:util';
import {
  getNativeBuildEnvironment,
  getSdkDirectory,
  type NativeBuildEnvironmentOptions,
} from './native.js';
import { getPluginTarget, muonPluginNdkVersion } from './targets.js';

/** A missing or incompatible prerequisite and its installation guidance. */
export interface PluginPrerequisiteIssue {
  /** Stable diagnostic identifier. */ readonly id: string;
  /** Requirement and location that could not be used. */ readonly message: string;
  /** Installation command or precise setup instruction. */ readonly install: string;
  /** Official documentation describing the prerequisite. */ readonly documentation: string;
}

/** Native authoring requirements; prebuilt consumers do not use this API. */
export interface PluginToolchainOptions extends NativeBuildEnvironmentOptions {
  /** Implementation language; defaults to C++20. */ language?: 'c' | 'c++';
  /** Whether to use cardio; defaults to true for C++. */ cardio?: boolean;
  /** Native build system, defaults to make. */ buildSystem?:
    'make' | 'cmake' | 'custom';
  /** Additional executables required by a custom build. */ tools?: readonly string[];
}

/** Complete prerequisite report; diagnostics never install software. */
export interface PluginToolchainReport {
  /** Whether every selected prerequisite is available. */ readonly ready: boolean;
  /** All missing requirements, rather than only the first failure. */ readonly issues: readonly PluginPrerequisiteIssue[];
  /** Environment to pass to the native build when ready. */ readonly environment: NodeJS.ProcessEnv;
}

const execute = promisify(execFile);
const exists = async (path: string, mode: number): Promise<boolean> => {
  try {
    await access(path, mode);
    return true;
  } catch {
    return false;
  }
};
const findExecutable = async (
  name: string,
  environment: NodeJS.ProcessEnv
): Promise<string | undefined> => {
  if (isAbsolute(name))
    return (await exists(name, constants.X_OK)) ? name : undefined;
  for (const directory of (environment.PATH ?? '')
    .split(delimiter)
    .filter(Boolean)) {
    for (const suffix of process.platform === 'win32'
      ? ['', '.exe', '.cmd']
      : ['']) {
      const path = join(directory, name + suffix);
      if (await exists(path, constants.X_OK)) return path;
    }
  }
  return undefined;
};

/**
 * Checks selected authoring prerequisites and gathers actionable diagnostics.
 * @param options - Target, language, build system and environment.
 * @returns All missing tools, SDK inputs and target libraries with install guidance.
 * @remarks No compiler is launched and no software or SDK license is installed.
 */
export const diagnosePluginToolchain = async (
  options: PluginToolchainOptions
): Promise<PluginToolchainReport> => {
  const target = getPluginTarget(options.target);
  const sdk = options.sdkDirectory ?? getSdkDirectory();
  const language = options.language ?? 'c++';
  const cardio = options.cardio ?? language === 'c++';
  const base = options.environment ?? process.env;
  const android =
    options.androidSdkPath ?? base.ANDROID_HOME ?? base.ANDROID_SDK_ROOT;
  const environment = await getNativeBuildEnvironment({
    ...options,
    androidSdkPath: android ?? join(sdk, '.missing-android-sdk'),
  });
  const issues: PluginPrerequisiteIssue[] = [];
  const add = (
    id: string,
    message: string,
    install: string,
    documentation: string
  ): void => {
    issues.push({ id, message, install, documentation });
  };
  const sdkDocs =
    'https://github.com/kekyo/muon-ui/blob/main/docs/en/muon-plugin-develop.md';
  const compilerPackage =
    target.os === 'windows'
      ? 'mingw-w64'
      : target.target === 'linux-arm64'
        ? 'g++-aarch64-linux-gnu'
        : target.target === 'linux-armhf'
          ? 'g++-arm-linux-gnueabihf'
          : 'build-essential';
  if (
    target.os === 'android' &&
    (!android ||
      !(await exists(
        join(android, 'ndk', muonPluginNdkVersion, 'source.properties'),
        constants.R_OK
      )))
  ) {
    add(
      'ndk',
      `Android NDK ${muonPluginNdkVersion} is missing at ${android ?? 'ANDROID_HOME (unset)'}.`,
      `Install Android command-line tools, then run sdkmanager "ndk;${muonPluginNdkVersion}" and set ANDROID_HOME.`,
      'https://developer.android.com/studio/projects/install-ndk'
    );
  }
  const tools: [string, string, string, string][] = [
    [
      'cc',
      environment.CC!,
      `sudo apt install ${compilerPackage}`,
      'https://gcc.gnu.org/install/',
    ],
    ...(language === 'c++'
      ? [
          [
            'cxx',
            environment.CXX!,
            `sudo apt install ${compilerPackage}`,
            'https://gcc.gnu.org/install/',
          ] as [string, string, string, string],
        ]
      : []),
    [
      'pkg-config',
      'pkg-config',
      'sudo apt install pkg-config',
      'https://www.freedesktop.org/wiki/Software/pkg-config/',
    ],
  ];
  const build = options.buildSystem ?? 'make';
  if (build === 'make')
    tools.push([
      'make',
      'make',
      'sudo apt install make',
      'https://www.gnu.org/software/make/',
    ]);
  if (build === 'cmake')
    tools.push([
      'cmake',
      'cmake',
      'Install CMake 3.24 or later (sudo apt install cmake).',
      'https://cmake.org/download/',
    ]);
  for (const name of options.tools ?? [])
    tools.push([
      `tool:${name}`,
      name,
      `Install ${name} and add its executable directory to PATH.`,
      sdkDocs,
    ]);
  for (const [id, name, install, documentation] of tools) {
    if (!(await findExecutable(name, environment))) {
      if (target.os === 'android' && (id === 'cc' || id === 'cxx')) {
        if (!issues.some((issue) => issue.id === 'ndk'))
          add(
            id,
            `NDK compiler is missing: ${name}`,
            `Reinstall sdkmanager "ndk;${muonPluginNdkVersion}" for the build host.`,
            'https://developer.android.com/ndk/downloads'
          );
      } else
        add(
          id,
          `Required executable was not found: ${name}. Searched PATH=${environment.PATH ?? ''}`,
          install,
          documentation
        );
    }
  }
  const required = [
    'include/muon_plugin_api.h',
    `native/${target.target}/lib/pkgconfig/muon-plugin.pc`,
  ];
  if (cardio)
    required.push(
      'include/cardio.h',
      `native/${target.target}/lib/${target.cardioLinkLibrary}`,
      `native/${target.target}/lib/pkgconfig/muon-plugin-cardio.pc`
    );
  for (const path of required)
    if (!(await exists(join(sdk, path), constants.R_OK)))
      add(
        `sdk:${path}`,
        `SDK input is missing: ${join(sdk, path)}`,
        `Install a complete muon-plugin-sdk package supporting ${target.target} using npm install -D muon-plugin-sdk.`,
        sdkDocs
      );
  if (options.sysroot && !(await exists(options.sysroot, constants.R_OK)))
    add(
      'sysroot',
      `Target sysroot does not exist: ${options.sysroot}`,
      `Install the ${target.target} development sysroot and set its absolute path.`,
      'https://gcc.gnu.org/onlinedocs/gcc/Directory-Options.html'
    );
  const pkg = await findExecutable('pkg-config', environment);
  if (cardio && pkg)
    for (const dependency of target.packages) {
      try {
        await execute(pkg, ['--exists', dependency], { env: environment });
      } catch {
        const suffix =
          target.target === 'linux-arm64'
            ? ':arm64'
            : target.target === 'linux-armhf'
              ? ':armhf'
              : '';
        add(
          dependency.split(' ')[0]!,
          `Target development package ${dependency} is unavailable for ${target.target}. PKG_CONFIG_LIBDIR=${environment.PKG_CONFIG_LIBDIR ?? '(host defaults)'}`,
          `Install the target development libraries: sudo apt install libglib2.0-dev${suffix}. For a cross target, configure its multiarch pkg-config directory or sysroot.`,
          'https://docs.gtk.org/glib/building.html'
        );
      }
    }
  return { ready: issues.length === 0, issues, environment };
};

/**
 * Formats prerequisites for a terminal or a build error.
 * @param report - Result of diagnosePluginToolchain.
 * @returns A readable list of all requirements and installation instructions.
 */
export const formatPluginToolchainReport = (
  report: PluginToolchainReport
): string =>
  report.ready
    ? 'Native plugin prerequisites are available.'
    : report.issues
        .map(
          (issue) =>
            `${issue.message}\n  ${issue.install}\n  ${issue.documentation}`
        )
        .join('\n\n');
