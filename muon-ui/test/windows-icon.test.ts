// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { deflateSync } from "node:zlib";

import { describe, expect, it } from "vitest";

import { createWindowsIconBufferFromPngData } from "../src/windows-icon.js";

interface IcoEntry {
  width: number;
  height: number;
  planes: number;
  bitCount: number;
  data: Buffer;
}

const pngSignature = Buffer.from([
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
]);
const windowsIconSizes = [16, 24, 32, 48, 64, 128, 256];

const crcTable = Array.from({ length: 256 }, (_, index) => {
  let value = index;
  for (let bit = 0; bit < 8; bit += 1) {
    value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
  }
  return value >>> 0;
});

const crc32 = (data: Buffer): number => {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc = crcTable[(crc ^ byte) & 0xff]! ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
};

const createPngChunk = (type: string, data: Buffer): Buffer => {
  const chunk = Buffer.alloc(12 + data.length);
  chunk.writeUInt32BE(data.length, 0);
  chunk.write(type, 4, 4, "ascii");
  data.copy(chunk, 8);
  chunk.writeUInt32BE(
    crc32(chunk.subarray(4, 8 + data.length)),
    8 + data.length,
  );
  return chunk;
};

const createRgbaPng = (
  width: number,
  height: number,
  rgba: readonly number[],
): Buffer => {
  const rowStride = width * 4;
  const rows = Buffer.alloc((rowStride + 1) * height);
  for (let y = 0; y < height; y += 1) {
    const rowOffset = y * (rowStride + 1);
    rows[rowOffset] = 0;
    for (let x = 0; x < width; x += 1) {
      const sourceOffset = (y * width + x) * 4;
      const targetOffset = rowOffset + 1 + x * 4;
      rows[targetOffset] = rgba[sourceOffset]!;
      rows[targetOffset + 1] = rgba[sourceOffset + 1]!;
      rows[targetOffset + 2] = rgba[sourceOffset + 2]!;
      rows[targetOffset + 3] = rgba[sourceOffset + 3]!;
    }
  }

  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0);
  header.writeUInt32BE(height, 4);
  header[8] = 8;
  header[9] = 6;
  header[10] = 0;
  header[11] = 0;
  header[12] = 0;

  return Buffer.concat([
    pngSignature,
    createPngChunk("IHDR", header),
    createPngChunk("IDAT", deflateSync(rows)),
    createPngChunk("IEND", Buffer.alloc(0)),
  ]);
};

const parseIco = (content: Buffer): IcoEntry[] => {
  expect(content.readUInt16LE(0)).toBe(0);
  expect(content.readUInt16LE(2)).toBe(1);
  const count = content.readUInt16LE(4);
  const entries: IcoEntry[] = [];
  for (let index = 0; index < count; index += 1) {
    const offset = 6 + index * 16;
    const width = content[offset] === 0 ? 256 : content[offset]!;
    const height = content[offset + 1] === 0 ? 256 : content[offset + 1]!;
    const dataSize = content.readUInt32LE(offset + 8);
    const dataOffset = content.readUInt32LE(offset + 12);
    entries.push({
      width,
      height,
      planes: content.readUInt16LE(offset + 4),
      bitCount: content.readUInt16LE(offset + 6),
      data: content.subarray(dataOffset, dataOffset + dataSize),
    });
  }
  return entries;
};

const isPng = (data: Buffer): boolean =>
  data.subarray(0, pngSignature.length).equals(pngSignature);

const readDibPixel = (
  entry: IcoEntry,
  x: number,
  y: number,
): { r: number; g: number; b: number; a: number } => {
  const width = entry.width;
  const height = entry.height;
  const bitmapHeight = entry.data.readInt32LE(8);
  expect(bitmapHeight).toBe(height * 2);
  const rowOffset = 40 + (height - 1 - y) * width * 4 + x * 4;
  return {
    b: entry.data[rowOffset]!,
    g: entry.data[rowOffset + 1]!,
    r: entry.data[rowOffset + 2]!,
    a: entry.data[rowOffset + 3]!,
  };
};

describe("Windows icon generation", () => {
  it("creates a seven-image ICO from a single PNG", async () => {
    const png = createRgbaPng(1, 1, [0x22, 0x55, 0xaa, 0xff]);

    const ico = await createWindowsIconBufferFromPngData(png, "single.png");
    const entries = parseIco(ico);

    expect(entries.map((entry) => entry.width)).toEqual(windowsIconSizes);
    expect(entries.map((entry) => entry.height)).toEqual(windowsIconSizes);
    for (const entry of entries.slice(0, -1)) {
      expect(entry.planes).toBe(1);
      expect(entry.bitCount).toBe(32);
      expect(isPng(entry.data)).toBe(false);
      expect(entry.data.readUInt32LE(0)).toBe(40);
      expect(entry.data.readInt32LE(4)).toBe(entry.width);
      expect(entry.data.readUInt16LE(12)).toBe(1);
      expect(entry.data.readUInt16LE(14)).toBe(32);
    }
    expect(isPng(entries[entries.length - 1]!.data)).toBe(true);
  });

  it("fits a non-square PNG into transparent padding without stretching it", async () => {
    const png = createRgbaPng(
      2,
      1,
      [0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff],
    );

    const ico = await createWindowsIconBufferFromPngData(png, "wide.png");
    const entry = parseIco(ico).find((candidate) => candidate.width === 32);
    expect(entry).toBeDefined();

    const top = readDibPixel(entry!, 16, 2);
    const left = readDibPixel(entry!, 8, 16);
    const right = readDibPixel(entry!, 24, 16);

    expect(top.a).toBe(0);
    expect(left.r).toBeGreaterThan(200);
    expect(left.b).toBeLessThan(80);
    expect(right.b).toBeGreaterThan(200);
    expect(right.r).toBeLessThan(80);
  });

  it("rejects empty and invalid PNG input", async () => {
    await expect(
      createWindowsIconBufferFromPngData(Buffer.alloc(0), "empty.png"),
    ).rejects.toThrow("Windows resource icon PNG must not be empty: empty.png");
    await expect(
      createWindowsIconBufferFromPngData(Buffer.from("not png"), "bad.png"),
    ).rejects.toThrow("Windows resource icon must be a valid PNG: bad.png");
  });
});
