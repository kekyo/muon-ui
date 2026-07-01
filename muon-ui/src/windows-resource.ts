// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { constants } from "node:fs";
import {
  access,
  chmod,
  copyFile,
  mkdtemp,
  readFile,
  rm,
  stat,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, extname, isAbsolute, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { parse } from "json5";

import { runMuonPrepareResourceUpdate } from "./prepare.js";
import type { MuonWindowsResourceOptions } from "./vite.js";
import {
  createNormalizedMuonIconPngData,
  createWindowsIconFromPngFile,
} from "./windows-icon.js";

export type { MuonWindowsResourceOptions } from "./vite.js";

const defaultConfigFileNames = ["muon.json5", "muon.jsonc", "muon.json"];
const defaultLanguage = 1033;
const defaultCodePage = 1200;
const moduleDirectory =
  typeof __dirname === "string"
    ? __dirname
    : dirname(fileURLToPath(import.meta.url));

type JsonObject = Record<string, unknown>;

interface WindowsResourceSource {
  options: MuonWindowsResourceOptions;
  directory: string;
}

interface WindowsResourceDefaults {
  productName: string;
  fileDescription: string;
  companyName: string;
  version: string;
  copyright: string | undefined;
}

interface ResolveWindowsResourceInput {
  root: string;
  packageDirectory: string;
  packageJson: JsonObject;
  muonConfig: JsonObject;
  muonConfigDirectory: string;
  options: MuonWindowsResourceOptions | undefined;
  defaults: WindowsResourceDefaults;
}

/**
 * Resolved Windows resource metadata used by build and pack outputs.
 */
export interface ResolvedMuonWindowsResource {
  /** Resolved `.png` icon file path, if one is available. */
  iconPath: string | undefined;
  /** Product name. */
  productName: string;
  /** File description. */
  fileDescription: string;
  /** Company name. */
  companyName: string;
  /** Original version string. */
  version: string;
  /** Four-part fixed version string. */
  fixedVersion: string;
  /** Legal copyright, if supplied. */
  copyright: string | undefined;
  /** Windows resource language identifier. */
  language: number;
  /** Windows version resource code page. */
  codePage: number;
}

/**
 * Muon config file content and directory used for resource option resolution.
 */
export interface MuonWindowsResourceConfigSource {
  /** Parsed Muon config object. */
  config: JsonObject;
  /** Directory that relative config paths are resolved from. */
  directory: string;
}

/**
 * Reads the Muon config file using the same default file names as muon build.
 */
export const readMuonConfigForWindowsResource = async (
  root: string,
  configPath: string | undefined,
): Promise<MuonWindowsResourceConfigSource> => {
  const resolvedConfigPath = await resolveConfigPath(root, configPath);
  if (resolvedConfigPath === undefined) {
    return {
      config: {},
      directory: root,
    };
  }
  return {
    config: await readJsonObjectFile(resolvedConfigPath, "Muon config file"),
    directory: dirname(resolvedConfigPath),
  };
};

/**
 * Merges Windows resource options while preserving field-level fallback.
 */
export const mergeMuonWindowsResourceOptions = (
  primary: MuonWindowsResourceOptions | undefined,
  fallback: MuonWindowsResourceOptions | undefined,
): MuonWindowsResourceOptions | undefined => {
  if (primary === undefined) {
    return fallback;
  }
  if (fallback === undefined) {
    return primary;
  }

  const merged: MuonWindowsResourceOptions = { ...fallback };
  if (primary.iconPath !== undefined) {
    merged.iconPath = primary.iconPath;
  }
  if (primary.productName !== undefined) {
    merged.productName = primary.productName;
  }
  if (primary.fileDescription !== undefined) {
    merged.fileDescription = primary.fileDescription;
  }
  if (primary.companyName !== undefined) {
    merged.companyName = primary.companyName;
  }
  if (primary.version !== undefined) {
    merged.version = primary.version;
  }
  if (primary.copyright !== undefined) {
    merged.copyright = primary.copyright;
  }
  if (primary.language !== undefined) {
    merged.language = primary.language;
  }
  if (primary.codePage !== undefined) {
    merged.codePage = primary.codePage;
  }
  return merged;
};

/**
 * Resolves Windows resource metadata from options, config files, and package metadata.
 */
export const resolveMuonWindowsResource = async (
  input: ResolveWindowsResourceInput,
): Promise<ResolvedMuonWindowsResource> => {
  const projectJson = await readProjectJson(input.root);
  const sources = [
    createOptionsSource(input.options, input.root, "windowsResource"),
    readConfigWindowsResourceSource(
      input.muonConfig,
      input.muonConfigDirectory,
      "muon.json",
    ),
    createProjectSource(projectJson, input.root),
    createPackageSource(input.packageJson, input.root),
  ].filter((source): source is WindowsResourceSource => source !== undefined);

  const productName = resolveStringField(
    sources,
    "productName",
    input.defaults.productName,
  );
  const fileDescription = resolveStringField(
    sources,
    "fileDescription",
    input.defaults.fileDescription,
  );
  const companyName = resolveStringField(
    sources,
    "companyName",
    input.defaults.companyName,
  );
  const version = resolveStringField(
    sources,
    "version",
    input.defaults.version,
  );
  const copyright = resolveOptionalStringField(
    sources,
    "copyright",
    input.defaults.copyright,
  );
  const language = resolveNumericField(sources, "language", defaultLanguage);
  const codePage = resolveNumericField(sources, "codePage", defaultCodePage);
  const fixedVersion = normalizeWindowsVersion(version);
  const iconPath = await resolveIconPath(
    sources,
    await resolveDefaultWindowsIcon(input.packageDirectory),
  );

  return {
    iconPath,
    productName,
    fileDescription,
    companyName,
    version,
    fixedVersion,
    copyright,
    language,
    codePage,
  };
};

/**
 * Removes build-only Windows resource settings from runtime config.
 */
export const stripBuildOnlyWindowsResourceConfig = (
  sourceConfig: JsonObject,
): JsonObject => {
  const sourceWindows = sourceConfig.windows;
  if (!isJsonObject(sourceWindows)) {
    return sourceConfig;
  }

  const windowsConfig: JsonObject = {};
  for (const [key, value] of Object.entries(sourceWindows)) {
    if (key !== "resource") {
      windowsConfig[key] = value;
    }
  }

  const output: JsonObject = {};
  for (const [key, value] of Object.entries(sourceConfig)) {
    if (key !== "windows") {
      output[key] = value;
    }
  }
  if (Object.keys(windowsConfig).length > 0) {
    output.windows = windowsConfig;
  }
  return output;
};

/**
 * Returns true when the file has a Windows PE header.
 */
export const isWindowsPeExecutable = async (path: string): Promise<boolean> => {
  const content = await readFile(path);
  if (content.length < 0x40 || content.toString("ascii", 0, 2) !== "MZ") {
    return false;
  }
  const peOffset = content.readUInt32LE(0x3c);
  return (
    peOffset + 4 <= content.length &&
    content.toString("ascii", peOffset, peOffset + 4) === "PE\0\0"
  );
};

/**
 * Applies resolved resource metadata to a Windows PE executable in place.
 */
export const updateWindowsPeResources = async (input: {
  executablePath: string;
  resource: ResolvedMuonWindowsResource;
  environment: NodeJS.ProcessEnv;
  cwd: string;
}): Promise<boolean> => {
  if (!(await isWindowsPeExecutable(input.executablePath))) {
    return false;
  }

  const mode = (await stat(input.executablePath)).mode & 0o777;
  const tempDirectory = await mkdtemp(join(tmpdir(), "muon-windows-resource-"));
  const updatesJsonPath = join(tempDirectory, "updates.json");
  const outputPath = join(tempDirectory, "output.exe");
  const iconPath =
    input.resource.iconPath === undefined
      ? undefined
      : join(tempDirectory, "icon.ico");
  try {
    if (input.resource.iconPath !== undefined && iconPath !== undefined) {
      await createWindowsIconFromPngFile(input.resource.iconPath, iconPath);
    }
    await writeFile(
      updatesJsonPath,
      `${JSON.stringify(createEngraverUpdate(input.resource, iconPath), null, 2)}\n`,
    );
    await runMuonPrepareResourceUpdate({
      inputPath: input.executablePath,
      updatesJsonPath,
      outputPath,
      quiet: true,
      prepareExecutablePath: undefined,
      environment: input.environment,
      cwd: input.cwd,
    });
    await copyFile(outputPath, input.executablePath);
    await chmod(input.executablePath, mode === 0 ? 0o755 : mode);
    return true;
  } finally {
    await rm(tempDirectory, { recursive: true, force: true });
  }
};

const createEngraverUpdate = (
  resource: ResolvedMuonWindowsResource,
  iconPath: string | undefined,
): JsonObject => {
  const strings: JsonObject = {
    CompanyName: resource.companyName,
    FileDescription: resource.fileDescription,
    FileVersion: resource.version,
    ProductName: resource.productName,
    ProductVersion: resource.version,
  };
  if (resource.copyright !== undefined) {
    strings.LegalCopyright = resource.copyright;
  }

  return {
    version: {
      language: resource.language,
      codePage: resource.codePage,
      fixed: {
        fileVersion: resource.fixedVersion,
        productVersion: resource.fixedVersion,
        fileOS: "windows32",
        fileType: "app",
      },
      strings,
    },
    icons:
      iconPath === undefined
        ? []
        : [
            {
              id: 1,
              language: resource.language,
              path: iconPath,
            },
          ],
  };
};

const resolveConfigPath = async (
  root: string,
  configPath: string | undefined,
): Promise<string | undefined> => {
  if (configPath !== undefined) {
    const resolvedPath = resolve(root, configPath);
    if (await fileExists(resolvedPath)) {
      return resolvedPath;
    }
    throw new Error(`Muon config file does not exist: ${resolvedPath}`);
  }

  for (const fileName of defaultConfigFileNames) {
    const candidatePath = join(root, fileName);
    if (await fileExists(candidatePath)) {
      return candidatePath;
    }
  }
  return undefined;
};

const readProjectJson = async (root: string): Promise<JsonObject> => {
  const projectJsonPath = join(root, "project.json");
  if (!(await fileExists(projectJsonPath))) {
    return {};
  }
  return await readJsonObjectFile(projectJsonPath, "project.json");
};

const readJsonObjectFile = async (
  filePath: string,
  label: string,
): Promise<JsonObject> => {
  const parsed = parse(await readFile(filePath, "utf8"));
  if (!isJsonObject(parsed)) {
    throw new Error(`${label} must contain a JSON object: ${filePath}`);
  }
  return parsed;
};

const createOptionsSource = (
  options: MuonWindowsResourceOptions | undefined,
  directory: string,
  label: string,
): WindowsResourceSource | undefined => {
  if (options === undefined) {
    return undefined;
  }
  return {
    options: validateWindowsResourceOptions(options, label),
    directory,
  };
};

const readConfigWindowsResourceSource = (
  config: JsonObject,
  directory: string,
  label: string,
): WindowsResourceSource | undefined => {
  const windows = config.windows;
  if (windows === undefined) {
    return undefined;
  }
  if (!isJsonObject(windows)) {
    throw new Error(`${label} windows must be an object when present.`);
  }
  const resource = windows.resource;
  if (resource === undefined) {
    return undefined;
  }
  return createOptionsSource(
    validateWindowsResourceOptions(resource, `${label} windows.resource`),
    directory,
    `${label} windows.resource`,
  );
};

const createProjectSource = (
  projectJson: JsonObject,
  directory: string,
): WindowsResourceSource | undefined => {
  const topLevel: MuonWindowsResourceOptions = {};
  copyStringField(projectJson, "iconPath", topLevel, "iconPath");
  copyStringField(projectJson, "productName", topLevel, "productName");
  copyStringField(projectJson, "name", topLevel, "productName");
  copyStringField(projectJson, "fileDescription", topLevel, "fileDescription");
  copyStringField(projectJson, "description", topLevel, "fileDescription");
  copyStringField(projectJson, "companyName", topLevel, "companyName");
  copyStringField(projectJson, "version", topLevel, "version");
  copyStringField(projectJson, "copyright", topLevel, "copyright");
  const author = stringifyAuthor(projectJson.author);
  if (author !== undefined && topLevel.companyName === undefined) {
    topLevel.companyName = author;
  }

  const configSource = readConfigWindowsResourceSource(
    projectJson,
    directory,
    "project.json",
  );
  const options = mergeMuonWindowsResourceOptions(
    configSource?.options,
    topLevel,
  );
  if (options === undefined || Object.keys(options).length === 0) {
    return undefined;
  }
  return {
    options,
    directory,
  };
};

const createPackageSource = (
  packageJson: JsonObject,
  directory: string,
): WindowsResourceSource | undefined => {
  const options: MuonWindowsResourceOptions = {};
  copyStringField(packageJson, "name", options, "productName");
  copyStringField(packageJson, "description", options, "fileDescription");
  copyStringField(packageJson, "version", options, "version");
  copyStringField(packageJson, "copyright", options, "copyright");
  const author = stringifyAuthor(packageJson.author);
  if (author !== undefined) {
    options.companyName = author;
  }
  return Object.keys(options).length === 0
    ? undefined
    : {
        options,
        directory,
      };
};

const copyStringField = (
  source: JsonObject,
  sourceKey: string,
  target: MuonWindowsResourceOptions,
  targetKey: keyof Pick<
    MuonWindowsResourceOptions,
    | "iconPath"
    | "productName"
    | "fileDescription"
    | "companyName"
    | "version"
    | "copyright"
  >,
): void => {
  const value = source[sourceKey];
  if (typeof value === "string" && value.trim() !== "") {
    target[targetKey] = value.trim();
  }
};

const validateWindowsResourceOptions = (
  value: unknown,
  label: string,
): MuonWindowsResourceOptions => {
  if (!isJsonObject(value)) {
    throw new Error(`${label} must be an object.`);
  }

  const output: MuonWindowsResourceOptions = {};
  copyOptionalStringResourceField(value, "iconPath", output);
  copyOptionalStringResourceField(value, "productName", output);
  copyOptionalStringResourceField(value, "fileDescription", output);
  copyOptionalStringResourceField(value, "companyName", output);
  copyOptionalStringResourceField(value, "version", output);
  copyOptionalStringResourceField(value, "copyright", output);
  copyOptionalNumberResourceField(value, "language", output, label);
  copyOptionalNumberResourceField(value, "codePage", output, label);
  return output;
};

const copyOptionalStringResourceField = (
  source: JsonObject,
  key: keyof Pick<
    MuonWindowsResourceOptions,
    | "iconPath"
    | "productName"
    | "fileDescription"
    | "companyName"
    | "version"
    | "copyright"
  >,
  target: MuonWindowsResourceOptions,
): void => {
  const value = source[key];
  if (value === undefined) {
    return;
  }
  if (typeof value !== "string") {
    throw new Error(`${key} must be a string when present.`);
  }
  if (value.trim() !== "") {
    target[key] = value.trim();
  }
};

const copyOptionalNumberResourceField = (
  source: JsonObject,
  key: keyof Pick<MuonWindowsResourceOptions, "language" | "codePage">,
  target: MuonWindowsResourceOptions,
  label: string,
): void => {
  const value = source[key];
  if (value === undefined) {
    return;
  }
  if (
    typeof value !== "number" ||
    !Number.isInteger(value) ||
    value < 0 ||
    value > 0xffff
  ) {
    throw new Error(`${label} ${key} must be an integer from 0 to 65535.`);
  }
  target[key] = value;
};

const resolveStringField = (
  sources: readonly WindowsResourceSource[],
  field: keyof Pick<
    MuonWindowsResourceOptions,
    "productName" | "fileDescription" | "companyName" | "version"
  >,
  fallback: string,
): string => {
  return resolveOptionalStringField(sources, field, fallback) ?? fallback;
};

const resolveOptionalStringField = (
  sources: readonly WindowsResourceSource[],
  field: keyof Pick<
    MuonWindowsResourceOptions,
    "productName" | "fileDescription" | "companyName" | "version" | "copyright"
  >,
  fallback: string | undefined,
): string | undefined => {
  for (const source of sources) {
    const value = source.options[field];
    if (value !== undefined && value.trim() !== "") {
      return value.trim();
    }
  }
  return fallback;
};

const resolveNumericField = (
  sources: readonly WindowsResourceSource[],
  field: keyof Pick<MuonWindowsResourceOptions, "language" | "codePage">,
  fallback: number,
): number => {
  for (const source of sources) {
    const value = source.options[field];
    if (value !== undefined) {
      return value;
    }
  }
  return fallback;
};

const resolveIconPath = async (
  sources: readonly WindowsResourceSource[],
  defaultIconPath: string | undefined,
): Promise<string | undefined> => {
  for (const source of sources) {
    const value = source.options.iconPath;
    if (value !== undefined && value.trim() !== "") {
      const iconPath = resolveResourcePath(source.directory, value);
      await assertIconPath(iconPath, true);
      return iconPath;
    }
  }
  if (defaultIconPath !== undefined && (await fileExists(defaultIconPath))) {
    await assertIconPath(defaultIconPath, false);
    return defaultIconPath;
  }
  return undefined;
};

const assertIconPath = async (
  iconPath: string,
  required: boolean,
): Promise<void> => {
  if (extname(iconPath).toLowerCase() !== ".png") {
    throw new Error(`Windows resource icon must be a .png file: ${iconPath}`);
  }
  if (required && !(await fileExists(iconPath))) {
    throw new Error(`Windows resource icon does not exist: ${iconPath}`);
  }
  if (await fileExists(iconPath)) {
    await createNormalizedMuonIconPngData(await readFile(iconPath), iconPath);
  }
};

const resolveDefaultWindowsIcon = async (
  packageDirectory: string,
): Promise<string | undefined> => {
  const candidates = [
    join(resolve(packageDirectory), "native", "muon-bootstrap.png"),
    join(moduleDirectory, "native", "muon-bootstrap.png"),
    join(moduleDirectory, "..", "dist", "native", "muon-bootstrap.png"),
    join(moduleDirectory, "..", "..", "images", "muon-bootstrap-256.png"),
  ];
  for (const candidate of candidates) {
    if (await fileExists(candidate)) {
      return candidate;
    }
  }
  return undefined;
};

const resolveResourcePath = (directory: string, path: string): string => {
  return isAbsolute(path) ? path : resolve(directory, path);
};

const normalizeWindowsVersion = (version: string): string => {
  const normalized = version.trim();
  const match = /^(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:\.(\d+))?(?:$|[-+])/.exec(
    normalized,
  );
  if (match === null) {
    throw new Error(
      `Windows resource version must start with numeric parts: ${version}`,
    );
  }
  const values = [match[1], match[2], match[3], match[4]].map((value) =>
    value === undefined ? 0 : Number.parseInt(value, 10),
  );
  for (const value of values) {
    if (!Number.isInteger(value) || value < 0 || value > 0xffff) {
      throw new Error(
        `Windows resource version part must be from 0 to 65535: ${version}`,
      );
    }
  }
  return values.join(".");
};

const stringifyAuthor = (value: unknown): string | undefined => {
  if (typeof value === "string" && value.trim() !== "") {
    return value.trim();
  }
  if (!isJsonObject(value) || typeof value.name !== "string") {
    return undefined;
  }
  const name = value.name.trim();
  if (name === "") {
    return undefined;
  }
  return value.email === undefined || typeof value.email !== "string"
    ? name
    : `${name} <${value.email}>`;
};

const fileExists = async (path: string): Promise<boolean> => {
  try {
    await access(path, constants.F_OK);
    return true;
  } catch {
    return false;
  }
};

const isJsonObject = (value: unknown): value is JsonObject =>
  typeof value === "object" && value !== null && !Array.isArray(value);
