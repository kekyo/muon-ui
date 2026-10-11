import { execFile } from 'node:child_process';
import { cp, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { promisify } from 'node:util';
import { afterEach, expect, it } from 'vitest';
import { getNativeBuildEnvironment, stageSdkTarget } from '../src/index.js';

const execute = promisify(execFile);
const directories: string[] = [];
afterEach(async () => {
  for (const directory of directories.splice(0))
    await rm(directory, { recursive: true, force: true });
});

it('builds and invokes a C++ plugin using only a relocated SDK and pkg-config', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-sdk-'));
  directories.push(root);
  const sdk = join(root, 'sdk');
  await stageSdkTarget({
    target: 'linux-amd64',
    runtimeDirectory: resolve('.build/linux-amd64'),
    cardioDirectory: resolve('../deps/cardio'),
    sdkDirectory: resolve('.'),
  });
  for (const path of ['include', 'native', 'make'])
    await cp(resolve(path), join(sdk, path), { recursive: true });
  const environment = await getNativeBuildEnvironment({
    target: 'linux-amd64',
    sdkDirectory: sdk,
  });
  await writeFile(
    join(root, 'plugin.cpp'),
    `#include "muon_plugin_api.h"\n#include <cardio.h>\nstatic void add(muon_completion_func done, int32_t value) { try { (void)cardio::get_current_dispatcher(); } catch (...) {} const int32_t result = value + 2; done(&result, nullptr); }\nstatic const muon_type_descriptor i32 = {MUON_TYPE_I32, nullptr};\nstatic const muon_plugin_function_metadata f = {"add", (muon_native_function)add, {1, &i32, &i32}, nullptr};\nstatic const muon_plugin_function_metadata* fs[] = {&f, nullptr};\nstatic const muon_plugin_namespace ns = {"muon.example", nullptr, fs};\nstatic const muon_plugin_namespace* namespaces[] = {&ns, nullptr};\nstatic const muon_plugin_metadata metadata = {namespaces, nullptr, nullptr};\nextern "C" MUON_PLUGIN_EXPORT const muon_plugin_metadata* muon_init_plugin(const muon_plugin_init_context*) { return &metadata; }\n`
  );
  await writeFile(
    join(root, 'Makefile'),
    'SOURCES := plugin.cpp\nOUTPUT := example.so\ninclude $(MUON_SDK)/make/plugin.mk\n'
  );
  await execute('make', [], { cwd: root, env: environment });
  await writeFile(
    join(root, 'driver.c'),
    '#include "muon_plugin_api.h"\n#include <dlfcn.h>\nstatic int result;\nstatic void done(const void* value, const char* error) { result = error ? -1 : *(const int32_t*)value; }\nint main(int argc, char** argv) { (void)argc; void* h = dlopen(argv[1], RTLD_NOW); if (!h) return 10; muon_init_plugin_func init = (muon_init_plugin_func)dlsym(h, "muon_init_plugin"); if (!init) return 11; const muon_plugin_metadata* p = init(0); ((void (*)(muon_completion_func, int32_t))p->namespaces[0]->functions[0]->native_func)(done, 40); dlclose(h); return result == 42 ? 0 : 12; }\n'
  );
  await execute('gcc', [
    '-std=c99',
    '-I' + join(sdk, 'include'),
    join(root, 'driver.c'),
    '-ldl',
    '-o',
    join(root, 'driver'),
  ]);
  await execute(join(root, 'driver'), [join(root, 'example.so')], {
    env: {
      ...environment,
      LD_LIBRARY_PATH: join(sdk, 'native/linux-amd64/lib'),
    },
  });
  expect(
    (await readFile(join(root, 'example.so'))).subarray(0, 4).toString()
  ).toBe('\x7fELF');
});
