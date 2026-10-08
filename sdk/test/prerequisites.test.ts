import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { expect, it } from 'vitest';
import { diagnosePluginToolchain } from '../src/index.js';

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
