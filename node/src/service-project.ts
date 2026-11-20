// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { timingSafeEqual } from 'node:crypto';
import { readFile, realpath } from 'node:fs/promises';
import { isAbsolute, join, relative, resolve, sep } from 'node:path';
import { pathToFileURL } from 'node:url';
import semverSatisfies from 'semver/functions/satisfies.js';
import semverValidRange from 'semver/ranges/valid.js';
declare const __MUON_NODE_SUPPORTED_ENGINE_RANGE__: string;

interface BridgeError extends Error {
  code: string;
}

const createBridgeError = (code: string, message: string): BridgeError => {
  const error = new Error(message) as BridgeError;
  error.code = code;
  return error;
};

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

const getErrorMessage = (error: unknown): string =>
  error instanceof Error ? error.message : String(error);

const requireRecord = (
  value: unknown,
  code: string,
  description: string
): Readonly<Record<string, unknown>> => {
  if (!isRecord(value)) {
    throw createBridgeError(code, `${description} must be an object.`);
  }
  return value;
};

const requireString = (
  value: unknown,
  code: string,
  description: string
): string => {
  if (typeof value !== 'string' || value.length === 0) {
    throw createBridgeError(code, `${description} must be a non-empty string.`);
  }
  return value;
};

const tokensMatch = (actual: string, expected: string): boolean => {
  const actualBytes = Buffer.from(actual, 'utf8');
  const expectedBytes = Buffer.from(expected, 'utf8');
  return (
    actualBytes.length === expectedBytes.length &&
    timingSafeEqual(actualBytes, expectedBytes)
  );
};

const readPackageManifest = async (
  projectRoot: string
): Promise<Readonly<Record<string, unknown>>> => {
  const packageFile = join(projectRoot, 'package.json');
  let source: string;
  try {
    source = await readFile(packageFile, 'utf8');
  } catch (error) {
    throw createBridgeError(
      'ERR_MUON_NODE_PROJECT',
      `Unable to read the Node project package.json: ${getErrorMessage(error)}`
    );
  }

  let parsed: unknown;
  try {
    parsed = JSON.parse(source) as unknown;
  } catch (error) {
    throw createBridgeError(
      'ERR_MUON_NODE_PROJECT',
      `The Node project package.json is invalid: ${getErrorMessage(error)}`
    );
  }
  return requireRecord(
    parsed,
    'ERR_MUON_NODE_PROJECT',
    'The Node project package.json'
  );
};

const validateNodeEngine = (
  manifest: Readonly<Record<string, unknown>>
): void => {
  const enginesValue = manifest.engines;
  if (enginesValue === undefined) {
    return;
  }
  const engines = requireRecord(
    enginesValue,
    'ERR_MUON_NODE_ENGINE',
    'package.json engines'
  );
  const nodeRange = engines.node;
  if (nodeRange === undefined) {
    return;
  }
  if (typeof nodeRange !== 'string' || nodeRange.length === 0) {
    throw createBridgeError(
      'ERR_MUON_NODE_ENGINE',
      'package.json engines.node must be a non-empty string.'
    );
  }

  // node-semver documents satisfies(version, range) as the strict range
  // membership API. Its default prerelease exclusion is retained here.
  if (semverValidRange(nodeRange) === null) {
    throw createBridgeError(
      'ERR_MUON_NODE_ENGINE',
      `package.json engines.node is not a valid range: ${nodeRange}.`
    );
  }
  if (!semverSatisfies(process.version, nodeRange)) {
    throw createBridgeError(
      'ERR_MUON_NODE_ENGINE',
      `Node ${process.version} does not satisfy engines.node ${nodeRange}.`
    );
  }
};

const validateHostedNodeEngine = (): void => {
  if (!semverSatisfies(process.version, __MUON_NODE_SUPPORTED_ENGINE_RANGE__)) {
    throw createBridgeError(
      'ERR_MUON_NODE_ENGINE',
      `Node ${process.version} is not supported by the muon Node service host; expected ${__MUON_NODE_SUPPORTED_ENGINE_RANGE__}.`
    );
  }
};

/**
 * Authenticates a child transport and resolves its independent project.
 * @param params - Protocol, token, and projectRoot from the initial request.
 * @param expectedToken - Authentication secret inherited from the service host.
 * @param expectedProtocol - Service protocol selected by the parent.
 * @returns Canonical service project directory.
 * @throws Error for authentication, engine, or project failures.
 */
export const initializeMuonNodeProject = async (
  params: Readonly<Record<string, unknown>>,
  expectedToken: string,
  expectedProtocol: string
): Promise<string> => {
  const requestedProtocol = requireString(
    params.protocol,
    'ERR_MUON_NODE_PROTOCOL',
    'initialize protocol'
  );
  if (requestedProtocol !== expectedProtocol) {
    throw createBridgeError(
      'ERR_MUON_NODE_PROTOCOL',
      `Unsupported muon Node protocol ${requestedProtocol}.`
    );
  }

  const actualToken = requireString(
    params.token,
    'ERR_MUON_NODE_AUTH',
    'initialize token'
  );
  if (!tokensMatch(actualToken, expectedToken)) {
    throw createBridgeError(
      'ERR_MUON_NODE_AUTH',
      'The initialize token is invalid.'
    );
  }

  validateHostedNodeEngine();

  const requestedRoot = requireString(
    params.projectRoot,
    'ERR_MUON_NODE_PROJECT',
    'initialize projectRoot'
  );
  let root: string;
  try {
    root = await realpath(resolve(requestedRoot));
  } catch (error) {
    throw createBridgeError(
      'ERR_MUON_NODE_PROJECT',
      `Unable to resolve the Node project: ${getErrorMessage(error)}`
    );
  }
  const manifest = await readPackageManifest(root);
  validateNodeEngine(manifest);
  if (process.platform === 'win32' && root.length >= 260) {
    throw createBridgeError(
      'ERR_MUON_NODE_PROJECT',
      'The Node service requires a short Windows working directory (under 260 UTF-16 code units). Use a short project root and load nested modules by relative paths.'
    );
  }
  try {
    process.chdir(root);
  } catch (error) {
    throw createBridgeError(
      'ERR_MUON_NODE_PROJECT',
      `Unable to use the Node project as the working directory: ${getErrorMessage(error)}`
    );
  }
  return root;
};

/**
 * Loads the default export of a compiled service entry inside its project.
 * @param root - Canonical project directory.
 * @param entry - Project-relative path from the service definition.
 * @returns The default export wrapped in a module namespace.
 */
export const loadNodeServiceEntry = async (
  root: string,
  entry: string
): Promise<{ default: unknown }> => {
  if (!entry.startsWith('./'))
    throw createBridgeError(
      'ERR_MUON_NODE_PROJECT',
      'Expected a project-relative service entry.'
    );
  const file = await realpath(resolve(root, entry));
  const path = relative(root, file);
  if (path === '..' || path.startsWith('..' + sep) || isAbsolute(path))
    throw createBridgeError(
      'ERR_MUON_NODE_PROJECT',
      'Service entry must be inside its project.'
    );
  // Forward just the default export so an entry exporting its own "then"
  // cannot be assimilated by the dynamic import.
  const source =
    'export { default } from ' + JSON.stringify(pathToFileURL(file).href);
  return await import(
    /* @vite-ignore */ 'data:text/javascript,' + encodeURIComponent(source)
  );
};
