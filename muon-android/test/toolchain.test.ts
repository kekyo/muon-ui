import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { describe, expect, it } from 'vitest';
import { prepareAndroid } from '../src/toolchain.js';

describe('Android toolchain diagnostics', () => {
  it('reports the SDK path and required package when the SDK is empty', async () => {
    const sdk = await mkdtemp(join(tmpdir(), 'muon-empty-sdk-'));
    try {
      await expect(
        prepareAndroid({
          componentsDirectory: resolve('.'),
          sdkPath: sdk,
          environment: process.env,
          prepareGradle: false,
        })
      ).rejects.toThrow(/platforms;android-37/);
    } finally {
      await rm(sdk, { recursive: true, force: true });
    }
  });
  it('explains how to configure a missing SDK without downloading it', async () => {
    await expect(
      prepareAndroid({
        componentsDirectory: resolve('.'),
        sdkPath: undefined,
        environment: { PATH: '' },
        prepareGradle: false,
      })
    ).rejects.toThrow(/ANDROID_HOME/);
  });
});
