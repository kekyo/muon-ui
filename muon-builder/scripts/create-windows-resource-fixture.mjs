import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";

const writeUInt16 = (buffer, offset, value) => {
  buffer.writeUInt16LE(value, offset);
};

const writeUInt32 = (buffer, offset, value) => {
  buffer.writeUInt32LE(value, offset);
};

const align = (value, alignment) =>
  Math.ceil(value / alignment) * alignment;

const createMinimalPe = (overlay) => {
  const fileAlignment = 0x200;
  const sectionAlignment = 0x1000;
  const ntOffset = 0x80;
  const optionalHeaderSize = 224;
  const optionalHeaderOffset = ntOffset + 24;
  const sectionHeaderOffset = optionalHeaderOffset + optionalHeaderSize;
  const headerSize = 0x400;
  const textRawPointer = align(headerSize, fileAlignment);
  const textRawSize = 0x200;
  const baseSize = textRawPointer + textRawSize;
  const buffer = Buffer.alloc(baseSize + overlay.length);

  writeUInt16(buffer, 0, 0x5a4d);
  writeUInt32(buffer, 0x3c, ntOffset);
  buffer.write("PE\0\0", ntOffset, "ascii");
  writeUInt16(buffer, ntOffset + 4, 0x14c);
  writeUInt16(buffer, ntOffset + 6, 1);
  writeUInt16(buffer, ntOffset + 20, optionalHeaderSize);
  writeUInt16(buffer, ntOffset + 22, 0x0102);

  writeUInt16(buffer, optionalHeaderOffset, 0x10b);
  writeUInt32(buffer, optionalHeaderOffset + 16, 0x1000);
  writeUInt32(buffer, optionalHeaderOffset + 20, 0x1000);
  writeUInt32(buffer, optionalHeaderOffset + 28, 0x00400000);
  writeUInt32(buffer, optionalHeaderOffset + 32, sectionAlignment);
  writeUInt32(buffer, optionalHeaderOffset + 36, fileAlignment);
  writeUInt16(buffer, optionalHeaderOffset + 40, 6);
  writeUInt16(buffer, optionalHeaderOffset + 48, 6);
  writeUInt32(buffer, optionalHeaderOffset + 56, 0x2000);
  writeUInt32(buffer, optionalHeaderOffset + 60, headerSize);
  writeUInt16(buffer, optionalHeaderOffset + 68, 3);
  writeUInt32(buffer, optionalHeaderOffset + 92, 16);

  buffer.write(".text", sectionHeaderOffset, "ascii");
  writeUInt32(buffer, sectionHeaderOffset + 8, 1);
  writeUInt32(buffer, sectionHeaderOffset + 12, 0x1000);
  writeUInt32(buffer, sectionHeaderOffset + 16, textRawSize);
  writeUInt32(buffer, sectionHeaderOffset + 20, textRawPointer);
  writeUInt32(buffer, sectionHeaderOffset + 36, 0x60000020);
  buffer[textRawPointer] = 0xc3;
  overlay.copy(buffer, baseSize);
  return buffer;
};

const createIco = () => {
  const image = Buffer.from(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==",
    "base64",
  );
  const header = Buffer.alloc(22);
  writeUInt16(header, 0, 0);
  writeUInt16(header, 2, 1);
  writeUInt16(header, 4, 1);
  header[6] = 1;
  header[7] = 1;
  header[8] = 0;
  header[9] = 0;
  writeUInt16(header, 10, 1);
  writeUInt16(header, 12, 32);
  writeUInt32(header, 14, image.length);
  writeUInt32(header, 18, header.length);
  return Buffer.concat([header, image]);
};

const [pePath, icoPath, overlayPath] = process.argv.slice(2);
if (pePath === undefined || icoPath === undefined) {
  console.error(
    "Usage: node create-windows-resource-fixture.mjs <pe> <ico> [overlay]",
  );
  process.exit(1);
}

const overlay = overlayPath === undefined ? Buffer.alloc(0) : await readFile(overlayPath);
await mkdir(dirname(pePath), { recursive: true });
await mkdir(dirname(icoPath), { recursive: true });
await writeFile(pePath, createMinimalPe(overlay));
await writeFile(icoPath, createIco());
