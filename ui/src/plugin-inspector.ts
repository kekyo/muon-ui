// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { constants } from "node:fs";
import { access, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

import { getDefaultMuonPrepareTarget } from "./prepare.js";
import type { MuonCapabilityImportOptions } from "./capability.js";
import type {
  MuonPluginAccessEntryOptions,
  MuonResolvedPluginAccessOptions,
} from "./plugin-access.js";

/**
 * Inputs for collecting external plugin function metadata.
 */
export interface MuonPluginFunctionCatalogOptions {
  /**
   * Absolute external plugin directory.
   */
  pluginPath: string;
  /**
   * Effective external plugin entries to inspect.
   */
  plugins: readonly MuonPluginAccessEntryOptions[];
  /**
   * Process environment used to locate and run the inspector.
   */
  environment?: NodeJS.ProcessEnv;
}

interface InspectorInputPlugin {
  name: string;
  signature?: string;
  salt?: string;
  config?: Readonly<Record<string, string>>;
}

interface InspectorOutputPlugin {
  name: string;
  functions: readonly string[];
}

const execFileAsync = promisify(execFile);
const moduleDirectory =
  typeof __dirname === "string"
    ? __dirname
    : dirname(fileURLToPath(import.meta.url));

const getInspectorExecutableName = (platform: NodeJS.Platform): string =>
  platform === "win32" ? "muon-plugin-inspector.exe" : "muon-plugin-inspector";

const canExecute = async (path: string): Promise<boolean> => {
  try {
    await access(path, constants.X_OK);
    return true;
  } catch {
    return false;
  }
};

const resolveMuonPluginInspectorExecutable = async (
  environment: NodeJS.ProcessEnv,
): Promise<string> => {
  const explicit = environment.MUON_PLUGIN_INSPECTOR_PATH;
  if (explicit !== undefined && explicit !== "") {
    return explicit;
  }

  const executableName = getInspectorExecutableName(process.platform);
  const target = getDefaultMuonPrepareTarget(process.platform, process.arch);
  const builderPath = environment.MUON_BUILDER_PATH;
  const candidates = [
    ...(builderPath === undefined || builderPath === ""
      ? []
      : [join(dirname(builderPath), executableName)]),
    join(moduleDirectory, "native", target, executableName),
    join(moduleDirectory, "..", "dist", "native", target, executableName),
  ];
  for (const candidate of candidates) {
    if (await canExecute(candidate)) {
      return candidate;
    }
  }
  return candidates[0] ?? executableName;
};

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const readInspectorOutput = (source: string): readonly string[] => {
  let parsed: unknown;
  try {
    parsed = JSON.parse(source);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    throw new Error(`muon plugin inspector returned invalid JSON: ${message}`);
  }
  if (!isRecord(parsed) || !Array.isArray(parsed.plugins)) {
    throw new Error("muon plugin inspector returned an invalid catalog.");
  }

  const functionPaths: string[] = [];
  for (const plugin of parsed.plugins) {
    if (
      !isRecord(plugin) ||
      typeof plugin.name !== "string" ||
      !Array.isArray(plugin.functions) ||
      !plugin.functions.every((entry) => typeof entry === "string")
    ) {
      throw new Error(
        "muon plugin inspector returned an invalid plugin entry.",
      );
    }
    functionPaths.push(
      ...(plugin as unknown as InspectorOutputPlugin).functions,
    );
  }
  return functionPaths;
};

const createInspectorInputPlugin = (
  plugin: MuonPluginAccessEntryOptions,
): InspectorInputPlugin => ({
  name: plugin.name,
  ...(plugin.signature === undefined ? {} : { signature: plugin.signature }),
  ...(plugin.salt === undefined ? {} : { salt: plugin.salt }),
  ...(plugin.config === undefined ? {} : { config: plugin.config }),
});

/**
 * Collects external plugin public function paths through the native inspector.
 *
 * @param options Inspector invocation options.
 * @returns Public function paths reported by inspected plugins.
 */
export const collectMuonPluginFunctionPaths = async ({
  pluginPath,
  plugins,
  environment = process.env,
}: MuonPluginFunctionCatalogOptions): Promise<readonly string[]> => {
  if (plugins.length === 0) {
    return [];
  }

  const temporaryDirectory = await mkdtemp(join(tmpdir(), "muon-inspector-"));
  const inputPath = join(temporaryDirectory, "input.json");
  try {
    await writeFile(
      inputPath,
      `${JSON.stringify(
        {
          path: pluginPath,
          plugins: plugins.map(createInspectorInputPlugin),
        },
        null,
        2,
      )}\n`,
    );
    const executablePath =
      await resolveMuonPluginInspectorExecutable(environment);
    let stdout: string;
    try {
      const result = await execFileAsync(executablePath, [inputPath], {
        encoding: "utf8",
        env: environment,
        maxBuffer: 1024 * 1024,
      });
      stdout = result.stdout;
    } catch (error) {
      const details =
        isRecord(error) && typeof error.stderr === "string"
          ? error.stderr.trim()
          : "";
      const message = error instanceof Error ? error.message : String(error);
      throw new Error(
        details === ""
          ? `muon plugin inspector failed: ${message}`
          : `muon plugin inspector failed: ${message}: ${details}`,
      );
    }
    return readInspectorOutput(stdout);
  } finally {
    await rm(temporaryDirectory, { recursive: true, force: true });
  }
};

const hasWildcardAllow = (rule: MuonCapabilityImportOptions): boolean =>
  rule.allow.some((entry) => entry.includes("*"));

/**
 * Collects function paths needed for wildcard validate-mode imports.
 *
 * @param pluginAccess Resolved plugin access configuration.
 * @param environment Process environment used to run the inspector.
 * @returns External plugin public function paths, or an empty catalog.
 */
export const collectMuonPluginFunctionPathsForAccess = async (
  pluginAccess: MuonResolvedPluginAccessOptions,
  environment: NodeJS.ProcessEnv = process.env,
): Promise<readonly string[]> => {
  if (pluginAccess.mode !== "validate") {
    return [];
  }

  const wildcardPluginNames = new Set(
    (pluginAccess.capabilityOptions.imports ?? [])
      .filter(
        (rule) =>
          rule.pluginName !== undefined &&
          rule.pluginName !== "internal" &&
          hasWildcardAllow(rule),
      )
      .map((rule) => rule.pluginName),
  );
  if (wildcardPluginNames.size === 0) {
    return [];
  }

  const plugins = pluginAccess.plugins.filter((plugin) =>
    wildcardPluginNames.has(plugin.name),
  );
  return await collectMuonPluginFunctionPaths({
    pluginPath: pluginAccess.pluginPath,
    plugins,
    environment,
  });
};
