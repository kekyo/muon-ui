// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { createHash } from "node:crypto";
import { relative } from "node:path";

/**
 * Import-side capability rule for Muon plugin virtual modules.
 */
export interface MuonCapabilityImportOptions {
  /**
   * Importer path globs relative to the bundler project root.
   */
  from: readonly string[];
  /**
   * Plugin function path globs allowed for matching importers.
   */
  allow: readonly string[];
}

/**
 * Capability import configuration shared by bundler integrations.
 */
export interface MuonCapabilityOptions {
  /**
   * Capability imports allowed by importer path.
   */
  imports?: readonly MuonCapabilityImportOptions[];
}

/**
 * Runtime capability policy generated for muon-core.
 */
export interface MuonCapabilityRuntimeEntry {
  /**
   * Opaque capability id embedded into generated virtual modules.
   */
  id: string;
  /**
   * Plugin function path globs allowed for this capability.
   */
  allow: readonly string[];
}

/**
 * Runtime plugin configuration generated for validate mode.
 */
export interface MuonCapabilityRuntimePluginConfig {
  /**
   * Plugin exposure mode.
   */
  mode: "validate";
  /**
   * Capability policies consumed by muon-core.
   */
  capabilities: readonly MuonCapabilityRuntimeEntry[];
}

/**
 * Resolved virtual module.
 */
export interface MuonResolvedCapabilityModule {
  /**
   * Bundler-internal virtual id.
   */
  id: string;
}

/**
 * Bundler-independent virtual module resolver.
 */
export interface MuonCapabilityModuleResolver {
  /**
   * Resolves a Muon virtual module specifier for one importer.
   */
  resolveId: (
    source: string,
    importer: string | undefined,
  ) => MuonResolvedCapabilityModule | undefined;
  /**
   * Loads the generated source for a resolved virtual module id.
   */
  load: (id: string) => string | undefined;
  /**
   * Returns runtime plugin config generated from all capability rules.
   */
  getRuntimePluginConfig: () => MuonCapabilityRuntimePluginConfig | undefined;
}

interface ParsedCapabilitySpecifier {
  moduleName: string;
  namespace: string;
}

interface ResolvedRule {
  index: number;
  id: string;
  rule: MuonCapabilityImportOptions;
  namespace: string;
  moduleName: string;
}

const virtualModulePrefix = "\0muon-capability:";
const moduleNamePattern =
  /^[A-Za-z_$][A-Za-z0-9_$]*:[A-Za-z_$][A-Za-z0-9_$]*(?:\.[A-Za-z_$][A-Za-z0-9_$]*)*$/u;
const jsIdentifierPattern = /^[A-Za-z_$][A-Za-z0-9_$]*$/u;

const normalizePath = (path: string): string =>
  (path.split("?")[0] ?? "").replaceAll("\\", "/");

const hashCapabilityRule = (
  index: number,
  rule: MuonCapabilityImportOptions,
): string =>
  `cap-${createHash("sha1")
    .update(
      JSON.stringify({
        index,
        from: [...rule.from],
        allow: [...rule.allow],
      }),
    )
    .digest("hex")
    .slice(0, 16)}`;

const parseCapabilitySpecifier = (
  source: string,
): ParsedCapabilitySpecifier | undefined => {
  if (!moduleNamePattern.test(source)) {
    return undefined;
  }
  const separator = source.indexOf(":");
  const namespace = `${source.slice(0, separator)}.${source.slice(separator + 1)}`;
  return {
    moduleName: source,
    namespace,
  };
};

const escapeRegExp = (source: string): string =>
  source.replace(/[\\^$+?.()|[\]{}]/gu, "\\$&");

const globToRegExp = (glob: string, separator: string): RegExp => {
  let source = "^";
  for (let index = 0; index < glob.length; index += 1) {
    const current = glob[index] ?? "";
    const next = glob[index + 1];
    if (current === "*" && next === "*") {
      source += ".*";
      index += 1;
      continue;
    }
    if (current === "*") {
      source += `[^${escapeRegExp(separator)}]*`;
      continue;
    }
    source += escapeRegExp(current);
  }
  source += "$";
  return new RegExp(source, "u");
};

const isGlobMatch = (glob: string, value: string, separator: string): boolean =>
  globToRegExp(glob, separator).test(value);

const toRootRelativeImporter = (root: string, importer: string): string =>
  normalizePath(relative(root, importer));

const hasNamespaceFunctionAllow = (
  namespace: string,
  allow: readonly string[],
): boolean =>
  allow.some((entry) => {
    if (entry.startsWith(`${namespace}.`)) {
      return true;
    }
    return isGlobMatch(entry, `${namespace}.placeholder`, ".");
  });

const getExactExportName = (
  namespace: string,
  functionPath: string,
): string | undefined => {
  const prefix = `${namespace}.`;
  if (!functionPath.startsWith(prefix)) {
    return undefined;
  }
  const name = functionPath.slice(prefix.length);
  if (!jsIdentifierPattern.test(name)) {
    return undefined;
  }
  return name;
};

const getExportedFunctions = (
  namespace: string,
  allow: readonly string[],
): [string, string][] =>
  allow.flatMap((functionPath) => {
    const exportName = getExactExportName(namespace, functionPath);
    return exportName === undefined ? [] : [[exportName, functionPath]];
  });

const createRuleRuntimeEntry = (
  rule: MuonCapabilityImportOptions,
  index: number,
): MuonCapabilityRuntimeEntry => ({
  id: hashCapabilityRule(index, rule),
  allow: [...rule.allow],
});

const createModuleVirtualId = (rule: ResolvedRule): string =>
  `${virtualModulePrefix}${rule.index}:${rule.moduleName}`;

const parseModuleVirtualId = (
  id: string,
): { index: number; moduleName: string } | undefined => {
  if (!id.startsWith(virtualModulePrefix)) {
    return undefined;
  }
  const payload = id.slice(virtualModulePrefix.length);
  const separator = payload.indexOf(":");
  if (separator <= 0) {
    return undefined;
  }
  const index = Number.parseInt(payload.slice(0, separator), 10);
  if (!Number.isInteger(index) || index < 0) {
    return undefined;
  }
  return {
    index,
    moduleName: payload.slice(separator + 1),
  };
};

const createExecutorSpawnExport = (
  capabilityId: string,
  functionPath: string,
): string => `export const spawn = async (options = {}) => {
  const source = await __muonCall(${JSON.stringify(capabilityId)}, ${JSON.stringify(functionPath)}, [JSON.stringify(options ?? {})]);
  return JSON.parse(source);
};`;

const createGenericFunctionExport = (
  exportName: string,
  capabilityId: string,
  functionPath: string,
): string => `export const ${exportName} = async (...args) =>
  await __muonCall(${JSON.stringify(capabilityId)}, ${JSON.stringify(functionPath)}, args);`;

const createModuleSource = (rule: ResolvedRule): string => {
  const exportedFunctions = getExportedFunctions(
    rule.namespace,
    rule.rule.allow,
  );
  if (exportedFunctions.length === 0) {
    throw new Error(
      `Muon capability import has no exact function exports: ${rule.moduleName}`,
    );
  }

  const exports = exportedFunctions.map(([exportName, functionPath]) =>
    rule.namespace === "muon.executor" && exportName === "spawn"
      ? createExecutorSpawnExport(rule.id, functionPath)
      : createGenericFunctionExport(exportName, rule.id, functionPath),
  );
  return `const __muonCall = globalThis.__muon_plugin_call;
if (typeof __muonCall !== "function") {
  throw new Error("Muon plugin capability bridge is not available.");
}

${exports.join("\n\n")}
`;
};

/**
 * Creates a capability virtual module resolver for a bundler adapter.
 *
 * @param root Bundler project root.
 * @param options Capability import options.
 */
export const createMuonCapabilityModuleResolver = (
  root: string,
  options: MuonCapabilityOptions | undefined,
): MuonCapabilityModuleResolver => {
  const rules = [...(options?.imports ?? [])];
  const runtimeEntries = rules.map((rule, index) =>
    createRuleRuntimeEntry(rule, index),
  );

  const resolveRule = (
    source: string,
    importer: string | undefined,
  ): ResolvedRule | undefined => {
    const parsed = parseCapabilitySpecifier(source);
    if (parsed === undefined) {
      return undefined;
    }
    const namespaceConfigured = rules.some((rule) =>
      hasNamespaceFunctionAllow(parsed.namespace, rule.allow),
    );
    if (!namespaceConfigured) {
      return undefined;
    }
    if (importer === undefined) {
      throw new Error(
        `Muon capability import is not allowed without an importer: ${source}`,
      );
    }

    const relativeImporter = toRootRelativeImporter(root, importer);
    for (let index = 0; index < rules.length; index += 1) {
      const rule = rules[index];
      if (
        rule === undefined ||
        !rule.from.some((pattern) =>
          isGlobMatch(normalizePath(pattern), relativeImporter, "/"),
        ) ||
        !hasNamespaceFunctionAllow(parsed.namespace, rule.allow)
      ) {
        continue;
      }
      return {
        index,
        id: runtimeEntries[index]?.id ?? hashCapabilityRule(index, rule),
        rule,
        namespace: parsed.namespace,
        moduleName: parsed.moduleName,
      };
    }

    throw new Error(
      `Muon capability import is not allowed: ${source} from ${relativeImporter}`,
    );
  };

  return {
    resolveId: (source, importer) => {
      const rule = resolveRule(source, importer);
      return rule === undefined
        ? undefined
        : {
            id: createModuleVirtualId(rule),
          };
    },
    load: (id) => {
      const parsed = parseModuleVirtualId(id);
      if (parsed === undefined) {
        return undefined;
      }
      const source = parseCapabilitySpecifier(parsed.moduleName);
      const rule = rules[parsed.index];
      const runtimeEntry = runtimeEntries[parsed.index];
      if (
        source === undefined ||
        rule === undefined ||
        runtimeEntry === undefined
      ) {
        return undefined;
      }
      return createModuleSource({
        index: parsed.index,
        id: runtimeEntry.id,
        rule,
        namespace: source.namespace,
        moduleName: source.moduleName,
      });
    },
    getRuntimePluginConfig: () =>
      runtimeEntries.length === 0
        ? undefined
        : {
            mode: "validate",
            capabilities: runtimeEntries,
          },
  };
};
