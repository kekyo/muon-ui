import { execFile } from 'node:child_process';
import { mkdir, readFile, readdir, writeFile } from 'node:fs/promises';
import { basename, dirname, join, resolve } from 'node:path';
import { promisify } from 'node:util';
import { runScriptOnceToText } from 'funcity';
import {
  packagePlugin,
  readPluginArtifact,
  type PackagePluginTargetInput,
} from './artifact.js';
import { getSdkDirectory, renderSdkTemplate } from './native.js';
import {
  diagnosePluginToolchain,
  formatPluginToolchainReport,
  type PluginToolchainOptions,
} from './prerequisites.js';
import {
  getPluginTarget,
  muonPluginAndroidApi,
  muonPluginTargets,
  type MuonPluginTarget,
} from './targets.js';

/** One explicit custom build command, executed without a shell. */
export interface PluginBuildCommand {
  /** Executable path or name. */ readonly command: string;
  /** Arguments; funcity variables source, build, output, sdk and target are available. */ readonly args: readonly string[];
}
/** Source-owned authoring configuration. */
export interface PluginBuildConfig {
  /** Plugin-specific target development dependencies checked before building. */ readonly packages?: PluginToolchainOptions['packages'];
  /** Configuration revision. */ readonly schemaVersion: 1;
  /** Plugin registration name. */ readonly name: string;
  /** C99 or C++20; defaults to C++20. */ readonly language?: 'c' | 'c++';
  /** Uses shared cardio; defaults to true for C++. */ readonly cardio?: boolean;
  /** Complete public function catalog. */ readonly functions: readonly string[];
  /** Targets supported by this source project. */ readonly targets: readonly MuonPluginTarget[];
  /** Source files for the standard Make build, relative to the configuration. */ readonly sources?: readonly string[];
  /** Build system and any explicit custom commands. */ readonly build?: {
    /** Defaults to Make. */ readonly system: 'make' | 'cmake' | 'custom';
    /** Build project directory, relative to this configuration. */ readonly directory?: string;
    /** Explicit custom build commands. */ readonly commands?: readonly PluginBuildCommand[];
    /** Additional executables checked before building. */ readonly tools?: readonly string[];
    /** Additional Make or CMake configuration arguments. */ readonly args?: readonly string[];
  };
  /** Header files relative to this configuration. */ readonly headers?: readonly string[];
  /** Type declarations relative to this configuration. */ readonly types?: readonly string[];
  /** License notices relative to this configuration. */ readonly licenses?: readonly string[];
  /** Ordinary shared libraries to copy; paths may use funcity build variables. */ readonly dependencies?: Partial<
    Record<MuonPluginTarget, readonly string[]>
  >;
  /** Development-only libraries to copy; paths may use funcity build variables. */ readonly linkLibraries?: Partial<
    Record<MuonPluginTarget, readonly string[]>
  >;
}
/** Inputs to generate a minimal independent source project. */
export interface CreatePluginProjectOptions {
  /** Empty destination directory. */ readonly directory: string;
  /** Plugin registration name. */ readonly name: string;
  /** Defaults to C++20 with cardio. */ readonly language?: 'c' | 'c++';
  /** Standard Make or an independent CMake project. */ readonly buildSystem?:
    'make' | 'cmake';
}

/**
 * Creates a runnable C or C++ plugin project with public type declarations.
 * @param options - Source directory, registration name, language and build system.
 * @returns Resolves when all source files have been written.
 */
export const createPluginProject = async (
  options: CreatePluginProjectOptions
): Promise<void> => {
  if (
    !/^[A-Za-z_][A-Za-z0-9_]*$/.test(options.name) ||
    options.name === 'internal'
  )
    throw new Error('Invalid plugin name.');
  await mkdir(options.directory, { recursive: true });
  if ((await readdir(options.directory)).length)
    throw new Error('Plugin project directory must be empty.');
  const language = options.language ?? 'c++';
  const build = options.buildSystem ?? 'make';
  const extension = language === 'c' ? 'c' : 'cpp';
  const variables = new Map<string, unknown>([
    ['name', options.name],
    ['language', language],
    ['extension', extension],
    ['cmakeLanguage', language === 'c' ? 'C' : 'CXX'],
    ['cmakeTarget', language === 'c' ? 'Plugin' : 'Cardio'],
    ['components', language === 'c' ? '' : 'COMPONENTS Cardio'],
  ]);
  for (const file of [
    `plugin.${extension}`,
    build === 'make' ? 'Makefile' : 'CMakeLists.txt',
    'index.d.ts',
  ])
    await writeFile(
      join(options.directory, file),
      await renderSdkTemplate(
        join(getSdkDirectory(), 'templates', file + '.fc'),
        variables
      )
    );
  const config: PluginBuildConfig = {
    schemaVersion: 1,
    name: options.name,
    language,
    functions: [`muon.${options.name}.increment`],
    targets: muonPluginTargets,
    sources: [`plugin.${extension}`],
    build: { system: build },
    types: ['index.d.ts'],
  };
  await writeFile(
    join(options.directory, 'muon-plugin.build.json'),
    JSON.stringify(config, null, 2) + '\n'
  );
  await writeFile(join(options.directory, '.gitignore'), '.muon-plugin/\n');
};

/** Selected source project and output environment. */
export interface BuildPluginOptions extends Omit<
  PluginToolchainOptions,
  'language' | 'cardio' | 'buildSystem' | 'tools'
> {
  /** Explicit source configuration file. */ readonly configPath: string;
  /** Artifact output directory, defaults to .muon-plugin next to the config. */ readonly outputDirectory?: string;
}

/**
 * Builds one explicitly selected target and packages its final outputs.
 * @param options - Source config and toolchain selection.
 * @returns Absolute common artifact manifest path.
 * @remarks Set outputDirectory when referencing a vendor or installed package's
 * configuration. Application integration always supplies an application-owned output.
 */
export const buildPlugin = async (
  options: BuildPluginOptions
): Promise<string> => {
  const configPath = resolve(options.configPath);
  const source = dirname(configPath);
  const config = JSON.parse(
    await readFile(configPath, 'utf8')
  ) as PluginBuildConfig;
  if (
    config.schemaVersion !== 1 ||
    !Array.isArray(config.targets) ||
    !Array.isArray(config.functions) ||
    !/^[A-Za-z_][A-Za-z0-9_]*$/.test(config.name)
  )
    throw new Error(`Invalid plugin build configuration: ${configPath}`);
  if (!config.targets.includes(options.target))
    throw new Error(
      `Plugin ${config.name} does not support ${options.target}.`
    );
  const language = config.language ?? 'c++';
  if (language !== 'c' && language !== 'c++')
    throw new Error('Plugin language must be c or c++.');
  const cardio = config.cardio ?? language === 'c++';
  const system = config.build?.system ?? 'make';
  if (!['make', 'cmake', 'custom'].includes(system))
    throw new Error(`Unsupported plugin build system: ${system}`);
  const workingDirectory = resolve(source, config.build?.directory ?? '.');
  const report = await diagnosePluginToolchain({
    ...options,
    language,
    cardio,
    buildSystem: system,
    ...(config.packages === undefined ? {} : { packages: config.packages }),
    tools: [
      ...(config.build?.tools ?? []),
      ...(config.build?.commands?.map((item) => item.command) ?? []),
    ].map((tool) =>
      tool.includes('/') ? resolve(workingDirectory, tool) : tool
    ),
  });
  if (!report.ready) throw new Error(formatPluginToolchainReport(report));
  const target = getPluginTarget(options.target);
  const outputDirectory = resolve(
    options.outputDirectory ?? join(source, '.muon-plugin')
  );
  const build = join(outputDirectory, '.build', options.target);
  const filename =
    (target.os === 'android' ? 'lib' : '') + config.name + target.extension;
  const output = join(build, filename);
  const sdk = options.sdkDirectory ?? getSdkDirectory();
  await mkdir(build, { recursive: true });
  const variables = new Map<string, unknown>([
    ['source', source],
    ['build', build],
    ['output', output],
    ['sdk', sdk],
    ['target', options.target],
  ]);
  const render = async (value: string): Promise<string> => {
    const rendered = await runScriptOnceToText(value, {
      sourceId: configPath,
      variables,
    });
    if (rendered === undefined)
      throw new Error(`Invalid build argument: ${value}`);
    return rendered;
  };
  const environment: NodeJS.ProcessEnv = {
    ...report.environment,
    MUON_SOURCE: source,
    MUON_BUILD: build,
    MUON_OUTPUT: output,
  };
  const execute = async (
    command: string,
    args: readonly string[]
  ): Promise<void> => {
    try {
      await promisify(execFile)(command, [...args], {
        cwd: workingDirectory,
        env: environment,
        maxBuffer: 16 * 1024 * 1024,
      });
    } catch (error) {
      const failure = error as Error & { stdout?: string; stderr?: string };
      throw new Error(
        `Plugin ${config.name} (${options.target}): ${failure.message}\n${failure.stdout ?? ''}\n${failure.stderr ?? ''}`
      );
    }
  };
  const args: string[] = [];
  for (const value of config.build?.args ?? []) args.push(await render(value));
  if (system === 'make') {
    const sources = config.sources ?? [];
    if (!sources.length)
      throw new Error('Standard Make build requires sources.');
    if (
      [...sources.map((path) => resolve(source, path)), sdk, output].some(
        (path) => /[^A-Za-z0-9_./+-]/.test(path)
      )
    )
      throw new Error(
        'Standard Make paths support letters, digits, _, ., /, + and -; use CMake or a custom build for other paths.'
      );
    await execute('make', [
      '-f',
      join(sdk, 'make/plugin.mk'),
      `SOURCES=${sources.map((path) => resolve(source, path)).join(' ')}`,
      `OUTPUT=${output}`,
      `LANGUAGE=${language}`,
      `CARDIO=${cardio ? 1 : 0}`,
      ...args,
    ]);
  } else if (system === 'cmake') {
    const platform: string[] = [];
    if (target.os === 'android') {
      const ndk = resolve(dirname(environment.CC!), '../../../../..');
      platform.push(
        `-DCMAKE_TOOLCHAIN_FILE=${join(ndk, 'build/cmake/android.toolchain.cmake')}`,
        `-DANDROID_ABI=${options.target.slice(8)}`,
        `-DANDROID_PLATFORM=${muonPluginAndroidApi}`,
        '-DANDROID_STL=c++_shared'
      );
    } else {
      platform.push(
        `-DCMAKE_C_COMPILER=${environment.CC}`,
        `-DCMAKE_CXX_COMPILER=${environment.CXX}`
      );
      if (target.os === 'windows') platform.push('-DCMAKE_SYSTEM_NAME=Windows');
      if (options.sysroot) platform.push(`-DCMAKE_SYSROOT=${options.sysroot}`);
    }
    await execute('cmake', [
      '-S',
      workingDirectory,
      '-B',
      join(build, 'cmake'),
      `-DMuonPlugin_DIR=${join(sdk, 'cmake')}`,
      `-DMUON_PLUGIN_TARGET=${options.target}`,
      `-DMUON_PLUGIN_OUTPUT_DIRECTORY=${build}`,
      `-DMUON_PLUGIN_OUTPUT_NAME=${basename(filename, target.extension)}`,
      '-DCMAKE_BUILD_TYPE=Release',
      ...platform,
      ...args,
    ]);
    await execute('cmake', ['--build', join(build, 'cmake')]);
  } else {
    if (!config.build?.commands?.length)
      throw new Error('Custom plugin builds require explicit commands.');
    for (const command of config.build.commands) {
      const values: string[] = [];
      for (const value of command.args) values.push(await render(value));
      await execute(command.command, values);
    }
  }
  const resources = (paths: readonly string[] | undefined): string[] =>
    (paths ?? []).map((path) => resolve(source, path));
  const renderedPaths = async (
    paths: readonly string[] | undefined
  ): Promise<string[]> => {
    const result: string[] = [];
    for (const path of paths ?? [])
      result.push(resolve(source, await render(path)));
    return result;
  };
  const inputs: Partial<Record<MuonPluginTarget, PackagePluginTargetInput>> =
    {};
  const manifestPath = join(outputDirectory, 'muon-plugin.json');
  // Preserve independently built targets when assembling a multi-target package.
  try {
    const previous = JSON.parse(await readFile(manifestPath, 'utf8'));
    const artifact = await readPluginArtifact(
      manifestPath,
      Object.keys(previous.targets) as MuonPluginTarget[]
    );
    if (
      artifact.manifest.name !== config.name ||
      JSON.stringify(artifact.manifest.functions) !==
        JSON.stringify(config.functions)
    )
      throw new Error(
        'Existing plugin output has a different name or catalog; select a fresh output directory.'
      );
    for (const [key, entry] of Object.entries(artifact.targets)) {
      const metadata = artifact.manifest.targets[key as MuonPluginTarget]!;
      inputs[key as MuonPluginTarget] = {
        library: entry.library,
        dependencies: entry.dependencies,
        linkLibraries: metadata.linkLibraries.map((file) =>
          join(outputDirectory, file.path)
        ),
        language: metadata.runtime.cpp ? 'c++' : 'c',
        cardio: metadata.runtime.cardio,
      };
    }
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error;
  }
  inputs[options.target] = {
    library: output,
    dependencies: await renderedPaths(config.dependencies?.[options.target]),
    linkLibraries: await renderedPaths(config.linkLibraries?.[options.target]),
    language,
    cardio,
  };
  return await packagePlugin({
    outputDirectory,
    name: config.name,
    functions: config.functions,
    headers: resources(config.headers),
    types: resources(config.types),
    licenses: resources(config.licenses),
    targets: inputs,
  });
};
