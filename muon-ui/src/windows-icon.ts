// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";

import sharp from "sharp";

const normalizedIconSize = 256;
const windowsIconDibSizes = [16, 24, 32, 48, 64, 128] as const;
const pngSignature = Buffer.from([
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
]);

interface RawIconImage {
  data: Buffer;
  width: number;
  height: number;
}

interface WindowsIconImage {
  data: Buffer;
  size: number;
  bitCount: number;
}

/**
 * Creates a normalized square PNG icon image.
 *
 * @param pngData Source PNG bytes.
 * @param source Diagnostic source label used in errors.
 * @param label User-facing diagnostic label.
 * @returns PNG bytes normalized to the muon base icon size.
 * @remarks Non-square images are fitted into transparent padding without
 * stretching. The normalized PNG is the source for all platform-specific icon
 * derivatives.
 */
export const createNormalizedIconPngData = async (
  pngData: Buffer,
  source: string,
  label: string,
): Promise<Buffer> => {
  await assertPngData(pngData, source, label);
  try {
    return await sharp(pngData)
      .resize(normalizedIconSize, normalizedIconSize, {
        fit: "contain",
        background: { r: 0, g: 0, b: 0, alpha: 0 },
      })
      .ensureAlpha()
      .png()
      .toBuffer();
  } catch {
    throw new Error(`${label} must be a valid PNG: ${source}`);
  }
};

/**
 * Creates a normalized square PNG icon image for Windows resources.
 *
 * @param pngData Source PNG bytes.
 * @param source Diagnostic source label used in errors.
 * @returns PNG bytes normalized to the muon base icon size.
 */
export const createNormalizedMuonIconPngData = async (
  pngData: Buffer,
  source: string,
): Promise<Buffer> =>
  await createNormalizedIconPngData(pngData, source, "Windows resource icon");

/**
 * Creates a Windows ICO file from PNG bytes.
 *
 * @param pngData Source PNG bytes.
 * @param source Diagnostic source label used in errors.
 * @returns ICO file bytes containing 16, 24, 32, 48, 64, 128, and 256 pixel images.
 * @remarks The 256px entry is PNG-compressed. Smaller entries are 32-bit DIB
 * images, matching the common Windows ICO layout recommended by Microsoft.
 */
export const createWindowsIconBufferFromPngData = async (
  pngData: Buffer,
  source: string,
): Promise<Buffer> => {
  const normalizedPng = await createNormalizedMuonIconPngData(pngData, source);
  const images: WindowsIconImage[] = [];
  for (const size of windowsIconDibSizes) {
    const rawImage = await resizePngToRawRgba(normalizedPng, size, source);
    images.push({
      data: createDibIconImage(rawImage),
      size,
      bitCount: 32,
    });
  }
  images.push({
    data: normalizedPng,
    size: normalizedIconSize,
    bitCount: 32,
  });
  return createIco(images);
};

/**
 * Creates a Windows ICO file from a PNG file.
 *
 * @param sourcePath Source PNG file path.
 * @param outputPath Output ICO file path.
 * @returns Promise resolved when the ICO file has been written.
 */
export const createWindowsIconFromPngFile = async (
  sourcePath: string,
  outputPath: string,
): Promise<void> => {
  const icon = await createWindowsIconBufferFromPngData(
    await readFile(sourcePath),
    sourcePath,
  );
  await mkdir(dirname(outputPath), { recursive: true });
  await writeFile(outputPath, icon);
};

const assertPngData = async (
  data: Buffer,
  source: string,
  label: string,
): Promise<void> => {
  if (data.length === 0) {
    throw new Error(`${label} PNG must not be empty: ${source}`);
  }
  if (!data.subarray(0, pngSignature.length).equals(pngSignature)) {
    throw new Error(`${label} must be a valid PNG: ${source}`);
  }
  try {
    const metadata = await sharp(data).metadata();
    if (metadata.format !== "png") {
      throw new Error();
    }
  } catch {
    throw new Error(`${label} must be a valid PNG: ${source}`);
  }
};

const resizePngToRawRgba = async (
  pngData: Buffer,
  size: number,
  source: string,
): Promise<RawIconImage> => {
  try {
    const { data, info } = await sharp(pngData)
      .resize(size, size, {
        fit: "fill",
        kernel: "lanczos3",
      })
      .ensureAlpha()
      .raw()
      .toBuffer({ resolveWithObject: true });
    if (info.width !== size || info.height !== size || info.channels !== 4) {
      throw new Error();
    }
    return {
      data,
      width: info.width,
      height: info.height,
    };
  } catch {
    throw new Error(`Failed to resize Windows resource icon PNG: ${source}`);
  }
};

const createDibIconImage = (image: RawIconImage): Buffer => {
  const pixelBytes = image.width * image.height * 4;
  const maskStride = Math.ceil(image.width / 32) * 4;
  const maskBytes = maskStride * image.height;
  const output = Buffer.alloc(40 + pixelBytes + maskBytes);
  output.writeUInt32LE(40, 0);
  output.writeInt32LE(image.width, 4);
  output.writeInt32LE(image.height * 2, 8);
  output.writeUInt16LE(1, 12);
  output.writeUInt16LE(32, 14);
  output.writeUInt32LE(0, 16);
  output.writeUInt32LE(pixelBytes + maskBytes, 20);
  output.writeInt32LE(0, 24);
  output.writeInt32LE(0, 28);
  output.writeUInt32LE(0, 32);
  output.writeUInt32LE(0, 36);

  for (let y = 0; y < image.height; y += 1) {
    const sourceY = image.height - 1 - y;
    for (let x = 0; x < image.width; x += 1) {
      const sourceOffset = (sourceY * image.width + x) * 4;
      const outputOffset = 40 + (y * image.width + x) * 4;
      output[outputOffset] = image.data[sourceOffset + 2]!;
      output[outputOffset + 1] = image.data[sourceOffset + 1]!;
      output[outputOffset + 2] = image.data[sourceOffset]!;
      output[outputOffset + 3] = image.data[sourceOffset + 3]!;
    }
  }
  return output;
};

const createIco = (images: readonly WindowsIconImage[]): Buffer => {
  const directorySize = 6 + images.length * 16;
  let imageOffset = directorySize;
  const header = Buffer.alloc(directorySize);
  header.writeUInt16LE(0, 0);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(images.length, 4);

  const imageData: Buffer[] = [];
  images.forEach((image, index) => {
    const entryOffset = 6 + index * 16;
    header[entryOffset] = image.size === 256 ? 0 : image.size;
    header[entryOffset + 1] = image.size === 256 ? 0 : image.size;
    header[entryOffset + 2] = 0;
    header[entryOffset + 3] = 0;
    header.writeUInt16LE(1, entryOffset + 4);
    header.writeUInt16LE(image.bitCount, entryOffset + 6);
    header.writeUInt32LE(image.data.length, entryOffset + 8);
    header.writeUInt32LE(imageOffset, entryOffset + 12);
    imageOffset += image.data.length;
    imageData.push(image.data);
  });

  return Buffer.concat([header, ...imageData]);
};
