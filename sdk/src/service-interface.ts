/** Primitive values available to native and JavaScript service implementations. */
export type ServicePrimitiveType =
  | 'void'
  | 'boolean'
  | 'int32'
  | 'uint32'
  | 'int64'
  | 'uint64'
  | 'float64'
  | 'string'
  | 'bytes';

/** Signature of a page callback, valid only while its parent method is pending. */
export interface ServiceCallbackType {
  /** Ordered arguments; callbacks cannot themselves receive callbacks. */
  readonly parameters: readonly ServiceType[];
  /** Result type, which may be void but cannot contain callbacks. */
  readonly returns: ServiceType;
}

/** Recursive runtime value description; arrays cannot contain void. */
export type ServiceType =
  | ServicePrimitiveType
  | { /** Ordered values of this element type. */ readonly array: ServiceType }
  | {
      /** Callback supplied by the page to the pending method. */ readonly callback: ServiceCallbackType;
    };

/** Runtime signature of one publicly callable service method. */
export interface ServiceMethod {
  /** Ordered input values; callbacks are allowed within these inputs. */
  readonly parameters: readonly ServiceType[];
  /** Result type; callbacks cannot escape the invocation in its result. */
  readonly returns: ServiceType;
  /** Application-defined error codes, in addition to common transport errors. */
  readonly errors: readonly string[];
}

/** Runtime interface shared by the page SDK and each service adapter. */
export interface ServiceInterface {
  /** Interface schema revision. */
  readonly schemaVersion: 1;
  /** Method names and their complete runtime signatures. */
  readonly methods: Readonly<Record<string, ServiceMethod>>;
  /** Named event payloads; these cannot contain void or callbacks. */
  readonly events: Readonly<Record<string, ServiceType>>;
}

/** Maps a runtime descriptor to the corresponding public JavaScript value. */
export type ServiceValue<Type extends ServiceType> = Type extends 'void'
  ? void
  : Type extends 'boolean'
    ? boolean
    : Type extends 'int64' | 'uint64'
      ? bigint
      : Type extends 'int32' | 'uint32' | 'float64'
        ? number
        : Type extends 'string'
          ? string
          : Type extends 'bytes'
            ? Uint8Array
            : Type extends { readonly array: infer Item extends ServiceType }
              ? readonly ServiceValue<Item>[]
              : Type extends {
                    readonly callback: infer Signature extends
                      ServiceCallbackType;
                  }
                ? (
                    ...args: ServiceArguments<Signature['parameters']>
                  ) =>
                    | ServiceValue<Signature['returns']>
                    | Promise<ServiceValue<Signature['returns']>>
                : never;

/** Maps an ordered runtime parameter list to a JavaScript argument tuple. */
export type ServiceArguments<Types extends readonly ServiceType[]> = {
  -readonly [Index in keyof Types]: ServiceValue<Types[Index]>;
};

const fail = (): never => {
  throw new TypeError('Invalid service interface');
};
const object = (
  value: unknown,
  keys: readonly string[]
): Record<string, unknown> => {
  if (!value || typeof value !== 'object' || Array.isArray(value))
    return fail();
  const record = value as Record<string, unknown>;
  if (
    Object.keys(record).length !== keys.length ||
    keys.some((key) => !Object.hasOwn(record, key))
  )
    return fail();
  return record;
};
const name = (value: string): string => {
  if (!/^[A-Za-z][A-Za-z0-9_.-]{0,127}$/.test(value)) return fail();
  return value;
};
const primitives = new Set<string>([
  'boolean',
  'int32',
  'uint32',
  'int64',
  'uint64',
  'float64',
  'string',
  'bytes',
]);

const parameters = (
  value: unknown,
  callbacks: boolean,
  depth: number
): readonly ServiceType[] => {
  if (!Array.isArray(value) || value.length > 32) return fail();
  return Object.freeze(
    value.map((item) => type(item, false, callbacks, depth))
  );
};

const type = (
  value: unknown,
  allowVoid: boolean,
  callbacks: boolean,
  depth: number
): ServiceType => {
  if (depth > 16) return fail();
  if (typeof value === 'string') {
    if (primitives.has(value) || (allowVoid && value === 'void'))
      return value as ServicePrimitiveType;
    return fail();
  }
  if (value && typeof value === 'object' && Object.hasOwn(value, 'array')) {
    if (depth >= 16) return fail();
    const record = object(value, ['array']);
    return Object.freeze({
      array: type(record.array, false, callbacks, depth + 1),
    });
  }
  if (!callbacks) return fail();
  const record = object(value, ['callback']);
  const signature = object(record.callback, ['parameters', 'returns']);
  return Object.freeze({
    callback: Object.freeze({
      parameters: parameters(signature.parameters, false, 0),
      returns: type(signature.returns, true, false, 0),
    }),
  });
};

const catalog = <T>(
  value: unknown,
  read: (item: unknown) => T
): Readonly<Record<string, T>> => {
  if (!value || typeof value !== 'object' || Array.isArray(value))
    return fail();
  const entries = Object.entries(value);
  if (entries.length > 256) return fail();
  const result = Object.create(null) as Record<string, T>;
  for (const [key, item] of entries) result[name(key)] = read(item);
  return Object.freeze(result);
};

/**
 * Validates untrusted runtime metadata and returns a detached immutable copy.
 * @param value - JSON-compatible interface metadata.
 * @returns Supported method and event signatures with all children frozen.
 * @throws TypeError for unsupported types, fields, names, or limits.
 * @remarks Callbacks are allowed in method inputs, including arrays. Callback
 * signatures, method results, and events cannot contain further callbacks.
 */
export const parseServiceInterface = (value: unknown): ServiceInterface => {
  const record = object(value, ['schemaVersion', 'methods', 'events']);
  if (record.schemaVersion !== 1) return fail();
  const methods = catalog(record.methods, (item) => {
    const method = object(item, ['parameters', 'returns', 'errors']);
    if (
      !Array.isArray(method.errors) ||
      method.errors.length > 64 ||
      method.errors.some((error) => typeof error !== 'string') ||
      new Set(method.errors).size !== method.errors.length
    )
      return fail();
    return Object.freeze({
      parameters: parameters(method.parameters, true, 0),
      returns: type(method.returns, true, false, 0),
      errors: Object.freeze(
        method.errors.map((error) => name(error as string))
      ),
    });
  });
  const events = catalog(record.events, (item) => type(item, false, false, 0));
  return Object.freeze({ schemaVersion: 1, methods, events });
};

/**
 * Defines an immutable interface while preserving literal types for client code.
 * @typeParam Definition - Literal method and event signatures.
 * @param definition - Complete service interface shared by its implementations.
 * @returns A validated copy with the original TypeScript literal information.
 */
export const defineServiceInterface = <
  const Definition extends ServiceInterface,
>(
  definition: Definition
): Definition => parseServiceInterface(definition) as Definition;
