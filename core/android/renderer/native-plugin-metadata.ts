// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

/** Value kinds supported by the native Muon plugin ABI. */
export type MuonNativeValueType =
  | 'void'
  | 'bool'
  | 'i8'
  | 'u8'
  | 'i16'
  | 'u16'
  | 'i32'
  | 'u32'
  | 'i64'
  | 'u64'
  | 'f32'
  | 'f64'
  | 'string'
  | 'pointer'
  | 'function'
  | 'buffer_view';

/** Recursive value metadata copied from one loaded native plugin. */
export interface MuonNativeTypeMetadata {
  /** ABI value kind. */
  readonly type: MuonNativeValueType;

  /** Argument types when this value is a function. */
  readonly args?: readonly MuonNativeTypeMetadata[];

  /** Return type when this value is a function. */
  readonly returnType?: MuonNativeTypeMetadata;
}

/** One namespace produced by a loaded native plugin. */
export interface MuonNativeNamespaceMetadata {
  /** Dot-separated namespace exposed in simple mode. */
  readonly namespace: string;

  /** Trusted plugin setup source executed after allowed functions exist. */
  readonly setupScript: string;

  /** Function property names that setup source may use. */
  readonly allowedFunctions: readonly string[];
}

/** One callable function produced by a loaded native plugin. */
export interface MuonNativeFunctionMetadata {
  /** Runtime-wide native function identifier. */
  readonly id: number;

  /** Dot-separated namespace owning this function. */
  readonly namespace: string;

  /** Property name exposed in simple mode. */
  readonly name: string;

  /** Public name used by capability policies. */
  readonly publicName: string;

  /** Capability id assigned by the Android package registry. */
  readonly capabilityId: string;

  /** Ordered native argument types. */
  readonly args: readonly MuonNativeTypeMetadata[];

  /** Native result type. */
  readonly returnType: MuonNativeTypeMetadata;
}

/** Renderer metadata captured before the trusted WebView document starts. */
export interface MuonAndroidRendererMetadata {
  /** Transport schema version. */
  readonly version: 1;

  /** Native RPC context that owns renderer function sources. */
  readonly contextId: number;

  /** Page exposure mode selected by the Android host. */
  readonly mode: 'simple' | 'validate';

  /** Built-in public function paths allowed by the native host. */
  readonly builtinFunctions: readonly string[];

  /** Loaded and allowed plugin namespaces. */
  readonly namespaces: readonly MuonNativeNamespaceMetadata[];

  /** Loaded and allowed plugin functions. */
  readonly functions: readonly MuonNativeFunctionMetadata[];
}

const valueTypes = new Set<MuonNativeValueType>([
  'void',
  'bool',
  'i8',
  'u8',
  'i16',
  'u16',
  'i32',
  'u32',
  'i64',
  'u64',
  'f32',
  'f64',
  'string',
  'pointer',
  'function',
  'buffer_view',
]);
const identifierExpression = /^[A-Za-z_$][A-Za-z0-9_$]*$/u;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

const readType = (
  value: unknown,
  allowVoid: boolean,
  depth: number
): MuonNativeTypeMetadata => {
  if (!isRecord(value) || depth > 16 || !valueTypes.has(value.type as never)) {
    throw new TypeError('Android native plugin type metadata is invalid');
  }
  const type = value.type as MuonNativeValueType;
  if (!allowVoid && type === 'void') {
    throw new TypeError('Android native plugin argument type is invalid');
  }
  if (type !== 'function') {
    return Object.freeze({ type });
  }
  if (!Array.isArray(value.args) || !isRecord(value.returnType)) {
    throw new TypeError('Android native function metadata is invalid');
  }
  const args = value.args.map((argument) =>
    readType(argument, false, depth + 1)
  );
  const returnType = readType(value.returnType, true, depth + 1);
  return Object.freeze({ type, args: Object.freeze(args), returnType });
};

const readIdentifier = (value: unknown, label: string): string => {
  if (typeof value !== 'string' || !identifierExpression.test(value)) {
    throw new TypeError(`Android native plugin ${label} is invalid`);
  }
  return value;
};

const readNamespaceName = (value: unknown): string => {
  if (
    typeof value !== 'string' ||
    value.split('.').some((segment) => !identifierExpression.test(segment))
  ) {
    throw new TypeError('Android native plugin namespace is invalid');
  }
  return value;
};

/**
 * Validates renderer metadata injected by the trusted Android host.
 *
 * @param value - Candidate document-start metadata value.
 * @returns A deeply immutable metadata snapshot.
 */
export const readMuonAndroidRendererMetadata = (
  value: unknown
): MuonAndroidRendererMetadata => {
  if (
    !isRecord(value) ||
    value.version !== 1 ||
    !Number.isInteger(value.contextId) ||
    typeof value.contextId !== 'number' ||
    value.contextId <= 0 ||
    value.contextId > 0x7fffffff ||
    (value.mode !== 'simple' && value.mode !== 'validate') ||
    !Array.isArray(value.builtinFunctions) ||
    value.builtinFunctions.some((path) => typeof path !== 'string') ||
    !Array.isArray(value.namespaces) ||
    !Array.isArray(value.functions)
  ) {
    throw new TypeError('Android native plugin renderer metadata is invalid');
  }

  const namespaces = value.namespaces.map((candidate) => {
    if (
      !isRecord(candidate) ||
      typeof candidate.setupScript !== 'string' ||
      !Array.isArray(candidate.allowedFunctions)
    ) {
      throw new TypeError(
        'Android native plugin namespace metadata is invalid'
      );
    }
    const allowedFunctions = candidate.allowedFunctions.map((name) =>
      readIdentifier(name, 'allowed function name')
    );
    return Object.freeze({
      namespace: readNamespaceName(candidate.namespace),
      setupScript: candidate.setupScript,
      allowedFunctions: Object.freeze(allowedFunctions),
    });
  });

  const functions = value.functions.map((candidate) => {
    if (
      !isRecord(candidate) ||
      !Number.isInteger(candidate.id) ||
      typeof candidate.id !== 'number' ||
      candidate.id < 0 ||
      candidate.id > 0xffffffff ||
      typeof candidate.capabilityId !== 'string' ||
      candidate.capabilityId.length === 0 ||
      !Array.isArray(candidate.args)
    ) {
      throw new TypeError('Android native plugin function metadata is invalid');
    }
    return Object.freeze({
      id: candidate.id,
      namespace: readNamespaceName(candidate.namespace),
      name: readIdentifier(candidate.name, 'function name'),
      publicName: readIdentifier(candidate.publicName, 'public function name'),
      capabilityId: candidate.capabilityId,
      args: Object.freeze(
        candidate.args.map((argument) => readType(argument, false, 0))
      ),
      returnType: readType(candidate.returnType, true, 0),
    });
  });

  return Object.freeze({
    version: 1,
    contextId: value.contextId,
    mode: value.mode,
    builtinFunctions: Object.freeze([...value.builtinFunctions] as string[]),
    namespaces: Object.freeze(namespaces),
    functions: Object.freeze(functions),
  });
};
