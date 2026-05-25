#!/usr/bin/env node
// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { Command } from "commander";

import {
  getDefaultMuonPrepareTarget,
  runMuonPrepare,
  type MuonPrepareOptions,
} from "./prepare.js";
import {
  embedMuonConfigInBootstrapFile,
  embedMuonConfigInCoreFile,
  embedMuonConfigInRuntime,
  type EmbedMuonConfigResult,
} from "./embed-config.js";
import { buildMuonApp, type MuonBuildOptions } from "./build.js";
import { git_commit_hash, version } from "./generated/packageMetadata.js";

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
  bootstrapPath: string | undefined;
  config: string | undefined;
  outputRuntimePath: string | undefined;
  output: string | undefined;
  outputBootstrap: string | undefined;
  json: boolean | undefined;
}

interface BuildCommandOptions {
  target: string[];
  all: boolean | undefined;
  assets: string | undefined;
  config: string | undefined;
  outDir: string | undefined;
  name: string | undefined;
  json: boolean | undefined;
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

const validateEmbedConfigOptions = (
  options: EmbedConfigCommandOptions,
): void => {
  if (options.runtimePath !== undefined && options.corePath !== undefined) {
    throw new Error("Specify at most one of --runtime-path or --core-path.");
  }
  if (
    options.runtimePath === undefined &&
    options.corePath === undefined &&
    options.bootstrapPath === undefined
  ) {
    throw new Error(
      "Specify at least one of --runtime-path, --core-path, or --bootstrap-path.",
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
    options.bootstrapPath === undefined &&
    options.outputBootstrap !== undefined
  ) {
    throw new Error("--output-bootstrap requires --bootstrap-path.");
  }
};

const runBuildCommand = async (
  commandOptions: BuildCommandOptions,
): Promise<void> => {
  const targets = commandOptions.target;
  if (commandOptions.all === true && targets.length > 0) {
    throw new Error("Specify either --all or --target, not both.");
  }

  const buildOptions: MuonBuildOptions = {
    root: process.cwd(),
    allTargets: commandOptions.all === true,
  };

  if (targets.length > 0) {
    buildOptions.targets = targets;
  }
  if (commandOptions.assets !== undefined) {
    buildOptions.assetSourcePath = commandOptions.assets;
  }
  if (commandOptions.config !== undefined) {
    buildOptions.configPath = commandOptions.config;
  }
  if (commandOptions.outDir !== undefined) {
    buildOptions.outputRoot = commandOptions.outDir;
  }
  if (commandOptions.name !== undefined) {
    buildOptions.appName = commandOptions.name;
  }

  const result = await buildMuonApp(buildOptions);
  if (commandOptions.json === true) {
    console.log(JSON.stringify(result, null, 2));
  } else {
    for (const target of result.targets) {
      console.log(target.outputPath);
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
  const bootstrapResult =
    commandOptions.bootstrapPath === undefined
      ? undefined
      : await embedMuonConfigInBootstrapFile({
          bootstrapPath: commandOptions.bootstrapPath,
          configPath,
          outputPath: commandOptions.outputBootstrap,
        });
  if (coreResult !== undefined && bootstrapResult !== undefined) {
    if (commandOptions.json === true) {
      console.log(
        JSON.stringify(
          {
            core: coreResult,
            bootstrap: bootstrapResult,
          },
          null,
          2,
        ),
      );
    } else {
      console.log(`${coreResult.outputPath}\n${bootstrapResult.outputPath}`);
    }
    return;
  }
  const result = coreResult ?? bootstrapResult;
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
    .description("Build CEF-free Muon app distribution directories")
    .option(
      "--target <target>",
      "target alias or comma-separated target aliases",
      appendTargetValues,
      [],
    )
    .option("--all", "build all supported targets")
    .option("--assets <path>", "asset root path")
    .option("--config <path>", "muon config path")
    .option("--out-dir <path>", "output root directory")
    .option("--name <name>", "launcher file name")
    .option("--json", "write result as JSON")
    .action(async (options: BuildCommandOptions) => {
      await runBuildCommand(options);
    });

  program
    .command("prepare")
    .description("Prepare a Muon runtime with CEF files")
    .requiredOption("--muon-path <path>", "Muon runtime file root")
    .option("--cef-path <path>", "CEF file root")
    .option("--stage-dir <path>", "prepared runtime output directory")
    .option("--target <target>", "prepare target")
    .option("--cache-dir <path>", "CEF artifact cache directory")
    .option("--force", "rebuild an existing prepared runtime")
    .option("-q, --quiet", "suppress native prepare progress messages")
    .option("--json", "write result as JSON")
    .action(async (options: PrepareCommandOptions) => {
      await runPrepareCommand(options);
    });

  program
    .command("embed-config")
    .description("Embed muon.json into Muon runtime files")
    .option("--runtime-path <path>", "prepared runtime directory")
    .option("--core-path <path>", "muon-core executable path")
    .option("--bootstrap-path <path>", "muon-bootstrap executable path")
    .requiredOption("--config <path>", "muon config path")
    .option("--output-runtime-path <path>", "patched runtime output directory")
    .option("--output <path>", "patched muon-core output path")
    .option("--output-bootstrap <path>", "patched bootstrap output path")
    .option("--json", "write result as JSON")
    .action(async (options: EmbedConfigCommandOptions) => {
      await runEmbedConfigCommand(options);
    });

  return program;
};

const main = async (): Promise<void> => {
  await createCliCommand().parseAsync(process.argv);
};

main().catch((error: unknown) => {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
});
