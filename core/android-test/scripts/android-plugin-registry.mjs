// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

const androidPluginAbis = ['x86_64', 'arm64-v8a'];
const expectedMachines = {
  x86_64: 'Advanced Micro Devices X86-64',
  'arm64-v8a': 'AArch64',
};

const isRecord = (value) =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

const requireRecord = (value, location) => {
  if (!isRecord(value)) {
    throw new Error(`${location} must be an object`);
  }
  return value;
};

const requireString = (value, location) => {
  if (typeof value !== 'string' || value.length === 0 || value.includes('\0')) {
    throw new Error(`${location} must be a non-empty string without NUL`);
  }
  return value;
};

const rejectUnknownFields = (record, location, allowed) => {
  for (const key of Object.keys(record)) {
    if (!allowed.has(key)) {
      throw new Error(`${location}.${key} is not supported`);
    }
  }
};

/**
 * Validates and canonicalizes the muon-owned Android plugin build manifest.
 *
 * @param {unknown} input Untrusted parsed manifest contents.
 * @returns {import('./android-plugin-registry.mjs').NormalizedAndroidPluginRegistry}
 *   A registry whose plugin order matches the input and whose configuration is
 *   sorted by key.
 */
export const normalizeAndroidPluginRegistry = (input) => {
  const registry = requireRecord(input, 'registry');
  rejectUnknownFields(
    registry,
    'registry',
    new Set(['schemaVersion', 'plugins'])
  );
  if (registry.schemaVersion !== 1) {
    throw new Error('registry.schemaVersion must be 1');
  }
  if (!Array.isArray(registry.plugins)) {
    throw new Error('registry.plugins must be an array');
  }

  const pluginNames = new Set();
  const pluginSonames = new Set();
  const plugins = registry.plugins.map((inputPlugin, index) => {
    const location = `registry.plugins[${index}]`;
    const plugin = requireRecord(inputPlugin, location);
    for (const desktopField of ['path', 'signature', 'salt']) {
      if (Object.hasOwn(plugin, desktopField)) {
        throw new Error(
          `${location}.${desktopField} is unavailable on Android`
        );
      }
    }
    rejectUnknownFields(
      plugin,
      location,
      new Set(['name', 'soname', 'source', 'artifacts', 'allow', 'config'])
    );

    const name = requireString(plugin.name, `${location}.name`);
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name) || name === 'internal') {
      throw new Error(`${location}.name is not a valid logical plugin name`);
    }
    if (pluginNames.has(name)) {
      throw new Error(`duplicate plugin name: ${name}`);
    }
    pluginNames.add(name);

    const soname = requireString(plugin.soname, `${location}.soname`);
    if (!/^lib[A-Za-z0-9_][A-Za-z0-9_.+-]*\.so$/.test(soname)) {
      throw new Error(`${location}.soname must use the lib<name>.so form`);
    }
    if (pluginSonames.has(soname)) {
      throw new Error(`duplicate plugin soname: ${soname}`);
    }
    pluginSonames.add(soname);

    const source = requireString(plugin.source, `${location}.source`);
    if (!source.endsWith('.cpp')) {
      throw new Error(`${location}.source must name a C++ source file`);
    }

    const inputArtifacts = requireRecord(
      plugin.artifacts,
      `${location}.artifacts`
    );
    rejectUnknownFields(
      inputArtifacts,
      `${location}.artifacts`,
      new Set(androidPluginAbis)
    );
    const artifacts = {};
    for (const abi of androidPluginAbis) {
      const artifact = requireString(
        inputArtifacts[abi],
        `${location}.artifacts.${abi}`
      );
      const expected = `lib/${abi}/${soname}`;
      if (artifact !== expected) {
        throw new Error(`${location}.artifacts.${abi} must be ${expected}`);
      }
      artifacts[abi] = artifact;
    }

    if (!Array.isArray(plugin.allow) || plugin.allow.length === 0) {
      throw new Error(`${location}.allow must not be empty`);
    }
    const allow = plugin.allow.map((pattern, patternIndex) =>
      requireString(pattern, `${location}.allow[${patternIndex}]`)
    );
    if (new Set(allow).size !== allow.length) {
      throw new Error(`${location}.allow contains a duplicate pattern`);
    }

    const inputConfig = requireRecord(plugin.config, `${location}.config`);
    const config = Object.entries(inputConfig)
      .map(([key, value]) => ({
        key: requireString(key, `${location}.config key`),
        value: requireString(value, `${location}.config.${key}`),
      }))
      .sort((left, right) => left.key.localeCompare(right.key, 'en'));

    return {
      name,
      soname,
      source,
      artifacts,
      allow,
      config,
    };
  });

  return { schemaVersion: 1, plugins };
};

/**
 * Verifies all ABI artifacts described by a normalized Android registry.
 *
 * @param {import('./android-plugin-registry.mjs').NormalizedAndroidPluginRegistry}
 *   registry Normalized registry to inspect.
 * @param {import('./android-plugin-registry.mjs').AndroidPluginArtifactInspector}
 *   inspect Synchronous artifact inspector supplied by the package verifier.
 * @returns {void}
 */
export const validateAndroidPluginArtifacts = (registry, inspect) => {
  for (const plugin of registry.plugins) {
    for (const abi of androidPluginAbis) {
      let inspection;
      try {
        inspection = inspect({
          abi,
          artifactPath: plugin.artifacts[abi],
          plugin,
        });
      } catch (error) {
        const diagnostic =
          error instanceof Error ? error.message : String(error);
        throw new Error(`${plugin.name} ${abi} artifact: ${diagnostic}`);
      }
      const expectedMachine = expectedMachines[abi];
      if (inspection.machine !== expectedMachine) {
        throw new Error(
          `${plugin.name} ${abi} ELF machine must be ${expectedMachine}, got ${inspection.machine}`
        );
      }
      if (inspection.soname !== plugin.soname) {
        throw new Error(
          `${plugin.name} ${abi} ELF soname must be ${plugin.soname}, got ${inspection.soname}`
        );
      }
      if (!inspection.exportedSymbols.includes('muon_init_plugin')) {
        throw new Error(
          `${plugin.name} ${abi} artifact does not export muon_init_plugin`
        );
      }
      if (
        inspection.loadAlignments.length === 0 ||
        inspection.loadAlignments.some((alignment) => alignment !== 0x4000)
      ) {
        throw new Error(`${plugin.name} ${abi} LOAD alignment must be 0x4000`);
      }
    }
  }
};
