#!/usr/bin/env node
// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { Command } from "commander";

import {
  getDefaultMuonPrepareTarget,
  runMuonPrepare,
  type MuonPrepareOptions,
} from "./prepare.js";
import { ensureMuonGitignoreEntry } from "./gitignore.js";
import {
  embedMuonConfigInLauncherFile,
  embedMuonConfigInCoreFile,
  embedMuonConfigInRuntime,
  type EmbedMuonConfigResult,
} from "./embed-config.js";
import {
  runMuonBuildSequence,
  type MuonBuildSequenceOptions,
} from "./build-sequence.js";
import { runMuonDev, type MuonDevOptions } from "./dev.js";
import { packMuonApp, type MuonPackOptions } from "./pack.js";
import type { MuonWindowsResourceOptions } from "./windows-resource.js";
import type { MuonLinuxDesktopOptions } from "./linux-desktop.js";
import { git_commit_hash, version } from "./generated/packageMetadata.js";
import {
  createMuonProgressRenderer,
  type MuonProgressCallback,
} from "./progress.js";
import type {
  MuonWindowsCodeSigningOptions,
  MuonWindowsCodeSigningTarget,
} from "./windows-code-signing.js";

interface PrepareCommandOptions {
  muonPath: string | undefined;
  cefPath: string | undefined;
  stageDir: string | undefined;
  target: string | undefined;
  cacheDir: string | undefined;
  force: boolean | undefined;
  quiet: boolean | undefined;
  json: boolean | undefined;
}

interface EmbedConfigCommandOptions {
  runtimePath: string | undefined;
  corePath: string | undefined;
  launcherPath: string | undefined;
  config: string | undefined;
  outputRuntimePath: string | undefined;
  output: string | undefined;
  outputLauncher: string | undefined;
  json: boolean | undefined;
}

interface BuildCommandOptions {
  target: string[];
  all: boolean | undefined;
  assets: string | undefined;
  config: string | undefined;
  icon: string | undefined;
  windowsIcon: string | undefined;
  windowsProductName: string | undefined;
  windowsFileDescription: string | undefined;
  windowsCompanyName: string | undefined;
  windowsVersion: string | undefined;
  windowsCopyright: string | undefined;
  windowsSignCommand: string | undefined;
  windowsSignArg: string[] | undefined;
  windowsSignTarget: string[] | undefined;
  windowsCodeSigning: boolean | undefined;
  linuxDesktopId: string | undefined;
  linuxName: string | undefined;
  linuxComment: string | undefined;
  linuxIcon: string | undefined;
  linuxCategories: string | undefined;
  linuxStartupNotify: string | undefined;
  outDir: string | undefined;
  name: string | undefined;
  appId: string | undefined;
  packageDirectory: string | undefined;
  json: boolean | undefined;
}

interface PackCommandOptions {
  type: string[] | undefined;
  target: string[];
  all: boolean | undefined;
  config: string | undefined;
  icon: string | undefined;
  windowsIcon: string | undefined;
  windowsProductName: string | undefined;
  windowsFileDescription: string | undefined;
  windowsCompanyName: string | undefined;
  windowsVersion: string | undefined;
  windowsCopyright: string | undefined;
  windowsSignCommand: string | undefined;
  windowsSignArg: string[] | undefined;
  windowsSignTarget: string[] | undefined;
  windowsCodeSigning: boolean | undefined;
  linuxDesktopId: string | undefined;
  linuxName: string | undefined;
  linuxComment: string | undefined;
  linuxIcon: string | undefined;
  linuxCategories: string | undefined;
  linuxStartupNotify: string | undefined;
  linuxSandbox: string | undefined;
  name: string | undefined;
  appId: string | undefined;
  packageDirectory: string | undefined;
  artifactsDir: string | undefined;
  packageName: string | undefined;
  packageVersion: string | undefined;
  description: string | undefined;
  author: string | undefined;
  json: boolean | undefined;
}

interface DevCommandOptions {
  muonPath: string | undefined;
  cefPath: string | undefined;
  stageDir: string | undefined;
  config: string | undefined;
  assets: string | undefined;
  debugger: boolean | undefined;
  allowInsecureLocalhost: boolean | undefined;
  json: boolean | undefined;
}

interface InternalMuonBuildSequenceOptions extends MuonBuildSequenceOptions {
  progress?: MuonProgressCallback;
}

interface InternalMuonPackOptions extends MuonPackOptions {
  progress?: MuonProgressCallback;
}

interface InternalMuonDevOptions extends MuonDevOptions {
  warning?: (message: string) => void;
  allowInsecureLocalhost?: boolean;
}

const readTargetValues = (value: string): string[] => {
  return value
    .split(",")
    .map((value) => value.trim())
    .filter((value) => value.length > 0);
};

const appendTargetValues = (value: string, previous: string[]): string[] => {
  return [...previous, ...readTargetValues(value)];
};

const appendPackTypeValues = (
  value: string,
  previous: string[] | undefined,
): string[] => {
  return [...(previous ?? []), ...readTargetValues(value)];
};

const appendWindowsSignArgValue = (
  value: string,
  previous: string[] | undefined,
): string[] => {
  return [...(previous ?? []), value];
};

const appendWindowsSignTargetValues = (
  value: string,
  previous: string[] | undefined,
): string[] => {
  return [...(previous ?? []), ...readTargetValues(value)];
};

const readCommaSeparatedValues = (value: string): string[] =>
  value
    .split(",")
    .map((entry) => entry.trim())
    .filter((entry) => entry.length > 0);

const readBooleanValue = (value: string, label: string): boolean => {
  const normalized = value.trim().toLowerCase();
  if (normalized === "true" || normalized === "1" || normalized === "yes") {
    return true;
  }
  if (normalized === "false" || normalized === "0" || normalized === "no") {
    return false;
  }
  throw new Error(`${label} must be true or false.`);
};

const validateEmbedConfigOptions = (
  options: EmbedConfigCommandOptions,
): void => {
  if (options.runtimePath !== undefined && options.corePath !== undefined) {
    throw new Error("Specify at most one of --runtime-path or --core-path.");
  }
  if (
    options.runtimePath === undefined &&
    options.corePath === undefined &&
    options.launcherPath === undefined
  ) {
    throw new Error(
      "Specify at least one of --runtime-path, --core-path, or --launcher-path.",
    );
  }
  if (
    options.corePath !== undefined &&
    options.outputRuntimePath !== undefined
  ) {
    throw new Error("--output-runtime-path requires --runtime-path.");
  }
  if (options.runtimePath !== undefined && options.output !== undefined) {
    throw new Error("--output requires --core-path.");
  }
  if (
    options.launcherPath === undefined &&
    options.outputLauncher !== undefined
  ) {
    throw new Error("--output-launcher requires --launcher-path.");
  }
};

const createWindowsResourceOptions = (commandOptions: {
  windowsIcon: string | undefined;
  windowsProductName: string | undefined;
  windowsFileDescription: string | undefined;
  windowsCompanyName: string | undefined;
  windowsVersion: string | undefined;
  windowsCopyright: string | undefined;
}): MuonWindowsResourceOptions | undefined => {
  const options: MuonWindowsResourceOptions = {};
  if (commandOptions.windowsIcon !== undefined) {
    options.iconPath = commandOptions.windowsIcon;
  }
  if (commandOptions.windowsProductName !== undefined) {
    options.productName = commandOptions.windowsProductName;
  }
  if (commandOptions.windowsFileDescription !== undefined) {
    options.fileDescription = commandOptions.windowsFileDescription;
  }
  if (commandOptions.windowsCompanyName !== undefined) {
    options.companyName = commandOptions.windowsCompanyName;
  }
  if (commandOptions.windowsVersion !== undefined) {
    options.version = commandOptions.windowsVersion;
  }
  if (commandOptions.windowsCopyright !== undefined) {
    options.copyright = commandOptions.windowsCopyright;
  }
  return Object.keys(options).length === 0 ? undefined : options;
};

const createLinuxDesktopOptions = (commandOptions: {
  linuxDesktopId: string | undefined;
  linuxName: string | undefined;
  linuxComment: string | undefined;
  linuxIcon: string | undefined;
  linuxCategories: string | undefined;
  linuxStartupNotify: string | undefined;
}): MuonLinuxDesktopOptions | undefined => {
  const options: MuonLinuxDesktopOptions = {};
  if (commandOptions.linuxDesktopId !== undefined) {
    options.desktopId = commandOptions.linuxDesktopId;
  }
  if (commandOptions.linuxName !== undefined) {
    options.name = commandOptions.linuxName;
  }
  if (commandOptions.linuxComment !== undefined) {
    options.comment = commandOptions.linuxComment;
  }
  if (commandOptions.linuxIcon !== undefined) {
    options.iconPath = commandOptions.linuxIcon;
  }
  if (commandOptions.linuxCategories !== undefined) {
    options.categories = readCommaSeparatedValues(
      commandOptions.linuxCategories,
    );
  }
  if (commandOptions.linuxStartupNotify !== undefined) {
    options.startupNotify = readBooleanValue(
      commandOptions.linuxStartupNotify,
      "--linux-startup-notify",
    );
  }
  return Object.keys(options).length === 0 ? undefined : options;
};

const createWindowsCodeSigningOptions = (
  commandOptions: {
    windowsSignCommand: string | undefined;
    windowsSignArg: string[] | undefined;
    windowsSignTarget: string[] | undefined;
    windowsCodeSigning: boolean | undefined;
  },
  command: Command,
): false | MuonWindowsCodeSigningOptions | undefined => {
  if (
    command.getOptionValueSource("windowsCodeSigning") === "cli" &&
    commandOptions.windowsCodeSigning === false
  ) {
    return false;
  }
  const args = commandOptions.windowsSignArg ?? [];
  const targets = commandOptions.windowsSignTarget ?? [];
  if (commandOptions.windowsSignCommand === undefined) {
    if (args.length > 0 || targets.length > 0) {
      throw new Error(
        "--windows-sign-command is required when Windows signing arguments or targets are specified.",
      );
    }
    return undefined;
  }

  const options: MuonWindowsCodeSigningOptions = {
    command: commandOptions.windowsSignCommand,
    args,
  };
  if (targets.length > 0) {
    options.targets = targets as MuonWindowsCodeSigningTarget[];
  }
  return options;
};

const runBuildCommand = async (
  commandOptions: BuildCommandOptions,
  command: Command,
): Promise<void> => {
  const targets = commandOptions.target;
  if (commandOptions.all === true && targets.length > 0) {
    throw new Error("Specify either --all or --target, not both.");
  }

  const buildOptions: MuonBuildSequenceOptions = {
    root: process.cwd(),
    defaultAllTargets: false,
  };

  if (targets.length > 0) {
    buildOptions.targets = targets;
  }
  if (commandOptions.all === true) {
    buildOptions.allTargets = true;
  }
  if (commandOptions.assets !== undefined) {
    buildOptions.assetSourcePath = commandOptions.assets;
  }
  if (commandOptions.config !== undefined) {
    buildOptions.configPath = commandOptions.config;
  }
  if (commandOptions.icon !== undefined) {
    buildOptions.iconPath = commandOptions.icon;
  }
  const windowsResource = createWindowsResourceOptions(commandOptions);
  if (windowsResource !== undefined) {
    buildOptions.windowsResource = windowsResource;
  }
  const windowsCodeSigning = createWindowsCodeSigningOptions(
    commandOptions,
    command,
  );
  if (windowsCodeSigning !== undefined) {
    buildOptions.windowsCodeSigning = windowsCodeSigning;
  }
  const linuxDesktop = createLinuxDesktopOptions(commandOptions);
  if (linuxDesktop !== undefined) {
    buildOptions.linuxDesktop = linuxDesktop;
  }
  if (commandOptions.outDir !== undefined) {
    buildOptions.outputRoot = commandOptions.outDir;
  }
  if (commandOptions.name !== undefined) {
    buildOptions.appName = commandOptions.name;
  }
  if (commandOptions.appId !== undefined) {
    buildOptions.appId = commandOptions.appId;
  }
  if (commandOptions.packageDirectory !== undefined) {
    buildOptions.packageDirectory = commandOptions.packageDirectory;
  }

  const progressRenderer = createMuonProgressRenderer();
  (buildOptions as InternalMuonBuildSequenceOptions).progress =
    progressRenderer.report;
  const result = await (async () => {
    try {
      return await runMuonBuildSequence(buildOptions);
    } finally {
      progressRenderer.flush();
    }
  })();
  if (commandOptions.json === true) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    for (const target of result.targets) {
      console.log(target.outputPath);
    }
  }
};

const runPackCommand = async (
  commandOptions: PackCommandOptions,
  command: Command,
): Promise<void> => {
  const targets = commandOptions.target;
  if (commandOptions.all === true && targets.length > 0) {
    throw new Error("Specify either --all or --target, not both.");
  }

  const packOptions: MuonPackOptions = {
    root: process.cwd(),
    environment: process.env,
  };
  if (commandOptions.type !== undefined) {
    packOptions.types = commandOptions.type;
  }
  if (targets.length > 0) {
    packOptions.targets = targets;
  }
  if (commandOptions.all !== undefined) {
    packOptions.allTargets = commandOptions.all;
  }
  if (commandOptions.config !== undefined) {
    packOptions.configPath = commandOptions.config;
  }
  if (commandOptions.icon !== undefined) {
    packOptions.iconPath = commandOptions.icon;
  }
  const windowsResource = createWindowsResourceOptions(commandOptions);
  if (windowsResource !== undefined) {
    packOptions.windowsResource = windowsResource;
  }
  const windowsCodeSigning = createWindowsCodeSigningOptions(
    commandOptions,
    command,
  );
  if (windowsCodeSigning !== undefined) {
    packOptions.windowsCodeSigning = windowsCodeSigning;
  }
  const linuxDesktop = createLinuxDesktopOptions(commandOptions);
  if (linuxDesktop !== undefined) {
    packOptions.linuxDesktop = linuxDesktop;
  }
  if (commandOptions.linuxSandbox !== undefined) {
    packOptions.linuxSandbox = commandOptions.linuxSandbox;
  }
  if (commandOptions.name !== undefined) {
    packOptions.appName = commandOptions.name;
  }
  if (commandOptions.appId !== undefined) {
    packOptions.appId = commandOptions.appId;
  }
  if (commandOptions.packageDirectory !== undefined) {
    packOptions.packageDirectory = commandOptions.packageDirectory;
  }
  if (commandOptions.artifactsDir !== undefined) {
    packOptions.artifactsDir = commandOptions.artifactsDir;
  }
  if (commandOptions.packageName !== undefined) {
    packOptions.packageName = commandOptions.packageName;
  }
  if (commandOptions.packageVersion !== undefined) {
    packOptions.packageVersion = commandOptions.packageVersion;
  }
  if (commandOptions.description !== undefined) {
    packOptions.description = commandOptions.description;
  }
  if (commandOptions.author !== undefined) {
    packOptions.author = commandOptions.author;
  }

  const progressRenderer = createMuonProgressRenderer();
  (packOptions as InternalMuonPackOptions).progress = progressRenderer.report;
  const result = await (async () => {
    try {
      return await packMuonApp(packOptions);
    } finally {
      progressRenderer.flush();
    }
  })();
  if (commandOptions.json === true) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    for (const artifact of result.artifacts) {
      console.log(artifact.path);
    }
  }
};

const runPrepareCommand = async (
  commandOptions: PrepareCommandOptions,
): Promise<void> => {
  const prepareOptions: MuonPrepareOptions = {
    muonPath: commandOptions.muonPath ?? "",
    cefPath: commandOptions.cefPath,
    stageDir: commandOptions.stageDir,
    target:
      commandOptions.target ??
      getDefaultMuonPrepareTarget(process.platform, process.arch),
    cacheDir: commandOptions.cacheDir,
    nodeRuntimeRequirement: undefined,
    force: commandOptions.force === true,
    quiet: commandOptions.quiet === true,
    prepareExecutablePath: undefined,
    environment: process.env,
    cwd: process.cwd(),
  };
  const result = await runMuonPrepare(prepareOptions);
  if (commandOptions.json === true) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    console.log(result.stagePath ?? result.cefPath);
  }
};

const runDevCommand = async (
  commandOptions: DevCommandOptions,
  command: Command,
): Promise<void> => {
  const devOptions: MuonDevOptions = {
    root: process.cwd(),
    environment: process.env,
    quietPrepare: commandOptions.json === true,
  };
  (devOptions as InternalMuonDevOptions).warning = (message): void => {
    console.warn(`Warning: ${message}`);
  };

  if (commandOptions.muonPath !== undefined) {
    devOptions.muonPath = commandOptions.muonPath;
  }
  if (commandOptions.cefPath !== undefined) {
    devOptions.cefPath = commandOptions.cefPath;
  }
  if (commandOptions.stageDir !== undefined) {
    devOptions.stagePath = commandOptions.stageDir;
  }
  if (commandOptions.config !== undefined) {
    devOptions.configPath = commandOptions.config;
  }
  if (commandOptions.assets !== undefined) {
    devOptions.assetSourcePath = commandOptions.assets;
  }
  if (command.getOptionValueSource("debugger") === "cli") {
    devOptions.enableDebugger = commandOptions.debugger === true;
  }
  if (commandOptions.allowInsecureLocalhost === true) {
    (devOptions as InternalMuonDevOptions).allowInsecureLocalhost = true;
  }

  const result = await runMuonDev(devOptions);
  if (commandOptions.json === true) {
    console.log(JSON.stringify(result, null, 2));
  }
  if (result.exitCode !== 0) {
    process.exitCode = result.exitCode;
  }
};

const runInitCommand = async (): Promise<void> => {
  const result = await ensureMuonGitignoreEntry(process.cwd());
  console.log(
    result.changed ? `Updated ${result.gitignorePath}` : result.gitignorePath,
  );
};

const printEmbedConfigResult = (
  result: EmbedMuonConfigResult,
  json: boolean | undefined,
): void => {
  if (json === true) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    console.log(result.outputPath);
  }
};

const runEmbedConfigCommand = async (
  commandOptions: EmbedConfigCommandOptions,
): Promise<void> => {
  validateEmbedConfigOptions(commandOptions);
  const configPath = commandOptions.config ?? "";
  const coreResult =
    commandOptions.corePath !== undefined
      ? await embedMuonConfigInCoreFile({
          corePath: commandOptions.corePath,
          configPath,
          outputPath: commandOptions.output,
        })
      : commandOptions.runtimePath !== undefined
        ? await embedMuonConfigInRuntime({
            runtimePath: commandOptions.runtimePath,
            configPath,
            outputRuntimePath: commandOptions.outputRuntimePath,
          })
        : undefined;
  const launcherResult =
    commandOptions.launcherPath === undefined
      ? undefined
      : await embedMuonConfigInLauncherFile({
          launcherPath: commandOptions.launcherPath,
          configPath,
          outputPath: commandOptions.outputLauncher,
        });
  if (coreResult !== undefined && launcherResult !== undefined) {
    if (commandOptions.json === true) {
      console.log(
        JSON.stringify(
          {
            core: coreResult,
            launcher: launcherResult,
          },
          null,
          2,
        ),
      );
    } else {
      console.log(`${coreResult.outputPath}\n${launcherResult.outputPath}`);
    }
    return;
  }
  const result = coreResult ?? launcherResult;
  if (result === undefined) {
    throw new Error("No embed-config target was specified.");
  }
  printEmbedConfigResult(result, commandOptions.json);
};

const createCliCommand = (): Command => {
  const program = new Command();

  program
    .name("muon")
    .description(
      "Multi-platform GUI application framework that uses CEF as its backend",
    )
    .version(`${version}-${git_commit_hash}`)
    .showSuggestionAfterError()
    .action(() => {
      program.outputHelp({ error: true });
      process.exitCode = 1;
    });

  program
    .command("build")
    .description("Build CEF-free muon app distribution directories")
    .option(
      "--target <target>",
      "public target or comma-separated public targets",
      appendTargetValues,
      [],
    )
    .option("--all", "build all supported targets")
    .option("--assets <path>", "asset root path")
    .option("--config <path>", "muon config path")
    .option("--icon <path>", "static application PNG icon path")
    .option("--windows-icon <path>", "Windows PNG icon resource path")
    .option("--windows-product-name <name>", "Windows product name")
    .option("--windows-file-description <text>", "Windows file description")
    .option("--windows-company-name <name>", "Windows company name")
    .option("--windows-version <version>", "Windows resource version")
    .option("--windows-copyright <text>", "Windows legal copyright")
    .option("--windows-sign-command <command>", "Windows code signing command")
    .option(
      "--windows-sign-arg <arg>",
      "Windows code signing command argument",
      appendWindowsSignArgValue,
    )
    .option(
      "--windows-sign-target <target>",
      "Windows code signing target or comma-separated targets",
      appendWindowsSignTargetValues,
    )
    .option("--no-windows-code-signing", "disable muon.json Windows signing")
    .option("--linux-desktop-id <id>", "Linux desktop entry identifier")
    .option("--linux-name <name>", "Linux desktop display name")
    .option("--linux-comment <text>", "Linux desktop comment")
    .option("--linux-icon <path>", "Linux desktop PNG icon path")
    .option("--linux-categories <list>", "Linux desktop categories")
    .option("--linux-startup-notify <boolean>", "Linux startup notification")
    .option("--out-dir <path>", "output root directory")
    .option("--name <name>", "launcher file name")
    .option("--app-id <id>", "stable application identifier")
    .option("--package-directory <path>", "muon package dist directory")
    .option("--json", "write result as JSON")
    .action(async (options: BuildCommandOptions, command: Command) => {
      await runBuildCommand(options, command);
    });

  program
    .command("pack")
    .description("Build and package a muon app")
    .option(
      "--type <type>",
      "package type or comma-separated package types: zip, tar.gz, tgz, deb, nsis (default: all)",
      appendPackTypeValues,
    )
    .option(
      "--target <target>",
      "public target, platform, arch, or comma-separated selectors",
      appendTargetValues,
      [],
    )
    .option("--all", "build all supported targets")
    .option("--config <path>", "muon config path")
    .option("--icon <path>", "static application PNG icon path")
    .option("--windows-icon <path>", "Windows PNG icon resource path")
    .option("--windows-product-name <name>", "Windows product name")
    .option("--windows-file-description <text>", "Windows file description")
    .option("--windows-company-name <name>", "Windows company name")
    .option("--windows-version <version>", "Windows resource version")
    .option("--windows-copyright <text>", "Windows legal copyright")
    .option("--windows-sign-command <command>", "Windows code signing command")
    .option(
      "--windows-sign-arg <arg>",
      "Windows code signing command argument",
      appendWindowsSignArgValue,
    )
    .option(
      "--windows-sign-target <target>",
      "Windows code signing target or comma-separated targets",
      appendWindowsSignTargetValues,
    )
    .option("--no-windows-code-signing", "disable muon.json Windows signing")
    .option("--linux-desktop-id <id>", "Linux desktop entry identifier")
    .option("--linux-name <name>", "Linux desktop display name")
    .option("--linux-comment <text>", "Linux desktop comment")
    .option("--linux-icon <path>", "Linux desktop PNG icon path")
    .option("--linux-categories <list>", "Linux desktop categories")
    .option("--linux-startup-notify <boolean>", "Linux startup notification")
    .option(
      "--linux-sandbox <mode>",
      "Linux deb CEF sandbox mode: disabled, setuid",
    )
    .option("--name <name>", "launcher file name")
    .option("--app-id <id>", "stable application identifier")
    .option("--package-directory <path>", "muon package dist directory")
    .option("--artifacts-dir <path>", "package artifact output directory")
    .option("--package-name <name>", "package name override")
    .option("--package-version <version>", "package version override")
    .option("--description <text>", "package description override")
    .option("--author <text>", "package author override")
    .option("--json", "write result as JSON")
    .action(async (options: PackCommandOptions, command: Command) => {
      await runPackCommand(options, command);
    });

  const devCommand = program
    .command("run")
    .description("Launch muon directly with local development assets")
    .option("--muon-path <path>", "muon runtime file root")
    .option("--cef-path <path>", "CEF file root")
    .option("--stage-dir <path>", "prepared runtime output directory")
    .option("--config <path>", "muon config path")
    .option("--assets <path>", "development asset directory")
    .option("--no-debugger", "disable muon debugger defaults")
    .option(
      "--allow-insecure-localhost",
      "ignore invalid HTTPS certificates for localhost",
    )
    .option("--json", "write result as JSON")
    .action(async (options: DevCommandOptions) => {
      await runDevCommand(options, devCommand);
    });

  program
    .command("init")
    .description("Initialize muon project helper files")
    .action(async () => {
      await runInitCommand();
    });

  program
    .command("prepare")
    .description("Prepare a muon runtime with CEF files")
    .requiredOption("--muon-path <path>", "muon runtime file root")
    .option("--cef-path <path>", "CEF file root")
    .option("--stage-dir <path>", "prepared runtime output directory")
    .option("--target <target>", "prepare target")
    .option("--cache-dir <path>", "CEF artifact cache directory")
    .option("--force", "rebuild an existing prepared runtime")
    .option("-q, --quiet", "suppress native builder progress messages")
    .option("--json", "write result as JSON")
    .action(async (options: PrepareCommandOptions) => {
      await runPrepareCommand(options);
    });

  program
    .command("embed-config")
    .description("Embed muon.json into muon runtime files")
    .option("--runtime-path <path>", "prepared runtime directory")
    .option("--core-path <path>", "muon-core executable path")
    .option("--launcher-path <path>", "muon-launcher executable path")
    .requiredOption("--config <path>", "muon config path")
    .option("--output-runtime-path <path>", "patched runtime output directory")
    .option("--output <path>", "patched muon-core output path")
    .option("--output-launcher <path>", "patched launcher output path")
    .option("--json", "write result as JSON")
    .action(async (options: EmbedConfigCommandOptions) => {
      await runEmbedConfigCommand(options);
    });

  return program;
};

const main = async (): Promise<void> => {
  try {
    await createCliCommand().parseAsync(process.argv);
  } catch (error) {
    console.error(error instanceof Error ? error.message : String(error));
    process.exitCode = 1;
  }
};

void main();
