// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it } from 'vitest';

import {
  normalizeAndroidPluginRegistry,
  validateAndroidPluginArtifacts,
} from '../scripts/android-plugin-registry.mjs';

const createValidRegistry = (): unknown => ({
  schemaVersion: 1,
  plugins: [
    {
      name: 'muon_test_plugin_alpha',
      soname: 'libmuon_test_plugin_alpha.so',
      source: '../core/common-test/plugins/muon_test_plugin_alpha.cpp',
      artifacts: {
        x86_64: 'lib/x86_64/libmuon_test_plugin_alpha.so',
        'arm64-v8a': 'lib/arm64-v8a/libmuon_test_plugin_alpha.so',
      },
      allow: ['muon.test.alpha.*'],
      config: {
        'alpha.config': 'android-registry',
        channel: 'test',
      },
    },
  ],
});

describe('muon Android plugin registry', () => {
  it('normalizes one ordered build-time plugin table', () => {
    expect(normalizeAndroidPluginRegistry(createValidRegistry())).toEqual({
      schemaVersion: 1,
      plugins: [
        {
          name: 'muon_test_plugin_alpha',
          soname: 'libmuon_test_plugin_alpha.so',
          source: '../core/common-test/plugins/muon_test_plugin_alpha.cpp',
          artifacts: {
            x86_64: 'lib/x86_64/libmuon_test_plugin_alpha.so',
            'arm64-v8a': 'lib/arm64-v8a/libmuon_test_plugin_alpha.so',
          },
          allow: ['muon.test.alpha.*'],
          config: [
            { key: 'alpha.config', value: 'android-registry' },
            { key: 'channel', value: 'test' },
          ],
        },
      ],
    });
  });

  it.each([
    {
      name: 'unsupported schema',
      mutate: (registry: Record<string, unknown>) => {
        registry.schemaVersion = 2;
      },
      diagnostic: 'schemaVersion',
    },
    {
      name: 'duplicate logical name',
      mutate: (registry: Record<string, unknown>) => {
        const plugins = registry.plugins as unknown[];
        plugins.push(structuredClone(plugins[0]!));
      },
      diagnostic: 'duplicate plugin name',
    },
    {
      name: 'duplicate soname',
      mutate: (registry: Record<string, unknown>) => {
        const plugins = registry.plugins as Array<Record<string, unknown>>;
        const duplicate = structuredClone(plugins[0]!);
        duplicate.name = 'second_plugin';
        duplicate.source =
          '../core/common-test/plugins/muon_test_plugin_beta.cpp';
        plugins.push(duplicate);
      },
      diagnostic: 'duplicate plugin soname',
    },
    {
      name: 'invalid soname',
      mutate: (registry: Record<string, unknown>) => {
        const plugins = registry.plugins as Array<Record<string, unknown>>;
        plugins[0]!.soname = '../libalpha.so';
      },
      diagnostic: 'soname',
    },
    {
      name: 'missing x86_64 artifact',
      mutate: (registry: Record<string, unknown>) => {
        const plugins = registry.plugins as Array<Record<string, unknown>>;
        const artifacts = plugins[0]!.artifacts as Record<string, unknown>;
        delete artifacts.x86_64;
      },
      diagnostic: 'artifacts.x86_64',
    },
    {
      name: 'missing arm64 artifact',
      mutate: (registry: Record<string, unknown>) => {
        const plugins = registry.plugins as Array<Record<string, unknown>>;
        const artifacts = plugins[0]!.artifacts as Record<string, unknown>;
        delete artifacts['arm64-v8a'];
      },
      diagnostic: 'artifacts.arm64-v8a',
    },
    {
      name: 'unsupported ABI artifact',
      mutate: (registry: Record<string, unknown>) => {
        const plugins = registry.plugins as Array<Record<string, unknown>>;
        const artifacts = plugins[0]!.artifacts as Record<string, unknown>;
        artifacts['armeabi-v7a'] =
          'lib/armeabi-v7a/libmuon_test_plugin_alpha.so';
      },
      diagnostic: 'artifacts.armeabi-v7a is not supported',
    },
    {
      name: 'mismatched package artifact',
      mutate: (registry: Record<string, unknown>) => {
        const plugins = registry.plugins as Array<Record<string, unknown>>;
        const artifacts = plugins[0]!.artifacts as Record<string, unknown>;
        artifacts.x86_64 = 'lib/x86_64/libwrong.so';
      },
      diagnostic: 'artifacts.x86_64',
    },
    {
      name: 'empty allow list',
      mutate: (registry: Record<string, unknown>) => {
        const plugins = registry.plugins as Array<Record<string, unknown>>;
        plugins[0]!.allow = [];
      },
      diagnostic: 'allow must not be empty',
    },
    {
      name: 'non-string config value',
      mutate: (registry: Record<string, unknown>) => {
        const plugins = registry.plugins as Array<Record<string, unknown>>;
        const config = plugins[0]!.config as Record<string, unknown>;
        config.channel = 1;
      },
      diagnostic: 'config.channel',
    },
    {
      name: 'desktop plugin path',
      mutate: (registry: Record<string, unknown>) => {
        const plugins = registry.plugins as Array<Record<string, unknown>>;
        plugins[0]!.path = 'plugins';
      },
      diagnostic: '.path is unavailable on Android',
    },
    {
      name: 'desktop plugin signature',
      mutate: (registry: Record<string, unknown>) => {
        const plugins = registry.plugins as Array<Record<string, unknown>>;
        plugins[0]!.signature = '00'.repeat(32);
      },
      diagnostic: '.signature is unavailable on Android',
    },
    {
      name: 'desktop plugin signature salt',
      mutate: (registry: Record<string, unknown>) => {
        const plugins = registry.plugins as Array<Record<string, unknown>>;
        plugins[0]!.salt = '00'.repeat(16);
      },
      diagnostic: '.salt is unavailable on Android',
    },
  ])('rejects $name', ({ mutate, diagnostic }) => {
    const registry = createValidRegistry() as Record<string, unknown>;
    mutate(registry);
    expect(() => normalizeAndroidPluginRegistry(registry)).toThrow(diagnostic);
  });

  it('accepts matching ABI artifacts with an exported plugin entry point', () => {
    const registry = normalizeAndroidPluginRegistry(createValidRegistry());
    expect(() =>
      validateAndroidPluginArtifacts(registry, ({ abi, plugin }) => ({
        machine: abi === 'x86_64' ? 'Advanced Micro Devices X86-64' : 'AArch64',
        soname: plugin.soname,
        exportedSymbols: ['muon_init_plugin'],
        loadAlignments: [0x4000, 0x4000, 0x4000],
      }))
    ).not.toThrow();
  });

  it.each([
    {
      name: 'missing file',
      inspect: () => {
        throw new Error('ENOENT');
      },
      diagnostic: 'muon_test_plugin_alpha x86_64 artifact',
    },
    {
      name: 'wrong ELF machine',
      inspect: () => ({
        machine: 'AArch64',
        soname: 'libmuon_test_plugin_alpha.so',
        exportedSymbols: ['muon_init_plugin'],
        loadAlignments: [0x4000],
      }),
      diagnostic: 'ELF machine',
    },
    {
      name: 'wrong ELF soname',
      inspect: () => ({
        machine: 'Advanced Micro Devices X86-64',
        soname: 'libwrong.so',
        exportedSymbols: ['muon_init_plugin'],
        loadAlignments: [0x4000],
      }),
      diagnostic: 'ELF soname',
    },
    {
      name: 'missing plugin entry point',
      inspect: () => ({
        machine: 'Advanced Micro Devices X86-64',
        soname: 'libmuon_test_plugin_alpha.so',
        exportedSymbols: [],
        loadAlignments: [0x4000],
      }),
      diagnostic: 'muon_init_plugin',
    },
    {
      name: 'non-16 KiB ELF alignment',
      inspect: () => ({
        machine: 'Advanced Micro Devices X86-64',
        soname: 'libmuon_test_plugin_alpha.so',
        exportedSymbols: ['muon_init_plugin'],
        loadAlignments: [0x1000],
      }),
      diagnostic: 'LOAD alignment',
    },
  ])(
    'rejects a real artifact inspection with $name',
    ({ inspect, diagnostic }) => {
      const registry = normalizeAndroidPluginRegistry(createValidRegistry());
      expect(() => validateAndroidPluginArtifacts(registry, inspect)).toThrow(
        diagnostic
      );
    }
  );
});
