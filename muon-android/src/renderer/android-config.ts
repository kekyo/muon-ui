// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

/** Result of validating a merged muon configuration for Android. */
export interface MuonAndroidConfigValidationResult {
  /** Diagnostics for accepted desktop settings that Android does not enforce. */
  readonly warnings: readonly string[];
}

type ConfigObject = Record<string, unknown>;

const isConfigObject = (value: unknown): value is ConfigObject =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

const hasOwn = (value: ConfigObject, name: string): boolean =>
  Object.prototype.hasOwnProperty.call(value, name);

const requireOptionalSection = (
  config: ConfigObject,
  name: string,
  path: string
): ConfigObject | undefined => {
  const value = config[name];
  if (value === undefined) {
    return undefined;
  }
  if (!isConfigObject(value)) {
    throw new Error(`muon.json ${path} must be an object`);
  }
  return value;
};

const rejectConfiguredValue = (
  section: ConfigObject,
  name: string,
  path: string
): void => {
  if (hasOwn(section, name) && section[name] !== undefined) {
    throw new Error(`muon.json ${path} is unavailable on Android`);
  }
};

const validateBrowserConfig = (browser: ConfigObject | undefined): void => {
  if (browser === undefined) {
    return;
  }

  rejectConfiguredValue(browser, 'profilePath', 'browser.profilePath');
  rejectConfiguredValue(browser, 'profile', 'browser.profile');
  rejectConfiguredValue(browser, 'titleBarType', 'browser.titleBarType');
  rejectConfiguredValue(
    browser,
    'initialTitleBarVisibility',
    'browser.initialTitleBarVisibility'
  );
  rejectConfiguredValue(
    browser,
    'initialTitleBarIcon',
    'browser.initialTitleBarIcon'
  );

  const initialWindowState = browser.initialWindowState;
  if (
    initialWindowState !== undefined &&
    initialWindowState !== 'normal' &&
    initialWindowState !== 'fullscreen'
  ) {
    throw new Error(
      'muon.json browser.initialWindowState supports only normal or fullscreen on Android'
    );
  }

  const contextMenu = requireOptionalSection(
    browser,
    'contextMenu',
    'browser.contextMenu'
  );
  const contextMenuMode = contextMenu?.mode;
  if (
    contextMenuMode !== undefined &&
    contextMenuMode !== 'standard' &&
    contextMenuMode !== 'disabled'
  ) {
    throw new Error(
      'muon.json browser.contextMenu.mode supports only standard or disabled on Android'
    );
  }

  const keybind = browser.keybind;
  if (
    keybind !== undefined &&
    (!isConfigObject(keybind) || Object.keys(keybind).length !== 0)
  ) {
    throw new Error(
      'muon.json browser.keybind must be an empty object on Android'
    );
  }

  const unsafeParentAccess = browser.allowUnsafeJavaScriptParentAccess;
  if (
    unsafeParentAccess !== undefined &&
    (!Array.isArray(unsafeParentAccess) || unsafeParentAccess.length !== 0)
  ) {
    throw new Error(
      'muon.json browser.allowUnsafeJavaScriptParentAccess must be an empty array on Android'
    );
  }
};

const validatePluginConfig = (
  plugin: ConfigObject | undefined,
  warnings: string[]
): void => {
  if (plugin === undefined) {
    return;
  }
  rejectConfiguredValue(plugin, 'path', 'plugin.path');

  if (hasOwn(plugin, 'pages') && plugin.pages !== undefined) {
    warnings.push(
      'muon.json plugin.pages cannot expose Android RPC outside configured asset origins.'
    );
  }

  const plugins = plugin.plugins;
  if (plugins === undefined) {
    return;
  }
  if (!Array.isArray(plugins)) {
    throw new Error('muon.json plugin.plugins must be an array');
  }
  for (const [index, entry] of plugins.entries()) {
    if (!isConfigObject(entry)) {
      throw new Error(`muon.json plugin.plugins[${index}] must be an object`);
    }
    rejectConfiguredValue(
      entry,
      'signature',
      `plugin.plugins[${index}].signature`
    );
    rejectConfiguredValue(entry, 'salt', `plugin.plugins[${index}].salt`);
  }
};

const validateNetworkConfig = (
  network: ConfigObject | undefined,
  warnings: string[]
): void => {
  if (
    network !== undefined &&
    ['allow', 'authorizedOrigin', 'localAccess'].some((name) =>
      hasOwn(network, name)
    )
  ) {
    warnings.push(
      'muon.json network policy is not enforced by the Android WebView backend.'
    );
  }
};

/**
 * Validates Android-specific acceptance rules for a merged muon.json value.
 *
 * @param config - Parsed and merged muon configuration.
 * @returns Warnings for accepted settings whose desktop policy is not enforced.
 * @throws When a setting has no Android-compatible meaning.
 */
export const validateMuonAndroidConfig = (
  config: unknown
): MuonAndroidConfigValidationResult => {
  if (!isConfigObject(config)) {
    throw new Error('muon.json must be an object');
  }

  const warnings: string[] = [];
  validateBrowserConfig(requireOptionalSection(config, 'browser', 'browser'));
  validatePluginConfig(
    requireOptionalSection(config, 'plugin', 'plugin'),
    warnings
  );
  validateNetworkConfig(
    requireOptionalSection(config, 'network', 'network'),
    warnings
  );

  const cdp = requireOptionalSection(config, 'cdp', 'cdp');
  if (cdp?.enable !== undefined && cdp.enable !== false) {
    throw new Error('muon.json cdp.enable must be false on Android');
  }

  const node = requireOptionalSection(config, 'node', 'node');
  rejectConfiguredValue(node ?? {}, 'project', 'node.project');

  return Object.freeze({ warnings: Object.freeze(warnings) });
};
