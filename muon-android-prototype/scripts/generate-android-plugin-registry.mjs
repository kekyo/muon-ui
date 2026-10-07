// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  existsSync,
  mkdirSync,
  readFileSync,
  writeFileSync,
} from 'node:fs';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { runScriptOnceToText } from 'funcity';

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

const cmakeTemplate = `# Generated from android-plugins.json. Do not edit.

set(MUON_ANDROID_PLUGIN_TARGETS)
{{for plugin plugins}}
add_library({{plugin.target}} SHARED
  "\${MUON_REPOSITORY_ROOT}/{{plugin.source}}"
  "\${MUON_CORE_ROOT}/include/muon_plugin_api.h"
  )
set_target_properties({{plugin.target}} PROPERTIES
  OUTPUT_NAME "{{plugin.outputName}}"
  )
target_compile_features({{plugin.target}} PRIVATE cxx_std_20)
target_compile_options({{plugin.target}} PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
  )
target_include_directories({{plugin.target}} PRIVATE
  "\${MUON_CORE_ROOT}/include"
  "\${MUON_CORE_ROOT}/src"
  )
target_link_libraries({{plugin.target}} PRIVATE
  muon_cardio
  )
target_link_options({{plugin.target}} PRIVATE
  -Wl,-z,max-page-size=16384
  )
list(APPEND MUON_ANDROID_PLUGIN_TARGETS {{plugin.target}})
{{end}}
`;

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
  join(outputRoot, 'muon_android_plugins.cmake'),
  await runScriptOnceToText(cmakeTemplate, {
    sourceId: 'android-plugin-build.fc',
    variables: new Map([
      [
        'plugins',
        registry.plugins.map((plugin, index) => ({
          target: `muon_android_packaged_plugin_${index}`,
          source: resolveRepositorySource(plugin.source),
          outputName: plugin.soname.slice(3, -3),
        })),
      ],
    ]),
  })
);

console.log(
  `Generated Android plugin registry with ${registry.plugins.length} plugin(s).`
);

writeIfChanged(
  join(
    projectRoot,
    'android',
    '.generated',
    'plugin-assets',
    'muon',
    'plugins.json'
  ),
  JSON.stringify(registry, null, 2) + '\n'
);
