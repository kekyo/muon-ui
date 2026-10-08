#!/usr/bin/env node
import { readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { Command, Option } from 'commander';
import {
  buildPlugin,
  createPluginProject,
  type PluginBuildConfig,
} from './authoring.js';
import { packagePlugin, readPluginArtifact } from './artifact.js';
import { getNativeBuildEnvironment } from './native.js';
import {
  diagnosePluginToolchain,
  formatPluginToolchainReport,
  type PluginToolchainOptions,
} from './prerequisites.js';
import { muonPluginTargets, type MuonPluginTarget } from './targets.js';

const targetOption = (): Option =>
  new Option('--target <target>', 'Explicit native output target')
    .choices([...muonPluginTargets])
    .makeOptionMandatory();
const languageOption = (): Option =>
  new Option('--language <language>', 'C99 or C++20 with cardio')
    .choices(['c', 'c++'])
    .default('c++');
const platformOptions = (command: Command): Command =>
  command
    .addOption(targetOption())
    .option('--android-sdk-path <path>', 'Android SDK directory')
    .option('--sysroot <path>', 'Cross target development sysroot')
    .option(
      '--pkg-config-directories <paths...>',
      'Explicit target development pkg-config directories'
    )
    .option('--cc <executable>', 'C compiler override')
    .option('--cxx <executable>', 'C++ compiler override');
const program = new Command()
  .name('muon-plugin')
  .description(
    'Build and distribute native muon plugins independently of muon-ui.'
  );
program
  .command('init <directory>')
  .requiredOption('--name <name>', 'Plugin registration name')
  .addOption(languageOption())
  .addOption(
    new Option('--build-system <system>', 'Project build system')
      .choices(['make', 'cmake'])
      .default('make')
  )
  .action(async (directory, options) => {
    await createPluginProject({ ...options, directory: resolve(directory) });
  });
platformOptions(program.command('doctor'))
  .addOption(languageOption())
  .addOption(
    new Option('--build-system <system>', 'Build requirements')
      .choices(['make', 'cmake', 'custom'])
      .default('make')
  )
  .option(
    '--config <file>',
    'Read language, build tools and library requirements from a source configuration'
  )
  .option('--json', 'Print machine-readable diagnostics')
  .action(
    async (
      options: PluginToolchainOptions & { config?: string; json?: boolean }
    ) => {
      const config: PluginBuildConfig | undefined = options.config
        ? JSON.parse(await readFile(options.config, 'utf8'))
        : undefined;
      const report = await diagnosePluginToolchain({
        ...options,
        ...(config
          ? {
              language: config.language ?? 'c++',
              cardio: config.cardio ?? config.language !== 'c',
              buildSystem: config.build?.system ?? 'make',
              tools: [
                ...(config.build?.tools ?? []),
                ...(config.build?.commands?.map((entry) => entry.command) ??
                  []),
              ].map((tool) =>
                tool.includes('/')
                  ? resolve(
                      dirname(resolve(options.config!)),
                      config.build?.directory ?? '.',
                      tool
                    )
                  : tool
              ),
              ...(config.packages === undefined
                ? {}
                : { packages: config.packages }),
            }
          : {}),
      });
      process.stdout.write(
        (options.json
          ? JSON.stringify(
              { ready: report.ready, issues: report.issues },
              null,
              2
            )
          : formatPluginToolchainReport(report)) + '\n'
      );
      if (!report.ready) process.exitCode = 1;
    }
  );
platformOptions(program.command('env'))
  .option('--shell', 'Print POSIX shell export statements instead of JSON')
  .action(async (options: PluginToolchainOptions & { shell?: boolean }) => {
    const environment = await getNativeBuildEnvironment(options);
    const values = Object.fromEntries(
      [
        'CC',
        'CXX',
        'MUON_SDK',
        'MUON_TARGET',
        'PKG_CONFIG_PATH',
        'PKG_CONFIG_LIBDIR',
        'PKG_CONFIG_SYSROOT_DIR',
        'PKG_CONFIG_FDO_SYSROOT_RULES',
        'CPPFLAGS',
        'LDFLAGS',
      ]
        .filter((key) => environment[key] !== undefined)
        .map((key) => [key, environment[key]!])
    );
    const quote = (value: string): string =>
      "'" + value.replaceAll("'", "'\\''") + "'";
    process.stdout.write(
      options.shell
        ? Object.entries(values)
            .map(([key, value]) => `export ${key}=${quote(value)}\n`)
            .join('')
        : JSON.stringify(values, null, 2) + '\n'
    );
  });
platformOptions(program.command('build [config]'))
  .option('--out <path>', 'Artifact destination')
  .action(
    async (
      config: string | undefined,
      options: PluginToolchainOptions & { out?: string }
    ) => {
      process.stdout.write(
        (await buildPlugin({
          ...options,
          configPath: resolve(config ?? 'muon-plugin.build.json'),
          ...(options.out ? { outputDirectory: resolve(options.out) } : {}),
        })) + '\n'
      );
    }
  );
program
  .command('verify <manifest>')
  .addOption(
    new Option('--target <targets...>', 'Required application targets').choices(
      [...muonPluginTargets]
    )
  )
  .action(async (path: string, options: { target?: MuonPluginTarget[] }) => {
    const raw = JSON.parse(await readFile(path, 'utf8'));
    const artifact = await readPluginArtifact(
      path,
      options.target ?? (Object.keys(raw.targets) as MuonPluginTarget[])
    );
    process.stdout.write(
      `${artifact.manifest.name}: verified ${Object.keys(artifact.targets).join(', ')}\n`
    );
  });
program
  .command('package <inputs>')
  .description(
    'Package final binaries described by a PackagePluginOptions JSON file; paths are relative to the working directory'
  )
  .action(async (path: string) => {
    process.stdout.write(
      (await packagePlugin(JSON.parse(await readFile(path, 'utf8')))) + '\n'
    );
  });
try {
  await program.parseAsync();
} catch (error) {
  process.stderr.write(
    `muon-plugin: ${error instanceof Error ? error.message : String(error)}\n`
  );
  process.exitCode = 1;
}
