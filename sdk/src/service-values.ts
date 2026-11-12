import type { ServiceCallbackType, ServiceType } from './service-interface.ts';

/** Invokable callback whose actual arguments are described by its signature. */
export type ServiceCallback = (...args: unknown[]) => unknown;
/**
 * Retains a page callback for one method invocation.
 * @param callback - Function to retain until its parent call settles.
 * @param signature - Validated argument and return description.
 * @returns Nonzero decimal uint64 identity scoped to that invocation.
 */
export type ServiceCallbackEncoder = (
  callback: ServiceCallback,
  signature: ServiceCallbackType
) => string;
/**
 * Resolves a callback token within its owning method invocation.
 * @param token - Nonzero decimal uint64 identity.
 * @param signature - Validated argument and return description.
 * @returns Function that rejects invocation after the parent call ends.
 */
export type ServiceCallbackDecoder = (
  token: string,
  signature: ServiceCallbackType
) => ServiceCallback;

interface Budget {
  bytes: number;
  nodes: number;
}
const fail = (): never => {
  throw new TypeError('Invalid or excessive service value');
};
const alphabet =
  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const spend = (budget: Budget, size: number): void => {
  if (size > budget.bytes) fail();
  budget.bytes -= size;
};
const text = (value: unknown, budget: Budget): string => {
  if (typeof value !== 'string') return fail();
  let bytes = 0;
  for (let index = 0; index < value.length; ++index) {
    const unit = value.charCodeAt(index);
    if (unit >= 0xd800 && unit <= 0xdbff) {
      const low = value.charCodeAt(++index);
      if (!(low >= 0xdc00 && low <= 0xdfff)) return fail();
      bytes += 4;
    } else if (unit >= 0xdc00 && unit <= 0xdfff) return fail();
    else bytes += unit < 0x80 ? 1 : unit < 0x800 ? 2 : 3;
  }
  spend(budget, bytes);
  return value;
};
const integer = (value: unknown, signed: boolean): bigint => {
  if (
    typeof value !== 'bigint' ||
    value < (signed ? -(1n << 63n) : 0n) ||
    value > (signed ? (1n << 63n) - 1n : (1n << 64n) - 1n)
  )
    return fail();
  return value;
};
const decimal = (value: unknown, signed: boolean): bigint => {
  if (
    typeof value !== 'string' ||
    value.length > 20 ||
    !/^(0|-?[1-9][0-9]*)$/.test(value)
  )
    return fail();
  return integer(BigInt(value), signed);
};
const token = (value: unknown): string => {
  if (decimal(value, false) === 0n) return fail();
  return value as string;
};
const base64 = (bytes: Uint8Array): string => {
  const parts: string[] = [];
  for (let index = 0; index < bytes.length; index += 3) {
    const a = bytes[index]!;
    const b = bytes[index + 1] ?? 0;
    const c = bytes[index + 2] ?? 0;
    parts.push(
      alphabet[a >> 2]! +
        alphabet[((a & 3) << 4) | (b >> 4)]! +
        (index + 1 < bytes.length
          ? alphabet[((b & 15) << 2) | (c >> 6)]!
          : '=') +
        (index + 2 < bytes.length ? alphabet[c & 63]! : '=')
    );
  }
  return parts.join('');
};
const unbase64 = (value: unknown, budget: Budget): Uint8Array => {
  if (typeof value !== 'string' || value.length % 4) return fail();
  const padding = value.endsWith('==') ? 2 : value.endsWith('=') ? 1 : 0;
  const size = (value.length / 4) * 3 - padding;
  if (size < 0) return fail();
  spend(budget, size);
  const result = new Uint8Array(size);
  for (let index = 0, output = 0; index < value.length; index += 4) {
    const last = index + 4 === value.length;
    const a = alphabet.indexOf(value[index]!);
    const b = alphabet.indexOf(value[index + 1]!);
    const c = last && padding === 2 ? 0 : alphabet.indexOf(value[index + 2]!);
    const d = last && padding ? 0 : alphabet.indexOf(value[index + 3]!);
    if (
      [a, b, c, d].some((part) => part < 0) ||
      (last && padding === 2 && b & 15) ||
      (last && padding === 1 && c & 3)
    )
      return fail();
    result[output++] = (a << 2) | (b >> 4);
    if (output < size) result[output++] = (b << 4) | (c >> 2);
    if (output < size) result[output++] = (c << 6) | d;
  }
  return result;
};

const convert = (
  type: ServiceType,
  value: unknown,
  encode: boolean,
  encoder: ServiceCallbackEncoder | undefined,
  decoder: ServiceCallbackDecoder | undefined,
  budget: Budget,
  depth: number
): unknown => {
  if (depth > 16 || --budget.nodes < 0) return fail();
  if (typeof type !== 'string') {
    if ('array' in type) {
      if (depth >= 16 || !Array.isArray(value) || value.length > budget.nodes)
        return fail();
      return value.map((item) =>
        convert(type.array, item, encode, encoder, decoder, budget, depth + 1)
      );
    }
    if (encode) {
      if (!encoder || typeof value !== 'function') return fail();
      return token(encoder(value as ServiceCallback, type.callback));
    }
    if (!decoder) return fail();
    const callback = decoder(token(value), type.callback);
    return typeof callback === 'function' ? callback : fail();
  }
  switch (type) {
    case 'void':
      return value === (encode ? undefined : null)
        ? encode
          ? null
          : undefined
        : fail();
    case 'boolean':
      return typeof value === 'boolean' ? value : fail();
    case 'int32':
    case 'uint32':
      return typeof value === 'number' &&
        Number.isInteger(value) &&
        value >= (type === 'int32' ? -2147483648 : 0) &&
        value <= (type === 'int32' ? 2147483647 : 4294967295)
        ? value
        : fail();
    case 'int64':
    case 'uint64':
      return encode
        ? String(integer(value, type === 'int64'))
        : decimal(value, type === 'int64');
    case 'float64': {
      // Decimal text also preserves -0. Exponent spelling need not match
      // Number.toString: native to_chars may use a zero-padded exponent.
      if (
        !encode &&
        (typeof value !== 'string' ||
          value.length > 32 ||
          !/^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?$/.test(value))
      )
        return fail();
      const number = encode ? value : Number(value);
      if (typeof number !== 'number' || !Number.isFinite(number)) return fail();
      if (
        !encode &&
        number === 0 &&
        /[1-9]/.test((value as string).split(/[eE]/)[0]!)
      )
        return fail();
      return encode ? (Object.is(number, -0) ? '-0' : String(number)) : number;
    }
    case 'string':
      return text(value, budget);
    case 'bytes':
      if (!encode) return unbase64(value, budget);
      if (!(value instanceof Uint8Array)) return fail();
      spend(budget, value.byteLength);
      return base64(value);
  }
};

/**
 * Copies and encodes a value for JSON transport using a validated interface.
 * @param type - Runtime type from a validated interface.
 * @param value - JavaScript value; 64-bit integers use bigint.
 * @param callbacks - Invocation-owned callback registration, absent outside inputs.
 * @returns JSON-compatible data; integer and floating-point text preserve precision.
 * @throws TypeError for malformed or excessive values.
 */
export const encodeServiceValue = (
  type: ServiceType,
  value: unknown,
  callbacks: ServiceCallbackEncoder | undefined
): unknown =>
  convert(
    type,
    value,
    true,
    callbacks,
    undefined,
    { bytes: 1024 * 1024, nodes: 65536 },
    0
  );

/**
 * Decodes a copied value received through JSON transport.
 * @param type - Runtime type from a validated interface.
 * @param value - JSON-compatible wire value.
 * @param callbacks - Invocation-owned callback resolver, absent outside method inputs.
 * @returns JavaScript value independent of transport buffers.
 * @throws TypeError for malformed or excessive values.
 */
export const decodeServiceValue = (
  type: ServiceType,
  value: unknown,
  callbacks: ServiceCallbackDecoder | undefined
): unknown =>
  convert(
    type,
    value,
    false,
    undefined,
    callbacks,
    { bytes: 1024 * 1024, nodes: 65536 },
    0
  );
