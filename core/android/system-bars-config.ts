// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

/** Placement of web content relative to system bars and display cutouts. */
export type MuonAndroidContentLayout = 'safe-area' | 'edge-to-edge';

/** Color of the system bar icons, independently of the content background. */
export type MuonAndroidSystemBarIcons = 'light' | 'dark';

/** Partial configuration of one Android system bar. */
export interface MuonAndroidSystemBarOptions {
  /** Whether the bar should be visible; omission preserves its request. */
  readonly visible?: boolean;
  /** Icon color; omission preserves the current appearance. */
  readonly icons?: MuonAndroidSystemBarIcons;
}

/** Partial Android system bar configuration. */
export interface MuonAndroidSystemBarsOptions {
  /** Top status bar configuration. */
  readonly statusBar?: MuonAndroidSystemBarOptions;
  /** Bottom navigation bar configuration. */
  readonly navigationBar?: MuonAndroidSystemBarOptions;
  /** Placement policy; omission preserves the current layout. */
  readonly contentLayout?: MuonAndroidContentLayout;
}

/** Initial system bar configuration with visibility and layout defaults resolved. */
export interface MuonAndroidInitialSystemBars {
  /** Initial status bar request, with omitted icons inheriting the theme. */
  readonly statusBar: MuonAndroidSystemBarOptions & {
    readonly visible: boolean;
  };
  /** Initial navigation bar request, with omitted icons inheriting the theme. */
  readonly navigationBar: MuonAndroidSystemBarOptions & {
    readonly visible: boolean;
  };
  /** Initial web content placement policy. */
  readonly contentLayout: MuonAndroidContentLayout;
}

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

/**
 * Validate and copy a partial system bar request.
 * @param options - Public configuration value.
 * @returns Only explicitly requested settings, after validating every key.
 * @throws When any value or key is unsupported.
 */
export const normalizeMuonAndroidSystemBarsOptions = (
  options: unknown
): MuonAndroidSystemBarsOptions => {
  if (!isRecord(options))
    throw new TypeError('System bar options must be an object');
  const result: Record<string, unknown> = {};
  for (const [name, value] of Object.entries(options)) {
    if (name === 'contentLayout') {
      if (value !== 'safe-area' && value !== 'edge-to-edge') {
        throw new TypeError('contentLayout must be safe-area or edge-to-edge');
      }
      result[name] = value;
      continue;
    }
    if (name !== 'statusBar' && name !== 'navigationBar') {
      throw new TypeError(`Unsupported system bar option: ${name}`);
    }
    if (!isRecord(value)) throw new TypeError(`${name} must be an object`);
    const bar: Record<string, unknown> = {};
    for (const [key, setting] of Object.entries(value)) {
      if (key === 'visible') {
        if (typeof setting !== 'boolean')
          throw new TypeError(`${name}.visible must be a boolean`);
      } else if (key === 'icons') {
        if (setting !== 'light' && setting !== 'dark')
          throw new TypeError(`${name}.icons must be light or dark`);
      } else {
        throw new TypeError(`Unsupported ${name} option: ${key}`);
      }
      bar[key] = setting;
    }
    result[name] = bar;
  }
  return result;
};

/**
 * Resolve initial visibility without confusing an omitted window state with normal.
 * @param options - Optional Android-specific configuration.
 * @param initialWindowState - Explicit common window state, or undefined.
 * @returns Initial configuration ready to package in the Android host.
 * @throws When the common and Android-specific requests conflict.
 */
export const resolveMuonAndroidInitialSystemBars = (
  options: unknown,
  initialWindowState: unknown
): MuonAndroidInitialSystemBars => {
  const bars = normalizeMuonAndroidSystemBarsOptions(
    options === undefined ? {} : options
  );
  if (
    initialWindowState !== undefined &&
    initialWindowState !== 'normal' &&
    initialWindowState !== 'fullscreen'
  ) {
    throw new TypeError(
      'Android initialWindowState must be normal or fullscreen'
    );
  }
  const visible = initialWindowState !== 'fullscreen';
  if (initialWindowState !== undefined) {
    for (const name of ['statusBar', 'navigationBar'] as const) {
      if (
        bars[name]?.visible !== undefined &&
        bars[name]?.visible !== visible
      ) {
        throw new TypeError(
          `android.systemBars.${name}.visible conflicts with browser.initialWindowState`
        );
      }
    }
  }
  return {
    contentLayout: bars.contentLayout ?? 'safe-area',
    statusBar: { visible, ...bars.statusBar },
    navigationBar: { visible, ...bars.navigationBar },
  };
};
