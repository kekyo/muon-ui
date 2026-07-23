// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

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
 * Import-side capability rule from public muon plugin access config.
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
   * @remarks Required in validate mode. Simple mode does not use import rules.
   */
  allow?: readonly string[];
}

/**
 * Plugin entry from public muon plugin access config.
 */
export interface MuonPluginAccessEntryOptions {
  /**
   * Plugin entry name.
   */
  name: string;
  /**
   * Optional 64-character hexadecimal SHA-256 digest of the final external
   * `.so` or `.dll` file bytes followed by the decoded `salt` bytes.
   *
   * @remarks Calculate this after all library post-processing, including
   * stripping and code signing. It is not supported for `internal`.
   */
  signature?: string;
  /**
   * Optional hex-encoded salt whose decoded bytes are appended when checking
   * the plugin signature.
   *
   * @remarks Required when `signature` is specified. It is not supported for
   * `internal`.
   */
  salt?: string;
  /**
   * Plugin-defined string key-value configuration entries.
   */
  config?: Readonly<Record<string, string>>;
  /**
   * Plugin function path globs allowed by the runtime plugin policy.
   *
   * @remarks Required in simple mode. Validate mode derives the runtime
   * allowlist from `imports[].allow` and rejects this field in public config.
   */
  allow?: readonly string[];
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
  /**
   * Resolved external plugin directory used by build-time inspectors.
   */
  pluginPath: string;
  /**
   * Effective plugin entries from muon.json and Vite options.
   */
  plugins: readonly MuonPluginAccessEntryOptions[];
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

const isSha256HexString = (value: string): boolean =>
  value.length === 64 && /^[0-9a-fA-F]+$/.test(value);

const isHexByteString = (value: string): boolean =>
  value.length % 2 === 0 && /^[0-9a-fA-F]*$/.test(value);

const containsNul = (value: string): boolean => value.includes("\0");

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
    throw new Error(`muon config file does not exist: ${resolvedPath}`);
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

const readOptionalStringRecord = (
  object: JsonObject,
  key: string,
  configPath: string,
): Readonly<Record<string, string>> | undefined => {
  const value = object[key];
  if (value === undefined) {
    return undefined;
  }
  if (!isJsonObject(value)) {
    throw new Error(`muon.json ${configPath} must be an object.`);
  }
  const config: Record<string, string> = {};
  for (const [entryKey, entryValue] of Object.entries(value)) {
    if (entryKey.length === 0) {
      throw new Error(`muon.json ${configPath} key must not be empty.`);
    }
    if (containsNul(entryKey)) {
      throw new Error(`muon.json ${configPath} key must not contain NUL.`);
    }
    const valuePath = `${configPath}.${entryKey}`;
    if (typeof entryValue !== "string") {
      throw new Error(`muon.json ${valuePath} must be a string.`);
    }
    if (containsNul(entryValue)) {
      throw new Error(`muon.json ${valuePath} must not contain NUL.`);
    }
    config[entryKey] = entryValue;
  }
  return config;
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
  if (value.signature !== undefined) {
    if (typeof value.signature !== "string") {
      throw new Error(`muon.json ${configPath}.signature must be a string.`);
    }
    if (!isSha256HexString(value.signature)) {
      throw new Error(
        `muon.json ${configPath}.signature must be a 64-character SHA-256 hex string.`,
      );
    }
  }
  if (value.salt !== undefined) {
    if (typeof value.salt !== "string") {
      throw new Error(`muon.json ${configPath}.salt must be a string.`);
    }
    if (!isHexByteString(value.salt)) {
      throw new Error(
        `muon.json ${configPath}.salt must be a hexadecimal byte string.`,
      );
    }
  }
  const allow = readOptionalStringArray(value, "allow", `${configPath}.allow`);
  const config = readOptionalStringRecord(
    value,
    "config",
    `${configPath}.config`,
  );

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
  };
  if (value.signature !== undefined) {
    options.signature = value.signature;
  }
  if (value.salt !== undefined) {
    options.salt = value.salt;
  }
  if (config !== undefined) {
    options.config = config;
  }
  if (allow !== undefined) {
    options.allow = allow;
  }
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

type EffectivePluginAccessOptions = Required<
  Pick<MuonPluginAccessOptions, "mode" | "pages" | "plugins">
> &
  Pick<MuonPluginAccessOptions, "path">;

const hasImportSourceSelector = (
  entry: MuonPluginAccessImportOptions,
): boolean =>
  (entry.sources !== undefined && entry.sources.length > 0) ||
  (entry.packages !== undefined && entry.packages.length > 0);

const validatePluginConfig = (rawConfig: unknown, configPath: string): void => {
  if (rawConfig === undefined) {
    return;
  }
  if (!isJsonObject(rawConfig)) {
    throw new Error(`muon.json ${configPath} must be an object.`);
  }
  for (const [entryKey, entryValue] of Object.entries(rawConfig)) {
    if (entryKey.length === 0) {
      throw new Error(`muon.json ${configPath} key must not be empty.`);
    }
    if (containsNul(entryKey)) {
      throw new Error(`muon.json ${configPath} key must not contain NUL.`);
    }
    const valuePath = `${configPath}.${entryKey}`;
    if (typeof entryValue !== "string") {
      throw new Error(`muon.json ${valuePath} must be a string.`);
    }
    if (containsNul(entryValue)) {
      throw new Error(`muon.json ${valuePath} must not contain NUL.`);
    }
  }
};

const validatePluginAccessOptions = (
  resolved: EffectivePluginAccessOptions,
): EffectivePluginAccessOptions => {
  for (const [pluginIndex, plugin] of resolved.plugins.entries()) {
    const pluginPath = `plugin.plugins[${pluginIndex}]`;
    const signature: unknown = plugin.signature;
    if (signature !== undefined) {
      if (typeof signature !== "string") {
        throw new Error(`muon.json ${pluginPath}.signature must be a string.`);
      }
      if (!isSha256HexString(signature)) {
        throw new Error(
          `muon.json ${pluginPath}.signature must be a 64-character SHA-256 hex string.`,
        );
      }
    }
    const salt: unknown = plugin.salt;
    if (salt !== undefined) {
      if (typeof salt !== "string") {
        throw new Error(`muon.json ${pluginPath}.salt must be a string.`);
      }
      if (!isHexByteString(salt)) {
        throw new Error(
          `muon.json ${pluginPath}.salt must be a hexadecimal byte string.`,
        );
      }
    }
    validatePluginConfig(
      (plugin as { config?: unknown }).config,
      `${pluginPath}.config`,
    );
    if (plugin.name === "internal" && plugin.signature !== undefined) {
      throw new Error(
        `muon.json ${pluginPath}.signature is not supported for internal plugins.`,
      );
    }
    if (plugin.name === "internal" && plugin.salt !== undefined) {
      throw new Error(
        `muon.json ${pluginPath}.salt is not supported for internal plugins.`,
      );
    }
    if (plugin.signature !== undefined && plugin.salt === undefined) {
      throw new Error(
        `muon.json ${pluginPath}.signature requires ${pluginPath}.salt.`,
      );
    }
    if (resolved.mode === "validate") {
      if (plugin.allow !== undefined) {
        throw new Error(
          `muon.json ${pluginPath}.allow is only supported in simple mode; use ${pluginPath}.imports[].allow in validate mode.`,
        );
      }
      if (plugin.imports === undefined) {
        throw new Error(
          `muon.json ${pluginPath}.imports is required in validate mode.`,
        );
      }
      if (plugin.imports.length === 0) {
        throw new Error(
          `muon.json ${pluginPath}.imports must not be empty in validate mode.`,
        );
      }

      for (const [importIndex, entry] of plugin.imports.entries()) {
        const importPath = `${pluginPath}.imports[${importIndex}]`;
        if (!hasImportSourceSelector(entry)) {
          throw new Error(
            `muon.json ${importPath} requires sources or packages.`,
          );
        }
        if (entry.allow === undefined) {
          throw new Error(
            `muon.json ${importPath}.allow is required in validate mode.`,
          );
        }
        if (entry.allow.length === 0) {
          throw new Error(
            `muon.json ${importPath}.allow must not be empty in validate mode.`,
          );
        }
      }
      continue;
    }

    if (plugin.allow === undefined) {
      throw new Error(
        `muon.json ${pluginPath}.allow is required in simple mode.`,
      );
    }
    if (plugin.imports !== undefined) {
      throw new Error(
        `muon.json ${pluginPath}.imports is only supported in validate mode.`,
      );
    }
  }
  return resolved;
};

const getValidateImportAllow = (
  entry: MuonPluginAccessImportOptions,
  pluginIndex: number,
  importIndex: number,
): readonly string[] => {
  if (entry.allow === undefined || entry.allow.length === 0) {
    throw new Error(
      `muon.json plugin.plugins[${pluginIndex}].imports[${importIndex}].allow is required in validate mode.`,
    );
  }
  return entry.allow;
};

const getSimplePluginAllow = (
  plugin: MuonPluginAccessEntryOptions,
  pluginIndex: number,
): readonly string[] => {
  if (plugin.allow === undefined) {
    throw new Error(
      `muon.json plugin.plugins[${pluginIndex}].allow is required in simple mode.`,
    );
  }
  return plugin.allow;
};

const toCapabilityImports = (
  plugins: readonly MuonPluginAccessEntryOptions[],
): readonly MuonCapabilityImportOptions[] =>
  plugins.flatMap((plugin, pluginIndex) =>
    (plugin.imports ?? []).map((entry, importIndex) => {
      if (!hasImportSourceSelector(entry)) {
        throw new Error(
          `muon.json plugin.plugins[${pluginIndex}].imports[${importIndex}] requires sources or packages.`,
        );
      }

      const rule: MuonCapabilityImportOptions = {
        allow: getValidateImportAllow(entry, pluginIndex, importIndex),
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

const toValidateRuntimePluginEntries = (
  plugins: readonly MuonPluginAccessEntryOptions[],
): readonly MuonRuntimePluginEntryConfig[] =>
  plugins.map((plugin, pluginIndex) => {
    const allow: string[] = [];
    const seenAllow = new Set<string>();
    for (const [importIndex, entry] of (plugin.imports ?? []).entries()) {
      for (const functionPath of getValidateImportAllow(
        entry,
        pluginIndex,
        importIndex,
      )) {
        if (!seenAllow.has(functionPath)) {
          seenAllow.add(functionPath);
          allow.push(functionPath);
        }
      }
    }
    return {
      name: plugin.name,
      ...(plugin.signature === undefined
        ? {}
        : { signature: plugin.signature }),
      ...(plugin.salt === undefined ? {} : { salt: plugin.salt }),
      ...(plugin.config === undefined ? {} : { config: plugin.config }),
      allow,
    };
  });

const toSimpleRuntimePluginEntries = (
  plugins: readonly MuonPluginAccessEntryOptions[],
): readonly MuonRuntimePluginEntryConfig[] =>
  plugins.map((plugin, pluginIndex) => ({
    name: plugin.name,
    ...(plugin.signature === undefined ? {} : { signature: plugin.signature }),
    ...(plugin.salt === undefined ? {} : { salt: plugin.salt }),
    ...(plugin.config === undefined ? {} : { config: plugin.config }),
    allow: [...getSimplePluginAllow(plugin, pluginIndex)],
  }));

const shouldOverlayRuntimePlugins = (
  resolved: EffectivePluginAccessOptions,
  override: false | MuonPluginAccessOptions | undefined,
): boolean =>
  override !== false &&
  (override?.plugins !== undefined ||
    (resolved.mode === "validate" && resolved.plugins.length > 0));

const toRuntimePluginEntries = (
  resolved: EffectivePluginAccessOptions,
): readonly MuonRuntimePluginEntryConfig[] =>
  resolved.mode === "validate"
    ? toValidateRuntimePluginEntries(resolved.plugins)
    : toSimpleRuntimePluginEntries(resolved.plugins);

const mergePluginAccessOptions = (
  base: MuonPluginAccessOptions,
  override: MuonPluginAccessOptions | undefined,
): EffectivePluginAccessOptions => ({
  ...((override?.path ?? base.path) === undefined
    ? {}
    : { path: override?.path ?? base.path }),
  mode: override?.mode ?? base.mode ?? "validate",
  pages: override?.pages ?? base.pages ?? defaultPluginPages,
  plugins: override?.plugins ?? base.plugins ?? [],
});

const createRuntimeOverlay = (
  root: string,
  resolved: EffectivePluginAccessOptions,
  override: false | MuonPluginAccessOptions | undefined,
): MuonPluginAccessRuntimeOverlay => {
  if (override === false) {
    return {};
  }

  const path =
    override?.path !== undefined ? resolve(root, override.path) : undefined;
  const plugins = shouldOverlayRuntimePlugins(resolved, override)
    ? toRuntimePluginEntries(resolved)
    : undefined;

  return {
    pages: resolved.pages,
    ...(path === undefined ? {} : { path }),
    ...(plugins === undefined ? {} : { plugins }),
  };
};

const resolveInspectorPluginPath = (
  root: string,
  configPath: string | undefined,
  resolved: EffectivePluginAccessOptions,
  override: false | MuonPluginAccessOptions | undefined,
): string => {
  if (override !== false && override?.path !== undefined) {
    return resolve(root, override.path);
  }
  const basePath = configPath === undefined ? root : resolve(configPath, "..");
  return resolve(basePath, resolved.path ?? "./plugins");
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
        await readJsonObjectFile(resolvedConfigPath, "muon config file"),
      );
    } catch (error) {
      if (onConfigReadError === undefined) {
        throw error;
      }
      onConfigReadError(error);
    }
  }

  if (pluginAccess === false) {
    const resolved = validatePluginAccessOptions(
      mergePluginAccessOptions(base, { mode: "simple" }),
    );
    return {
      mode: "simple",
      pages: resolved.pages,
      capabilityOptions: { imports: [] },
      runtimeOverlay: createRuntimeOverlay(root, resolved, pluginAccess),
      pluginPath: resolveInspectorPluginPath(
        root,
        resolvedConfigPath,
        resolved,
        pluginAccess,
      ),
      plugins: resolved.plugins,
    };
  }

  const resolved = validatePluginAccessOptions(
    mergePluginAccessOptions(base, pluginAccess),
  );
  const capabilityOptions: MuonCapabilityOptions = {
    imports:
      resolved.mode === "validate" ? toCapabilityImports(resolved.plugins) : [],
  };
  return {
    mode: resolved.mode,
    pages: resolved.pages,
    capabilityOptions,
    runtimeOverlay: createRuntimeOverlay(root, resolved, pluginAccess),
    pluginPath: resolveInspectorPluginPath(
      root,
      resolvedConfigPath,
      resolved,
      pluginAccess,
    ),
    plugins: resolved.plugins,
  };
};
