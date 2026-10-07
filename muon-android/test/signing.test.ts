import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, expect, it, vi } from 'vitest';
import {
  resolveAndroidSigning,
  signAndroidApplication,
} from '../src/signing.js';

const command = vi.hoisted(() =>
  vi.fn(async (_command, args, _cwd, _env, _output) => {
    if (args[0] === 'sign') {
      const { copyFile } = await import('node:fs/promises');
      await copyFile(args.at(-1), args[args.indexOf('--out') + 1]);
    }
    return 'Signer #1 certificate SHA-256 digest: ' + 'a'.repeat(64);
  })
);
vi.mock('../src/command.js', () => ({ runAndroidCommand: command }));
const roots: string[] = [];
afterEach(async () => {
  command.mockClear();
  await Promise.all(
    roots
      .splice(0)
      .map(async (root) => await rm(root, { force: true, recursive: true }))
  );
});

it('requires external signing configuration and password environment variables', async () => {
  expect(() => resolveAndroidSigning(undefined, '/tmp', {})).toThrow(/signing/);
  expect(() =>
    resolveAndroidSigning(
      {
        keystore: 'app.jks',
        keyAlias: 'release',
        storePasswordEnv: 'STORE_PASS',
      },
      '/tmp',
      {}
    )
  ).toThrow(/STORE_PASS/);
  expect(() =>
    resolveAndroidSigning(
      { keystore: 'app.jks', keyAlias: 'release', storePassword: 'plain-text' },
      '/tmp',
      {}
    )
  ).toThrow(/storePassword/);
});

it('passes password references to apksigner and reports verified release metadata', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-signing-'));
  roots.push(root);
  await writeFile(join(root, 'app.jks'), 'test keystore');
  const packagePath = join(root, 'app-release.apk');
  await writeFile(packagePath, 'test apk');
  const environment = {
    STORE_PASS: 'secret-store-value',
    KEY_PASS: 'secret-key-value',
  };
  const signing = resolveAndroidSigning(
    {
      keystore: 'app.jks',
      keyAlias: 'release',
      storePasswordEnv: 'STORE_PASS',
      keyPasswordEnv: 'KEY_PASS',
    },
    root,
    environment
  );
  const result = await signAndroidApplication(
    {
      target: 'android',
      packagePath,
      projectDirectory: root,
      variant: 'release',
      abis: ['arm64-v8a'],
      applicationId: 'dev.example.app',
      versionCode: 1,
      versionName: '1',
      signing: 'unsigned',
    },
    signing,
    { sdkPath: '/sdk', buildTools: '36.0.0' },
    environment
  );
  expect(result).toMatchObject({
    signing: 'release',
    certificateSha256: 'a'.repeat(64),
  });
  const invocations = command.mock.calls.map(([program, args]) => ({
    program,
    args,
  }));
  expect(JSON.stringify(invocations)).not.toContain('secret-');
  expect(invocations).toContainEqual(
    expect.objectContaining({
      args: expect.arrayContaining(['env:STORE_PASS', 'env:KEY_PASS']),
    })
  );
  expect(await readFile(packagePath + '.json', 'utf8')).not.toMatch(
    /STORE_PASS|KEY_PASS|app.jks|secret-/
  );
});

it('redacts secrets from a failing signer diagnostic', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-signing-error-'));
  roots.push(root);
  await writeFile(join(root, 'app.jks'), 'test keystore');
  command.mockRejectedValueOnce(new Error('rejected secret-value'));
  const environment = { STORE_PASS: 'secret-value' };
  const signing = resolveAndroidSigning(
    {
      keystore: 'app.jks',
      keyAlias: 'release',
      storePasswordEnv: 'STORE_PASS',
    },
    root,
    environment
  );
  await expect(
    signAndroidApplication(
      {
        target: 'android',
        packagePath: join(root, 'release.apk'),
        projectDirectory: root,
        variant: 'release',
        abis: ['arm64-v8a'],
        applicationId: 'dev.example.app',
        versionCode: 1,
        versionName: '1',
        signing: 'unsigned',
      },
      signing,
      { sdkPath: '/sdk', buildTools: '36.0.0' },
      environment
    )
  ).rejects.toThrow('rejected [redacted]');
});
