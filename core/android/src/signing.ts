// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { access, rename, rm, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import type { MuonAndroidApplicationResult } from './build.js';
import { runAndroidCommand } from './command.js';

/** Signing references; password values are held only in the child environment. */
export interface MuonAndroidSigning {
  /** Absolute path to an external keystore. */ readonly keystore: string;
  /** Keystore key alias. */ readonly keyAlias: string;
  /** Environment variable holding the keystore password. */ readonly storePasswordEnv: string;
  /** Environment variable holding the key password. */ readonly keyPasswordEnv: string;
}

/** APK whose release signature and ZIP alignment have been verified. */
export interface MuonAndroidSignedApplicationResult extends MuonAndroidApplicationResult {
  /** Verified signing state. */ readonly signing: 'release';
  /** Public signer certificate fingerprint. */ readonly certificateSha256: string;
}

/**
 * Validates external signing references before building a release application.
 * @param input - Parsed android.signing object.
 * @param directory - Directory resolving relative keystore paths.
 * @param environment - Environment containing password values.
 * @returns Signing references, without copying password values.
 */
export const resolveAndroidSigning = (
  input: unknown,
  directory: string,
  environment: NodeJS.ProcessEnv
): MuonAndroidSigning => {
  if (input === null || typeof input !== 'object' || Array.isArray(input))
    throw new Error(
      'Android release APKs require android.signing with an external keystore and password environment variables.'
    );
  const value = input as Record<string, unknown>;
  const allowed = new Set([
    'keystore',
    'keyAlias',
    'storePasswordEnv',
    'keyPasswordEnv',
  ]);
  for (const key of Object.keys(value))
    if (!allowed.has(key))
      throw new Error(
        `android.signing.${key} is unsupported. Use password environment variable names, never inline passwords.`
      );
  const string = (key: string): string => {
    const text = value[key];
    if (typeof text !== 'string' || !text || text.includes('\0'))
      throw new Error(
        `android.signing.${key} must be a nonempty string without NUL.`
      );
    return text;
  };
  const keystore = resolve(directory, string('keystore'));
  const keyAlias = string('keyAlias');
  const storePasswordEnv = string('storePasswordEnv');
  const keyPasswordEnv =
    value.keyPasswordEnv === undefined
      ? storePasswordEnv
      : string('keyPasswordEnv');
  for (const key of [storePasswordEnv, keyPasswordEnv]) {
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/u.test(key))
      throw new Error(
        'Android signing password environment variable names are invalid.'
      );
    if (!environment[key])
      throw new Error(
        `Android signing requires the ${key} environment variable.`
      );
  }
  return { keystore, keyAlias, storePasswordEnv, keyPasswordEnv };
};

/**
 * Signs an unsigned release APK, verifies the signature, then publishes metadata.
 * @param result - Unsigned application build result.
 * @param signing - External signing references.
 * @param tools - Validated SDK and Build Tools version.
 * @param environment - Signer environment; credentials never become CLI arguments.
 * @returns Verified APK metadata, including only the public certificate fingerprint.
 */
export const signAndroidApplication = async (
  result: MuonAndroidApplicationResult,
  signing: MuonAndroidSigning,
  tools: { sdkPath: string; buildTools: string },
  environment: NodeJS.ProcessEnv
): Promise<MuonAndroidSignedApplicationResult> => {
  if (result.variant !== 'release' || result.signing !== 'unsigned')
    throw new Error(
      'Android release signing requires an unsigned release APK.'
    );
  const redact = (message: string): string => {
    for (const key of [signing.storePasswordEnv, signing.keyPasswordEnv]) {
      const value = environment[key];
      if (value) message = message.replaceAll(value, '[redacted]');
    }
    return message;
  };
  const temporary = `${result.packagePath}.signed`;
  const signer = join(
    tools.sdkPath,
    'build-tools',
    tools.buildTools,
    'apksigner'
  );
  try {
    await access(signing.keystore);
    await runAndroidCommand(
      signer,
      [
        'sign',
        '--ks',
        signing.keystore,
        '--ks-key-alias',
        signing.keyAlias,
        '--ks-pass',
        `env:${signing.storePasswordEnv}`,
        '--key-pass',
        `env:${signing.keyPasswordEnv}`,
        '--v4-signing-enabled',
        'false',
        '--debuggable-apk-permitted',
        'false',
        '--lib-page-alignment',
        '16384',
        '--out',
        temporary,
        result.packagePath,
      ],
      dirname(result.packagePath),
      environment,
      undefined
    );
    const verification = await runAndroidCommand(
      signer,
      ['verify', '--verbose', '--print-certs', temporary],
      dirname(result.packagePath),
      environment,
      undefined
    );
    const certificateSha256 =
      /Signer #1 certificate SHA-256 digest: ([a-f0-9]{64})/iu
        .exec(verification)?.[1]
        ?.toLowerCase();
    if (!certificateSha256)
      throw new Error(
        'Android signer did not return a verified certificate fingerprint.'
      );
    if (/Signer #1 certificate DN:.*CN=Android Debug/iu.test(verification))
      throw new Error(
        'A release APK must use an application signing key, not the Android debug certificate.'
      );
    await runAndroidCommand(
      join(tools.sdkPath, 'build-tools', tools.buildTools, 'zipalign'),
      ['-c', '-P', '16', '4', temporary],
      dirname(result.packagePath),
      environment,
      undefined
    );
    await rename(temporary, result.packagePath);
    const signed: MuonAndroidSignedApplicationResult = {
      ...result,
      signing: 'release',
      certificateSha256,
    };
    await writeFile(
      `${result.packagePath}.json`,
      JSON.stringify(signed, null, 2) + '\n'
    );
    return signed;
  } catch (error) {
    throw new Error(
      redact(error instanceof Error ? error.message : String(error))
    );
  } finally {
    await rm(temporary, { force: true });
  }
};
