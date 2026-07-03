// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { access, cp, readFile, stat, writeFile } from "node:fs/promises";
import { randomBytes } from "node:crypto";
import { basename, dirname, join } from "node:path";

import { parse } from "json5";

/**
 * Fixed byte size reserved in muon-core for an embedded muon.json payload.
 */
export const muonEmbeddedConfigSlotSize = 64 * 1024;

/**
 * Fixed byte sequence used to locate an unembedded config slot.
 */
const muonEmbeddedConfigEmptySlotMarker = Buffer.from([
  0x6d, 0x75, 0x6f, 0x6e, 0x2d, 0x63, 0x6f, 0x72, 0x65, 0x3a, 0x65, 0x6d, 0x62,
  0x65, 0x64, 0x2d, 0x63, 0x6f, 0x6e, 0x66, 0x69, 0x67, 0x3a, 0x73, 0x6c, 0x6f,
  0x74, 0x3a, 0x76, 0x31, 0x00, 0x5d,
]);

const muonEmbeddedConfigPayloadCapacity = muonEmbeddedConfigSlotSize;
const muonBootstrapEmbeddedConfigSlotSize = 64 * 1024;
const muonBootstrapEmbeddedConfigEmptySlotMarker = Buffer.from([
  0x6d, 0x75, 0x6f, 0x6e, 0x2d, 0x62, 0x6f, 0x6f, 0x74, 0x73, 0x74, 0x72, 0x61,
  0x70, 0x3a, 0x65, 0x6d, 0x62, 0x65, 0x64, 0x2d, 0x63, 0x6f, 0x6e, 0x66, 0x69,
  0x67, 0x3a, 0x76, 0x31, 0x00, 0x5d,
]);
const muonBootstrapEmbeddedConfigPayloadCapacity =
  muonBootstrapEmbeddedConfigSlotSize;

const tlvNullTag = 0;
const tlvFalseTag = 1;
const tlvTrueTag = 2;
const tlvUintTag = 3;
const tlvStringTag = 4;
const tlvBinaryTag = 5;
const tlvArrayTag = 6;
const tlvObjectTag = 7;

/**
 * Result returned by a muon executable config embedding operation.
 */
export interface EmbedMuonConfigResult {
  /** Input executable path. For bootstrap embedding this is the bootstrap path. */
  readonly corePath: string;
  /** Written executable path. */
  readonly outputPath: string;
  /** Byte offset of the fixed embedded config slot. */
  readonly slotOffset: number;
  /** Encoded TLV payload size in bytes. */
  readonly payloadSize: number;
  /** Whether an existing embedded payload was replaced. Always false. */
  readonly replaced: boolean;
}

/**
 * Options for embedding a config into a single muon-core executable.
 */
export interface EmbedMuonConfigCoreOptions {
  /** muon-core executable path to patch. */
  readonly corePath: string;
  /** muon.json, muon.json5, or muon.jsonc input path. */
  readonly configPath: string;
  /** Optional output executable path. When omitted, corePath is updated. */
  readonly outputPath: string | undefined;
}

/**
 * Options for embedding a config into a single muon-bootstrap executable.
 */
export interface EmbedMuonConfigBootstrapOptions {
  /** muon-bootstrap executable path to patch. */
  readonly bootstrapPath: string;
  /** muon.json, muon.json5, or muon.jsonc input path. */
  readonly configPath: string;
  /** Optional output executable path. When omitted, bootstrapPath is updated. */
  readonly outputPath: string | undefined;
}

/**
 * Options for embedding a config into a runtime directory.
 */
export interface EmbedMuonConfigRuntimeOptions {
  /** Runtime directory containing muon-core or muon-core.exe. */
  readonly runtimePath: string;
  /** muon.json, muon.json5, or muon.jsonc input path. */
  readonly configPath: string;
  /** Optional output runtime directory. When omitted, runtimePath is updated. */
  readonly outputRuntimePath: string | undefined;
}

/**
 * Unembedded config slot found in a muon-core executable.
 */
export interface MuonEmbeddedConfigSlot {
  /** Empty slot state. */
  readonly state: "empty";
  /** Byte offset of the slot. */
  readonly offset: number;
  /** Empty payload. */
  readonly payload: Buffer;
  /** Payload byte size. */
  readonly payloadSize: 0;
}

/**
 * Unembedded bootstrap config slot found in a muon-bootstrap executable.
 */
export interface MuonBootstrapEmbeddedConfigSlot {
  /** Empty slot state. */
  readonly state: "empty";
  /** Byte offset of the slot. */
  readonly offset: number;
  /** Empty payload. */
  readonly payload: Buffer;
  /** Payload byte size. */
  readonly payloadSize: 0;
}

const getEmbeddedConfigEmptySlotByte = (index: number): number => {
  if (index < muonEmbeddedConfigEmptySlotMarker.length) {
    return muonEmbeddedConfigEmptySlotMarker[index] ?? 0;
  }
  const value =
    0xa5 ^
    Math.imul(index, 0x25) ^
    Math.imul(index >>> 8, 0x6d) ^
    Math.imul(index >>> 16, 0x3b) ^
    (Math.imul(index, 0x9e3779b1) >>> 24);
  return value & 0xff;
};

const createEmptySlot = (): Buffer => {
  const slot = Buffer.allocUnsafe(muonEmbeddedConfigSlotSize);
  for (let index = 0; index < slot.length; index += 1) {
    slot[index] = getEmbeddedConfigEmptySlotByte(index);
  }
  return slot;
};

const encodeVarUint = (value: bigint): Buffer => {
  const bytes: number[] = [];
  let remaining = value;
  do {
    let byte = Number(remaining & 0x7fn);
    remaining >>= 7n;
    if (remaining !== 0n) {
      byte |= 0x80;
    }
    bytes.push(byte);
  } while (remaining !== 0n);
  return Buffer.from(bytes);
};

const encodeRawString = (value: string): Buffer => {
  const bytes = Buffer.from(value, "utf8");
  return Buffer.concat([encodeVarUint(BigInt(bytes.length)), bytes]);
};

const encodeTaggedBytes = (tag: number, bytes: Uint8Array): Buffer =>
  Buffer.concat([
    Buffer.from([tag]),
    encodeVarUint(BigInt(bytes.length)),
    Buffer.from(bytes),
  ]);

const isJsonObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const isPath = (
  path: readonly string[],
  first: string,
  second: string,
): boolean => path.length === 2 && path[0] === first && path[1] === second;

const isPluginSignaturePath = (path: readonly string[]): boolean =>
  path.length === 3 &&
  path[0] === "plugin" &&
  path[1] === "plugins" &&
  path[2] === "signature";

const isHexString = (value: string): boolean =>
  value.length % 2 === 0 && /^[0-9a-fA-F]*$/.test(value);

const decodeHexString = (value: string): Buffer => {
  const bytes = Buffer.alloc(value.length / 2);
  for (let index = 0; index < value.length; index += 2) {
    bytes[index / 2] = Number.parseInt(value.slice(index, index + 2), 16);
  }
  return bytes;
};

const encodeKnownBinaryString = (
  path: readonly string[],
  value: string,
): Buffer | undefined => {
  if (isPath(path, "asset", "signature")) {
    return value.length === 40 && isHexString(value)
      ? decodeHexString(value)
      : undefined;
  }
  if (isPath(path, "asset", "salt")) {
    return isHexString(value) ? decodeHexString(value) : undefined;
  }
  if (isPluginSignaturePath(path)) {
    return value.length === 40 && isHexString(value)
      ? decodeHexString(value)
      : undefined;
  }
  if (isPath(path, "browser", "backgroundColor")) {
    const hex = value.startsWith("#") ? value.slice(1) : value;
    return hex.length === 6 && isHexString(hex)
      ? decodeHexString(hex)
      : undefined;
  }
  return undefined;
};

const encodeTlvValue = (value: unknown, path: readonly string[]): Buffer => {
  if (value === null) {
    return Buffer.from([tlvNullTag]);
  }
  if (typeof value === "boolean") {
    return Buffer.from([value ? tlvTrueTag : tlvFalseTag]);
  }
  if (typeof value === "number") {
    if (!Number.isSafeInteger(value) || value < 0) {
      throw new Error("muon embedded config supports only unsigned integers.");
    }
    return Buffer.concat([
      Buffer.from([tlvUintTag]),
      encodeVarUint(BigInt(value)),
    ]);
  }
  if (typeof value === "string") {
    const binaryValue = encodeKnownBinaryString(path, value);
    if (binaryValue !== undefined) {
      return encodeTaggedBytes(tlvBinaryTag, binaryValue);
    }
    return encodeTaggedBytes(tlvStringTag, Buffer.from(value, "utf8"));
  }
  if (Array.isArray(value)) {
    return Buffer.concat([
      Buffer.from([tlvArrayTag]),
      encodeVarUint(BigInt(value.length)),
      ...value.map((entry) => encodeTlvValue(entry, path)),
    ]);
  }
  if (isJsonObject(value)) {
    const entries = Object.entries(value);
    return Buffer.concat([
      Buffer.from([tlvObjectTag]),
      encodeVarUint(BigInt(entries.length)),
      ...entries.flatMap(([key, entry]) => [
        encodeRawString(key),
        encodeTlvValue(entry, [...path, key]),
      ]),
    ]);
  }
  throw new Error("muon embedded config contains an unsupported value.");
};

/**
 * Encodes a parsed muon config object as the embedded TLV payload.
 *
 * @param config Parsed muon config value.
 * @returns Encoded TLV payload bytes.
 */
export const encodeMuonConfigTlv = (config: unknown): Buffer =>
  encodeTlvValue(config, []);

/**
 * Creates an empty or payload-filled fixed embedded config slot.
 *
 * @param payload Optional encoded TLV payload bytes.
 * @returns Fixed-size slot image.
 */
export const createMuonEmbeddedConfigSlot = (
  payload: Uint8Array | undefined = undefined,
): Buffer => {
  if (
    payload !== undefined &&
    payload.length > muonEmbeddedConfigPayloadCapacity
  ) {
    throw new Error(
      `Encoded muon config exceeds the embedded slot capacity: ${payload.length} > ${muonEmbeddedConfigPayloadCapacity}`,
    );
  }

  if (payload === undefined) {
    return createEmptySlot();
  }

  const slot = randomBytes(muonEmbeddedConfigSlotSize);
  Buffer.from(payload).copy(slot, 0);
  return slot;
};

/**
 * Creates an empty or payload-filled fixed muon-bootstrap embedded config slot.
 *
 * @param payload Optional encoded TLV payload bytes.
 * @returns Fixed-size slot image.
 */
export const createMuonBootstrapEmbeddedConfigSlot = (
  payload: Uint8Array | undefined = undefined,
): Buffer => {
  if (
    payload !== undefined &&
    payload.length > muonBootstrapEmbeddedConfigPayloadCapacity
  ) {
    throw new Error(
      `Encoded muon config exceeds the embedded bootstrap slot capacity: ${payload.length} > ${muonBootstrapEmbeddedConfigPayloadCapacity}`,
    );
  }

  if (payload === undefined) {
    const slot = Buffer.alloc(muonBootstrapEmbeddedConfigSlotSize, 0);
    muonBootstrapEmbeddedConfigEmptySlotMarker.copy(slot, 0);
    return slot;
  }

  const slot = randomBytes(muonBootstrapEmbeddedConfigSlotSize);
  Buffer.from(payload).copy(slot, 0);
  return slot;
};

const isEmptySlotAt = (content: Buffer, offset: number): boolean => {
  for (let index = 0; index < muonEmbeddedConfigSlotSize; index += 1) {
    if (content[offset + index] !== getEmbeddedConfigEmptySlotByte(index)) {
      return false;
    }
  }
  return true;
};

const inspectUnembeddedSlotAt = (
  content: Buffer,
  offset: number,
): MuonEmbeddedConfigSlot | undefined => {
  if (offset < 0 || offset + muonEmbeddedConfigSlotSize > content.length) {
    return undefined;
  }
  if (isEmptySlotAt(content, offset)) {
    return {
      state: "empty",
      offset,
      payload: Buffer.alloc(0),
      payloadSize: 0,
    };
  }
  return undefined;
};

const collectSlotCandidates = (content: Buffer): MuonEmbeddedConfigSlot[] => {
  const candidates = new Map<number, MuonEmbeddedConfigSlot>();
  for (
    let offset = content.indexOf(muonEmbeddedConfigEmptySlotMarker);
    offset >= 0;
    offset = content.indexOf(muonEmbeddedConfigEmptySlotMarker, offset + 1)
  ) {
    const slot = inspectUnembeddedSlotAt(content, offset);
    if (slot !== undefined) {
      candidates.set(slot.offset, slot);
    }
  }
  return [...candidates.values()].sort(
    (left, right) => left.offset - right.offset,
  );
};

const isEmptyBootstrapSlotAt = (content: Buffer, offset: number): boolean => {
  if (
    offset < 0 ||
    offset + muonBootstrapEmbeddedConfigSlotSize > content.length
  ) {
    return false;
  }
  if (
    !content
      .subarray(
        offset,
        offset + muonBootstrapEmbeddedConfigEmptySlotMarker.length,
      )
      .equals(muonBootstrapEmbeddedConfigEmptySlotMarker)
  ) {
    return false;
  }
  for (
    let index = muonBootstrapEmbeddedConfigEmptySlotMarker.length;
    index < muonBootstrapEmbeddedConfigSlotSize;
    index += 1
  ) {
    if (content[offset + index] !== 0) {
      return false;
    }
  }
  return true;
};

const inspectUnembeddedBootstrapSlotAt = (
  content: Buffer,
  offset: number,
): MuonBootstrapEmbeddedConfigSlot | undefined => {
  if (!isEmptyBootstrapSlotAt(content, offset)) {
    return undefined;
  }
  return {
    state: "empty",
    offset,
    payload: Buffer.alloc(0),
    payloadSize: 0,
  };
};

const collectBootstrapSlotCandidates = (
  content: Buffer,
): MuonBootstrapEmbeddedConfigSlot[] => {
  const candidates = new Map<number, MuonBootstrapEmbeddedConfigSlot>();
  for (
    let offset = content.indexOf(muonBootstrapEmbeddedConfigEmptySlotMarker);
    offset >= 0;
    offset = content.indexOf(
      muonBootstrapEmbeddedConfigEmptySlotMarker,
      offset + 1,
    )
  ) {
    const slot = inspectUnembeddedBootstrapSlotAt(content, offset);
    if (slot !== undefined) {
      candidates.set(slot.offset, slot);
    }
  }
  return [...candidates.values()].sort(
    (left, right) => left.offset - right.offset,
  );
};

/**
 * Finds the fixed unembedded config slot in a muon-core executable image.
 *
 * @param content muon-core executable bytes.
 * @returns The single detected unembedded config slot.
 */
export const findMuonEmbeddedConfigSlot = (
  content: Uint8Array,
): MuonEmbeddedConfigSlot => {
  const buffer = Buffer.isBuffer(content) ? content : Buffer.from(content);
  const candidates = collectSlotCandidates(buffer);
  if (candidates.length !== 1) {
    throw new Error(
      `Expected exactly one unembedded muon config slot, found ${candidates.length}.`,
    );
  }
  const [candidate] = candidates;
  if (candidate === undefined) {
    throw new Error("Embedded muon config slot was not found.");
  }
  return candidate;
};

/**
 * Finds the fixed unembedded config slot in a muon-bootstrap executable image.
 *
 * @param content muon-bootstrap executable bytes.
 * @returns The single detected unembedded bootstrap config slot.
 */
export const findMuonBootstrapEmbeddedConfigSlot = (
  content: Uint8Array,
): MuonBootstrapEmbeddedConfigSlot => {
  const buffer = Buffer.isBuffer(content) ? content : Buffer.from(content);
  const candidates = collectBootstrapSlotCandidates(buffer);
  if (candidates.length !== 1) {
    throw new Error(
      `Expected exactly one unembedded muon-bootstrap config slot, found ${candidates.length}.`,
    );
  }
  const [candidate] = candidates;
  if (candidate === undefined) {
    throw new Error("Embedded muon-bootstrap config slot was not found.");
  }
  return candidate;
};

const fileExists = async (path: string): Promise<boolean> => {
  try {
    const stats = await stat(path);
    return stats.isFile();
  } catch {
    return false;
  }
};

const resolveMuonConfigInputPath = async (
  configPath: string,
): Promise<string> => {
  if (basename(configPath) === "muon.json") {
    const directory = dirname(configPath);
    for (const fileName of ["muon.json5", "muon.jsonc", "muon.json"]) {
      const candidate = join(directory, fileName);
      if (await fileExists(candidate)) {
        return candidate;
      }
    }
  }
  if (!(await fileExists(configPath))) {
    throw new Error(`muon config does not exist: ${configPath}`);
  }
  return configPath;
};

const readMuonConfigInput = async (configPath: string): Promise<unknown> => {
  const resolvedPath = await resolveMuonConfigInputPath(configPath);
  return parse(await readFile(resolvedPath, "utf8"));
};

const isSignedPeExecutable = (content: Buffer): boolean => {
  if (content.length < 0x40 || content[0] !== 0x4d || content[1] !== 0x5a) {
    return false;
  }
  const peOffset = content.readUInt32LE(0x3c);
  if (
    peOffset + 0x18 >= content.length ||
    content.toString("ascii", peOffset, peOffset + 4) !== "PE\u0000\u0000"
  ) {
    return false;
  }

  const optionalHeaderOffset = peOffset + 0x18;
  const optionalHeaderSize = content.readUInt16LE(peOffset + 0x14);
  const optionalHeaderEnd = optionalHeaderOffset + optionalHeaderSize;
  if (optionalHeaderEnd > content.length) {
    return false;
  }

  const magic = content.readUInt16LE(optionalHeaderOffset);
  const dataDirectoryOffset =
    magic === 0x20b ? optionalHeaderOffset + 112 : optionalHeaderOffset + 96;
  const certificateEntryOffset = dataDirectoryOffset + 4 * 8;
  if (certificateEntryOffset + 8 > optionalHeaderEnd) {
    return false;
  }
  const certificateSize = content.readUInt32LE(certificateEntryOffset + 4);
  return certificateSize !== 0;
};

/**
 * Embeds a muon config into a single muon-core executable.
 *
 * @param options Core executable embedding options.
 * @returns Embedding result metadata.
 */
export const embedMuonConfigInCoreFile = async ({
  corePath,
  configPath,
  outputPath,
}: EmbedMuonConfigCoreOptions): Promise<EmbedMuonConfigResult> => {
  const config = await readMuonConfigInput(configPath);
  const payload = encodeMuonConfigTlv(config);
  const embeddedSlot = createMuonEmbeddedConfigSlot(payload);
  const coreContent = await readFile(corePath);
  if (isSignedPeExecutable(coreContent)) {
    throw new Error("Cannot embed muon config into a signed PE executable.");
  }

  const slot = findMuonEmbeddedConfigSlot(coreContent);
  const patchedContent = Buffer.from(coreContent);
  embeddedSlot.copy(patchedContent, slot.offset);
  const resolvedOutputPath = outputPath ?? corePath;
  await writeFile(resolvedOutputPath, patchedContent);

  return {
    corePath,
    outputPath: resolvedOutputPath,
    slotOffset: slot.offset,
    payloadSize: payload.length,
    replaced: false,
  };
};

/**
 * Embeds a muon config into a single muon-bootstrap executable.
 *
 * @param options Bootstrap executable embedding options.
 * @returns Embedding result metadata.
 */
export const embedMuonConfigInBootstrapFile = async ({
  bootstrapPath,
  configPath,
  outputPath,
}: EmbedMuonConfigBootstrapOptions): Promise<EmbedMuonConfigResult> => {
  const config = await readMuonConfigInput(configPath);
  const payload = encodeMuonConfigTlv(config);
  const embeddedSlot = createMuonBootstrapEmbeddedConfigSlot(payload);
  const bootstrapContent = await readFile(bootstrapPath);
  if (isSignedPeExecutable(bootstrapContent)) {
    throw new Error(
      "Cannot embed muon config into a signed muon-bootstrap PE executable.",
    );
  }

  const slot = findMuonBootstrapEmbeddedConfigSlot(bootstrapContent);
  const patchedContent = Buffer.from(bootstrapContent);
  embeddedSlot.copy(patchedContent, slot.offset);
  const resolvedOutputPath = outputPath ?? bootstrapPath;
  await writeFile(resolvedOutputPath, patchedContent);

  return {
    corePath: bootstrapPath,
    outputPath: resolvedOutputPath,
    slotOffset: slot.offset,
    payloadSize: payload.length,
    replaced: false,
  };
};

const resolveRuntimeCorePath = async (runtimePath: string): Promise<string> => {
  const candidates = [
    join(runtimePath, "muon-core"),
    join(runtimePath, "muon-core.exe"),
  ];
  const existing: string[] = [];
  for (const candidate of candidates) {
    try {
      await access(candidate);
      existing.push(candidate);
    } catch {
      // Missing candidates are ignored until the final count check.
    }
  }
  if (existing.length !== 1) {
    throw new Error(
      `Expected exactly one muon-core executable in runtime path, found ${existing.length}.`,
    );
  }
  const [corePath] = existing;
  if (corePath === undefined) {
    throw new Error("muon-core executable was not found.");
  }
  return corePath;
};

/**
 * Embeds a muon config into a runtime directory containing muon-core.
 *
 * @param options Runtime directory embedding options.
 * @returns Embedding result metadata.
 */
export const embedMuonConfigInRuntime = async ({
  runtimePath,
  configPath,
  outputRuntimePath,
}: EmbedMuonConfigRuntimeOptions): Promise<EmbedMuonConfigResult> => {
  const targetRuntimePath = outputRuntimePath ?? runtimePath;
  if (outputRuntimePath !== undefined) {
    await cp(runtimePath, outputRuntimePath, {
      recursive: true,
      force: true,
    });
  }
  return embedMuonConfigInCoreFile({
    corePath: await resolveRuntimeCorePath(targetRuntimePath),
    configPath,
    outputPath: undefined,
  });
};
