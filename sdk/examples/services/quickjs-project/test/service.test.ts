import { expect, it } from 'vitest';
import { createServiceRuntime } from 'muon-plugin-sdk/services';
import service from '../src/service.ts';

it('echoes binary input as Base64', async () => {
  const runtime = createServiceRuntime(service, {
    callback: async () => { throw new Error('Unexpected callback'); },
    emit: () => {},
  });
  await runtime.start({ id: 'echo', generation: '1', config: {}, dataDirectory: '.' });
  expect(await runtime.invoke('1', 'base64', ['/wA='])).toEqual({ status: 'ok', value: '/wA=' });
  await runtime.stop('released');
});
