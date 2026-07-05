// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

/**
 * Asset layout derived from Vite's resolved base URL for packaged Muon apps.
 */
export interface MuonVitePackagedAssetOptions {
  /** ZIP entry prefix used for copied Vite build output. */
  assetPrefix: string;
  /** Generated browser start page when Vite base needs a packaged path. */
  browserStartPage: string | undefined;
}

/**
 * Returns the package-local asset base path represented by a Vite base URL.
 */
export const resolveVitePackagedAssetBasePath = (
  base: string,
): string | undefined => {
  const normalizedBase = base.trim();
  if (
    normalizedBase === "" ||
    normalizedBase === "/" ||
    normalizedBase === "./" ||
    normalizedBase.startsWith("//") ||
    /^[a-zA-Z][a-zA-Z\d+.-]*:/.test(normalizedBase) ||
    !normalizedBase.startsWith("/")
  ) {
    return undefined;
  }

  const normalizedPath = normalizedBase.split(/[?#]/, 1)[0] ?? "";
  const segments = normalizedPath
    .split("/")
    .filter((segment) => segment.length > 0);
  return segments.length > 0 ? segments.join("/") : undefined;
};

/**
 * Creates Muon asset options for packaging Vite build output.
 */
export const createVitePackagedAssetOptions = (
  base: string,
): MuonVitePackagedAssetOptions => {
  const packagedAssetBasePath = resolveVitePackagedAssetBasePath(base);
  const assetPrefix =
    packagedAssetBasePath === undefined
      ? "main"
      : `main/${packagedAssetBasePath}`;
  return {
    assetPrefix,
    browserStartPage:
      packagedAssetBasePath === undefined
        ? undefined
        : `asset://${assetPrefix}/index.html`,
  };
};
