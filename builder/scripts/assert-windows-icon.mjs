import { readFile } from "node:fs/promises";

const formatResourceName = (resourceName) => {
  if (resourceName.kind === "id") {
    return `ID ${resourceName.value}`;
  }
  return `"${resourceName.value}"`;
};

const fail = (message) => {
  console.error(message);
  process.exit(1);
};

const readUInt16 = (buffer, offset) => buffer.readUInt16LE(offset);
const readUInt32 = (buffer, offset) => buffer.readUInt32LE(offset);

const parseIco = (content) => {
  if (content.length < 6) {
    throw new Error("ICO file is too small.");
  }
  const reserved = readUInt16(content, 0);
  const type = readUInt16(content, 2);
  const count = readUInt16(content, 4);
  if (reserved !== 0 || type !== 1 || count === 0) {
    throw new Error("Expected a Windows icon ICO file.");
  }
  const directorySize = 6 + count * 16;
  if (content.length < directorySize) {
    throw new Error("ICO directory is truncated.");
  }
  const entries = [];
  for (let index = 0; index < count; index += 1) {
    const offset = 6 + index * 16;
    const imageSize = readUInt32(content, offset + 8);
    const imageOffset = readUInt32(content, offset + 12);
    if (imageOffset + imageSize > content.length) {
      throw new Error("ICO image data is truncated.");
    }
    entries.push({
      width: content.readUInt8(offset),
      height: content.readUInt8(offset + 1),
      colorCount: content.readUInt8(offset + 2),
      reserved: content.readUInt8(offset + 3),
      planes: readUInt16(content, offset + 4),
      bitCount: readUInt16(content, offset + 6),
      bytesInRes: imageSize,
      data: content.subarray(imageOffset, imageOffset + imageSize),
    });
  }
  return entries;
};

const parseSections = (content) => {
  if (content.length < 0x40 || content.toString("ascii", 0, 2) !== "MZ") {
    throw new Error("Expected a PE executable.");
  }
  const peOffset = readUInt32(content, 0x3c);
  if (content.toString("ascii", peOffset, peOffset + 4) !== "PE\u0000\u0000") {
    throw new Error("PE signature was not found.");
  }

  const coffOffset = peOffset + 4;
  const sectionCount = readUInt16(content, coffOffset + 2);
  const optionalHeaderSize = readUInt16(content, coffOffset + 16);
  const optionalHeaderOffset = coffOffset + 20;
  const optionalHeaderMagic = readUInt16(content, optionalHeaderOffset);
  const dataDirectoryOffset =
    optionalHeaderMagic === 0x20b
      ? optionalHeaderOffset + 112
      : optionalHeaderMagic === 0x10b
        ? optionalHeaderOffset + 96
        : -1;
  if (dataDirectoryOffset < 0) {
    throw new Error("Unsupported PE optional header.");
  }
  const resourceDirectoryRva = readUInt32(content, dataDirectoryOffset + 16);
  const resourceDirectorySize = readUInt32(content, dataDirectoryOffset + 20);
  if (resourceDirectoryRva === 0 || resourceDirectorySize === 0) {
    throw new Error("PE file does not contain a resource directory.");
  }

  const sectionTableOffset = optionalHeaderOffset + optionalHeaderSize;
  const sections = [];
  for (let index = 0; index < sectionCount; index += 1) {
    const offset = sectionTableOffset + index * 40;
    sections.push({
      virtualSize: readUInt32(content, offset + 8),
      virtualAddress: readUInt32(content, offset + 12),
      rawDataSize: readUInt32(content, offset + 16),
      rawDataPointer: readUInt32(content, offset + 20),
    });
  }

  const rvaToFileOffset = (rva) => {
    for (const section of sections) {
      const size = Math.max(section.virtualSize, section.rawDataSize);
      if (
        rva >= section.virtualAddress &&
        rva < section.virtualAddress + size
      ) {
        return section.rawDataPointer + (rva - section.virtualAddress);
      }
    }
    throw new Error(`RVA 0x${rva.toString(16)} is outside PE sections.`);
  };

  return {
    resourceBaseOffset: rvaToFileOffset(resourceDirectoryRva),
    rvaToFileOffset,
  };
};

const readResourceName = (content, resourceBaseOffset, nameOrId) => {
  if ((nameOrId & 0x80000000) === 0) {
    return { kind: "id", value: nameOrId & 0xffff };
  }

  const stringOffset = resourceBaseOffset + (nameOrId & 0x7fffffff);
  const length = readUInt16(content, stringOffset);
  return {
    kind: "name",
    value: content.toString(
      "utf16le",
      stringOffset + 2,
      stringOffset + 2 + length * 2,
    ),
  };
};

const readResourceDirectory = (content, resourceBaseOffset, relativeOffset) => {
  const directoryOffset = resourceBaseOffset + relativeOffset;
  const namedCount = readUInt16(content, directoryOffset + 12);
  const idCount = readUInt16(content, directoryOffset + 14);
  const entries = [];
  for (let index = 0; index < namedCount + idCount; index += 1) {
    const entryOffset = directoryOffset + 16 + index * 8;
    const nameOrId = readUInt32(content, entryOffset);
    const offsetToData = readUInt32(content, entryOffset + 4);
    entries.push({
      name: readResourceName(content, resourceBaseOffset, nameOrId),
      isDirectory: (offsetToData & 0x80000000) !== 0,
      relativeOffset: offsetToData & 0x7fffffff,
    });
  }
  return entries;
};

const collectResourceData = (content, typeId) => {
  const pe = parseSections(content);
  const rootEntries = readResourceDirectory(content, pe.resourceBaseOffset, 0);
  const typeEntry = rootEntries.find(
    (entry) =>
      entry.name.kind === "id" &&
      entry.name.value === typeId &&
      entry.isDirectory,
  );
  if (typeEntry === undefined) {
    return [];
  }

  const resources = [];
  const nameEntries = readResourceDirectory(
    content,
    pe.resourceBaseOffset,
    typeEntry.relativeOffset,
  );
  for (const nameEntry of nameEntries) {
    if (!nameEntry.isDirectory) {
      continue;
    }

    const languageEntries = readResourceDirectory(
      content,
      pe.resourceBaseOffset,
      nameEntry.relativeOffset,
    );
    for (const languageEntry of languageEntries) {
      if (languageEntry.isDirectory) {
        continue;
      }

      const dataEntryOffset =
        pe.resourceBaseOffset + languageEntry.relativeOffset;
      const dataRva = readUInt32(content, dataEntryOffset);
      const dataSize = readUInt32(content, dataEntryOffset + 4);
      const fileOffset = pe.rvaToFileOffset(dataRva);
      resources.push({
        name: nameEntry.name,
        language: languageEntry.name,
        data: content.subarray(fileOffset, fileOffset + dataSize),
      });
    }
  }
  return resources;
};

const parseGroupIcon = (data) => {
  if (data.length < 6) {
    throw new Error("Group icon resource is too small.");
  }
  const reserved = readUInt16(data, 0);
  const type = readUInt16(data, 2);
  const count = readUInt16(data, 4);
  if (reserved !== 0 || type !== 1 || count === 0) {
    throw new Error("Expected a Windows group icon resource.");
  }
  if (data.length < 6 + count * 14) {
    throw new Error("Group icon resource is truncated.");
  }

  const entries = [];
  for (let index = 0; index < count; index += 1) {
    const offset = 6 + index * 14;
    entries.push({
      width: data.readUInt8(offset),
      height: data.readUInt8(offset + 1),
      colorCount: data.readUInt8(offset + 2),
      reserved: data.readUInt8(offset + 3),
      planes: readUInt16(data, offset + 4),
      bitCount: readUInt16(data, offset + 6),
      bytesInRes: readUInt32(data, offset + 8),
      iconId: readUInt16(data, offset + 12),
    });
  }
  return entries;
};

const entriesMatch = (icoEntry, groupEntry, iconData) =>
  icoEntry.width === groupEntry.width &&
  icoEntry.height === groupEntry.height &&
  icoEntry.colorCount === groupEntry.colorCount &&
  icoEntry.reserved === groupEntry.reserved &&
  icoEntry.planes === groupEntry.planes &&
  icoEntry.bitCount === groupEntry.bitCount &&
  icoEntry.bytesInRes === groupEntry.bytesInRes &&
  Buffer.compare(icoEntry.data, iconData) === 0;

const executablePath = process.argv[2];
const iconPath = process.argv[3];
if (executablePath === undefined || iconPath === undefined) {
  fail("Usage: node assert-windows-icon.mjs <exe> <ico>");
}

const executable = await readFile(executablePath);
const ico = await readFile(iconPath);
const icoEntries = parseIco(ico);
const iconResources = collectResourceData(executable, 3);
const iconDataById = new Map(
  iconResources
    .filter((resource) => resource.name.kind === "id")
    .map((resource) => [resource.name.value, resource.data]),
);
const groupIconResources = collectResourceData(executable, 14);

for (const groupIconResource of groupIconResources) {
  const groupEntries = parseGroupIcon(groupIconResource.data);
  if (groupEntries.length !== icoEntries.length) {
    continue;
  }

  const isMatch = groupEntries.every((groupEntry, index) => {
    const iconData = iconDataById.get(groupEntry.iconId);
    return (
      iconData !== undefined &&
      entriesMatch(icoEntries[index], groupEntry, iconData)
    );
  });
  if (isMatch) {
    process.exit(0);
  }
}

const groupDescriptions = groupIconResources
  .map((resource) => {
    const groupEntries = parseGroupIcon(resource.data);
    return `${formatResourceName(resource.name)} (${groupEntries.length} images)`;
  })
  .join(", ");
fail(
  `${executablePath} does not contain icon resources matching ${iconPath}. ` +
    `Found group icons: ${groupDescriptions || "none"}.`,
);
