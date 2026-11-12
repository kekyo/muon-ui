import { execFile } from 'node:child_process';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import vm from 'node:vm';
import { expect, it } from 'vitest';
import type { ServiceRuntime } from '../src/service-runtime.ts';

it('bundles a typed service with only host callbacks and timer primitives', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'muon-service-runtime-'));
  try {
    const output = join(directory, 'service.js');
    const header = join(directory, 'service.h');
    const result = await promisify(execFile)(process.execPath, [
      fileURLToPath(
        new URL('../scripts/build-service-runtime-fixture.mjs', import.meta.url)
      ),
      output,
      header,
    ]);
    expect(result.stderr).toBe('');
    let timer!: () => void, entered!: () => void;
    const scheduled = new Promise<void>((resolve) => {
      entered = resolve;
    });
    const events: unknown[] = [];
    const context = vm.createContext({
      __muonServiceCallback: async (
        _call: string,
        _token: string,
        args: readonly unknown[]
      ) => ({ status: 'ok', value: args[0] + ':returned' }),
      __muonServiceEmit: (event: string, value: unknown) =>
        events.push([event, value]),
      setTimeout: (callback: () => void) => {
        timer = callback;
        entered();
        return 1;
      },
    });
    await writeFile(
      join(directory, 'driver.cpp'),
      '#include "service.h"\n#include <iostream>\nint main() { std::cout << kMuonQuickJsServiceFixture; }\n'
    );
    await promisify(execFile)('g++', [
      '-std=c++20',
      '-Wall',
      '-Wextra',
      '-Wpedantic',
      '-Werror',
      join(directory, 'driver.cpp'),
      '-o',
      join(directory, 'driver'),
    ]);
    const emitted = await promisify(execFile)(join(directory, 'driver'));
    vm.runInContext(emitted.stdout, context);
    const runtime = context.__muonServiceRuntime as ServiceRuntime;
    await runtime.start({
      id: 'fixture',
      generation: '1',
      config: {},
      dataDirectory: '/private/fixture',
    });
    expect(await runtime.invoke('1', 'echo', ['日本語\0😀'])).toEqual({
      status: 'ok',
      value: '日本語\0😀',
    });
    expect(
      await runtime.invoke('2', 'echoInt64', ['-9223372036854775808'])
    ).toEqual({ status: 'ok', value: '-9223372036854775808' });
    expect(
      await runtime.invoke('3', 'echoUint64', ['18446744073709551615'])
    ).toEqual({ status: 'ok', value: '18446744073709551615' });
    expect(await runtime.invoke('4', 'echoBytes', ['AP8='])).toEqual({
      status: 'ok',
      value: 'AP8=',
    });
    expect(await runtime.invoke('5', 'callback', ['hello', '9'])).toEqual({
      status: 'ok',
      value: 'hello:returned',
    });
    expect(await runtime.invoke('6', 'emit', [37])).toEqual({
      status: 'ok',
      value: null,
    });
    expect(events).toEqual([['progress', 37]]);
    const stopped = runtime.stop('shutdown');
    await scheduled;
    timer();
    await stopped;
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
