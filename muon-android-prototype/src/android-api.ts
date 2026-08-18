// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { MuonWebViewRpcClient } from './webview-rpc.js';

const browserFunctionNames = [
  'reload',
  'toggleFullscreen',
  'enterFullscreen',
  'exitFullscreen',
  'zoomIn',
  'zoomOut',
  'resetZoom',
  'close',
] as const;

const environmentFunctionNames = [
  'getVariables',
  'getConfigValues',
  'getProcessId',
  'getRuntimeInfo',
] as const;

/** Fully-qualified built-in function paths implemented by the Android backend. */
export const muonAndroidBuiltinFunctionPaths = [
  ...browserFunctionNames.map((name) => `muon.browser.${name}` as const),
  ...environmentFunctionNames.map(
    (name) => `muon.environments.${name}` as const
  ),
] as const;

/** A built-in function path available on Android. */
export type MuonAndroidBuiltinFunctionPath =
  (typeof muonAndroidBuiltinFunctionPaths)[number];

/** Capability ids assigned to Android built-in namespaces. */
export interface MuonAndroidCapabilityIds {
  /** Capability for browser operations. */
  readonly 'muon.browser': string;

  /** Capability for environment information. */
  readonly 'muon.environments': string;
}

/** Android browser functions available in simple mode. */
export type MuonAndroidBrowserApi = Readonly<
  Record<(typeof browserFunctionNames)[number], () => Promise<void>>
>;

/** Runtime information reported by the Android WebView backend. */
export interface MuonAndroidRuntimeInfo {
  /** Selects the Android arm of the shared runtime-information union. */
  readonly backend: 'android-webview';

  /** Android operating-system identifier. */
  readonly os: 'android';

  /** User-visible Android release string. */
  readonly osVersion: string;

  /** Android SDK level running this application process. */
  readonly apiLevel: number;

  /** Primary ABI selected for this process. */
  readonly abi: string;

  /** Android application package name. */
  readonly applicationId: string;

  /** Android application version name. */
  readonly applicationVersion: string;

  /** Package providing WebView to this process. */
  readonly webViewPackage: string;

  /** Version of the WebView provider package. */
  readonly webViewVersion: string;
}

/** Android environment functions available in simple mode. */
export interface MuonAndroidEnvironmentsApi {
  /** Returns the current process environment variables. */
  readonly getVariables: () => Promise<Record<string, string>>;

  /** Returns the merged application string configuration. */
  readonly getConfigValues: () => Promise<Record<string, string>>;

  /** Returns the current Android application process id. */
  readonly getProcessId: () => Promise<number>;

  /** Returns Android and WebView runtime information. */
  readonly getRuntimeInfo: () => Promise<MuonAndroidRuntimeInfo>;
}

/** Built-in API object exposed by Android simple mode. */
export interface MuonAndroidSimpleApi {
  /** Operations for the owning Activity and WebView. */
  readonly browser: MuonAndroidBrowserApi;

  /** Information about the current Android application process. */
  readonly environments: MuonAndroidEnvironmentsApi;
}

const escapeRegularExpression = (source: string): string =>
  source.replace(/[\\^$+?.()|[\]{}]/gu, '\\$&');

const createFunctionAllowExpression = (allow: string): RegExp => {
  let source = '^';
  for (let index = 0; index < allow.length; index += 1) {
    const character = allow[index] ?? '';
    const next = allow[index + 1];
    if (character === '*' && next === '*') {
      source += '.*';
      index += 1;
    } else if (character === '*') {
      source += '[^.]*';
    } else {
      source += escapeRegularExpression(character);
    }
  }
  source += '$';
  return new RegExp(source, 'u');
};

/**
 * Expands Android capability allow patterns into available function paths.
 *
 * @param allows - Exact paths or glob patterns requested by a validate build.
 * @returns Available Android paths in stable metadata order.
 * @remarks An exact unsupported path, or a pattern matching no Android
 * functions, fails during the build instead of producing a runtime stub.
 */
export const expandMuonAndroidFunctionAllows = (
  allows: readonly string[]
): readonly MuonAndroidBuiltinFunctionPath[] => {
  const selected = new Set<MuonAndroidBuiltinFunctionPath>();
  for (const allow of allows) {
    if (!allow.includes('*')) {
      const functionPath = muonAndroidBuiltinFunctionPaths.find(
        (candidate) => candidate === allow
      );
      if (functionPath === undefined) {
        throw new Error(`Muon function is unavailable for Android: ${allow}`);
      }
      selected.add(functionPath);
      continue;
    }

    const expression = createFunctionAllowExpression(allow);
    const matches = muonAndroidBuiltinFunctionPaths.filter((functionPath) =>
      expression.test(functionPath)
    );
    if (matches.length === 0) {
      throw new Error(
        `Muon function pattern is unavailable for Android: ${allow}`
      );
    }
    for (const functionPath of matches) {
      selected.add(functionPath);
    }
  }
  return muonAndroidBuiltinFunctionPaths.filter((functionPath) =>
    selected.has(functionPath)
  );
};

const createVoidNamespace = <TName extends string>(
  client: MuonWebViewRpcClient,
  capabilityId: string,
  namespace: string,
  functionNames: readonly TName[]
): Readonly<Record<TName, () => Promise<void>>> => {
  const entries = functionNames.map(
    (name) =>
      [
        name,
        async () => {
          await client.call(capabilityId, `${namespace}.${name}`, []);
        },
      ] as const
  );
  return Object.freeze(Object.fromEntries(entries)) as Readonly<
    Record<TName, () => Promise<void>>
  >;
};

const parseNativeJson = async <T>(source: Promise<unknown>): Promise<T> => {
  const value = await source;
  if (typeof value !== 'string') {
    throw new TypeError('Android native JSON result is invalid');
  }
  return JSON.parse(value) as T;
};

/**
 * Creates the Android simple-mode `window.muon` object.
 *
 * @param client - WebView RPC client used by every exposed function.
 * @param capabilityIds - Namespace capability ids generated for this page.
 * @returns An immutable object containing only implemented Android functions.
 */
export const createMuonAndroidSimpleApi = (
  client: MuonWebViewRpcClient,
  capabilityIds: MuonAndroidCapabilityIds
): MuonAndroidSimpleApi =>
  Object.freeze({
    browser: createVoidNamespace(
      client,
      capabilityIds['muon.browser'],
      'muon.browser',
      browserFunctionNames
    ),
    environments: Object.freeze({
      getVariables: () =>
        parseNativeJson<Record<string, string>>(
          client.call(
            capabilityIds['muon.environments'],
            'muon.environments.getVariables',
            []
          )
        ),
      getConfigValues: () =>
        parseNativeJson<Record<string, string>>(
          client.call(
            capabilityIds['muon.environments'],
            'muon.environments.getConfigValues',
            []
          )
        ),
      getProcessId: async () => {
        const value = await client.call(
          capabilityIds['muon.environments'],
          'muon.environments.getProcessId',
          []
        );
        if (
          typeof value !== 'number' ||
          !Number.isInteger(value) ||
          value <= 0 ||
          value > 0xffffffff
        ) {
          throw new TypeError('Android process id result is invalid');
        }
        return value;
      },
      getRuntimeInfo: () =>
        parseNativeJson<MuonAndroidRuntimeInfo>(
          client.call(
            capabilityIds['muon.environments'],
            'muon.environments.getRuntimeInfo',
            []
          )
        ),
    }),
  });
