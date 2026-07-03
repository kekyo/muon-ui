// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import type { MuonVitePluginOptions } from "./vite.js";

/**
 * Metadata symbol used to recover `muon()` plugin options from `vite.config.*`.
 */
export const muonVitePluginOptionsSymbol = Symbol.for(
  "muon.vite.plugin.options",
);

interface MuonVitePluginWithOptions {
  [muonVitePluginOptionsSymbol]: MuonVitePluginOptions;
}

const isRecord = (value: unknown): value is Record<string | symbol, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const isStringArray = (value: unknown): value is readonly string[] =>
  Array.isArray(value) && value.every((entry) => typeof entry === "string");

const isWindowsResourceOptions = (value: unknown): boolean => {
  if (!isRecord(value)) {
    return false;
  }

  return (
    (value.iconPath === undefined || typeof value.iconPath === "string") &&
    (value.productName === undefined ||
      typeof value.productName === "string") &&
    (value.fileDescription === undefined ||
      typeof value.fileDescription === "string") &&
    (value.companyName === undefined ||
      typeof value.companyName === "string") &&
    (value.version === undefined || typeof value.version === "string") &&
    (value.copyright === undefined || typeof value.copyright === "string") &&
    (value.language === undefined ||
      (typeof value.language === "number" &&
        Number.isInteger(value.language))) &&
    (value.codePage === undefined ||
      (typeof value.codePage === "number" && Number.isInteger(value.codePage)))
  );
};

const isLinuxDesktopOptions = (value: unknown): boolean => {
  if (!isRecord(value)) {
    return false;
  }

  return (
    (value.desktopId === undefined || typeof value.desktopId === "string") &&
    (value.name === undefined || typeof value.name === "string") &&
    (value.comment === undefined || typeof value.comment === "string") &&
    (value.iconPath === undefined || typeof value.iconPath === "string") &&
    (value.categories === undefined || isStringArray(value.categories)) &&
    (value.startupNotify === undefined ||
      typeof value.startupNotify === "boolean")
  );
};

const isMuonViteBuildOptions = (value: unknown): boolean => {
  if (!isRecord(value)) {
    return false;
  }

  return (
    (value.targets === undefined || isStringArray(value.targets)) &&
    (value.allTargets === undefined || typeof value.allTargets === "boolean") &&
    (value.appName === undefined || typeof value.appName === "string") &&
    (value.appId === undefined || typeof value.appId === "string") &&
    (value.outputRoot === undefined || typeof value.outputRoot === "string") &&
    (value.configPath === undefined || typeof value.configPath === "string") &&
    (value.iconPath === undefined || typeof value.iconPath === "string") &&
    (value.windowsResource === undefined ||
      isWindowsResourceOptions(value.windowsResource)) &&
    (value.linuxDesktop === undefined ||
      isLinuxDesktopOptions(value.linuxDesktop)) &&
    (value.packageDirectory === undefined ||
      typeof value.packageDirectory === "string") &&
    (value.assetSalt === undefined || value.assetSalt instanceof Uint8Array)
  );
};

const isMuonVitePluginAccessImportOptions = (value: unknown): boolean => {
  if (!isRecord(value)) {
    return false;
  }

  return (
    (value.sources === undefined || isStringArray(value.sources)) &&
    (value.packages === undefined || isStringArray(value.packages)) &&
    (value.allow === undefined || isStringArray(value.allow))
  );
};

const isMuonVitePluginAccessEntryOptions = (value: unknown): boolean => {
  if (!isRecord(value)) {
    return false;
  }

  return (
    typeof value.name === "string" &&
    (value.allow === undefined || isStringArray(value.allow)) &&
    (value.imports === undefined ||
      (Array.isArray(value.imports) &&
        value.imports.every(isMuonVitePluginAccessImportOptions)))
  );
};

const isMuonVitePluginAccessOptions = (value: unknown): boolean => {
  if (!isRecord(value)) {
    return false;
  }

  return (
    (value.path === undefined || typeof value.path === "string") &&
    (value.mode === undefined ||
      value.mode === "simple" ||
      value.mode === "validate") &&
    (value.pages === undefined || isStringArray(value.pages)) &&
    (value.plugins === undefined ||
      (Array.isArray(value.plugins) &&
        value.plugins.every(isMuonVitePluginAccessEntryOptions)))
  );
};

const isMuonVitePluginOptions = (
  value: unknown,
): value is MuonVitePluginOptions => {
  if (!isRecord(value)) {
    return false;
  }

  return (
    (value.muonPath === undefined || typeof value.muonPath === "string") &&
    (value.cefPath === undefined || typeof value.cefPath === "string") &&
    (value.stagePath === undefined || typeof value.stagePath === "string") &&
    (value.open === undefined || typeof value.open === "boolean") &&
    (value.enableDebugger === undefined ||
      typeof value.enableDebugger === "boolean") &&
    (value.pluginAccess === undefined ||
      value.pluginAccess === false ||
      isMuonVitePluginAccessOptions(value.pluginAccess)) &&
    (value.build === undefined ||
      typeof value.build === "boolean" ||
      isMuonViteBuildOptions(value.build))
  );
};

/**
 * Attaches raw Muon Vite plugin options to a plugin instance.
 *
 * @param plugin Plugin object.
 * @param options Raw plugin options.
 * @returns Plugin object with internal Muon metadata.
 */
export const attachMuonVitePluginOptions = <TPlugin extends object>(
  plugin: TPlugin,
  options: MuonVitePluginOptions,
): TPlugin & MuonVitePluginWithOptions => {
  Object.defineProperty(plugin, muonVitePluginOptionsSymbol, {
    configurable: false,
    enumerable: false,
    value: { ...options },
    writable: false,
  });

  return plugin as TPlugin & MuonVitePluginWithOptions;
};

/**
 * Reads raw Muon Vite plugin options from a plugin instance.
 *
 * @param plugin Candidate plugin object.
 * @returns Attached Muon options, if present.
 */
export const getMuonVitePluginOptions = (
  plugin: unknown,
): MuonVitePluginOptions | undefined => {
  if (!isRecord(plugin)) {
    return undefined;
  }

  const options = plugin[muonVitePluginOptionsSymbol];
  return isMuonVitePluginOptions(options) ? options : undefined;
};

/**
 * Resolves nested Vite plugin option values into a flat list.
 *
 * @param pluginOptions Raw `plugins` field from a Vite config.
 * @returns Flat plugin object list.
 */
export const flattenVitePluginOptions = async (
  pluginOptions: unknown,
): Promise<unknown[]> => {
  const resolvedValue = await pluginOptions;
  if (resolvedValue === null || resolvedValue === undefined || !resolvedValue) {
    return [];
  }

  if (Array.isArray(resolvedValue)) {
    const nested = await Promise.all(
      resolvedValue.map((entry) => flattenVitePluginOptions(entry)),
    );
    return nested.flat();
  }

  return [resolvedValue];
};
