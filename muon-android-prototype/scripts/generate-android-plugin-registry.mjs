// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

import { normalizeAndroidPluginRegistry } from './android-plugin-registry.mjs';

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repositoryRoot = resolve(projectRoot, '..');
const manifestPath = join(projectRoot, 'android-plugins.json');
const outputRoot = join(
  projectRoot,
  'android',
  '.generated',
  'plugin-registry'
);

const writeIfChanged = (filePath, contents) => {
  if (existsSync(filePath) && readFileSync(filePath, 'utf8') === contents) {
    return;
  }
  mkdirSync(dirname(filePath), { recursive: true });
  writeFileSync(filePath, contents);
};

const cppString = (value) => JSON.stringify(value);

const resolveRepositorySource = (source) => {
  const sourcePath = resolve(projectRoot, source);
  const repositoryRelativePath = relative(repositoryRoot, sourcePath);
  if (
    repositoryRelativePath === '..' ||
    repositoryRelativePath.startsWith(`..${sep}`) ||
    !existsSync(sourcePath)
  ) {
    throw new Error(
      `Plugin source is outside the repository or missing: ${source}`
    );
  }
  return repositoryRelativePath.split(sep).join('/');
};

const renderHeader =
  () => `/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "plugins/muon_plugin_runtime.h"

#include <string>
#include <vector>

/**
 * Creates the ordered packaged-plugin load table generated for Android.
 *
 * @param entries Receives entries in manifest order.
 * @param error_message Receives a policy validation error on failure.
 * @return true when every registry entry was created.
 */
bool CreateMuonAndroidPluginLoadEntries(
    std::vector<MuonPluginRuntimeLoadEntry>* entries,
    std::string* error_message);
`;

const renderSource = (registry) => {
  const blocks = registry.plugins.map((plugin) => {
    const allow = plugin.allow.map(cppString).join(', ');
    const config = plugin.config
      .map(
        (entry) =>
          `        {${cppString(entry.key)}, ${cppString(entry.value)}},`
      )
      .join('\n');
    return `  {
    MuonPluginRuntimeLoadEntry plugin;
    plugin.plugin = ${cppString(plugin.name)};
    plugin.has_library_locator = true;
    plugin.library_locator = ${cppString(plugin.soname)};
    const auto allow_patterns = std::vector<std::string>{${allow}};
    if (!CreateMuonPluginPolicy(
            allow_patterns, &plugin.plugin_policy, error_message)) {
      *error_message = plugin.plugin + ": " + *error_message;
      entries->clear();
      return false;
    }
    plugin.config = {
${config}
    };
    entries->push_back(std::move(plugin));
  }`;
  });
  return `/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

// Generated from android-plugins.json. Do not edit.

#include "muon_android_plugin_registry.h"

#include "plugins/muon_plugin_policy.h"

#include <utility>

bool CreateMuonAndroidPluginLoadEntries(
    std::vector<MuonPluginRuntimeLoadEntry>* entries,
    std::string* error_message) {
  if (entries == nullptr || error_message == nullptr) {
    return false;
  }
  entries->clear();
  error_message->clear();
${blocks.join('\n')}
  return true;
}
`;
};

const renderCmake = (registry) => {
  const blocks = registry.plugins.map((plugin, index) => {
    const target = `muon_android_packaged_plugin_${index}`;
    const outputName = plugin.soname.slice(3, -3);
    const repositorySource = resolveRepositorySource(plugin.source);
    return `add_library(${target} SHARED
  "\${MUON_REPOSITORY_ROOT}/${repositorySource}"
  "\${MUON_CORE_ROOT}/include/muon_plugin_api.h"
  )
set_target_properties(${target} PROPERTIES
  OUTPUT_NAME "${outputName}"
  )
target_compile_features(${target} PRIVATE cxx_std_20)
target_compile_options(${target} PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
  )
target_include_directories(${target} PRIVATE
  "\${MUON_CORE_ROOT}/include"
  "\${MUON_CORE_ROOT}/src"
  )
target_link_libraries(${target} PRIVATE
  muon_cardio
  )
target_link_options(${target} PRIVATE
  -Wl,-z,max-page-size=16384
  )
list(APPEND MUON_ANDROID_PLUGIN_TARGETS ${target})`;
  });
  return `# Generated from android-plugins.json. Do not edit.

set(MUON_ANDROID_PLUGIN_TARGETS)
${blocks.join('\n\n')}
`;
};

const input = JSON.parse(readFileSync(manifestPath, 'utf8'));
const registry = normalizeAndroidPluginRegistry(input);
for (const plugin of registry.plugins) {
  resolveRepositorySource(plugin.source);
}

writeIfChanged(
  join(outputRoot, 'android-plugin-registry.json'),
  `${JSON.stringify(registry, null, 2)}\n`
);
writeIfChanged(
  join(outputRoot, 'muon_android_plugin_registry.h'),
  renderHeader()
);
writeIfChanged(
  join(outputRoot, 'muon_android_plugin_registry.cpp'),
  renderSource(registry)
);
writeIfChanged(
  join(outputRoot, 'muon_android_plugins.cmake'),
  renderCmake(registry)
);

console.log(
  `Generated Android plugin registry with ${registry.plugins.length} plugin(s).`
);
