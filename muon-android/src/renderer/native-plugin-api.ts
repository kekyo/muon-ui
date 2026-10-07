// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type {
  MuonAndroidRendererMetadata,
  MuonNativeNamespaceMetadata,
} from './native-plugin-metadata.js';
import type { MuonWebViewRpcClient } from './webview-rpc.js';

/** Existing root objects merged with native namespaces in simple mode. */
export type MuonAndroidNativePluginBaseRoots = Readonly<Record<string, object>>;

const getNamespaceSegments = (namespace: string): readonly string[] =>
  namespace.split('.');

const getOrCreateNamespace = (
  roots: Record<string, Record<string, unknown>>,
  namespace: string
): Record<string, unknown> => {
  const segments = getNamespaceSegments(namespace);
  let current = roots[segments[0]!];
  if (current === undefined) {
    current = {};
    roots[segments[0]!] = current;
  }
  for (const segment of segments.slice(1)) {
    const existing = current[segment];
    if (existing === undefined) {
      const created: Record<string, unknown> = {};
      Object.defineProperty(current, segment, {
        configurable: false,
        enumerable: !segment.startsWith('__'),
        writable: false,
        value: created,
      });
      current = created;
    } else if (typeof existing === 'object' && existing !== null) {
      current = existing as Record<string, unknown>;
    } else {
      throw new Error(`Muon plugin namespace conflicts with ${namespace}`);
    }
  }
  return current;
};

const deepFreeze = (value: unknown, seen: WeakSet<object>): void => {
  if (
    value === null ||
    (typeof value !== 'object' && typeof value !== 'function')
  ) {
    return;
  }
  if (seen.has(value)) {
    return;
  }
  seen.add(value);
  for (const key of Reflect.ownKeys(value)) {
    deepFreeze(Reflect.get(value, key), seen);
  }
  Object.freeze(value);
};

const executeSetupScript = (
  pluginNamespace: MuonNativeNamespaceMetadata,
  namespaceObject: Record<string, unknown>,
  target: Record<PropertyKey, unknown>
): void => {
  if (pluginNamespace.setupScript.length === 0) {
    return;
  }
  const setup = Function(
    'namespace',
    'globalThis',
    'isAllowed',
    `"use strict";\n${pluginNamespace.setupScript}`
  ) as (
    namespace: Record<string, unknown>,
    global: Record<PropertyKey, unknown>,
    isAllowed: (name: string) => boolean
  ) => void;
  const allowed = new Set(pluginNamespace.allowedFunctions);
  setup(namespaceObject, target, (name) => allowed.has(name));
};

/**
 * Installs loaded native plugin functions in the trusted simple-mode context.
 *
 * @param client - Typed WebView RPC client.
 * @param metadata - Runtime metadata captured before document loading.
 * @param baseRoots - Existing Android platform API root objects.
 * @param target - Global-like object receiving plugin roots.
 * @returns A function that restores the previous root descriptors.
 */
export const installMuonAndroidNativePluginApi = (
  client: MuonWebViewRpcClient,
  metadata: MuonAndroidRendererMetadata,
  baseRoots: MuonAndroidNativePluginBaseRoots,
  target: Record<PropertyKey, unknown> = globalThis as unknown as Record<
    PropertyKey,
    unknown
  >
): (() => void) => {
  if (metadata.mode !== 'simple') {
    return () => {};
  }
  const namespaceMetadata = new Map(
    metadata.namespaces.map((namespace) => [namespace.namespace, namespace])
  );
  const roots: Record<string, Record<string, unknown>> = {};
  for (const [name, value] of Object.entries(baseRoots)) {
    roots[name] = { ...(value as Record<string, unknown>) };
  }

  for (const pluginNamespace of metadata.namespaces) {
    getOrCreateNamespace(roots, pluginNamespace.namespace);
  }
  for (const function_ of metadata.functions) {
    const pluginNamespace = namespaceMetadata.get(function_.namespace);
    if (
      pluginNamespace === undefined ||
      !pluginNamespace.allowedFunctions.includes(function_.name)
    ) {
      throw new Error(
        `Android native plugin function is not allowed: ${function_.namespace}.${function_.name}`
      );
    }
    const namespaceObject = getOrCreateNamespace(roots, function_.namespace);
    if (Reflect.has(namespaceObject, function_.name)) {
      throw new Error(
        `Muon plugin function conflicts with ${function_.namespace}.${function_.name}`
      );
    }
    const call = (...arguments_: readonly unknown[]): Promise<unknown> =>
      client.call(
        function_.capabilityId,
        `${function_.namespace}.${function_.publicName}`,
        arguments_
      );
    Object.defineProperty(namespaceObject, function_.name, {
      configurable: false,
      enumerable: !function_.name.startsWith('__'),
      writable: false,
      value: call,
    });
  }

  const previousDescriptors = new Map<string, PropertyDescriptor | undefined>();
  for (const [name, root] of Object.entries(roots)) {
    previousDescriptors.set(
      name,
      Object.getOwnPropertyDescriptor(target, name)
    );
    Object.defineProperty(target, name, {
      configurable: true,
      enumerable: true,
      writable: false,
      value: root,
    });
  }
  try {
    for (const pluginNamespace of metadata.namespaces) {
      executeSetupScript(
        pluginNamespace,
        getOrCreateNamespace(roots, pluginNamespace.namespace),
        target
      );
    }
    const seen = new WeakSet<object>();
    for (const root of Object.values(roots)) {
      deepFreeze(root, seen);
    }
  } catch (error) {
    for (const [name, previous] of previousDescriptors) {
      if (previous === undefined) {
        Reflect.deleteProperty(target, name);
      } else {
        Object.defineProperty(target, name, previous);
      }
    }
    throw error;
  }

  return () => {
    for (const [name, previous] of previousDescriptors) {
      if (previous === undefined) {
        Reflect.deleteProperty(target, name);
      } else {
        Object.defineProperty(target, name, previous);
      }
    }
  };
};
