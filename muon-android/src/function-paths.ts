// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

/** Browser functions implemented by Android. */
export const browserFunctionNames = [
  'reload',
  'toggleFullscreen',
  'enterFullscreen',
  'exitFullscreen',
  'zoomIn',
  'zoomOut',
  'resetZoom',
  'close',
] as const;

/** Environment functions implemented by Android. */
export const environmentFunctionNames = [
  'getVariables',
  'getConfigValues',
  'getProcessId',
  'getRuntimeInfo',
] as const;

/** Filesystem functions implemented by Android. */
export const filesystemFunctionNames = [
  'readFile',
  'writeFile',
  'readTextFile',
  'writeTextFile',
  'stat',
  'lstat',
  'exists',
  'access',
  'readdir',
  'mkdir',
  'rm',
  'unlink',
  'rmdir',
  'rename',
  'copyFile',
  'appendFile',
  'appendTextFile',
  'truncate',
  'realpath',
  'readlink',
  'symlink',
  'watch',
] as const;

/** Fully-qualified built-in function paths implemented by the Android backend. */
export const muonAndroidBuiltinFunctionPaths = [
  ...browserFunctionNames.map((name) => `muon.browser.${name}` as const),
  ...environmentFunctionNames.map(
    (name) => `muon.environments.${name}` as const
  ),
  ...filesystemFunctionNames.map((name) => `muon.fs.${name}` as const),
] as const;

/** A built-in function path available on Android. */
export type MuonAndroidBuiltinFunctionPath =
  (typeof muonAndroidBuiltinFunctionPaths)[number];

const escapeRegularExpression = (source: string): string =>
  source.replace(/[\\^$+?.()|[\]{}]/gu, '\\$&');

const createFunctionAllowExpression = (allow: string): RegExp => {
  let source = '^';
  for (let index = 0; index < allow.length; index += 1) {
    const character = allow[index] ?? '';
    const next = allow[index + 1];
    if (character === '*' && next === '*') {
      source += '.*';
      index += 1;
    } else if (character === '*') {
      source += '[^.]*';
    } else {
      source += escapeRegularExpression(character);
    }
  }
  source += '$';
  return new RegExp(source, 'u');
};

/**
 * Expands Android capability allow patterns into available function paths.
 *
 * @param allows - Exact paths or glob patterns requested by a validate build.
 * @returns Available Android paths in stable metadata order.
 * @remarks An exact unsupported path, or a pattern matching no Android
 * functions, fails during the build instead of producing a runtime stub.
 */
export const expandMuonAndroidFunctionAllows = (
  allows: readonly string[]
): readonly MuonAndroidBuiltinFunctionPath[] => {
  const selected = new Set<MuonAndroidBuiltinFunctionPath>();
  for (const allow of allows) {
    if (!allow.includes('*')) {
      const functionPath = muonAndroidBuiltinFunctionPaths.find(
        (candidate) => candidate === allow
      );
      if (functionPath === undefined) {
        throw new Error(`Muon function is unavailable for Android: ${allow}`);
      }
      selected.add(functionPath);
      continue;
    }

    const expression = createFunctionAllowExpression(allow);
    const matches = muonAndroidBuiltinFunctionPaths.filter((functionPath) =>
      expression.test(functionPath)
    );
    if (matches.length === 0) {
      throw new Error(
        `Muon function pattern is unavailable for Android: ${allow}`
      );
    }
    for (const functionPath of matches) {
      selected.add(functionPath);
    }
  }
  return muonAndroidBuiltinFunctionPaths.filter((functionPath) =>
    selected.has(functionPath)
  );
};
