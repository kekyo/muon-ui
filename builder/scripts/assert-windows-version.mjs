import { readFile } from "node:fs/promises";

const fail = (message) => {
  console.error(message);
  process.exit(1);
};

const readUInt16 = (buffer, offset) => buffer.readUInt16LE(offset);
const readUInt32 = (buffer, offset) => buffer.readUInt32LE(offset);
const align = (value, alignment) => Math.ceil(value / alignment) * alignment;

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
      resources.push(content.subarray(fileOffset, fileOffset + dataSize));
    }
  }
  return resources;
};

const readBlockKey = (data, offset, blockEnd) => {
  let cursor = offset + 6;
  const keyStart = cursor;
  while (cursor + 2 <= blockEnd && readUInt16(data, cursor) !== 0) {
    cursor += 2;
  }
  if (cursor + 2 > blockEnd) {
    throw new Error("Version block key is truncated.");
  }
  const key = data.toString("utf16le", keyStart, cursor);
  cursor += 2;
  return {
    key,
    valueOffset: align(cursor, 4),
  };
};

const readVersionQuad = (data, offset) => {
  const most = readUInt32(data, offset);
  const least = readUInt32(data, offset + 4);
  return [
    most >>> 16,
    most & 0xffff,
    least >>> 16,
    least & 0xffff,
  ].join(".");
};

const readUtf16Value = (data, offset, units) => {
  let endUnits = units;
  while (endUnits > 0 && readUInt16(data, offset + (endUnits - 1) * 2) === 0) {
    endUnits -= 1;
  }
  return data.toString("utf16le", offset, offset + endUnits * 2);
};

const parseStringTable = (data, offset, end, strings) => {
  const length = readUInt16(data, offset);
  const blockEnd = offset + length;
  const { valueOffset } = readBlockKey(data, offset, blockEnd);
  let cursor = valueOffset;
  while (cursor + 6 <= blockEnd && cursor < end) {
    const childLength = readUInt16(data, cursor);
    const valueLength = readUInt16(data, cursor + 2);
    if (childLength === 0) {
      break;
    }
    const childEnd = cursor + childLength;
    const child = readBlockKey(data, cursor, childEnd);
    strings.set(child.key, readUtf16Value(data, child.valueOffset, valueLength));
    cursor = align(childEnd, 4);
  }
};

const parseVersion = (data) => {
  const rootLength = readUInt16(data, 0);
  const rootValueLength = readUInt16(data, 2);
  const root = readBlockKey(data, 0, rootLength);
  if (root.key !== "VS_VERSION_INFO") {
    throw new Error("Expected VS_VERSION_INFO root.");
  }
  const fixed = {
    fileVersion: readVersionQuad(data, root.valueOffset + 8),
    productVersion: readVersionQuad(data, root.valueOffset + 16),
  };
  const strings = new Map();
  let cursor = align(root.valueOffset + rootValueLength, 4);
  while (cursor + 6 <= rootLength) {
    const childLength = readUInt16(data, cursor);
    if (childLength === 0) {
      break;
    }
    const childEnd = cursor + childLength;
    const child = readBlockKey(data, cursor, childEnd);
    if (child.key === "StringFileInfo") {
      let tableCursor = child.valueOffset;
      while (tableCursor + 6 <= childEnd) {
        const tableLength = readUInt16(data, tableCursor);
        if (tableLength === 0) {
          break;
        }
        parseStringTable(data, tableCursor, childEnd, strings);
        tableCursor = align(tableCursor + tableLength, 4);
      }
    }
    cursor = align(childEnd, 4);
  }
  return { fixed, strings };
};

const [executablePath, ...expectations] = process.argv.slice(2);
if (executablePath === undefined) {
  fail(
    "Usage: node assert-windows-version.mjs <exe> [--file-version x.x.x.x] [--product-version x.x.x.x] [Key=value...]",
  );
}

const resources = collectResourceData(await readFile(executablePath), 16);
if (resources.length === 0) {
  fail(`${executablePath} does not contain a version resource.`);
}
const version = parseVersion(resources[0]);

for (let index = 0; index < expectations.length; index += 1) {
  const expectation = expectations[index];
  if (expectation === "--file-version") {
    const expected = expectations[++index];
    if (version.fixed.fileVersion !== expected) {
      fail(`Expected fixed file version ${expected}, got ${version.fixed.fileVersion}.`);
    }
  } else if (expectation === "--product-version") {
    const expected = expectations[++index];
    if (version.fixed.productVersion !== expected) {
      fail(
        `Expected fixed product version ${expected}, got ${version.fixed.productVersion}.`,
      );
    }
  } else {
    const separator = expectation.indexOf("=");
    if (separator <= 0) {
      fail(`Invalid version string expectation: ${expectation}`);
    }
    const key = expectation.slice(0, separator);
    const expected = expectation.slice(separator + 1);
    const actual = version.strings.get(key);
    if (actual !== expected) {
      fail(`Expected ${key}=${expected}, got ${actual ?? "(missing)"}.`);
    }
  }
}
