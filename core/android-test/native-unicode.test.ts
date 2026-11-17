import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { expect, it } from 'vitest';

const execute = promisify(execFile);

it('preserves standard UTF-8 across the Android UTF-16 boundary', async () => {
  const result = await execute('make', [
    '-C',
    fileURLToPath(new URL('./native', import.meta.url)),
    'test',
  ]);
  expect(result.stdout).toContain('preserves emoji and embedded NUL');
  expect(result.stderr).toBe('');
});
