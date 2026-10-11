import { getPluginTarget, type MuonPluginTarget } from './targets.js';

/** Loader information read from a native library without executing it. */
export interface PluginBinaryInfo {
  /** Exported symbol names. */ readonly exports: readonly string[];
  /** Imported shared libraries. */ readonly needed: readonly string[];
  /** ELF SONAME, or undefined for PE. */ readonly soname: string | undefined;
  /** ELF runtime search paths. */ readonly runpath: readonly string[];
}

/**
 * Validates the binary format and architecture and reads loader metadata.
 * @param data - Complete final library bytes.
 * @param target - Required output platform.
 * @returns Symbols and shared library dependencies; never runs native code.
 */
export const inspectPluginBinary = (
  data: Buffer,
  target: MuonPluginTarget
): PluginBinaryInfo => {
  const descriptor = getPluginTarget(target);
  const range = (offset: number, length: number): number => {
    if (
      !Number.isSafeInteger(offset) ||
      !Number.isSafeInteger(length) ||
      offset < 0 ||
      length < 0 ||
      offset + length > data.length
    )
      throw new Error('Native library contains an invalid table or offset.');
    return offset;
  };
  const u16 = (offset: number): number => data.readUInt16LE(range(offset, 2));
  const u32 = (offset: number): number => data.readUInt32LE(range(offset, 4));
  const u64 = (offset: number): number => {
    const value = data.readBigUInt64LE(range(offset, 8));
    if (value > BigInt(Number.MAX_SAFE_INTEGER))
      throw new Error('Native library offset exceeds the supported size.');
    return Number(value);
  };
  const string = (offset: number, end: number): string => {
    range(offset, end - offset);
    const nul = data.indexOf(0, offset);
    if (nul < offset || nul >= end)
      throw new Error('Native library contains an unterminated string.');
    return data.toString('utf8', offset, nul);
  };
  const exports: string[] = [];
  const needed: string[] = [];
  const runpath: string[] = [];
  let soname: string | undefined;
  if (descriptor.os === 'windows') {
    if (u16(0) !== 0x5a4d) throw new Error('Expected a PE DLL.');
    const pe = u32(60);
    if (u32(pe) !== 0x4550 || !(u16(pe + 22) & 0x2000))
      throw new Error('Expected a PE DLL.');
    if (u16(pe + 4) !== (target === 'windows-i686' ? 0x14c : 0x8664))
      throw new Error(`PE machine does not match ${target}.`);
    const optional = pe + 24;
    const is64 = u16(optional) === 0x20b;
    if (u16(optional) !== (target === 'windows-i686' ? 0x10b : 0x20b))
      throw new Error('Invalid PE optional header.');
    const table = optional + u16(pe + 20);
    const sections = Array.from({ length: u16(pe + 6) }, (_, i) => {
      const offset = table + i * 40;
      return {
        address: u32(offset + 12),
        size: u32(offset + 16),
        offset: u32(offset + 20),
      };
    });
    const rva = (address: number, size: number): number => {
      const section = sections.find(
        (item) =>
          address >= item.address && address + size <= item.address + item.size
      );
      if (!section) throw new Error('Invalid PE virtual address.');
      return range(section.offset + address - section.address, size);
    };
    const directory = optional + (is64 ? 112 : 96);
    const exportAddress = u32(directory);
    if (exportAddress) {
      const entry = rva(exportAddress, 40);
      const count = u32(entry + 24);
      const names = rva(u32(entry + 32), count * 4);
      for (let i = 0; i < count; i++)
        exports.push(string(rva(u32(names + i * 4), 1), data.length));
    }
    const importAddress = u32(directory + 8);
    if (importAddress) {
      const importSize = u32(directory + 12);
      const base = rva(importAddress, importSize);
      for (
        let offset = base;
        offset + 20 <= base + importSize && u32(offset + 12);
        offset += 20
      )
        needed.push(string(rva(u32(offset + 12), 1), data.length));
    }
  } else {
    const is64 = target !== 'linux-armhf';
    if (
      u32(0) !== 0x464c457f ||
      data[4] !== (is64 ? 2 : 1) ||
      data[5] !== 1 ||
      u16(16) !== 3
    )
      throw new Error(
        `Expected an ELF${is64 ? 64 : 32} little-endian shared library.`
      );
    const machine = target.includes('arm64')
      ? 183
      : target === 'linux-armhf'
        ? 40
        : 62;
    if (u16(18) !== machine)
      throw new Error(`ELF machine does not match ${target}.`);
    const word = is64 ? u64 : u32;
    const phoff = word(is64 ? 32 : 28);
    const phsize = u16(is64 ? 54 : 42);
    const phnum = u16(is64 ? 56 : 44);
    range(phoff, phsize * phnum);
    let loads = 0;
    for (let i = 0; i < phnum; i++) {
      const offset = phoff + i * phsize;
      if (u32(offset) !== 1) continue;
      loads++;
      if (
        descriptor.os === 'android' &&
        (word(offset + 48) < 16384 ||
          word(offset + 8) % 16384 !== word(offset + 16) % 16384)
      )
        throw new Error('Android ELF load segments must support 16 KiB pages.');
    }
    if (!loads) throw new Error('ELF has no load segments.');
    const shoff = word(is64 ? 40 : 32);
    const shsize = u16(is64 ? 58 : 46);
    const shnum = u16(is64 ? 60 : 48);
    range(shoff, shsize * shnum);
    const sections = Array.from({ length: shnum }, (_, i) => {
      const offset = shoff + i * shsize;
      return {
        type: u32(offset + 4),
        offset: word(offset + (is64 ? 24 : 16)),
        size: word(offset + (is64 ? 32 : 20)),
        link: u32(offset + (is64 ? 40 : 24)),
        entrySize: word(offset + (is64 ? 56 : 36)),
      };
    });
    for (const section of sections) {
      if (section.type !== 6 && section.type !== 11) continue;
      const size = section.type === 6 ? (is64 ? 16 : 8) : is64 ? 24 : 16;
      if (section.entrySize !== size || section.size % size)
        throw new Error('Invalid ELF dynamic table.');
      range(section.offset, section.size);
      const strings = sections[section.link];
      if (!strings) throw new Error('Missing ELF string table.');
      range(strings.offset, strings.size);
      const name = (index: number): string =>
        string(range(strings.offset + index, 1), strings.offset + strings.size);
      for (
        let offset = section.offset;
        offset < section.offset + section.size;
        offset += size
      ) {
        if (section.type === 11) {
          const info = data[offset + (is64 ? 4 : 12)]!;
          const visibility = data[offset + (is64 ? 5 : 13)]! & 3;
          if (
            (info >> 4 === 1 || info >> 4 === 2) &&
            visibility === 0 &&
            u16(offset + (is64 ? 6 : 14)) !== 0
          )
            exports.push(name(u32(offset)));
        } else {
          const tag = word(offset);
          if (tag === 1 || tag === 14 || tag === 15 || tag === 29) {
            const value = name(word(offset + (is64 ? 8 : 4)));
            if (tag === 1) needed.push(value);
            else if (tag === 14) soname = value;
            else runpath.push(...value.split(':'));
          }
        }
      }
    }
  }
  return { exports, needed, soname, runpath };
};
