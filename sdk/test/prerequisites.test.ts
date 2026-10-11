import { chmod, mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { expect, it } from 'vitest';
import {
  diagnosePluginToolchain,
  getNativeBuildEnvironment,
} from '../src/index.js';

it('reports all missing tools with installation instructions and only requires C for C projects', async () => {
  const empty = await mkdtemp(join(tmpdir(), 'muon-tools-'));
  try {
    const report = await diagnosePluginToolchain({
      target: 'linux-amd64',
      language: 'c',
      sdkDirectory: resolve('.'),
      environment: { PATH: empty },
    });
    expect(report.ready).toBe(false);
    expect(report.issues.map((issue) => issue.id)).toEqual(
      expect.arrayContaining(['cc', 'make', 'pkg-config'])
    );
    expect(report.issues.map((issue) => issue.id)).not.toContain('cxx');
    for (const issue of report.issues) {
      expect(issue.install.length).toBeGreaterThan(0);
      expect(issue.documentation).toMatch(/^https:\/\//u);
    }
  } finally {
    await rm(empty, { recursive: true, force: true });
  }
});

it('finds installed C++/cardio prerequisites and detects missing target development libraries', async () => {
  expect(
    (
      await diagnosePluginToolchain({
        target: 'linux-amd64',
        sdkDirectory: resolve('.'),
      })
    ).ready
  ).toBe(true);
  const empty = await mkdtemp(join(tmpdir(), 'muon-pc-'));
  try {
    const report = await diagnosePluginToolchain({
      target: 'linux-amd64',
      sdkDirectory: resolve('.'),
      environment: { ...process.env, PKG_CONFIG_LIBDIR: empty },
    });
    expect(report.ready).toBe(false);
    expect(report.issues.map((issue) => issue.id)).toContain('glib-2.0');
    expect(
      report.issues.find((issue) => issue.id === 'glib-2.0')?.install
    ).toContain('libglib2.0-dev');
  } finally {
    await rm(empty, { recursive: true, force: true });
  }
});

it('explains how to install the exact Android NDK instead of throwing ENOENT', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-ndk-'));
  try {
    const report = await diagnosePluginToolchain({
      target: 'android-arm64-v8a',
      androidSdkPath: root,
      sdkDirectory: resolve('.'),
    });
    expect(report.ready).toBe(false);
    expect(
      report.issues.find((issue) => issue.id === 'ndk')?.install
    ).toContain('ndk;29.0.14206865');
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

it('reports plugin-specific native development packages and their installation instructions', async () => {
  const report = await diagnosePluginToolchain({
    target: 'linux-amd64',
    language: 'c',
    packages: [
      {
        name: 'muon-nonexistent-vendor >= 9',
        install: 'Install vendor-devel from the vendor repository.',
        documentation: 'https://example.org/vendor/install',
      },
    ],
  });
  expect(report.ready).toBe(false);
  expect(
    report.issues.find((issue) => issue.id === 'muon-nonexistent-vendor')
      ?.install
  ).toContain('vendor-devel');
});

it('diagnoses an installed but incompatible CMake version before configuration', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-old-cmake-'));
  try {
    await writeFile(
      join(root, 'cmake'),
      '#!/bin/sh\necho "cmake version 3.10.2"\n'
    );
    await chmod(join(root, 'cmake'), 0o755);
    const report = await diagnosePluginToolchain({
      target: 'linux-amd64',
      language: 'c',
      buildSystem: 'cmake',
      environment: { ...process.env, PATH: root + ':' + process.env.PATH },
    });
    expect(report.ready).toBe(false);
    expect(
      report.issues.find((issue) => issue.id === 'cmake-version')?.install
    ).toContain('3.24');
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

it('keeps SDK include paths outside a cross sysroot while isolating target pkg-config dependencies', async () => {
  const sysroot = await mkdtemp(join(tmpdir(), 'muon-sysroot-'));
  try {
    const directory = join(sysroot, 'usr/lib/x86_64-linux-gnu/pkgconfig');
    await mkdir(directory, { recursive: true });
    await writeFile(
      join(directory, 'vendor.pc'),
      'prefix=/usr\nName: vendor\nDescription: vendor test\nVersion: 1\nCflags: -I${prefix}/include/vendor\n'
    );
    const environment = await getNativeBuildEnvironment({
      target: 'linux-amd64',
      sysroot,
    });
    const flags = await promisify(execFile)(
      'pkg-config',
      ['--cflags', 'muon-plugin', 'vendor'],
      { env: environment }
    );
    expect(flags.stdout).toContain(sysroot + '/usr/include/vendor');
    expect(flags.stdout).not.toContain(sysroot + resolve('.'));
  } finally {
    await rm(sysroot, { recursive: true, force: true });
  }
});
