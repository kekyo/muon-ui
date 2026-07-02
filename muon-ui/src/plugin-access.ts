// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { constants } from "node:fs";
import { access, readFile } from "node:fs/promises";
import { join, resolve } from "node:path";

import { parse } from "json5";

import type {
  MuonCapabilityImportOptions,
  MuonCapabilityOptions,
  MuonRuntimePluginEntryConfig,
} from "./capability.js";

/**
 * Plugin JavaScript exposure mode.
 */
export type MuonPluginAccessMode = "simple" | "validate";

/**
 * Import-side capability rule from public Muon plugin access config.
 */
export interface MuonPluginAccessImportOptions {
  /**
   * Importer source globs relative to the bundler project root.
   */
  sources?: readonly string[];
  /**
   * NPM package names allowed to import the virtual module.
   */
  packages?: readonly string[];
  /**
   * Plugin function paths allowed for this import rule.
   *
   * @remarks When omitted, the parent plugin entry `allow` list is inherited.
   */
  allow?: readonly string[];
}

/**
 * Plugin entry from public Muon plugin access config.
 */
export interface MuonPluginAccessEntryOptions {
  /**
   * Plugin entry name.
   */
  name: string;
  /**
   * Plugin function path globs allowed by the runtime plugin policy.
   */
  allow: readonly string[];
  /**
   * Validate-mode import rules for this plugin entry.
   */
  imports?: readonly MuonPluginAccessImportOptions[];
}

/**
 * Public plugin access config shared by muon.json and the Vite option.
 */
export interface MuonPluginAccessOptions {
  /**
   * External plugin directory override.
   */
  path?: string;
  /**
   * Plugin exposure mode.
   */
  mode?: MuonPluginAccessMode;
  /**
   * Page URL patterns where the plugin bridge is exposed.
   */
  pages?: readonly string[];
  /**
   * Runtime plugin entries and validate-mode import rules.
   */
  plugins?: readonly MuonPluginAccessEntryOptions[];
}

/**
 * Resolved plugin access config used by bundler integrations.
 */
export interface MuonResolvedPluginAccessOptions {
  /**
   * Effective plugin exposure mode.
   */
  mode: MuonPluginAccessMode;
  /**
   * Effective page URL allowlist.
   */
  pages: readonly string[];
  /**
   * Capability resolver inputs generated from plugin imports.
   */
  capabilityOptions: MuonCapabilityOptions;
  /**
   * Runtime config fields added around generated capability policies.
   */
  runtimeOverlay: MuonPluginAccessRuntimeOverlay;
}

/**
 * Runtime plugin config fields that are not derived from capability IDs.
 */
export interface MuonPluginAccessRuntimeOverlay {
  /**
   * Page URL allowlist to write into generated runtime config.
   */
  pages?: readonly string[];
  /**
   * External plugin path override.
   */
  path?: string;
  /**
   * Plugin loading allowlist override.
   */
  plugins?: readonly MuonRuntimePluginEntryConfig[];
}

export interface ResolveMuonPluginAccessOptionsInput {
  /**
   * Bundler project root.
   */
  root: string;
  /**
   * Optional muon.json path relative to root.
   */
  configPath: string | undefined;
  /**
   * Vite plugin access override.
   */
  pluginAccess: false | MuonPluginAccessOptions | undefined;
  /**
   * Handles project config read failures. When omitted, read failures are
   * reported as errors.
   */
  onConfigReadError?: (error: unknown) => void;
}

type JsonObject = Record<string, unknown>;

const defaultConfigFileNames = ["muon.json5", "muon.jsonc", "muon.json"];
const defaultPluginPages = ["asset://main/**"];

const isJsonObject = (value: unknown): value is JsonObject =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const isStringArray = (value: unknown): value is readonly string[] =>
  Array.isArray(value) && value.every((entry) => typeof entry === "string");

const getErrorMessage = (error: unknown): string =>
  error instanceof Error ? error.message : String(error);

const fileExists = async (path: string): Promise<boolean> => {
  try {
    await access(path, constants.F_OK);
    return true;
  } catch {
    return false;
  }
};

const resolveMuonConfigPath = async (
  root: string,
  configPath: string | undefined,
): Promise<string | undefined> => {
  if (configPath !== undefined) {
    const resolvedPath = resolve(root, configPath);
    if (await fileExists(resolvedPath)) {
      return resolvedPath;
    }
    throw new Error(`Muon config file does not exist: ${resolvedPath}`);
  }

  for (const fileName of defaultConfigFileNames) {
    const candidatePath = join(root, fileName);
    if (await fileExists(candidatePath)) {
      return candidatePath;
    }
  }
  return undefined;
};

const readJsonObjectFile = async (
  path: string,
  label: string,
): Promise<JsonObject> => {
  let content: string;
  try {
    content = await readFile(path, "utf8");
  } catch (error) {
    throw new Error(
      `${label} could not be read: ${path}: ${getErrorMessage(error)}`,
    );
  }

  let parsed: unknown;
  try {
    parsed = parse(content);
  } catch (error) {
    throw new Error(
      `${label} could not be parsed: ${path}: ${getErrorMessage(error)}`,
    );
  }

  if (!isJsonObject(parsed)) {
    throw new Error(`${label} must contain a JSON object: ${path}`);
  }
  return parsed;
};

const readOptionalStringArray = (
  object: JsonObject,
  key: string,
  configPath: string,
): readonly string[] | undefined => {
  const value = object[key];
  if (value === undefined) {
    return undefined;
  }
  if (!isStringArray(value)) {
    throw new Error(`muon.json ${configPath} must be an array of strings.`);
  }
  return value;
};

const readPluginAccessImportOptions = (
  value: unknown,
  configPath: string,
): MuonPluginAccessImportOptions => {
  if (!isJsonObject(value)) {
    throw new Error(`muon.json ${configPath} must be an object.`);
  }

  const sources = readOptionalStringArray(
    value,
    "sources",
    `${configPath}.sources`,
  );
  const packages = readOptionalStringArray(
    value,
    "packages",
    `${configPath}.packages`,
  );
  const allow = readOptionalStringArray(value, "allow", `${configPath}.allow`);
  const options: MuonPluginAccessImportOptions = {};
  if (sources !== undefined) {
    options.sources = sources;
  }
  if (packages !== undefined) {
    options.packages = packages;
  }
  if (allow !== undefined) {
    options.allow = allow;
  }
  return options;
};

const readPluginAccessEntryOptions = (
  value: unknown,
  index: number,
): MuonPluginAccessEntryOptions => {
  const configPath = `plugin.plugins[${index}]`;
  if (!isJsonObject(value)) {
    throw new Error(`muon.json ${configPath} must be an object.`);
  }
  if (typeof value.name !== "string") {
    throw new Error(`muon.json ${configPath}.name must be a string.`);
  }
  const allow = readOptionalStringArray(value, "allow", `${configPath}.allow`);
  if (allow === undefined) {
    throw new Error(`muon.json ${configPath}.allow is required.`);
  }

  const rawImports = value.imports;
  if (rawImports !== undefined && !Array.isArray(rawImports)) {
    throw new Error(`muon.json ${configPath}.imports must be an array.`);
  }
  const imports =
    rawImports === undefined
      ? undefined
      : rawImports.map((entry, importIndex) =>
          readPluginAccessImportOptions(
            entry,
            `${configPath}.imports[${importIndex}]`,
          ),
        );
  const options: MuonPluginAccessEntryOptions = {
    name: value.name,
    allow,
  };
  if (imports !== undefined) {
    options.imports = imports;
  }
  return options;
};

const readPluginAccessOptions = (
  config: JsonObject,
): MuonPluginAccessOptions => {
  const browser = config.browser;
  if (isJsonObject(browser) && browser.plugin !== undefined) {
    throw new Error(
      "muon.json browser.plugin is no longer supported; use plugin.mode and plugin.pages instead.",
    );
  }

  const plugin = config.plugin;
  if (plugin === undefined) {
    return {};
  }
  if (!isJsonObject(plugin)) {
    throw new Error("muon.json plugin must be an object.");
  }

  const options: MuonPluginAccessOptions = {};
  if (plugin.path !== undefined) {
    if (typeof plugin.path !== "string") {
      throw new Error("muon.json plugin.path must be a string.");
    }
    if (plugin.path.length === 0) {
      throw new Error("muon.json plugin.path must not be empty.");
    }
    options.path = plugin.path;
  }
  if (plugin.mode !== undefined) {
    if (typeof plugin.mode !== "string") {
      throw new Error("muon.json plugin.mode must be a string.");
    }
    if (plugin.mode.length === 0) {
      throw new Error("muon.json plugin.mode must not be empty.");
    }
    if (plugin.mode !== "simple" && plugin.mode !== "validate") {
      throw new Error("muon.json plugin.mode has unknown value.");
    }
    options.mode = plugin.mode;
  }
  const pages = readOptionalStringArray(plugin, "pages", "plugin.pages");
  if (pages !== undefined) {
    options.pages = pages;
  }

  const rawPlugins = plugin.plugins;
  if (rawPlugins !== undefined) {
    if (!Array.isArray(rawPlugins)) {
      throw new Error("muon.json plugin.plugins must be an array.");
    }
    options.plugins = rawPlugins.map((entry, index) =>
      readPluginAccessEntryOptions(entry, index),
    );
  }
  return options;
};

const escapeRegExp = (source: string): string =>
  source.replace(/[\\^$+?.()|[\]{}]/gu, "\\$&");

const globToRegExp = (glob: string, separator: string): RegExp => {
  let source = "^";
  for (let index = 0; index < glob.length; index += 1) {
    const current = glob[index] ?? "";
    const next = glob[index + 1];
    if (current === "*" && next === "*") {
      source += ".*";
      index += 1;
      continue;
    }
    if (current === "*") {
      source += `[^${escapeRegExp(separator)}]*`;
      continue;
    }
    source += escapeRegExp(current);
  }
  source += "$";
  return new RegExp(source, "u");
};

const isGlobMatch = (glob: string, value: string, separator: string): boolean =>
  globToRegExp(glob, separator).test(value);

const isAllowCoveredByParent = (
  allow: string,
  parentAllow: readonly string[],
): boolean =>
  parentAllow.some((parent) =>
    allow.includes("*") ? parent === allow : isGlobMatch(parent, allow, "."),
  );

const toCapabilityImports = (
  plugins: readonly MuonPluginAccessEntryOptions[],
): readonly MuonCapabilityImportOptions[] =>
  plugins.flatMap((plugin, pluginIndex) =>
    (plugin.imports ?? []).map((entry, importIndex) => {
      if (
        (entry.sources === undefined || entry.sources.length === 0) &&
        (entry.packages === undefined || entry.packages.length === 0)
      ) {
        throw new Error(
          `muon.json plugin.plugins[${pluginIndex}].imports[${importIndex}] requires sources or packages.`,
        );
      }

      const allow = entry.allow ?? plugin.allow;
      const uncoveredAllow = allow.find(
        (functionPath) => !isAllowCoveredByParent(functionPath, plugin.allow),
      );
      if (uncoveredAllow !== undefined) {
        throw new Error(
          `muon.json plugin.plugins[${pluginIndex}].imports[${importIndex}].allow exceeds plugin.plugins[${pluginIndex}].allow: ${uncoveredAllow}`,
        );
      }

      const rule: MuonCapabilityImportOptions = {
        allow,
        pluginName: plugin.name,
      };
      if (entry.sources !== undefined) {
        rule.sources = entry.sources;
      }
      if (entry.packages !== undefined) {
        rule.packages = entry.packages;
      }
      return rule;
    }),
  );

const toRuntimePluginEntries = (
  plugins: readonly MuonPluginAccessEntryOptions[],
): readonly MuonRuntimePluginEntryConfig[] =>
  plugins.map((plugin) => ({
    name: plugin.name,
    allow: [...plugin.allow],
  }));

const mergePluginAccessOptions = (
  base: MuonPluginAccessOptions,
  override: MuonPluginAccessOptions | undefined,
): Required<Pick<MuonPluginAccessOptions, "mode" | "pages" | "plugins">> &
  Pick<MuonPluginAccessOptions, "path"> => ({
  ...(override?.path === undefined ? {} : { path: override.path }),
  mode: override?.mode ?? base.mode ?? "validate",
  pages: override?.pages ?? base.pages ?? defaultPluginPages,
  plugins: override?.plugins ?? base.plugins ?? [],
});

const createRuntimeOverlay = (
  root: string,
  resolved: Required<
    Pick<MuonPluginAccessOptions, "mode" | "pages" | "plugins">
  > &
    Pick<MuonPluginAccessOptions, "path">,
  override: false | MuonPluginAccessOptions | undefined,
): MuonPluginAccessRuntimeOverlay => {
  const path =
    override !== false && override?.path !== undefined
      ? resolve(root, override.path)
      : undefined;
  const plugins =
    override !== false && override?.plugins !== undefined
      ? toRuntimePluginEntries(resolved.plugins)
      : undefined;

  return {
    pages: resolved.pages,
    ...(path === undefined ? {} : { path }),
    ...(plugins === undefined ? {} : { plugins }),
  };
};

/**
 * Resolves public plugin access config from muon.json and Vite overrides.
 *
 * @param input Resolution inputs.
 * @returns Effective plugin access config for bundler integrations.
 */
export const resolveMuonPluginAccessOptions = async ({
  root,
  configPath,
  pluginAccess,
  onConfigReadError,
}: ResolveMuonPluginAccessOptionsInput): Promise<MuonResolvedPluginAccessOptions> => {
  const resolvedConfigPath = await resolveMuonConfigPath(root, configPath);
  let base: MuonPluginAccessOptions = {};
  if (resolvedConfigPath !== undefined) {
    try {
      base = readPluginAccessOptions(
        await readJsonObjectFile(resolvedConfigPath, "Muon config file"),
      );
    } catch (error) {
      if (onConfigReadError === undefined) {
        throw error;
      }
      onConfigReadError(error);
    }
  }

  if (pluginAccess === false) {
    return {
      mode: "simple",
      pages: base.pages ?? defaultPluginPages,
      capabilityOptions: { imports: [] },
      runtimeOverlay: {},
    };
  }

  const resolved = mergePluginAccessOptions(base, pluginAccess);
  const capabilityOptions: MuonCapabilityOptions = {
    imports:
      resolved.mode === "validate" ? toCapabilityImports(resolved.plugins) : [],
  };
  return {
    mode: resolved.mode,
    pages: resolved.pages,
    capabilityOptions,
    runtimeOverlay: createRuntimeOverlay(root, resolved, pluginAccess),
  };
};
