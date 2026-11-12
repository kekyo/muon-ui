import { execFile } from 'node:child_process';
import { mkdir, mkdtemp, rm, symlink, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';

const execute = promisify(execFile);
const compiler = createRequire(import.meta.url).resolve('typescript/bin/tsc');

it.each([
  'ES2022',
  'DOM',
  'Node',
  'DOM+Node',
  'tsconfig-types',
  'without-globals',
] as const)(
  'types optional QuickJS globals in a %s consumer',
  async (environment) => {
    const directory = await mkdtemp(join(tmpdir(), 'muon-service-globals-'));
    try {
      await mkdir(join(directory, 'node_modules'));
      await symlink(
        fileURLToPath(new URL('..', import.meta.url)),
        join(directory, 'node_modules/muon-plugin-sdk'),
        'dir'
      );
      await symlink(
        fileURLToPath(new URL('../../node_modules/@types', import.meta.url)),
        join(directory, 'node_modules/@types'),
        'dir'
      );
      await writeFile(
        join(directory, 'tsconfig.json'),
        JSON.stringify({
          compilerOptions: {
            target: 'ES2022',
            module: 'ESNext',
            moduleResolution: 'Bundler',
            strict: true,
            noEmit: true,
            skipLibCheck: false,
            lib: environment.includes('DOM') ? ['ES2022', 'DOM'] : ['ES2022'],
            types: environment.includes('Node')
              ? ['node']
              : environment === 'tsconfig-types'
                ? ['muon-plugin-sdk/service-globals']
                : [],
          },
          files: ['consumer.ts'],
        })
      );
      await writeFile(
        join(directory, 'consumer.ts'),
        environment === 'without-globals'
          ? `
import { defineServiceInterface } from 'muon-plugin-sdk/services';
void defineServiceInterface;
// @ts-expect-error ordinary service imports do not add host globals
setTimeout(() => {}, 1);
// @ts-expect-error ordinary service imports do not add host globals
console.log('not available');
`
          : `
${environment === 'tsconfig-types' ? '' : "import type {} from 'muon-plugin-sdk/service-globals';"}
const timer: ${environment.includes('Node') ? 'NodeJS.Timeout' : 'number'} = setTimeout((message: string, count: number) => {
  console.debug(message, count);
  console.log(message, count);
  console.info(message, count);
  console.warn(message, count);
  console.error(message, count);
}, 1, 'hello', 42);
clearTimeout(timer);
clearTimeout(undefined);
const delayed: Promise<void> = new Promise(resolve => setTimeout(resolve, 1));
void delayed;
${environment.includes('Node') ? 'timer.ref(); timer.unref();' : ''}
${
  environment === 'ES2022' || environment === 'tsconfig-types'
    ? `
// @ts-expect-error QuickJS timers accept functions, not source strings
setTimeout('code', 1);
// @ts-expect-error callback arguments must match the supplied values
setTimeout((value: number) => {}, 1, 'wrong');
// @ts-expect-error required callback arguments cannot be omitted
setTimeout((value: number) => {}, 1);
// @ts-expect-error the QuickJS host does not expose intervals
setInterval(() => {}, 1);
// @ts-expect-error the QuickJS host does not expose browser networking
fetch('https://example.test');
// @ts-expect-error the QuickJS host does not expose console timers
console.time('unsupported');
`
    : ''
}
`
      );
      try {
        const result = await execute(process.execPath, [
          compiler,
          '-p',
          join(directory, 'tsconfig.json'),
        ]);
        expect(result.stdout).toBe('');
        expect(result.stderr).toBe('');
      } catch (error) {
        const detail = error as Error & { stdout?: string; stderr?: string };
        throw new Error(
          `${detail.message}\n${detail.stdout ?? ''}${detail.stderr ?? ''}`
        );
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);
