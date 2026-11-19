import { expect, it } from 'vitest';
import { createServiceRuntime, defineService } from '../src/service-runtime.ts';
import type { ServiceIoBridge } from '../src/service-io.ts';

it('gives the author the HTTP status, headers and owned binary or text, including non-success status', async () => {
  const messages: unknown[] = [];
  const io: ServiceIoBridge = async (operation, options, body, signal) => {
    expect(operation).toBe('http.request');
    expect(signal.aborted).toBe(false);
    messages.push([options.url, options.method, Array.from(body)]);
    return {
      metadata: {
        status: 418,
        headers: { 'x-value': 'present' },
        ...(options.responseType === 'text' ? { text: '茶' } : {}),
      },
      body: new Uint8Array([0, 255]),
    };
  };
  const runtime = createServiceRuntime(
    defineService({
      interface: { schemaVersion: 1, methods: {}, events: {} },
      start: async (context) => {
        const reply = await context.http.request({
          url: 'https://example.test/',
          method: 'POST',
          body: new Uint8Array([255]),
        });
        expect(reply.status).toBe(418);
        expect(reply.headers['x-value']).toBe('present');
        expect(Array.from(reply.body)).toEqual([0, 255]);
        const text = await context.http.request({
          url: 'https://example.test/',
          responseType: 'text',
        });
        expect(text.body).toBe('茶');
        return { methods: {}, stop: () => {} };
      },
    }),
    {
      io,
      callback: async () => {
        throw new Error('Unexpected callback');
      },
      emit: () => {},
    }
  );
  await runtime.start({
    id: 'http',
    generation: '1',
    config: {},
    dataDirectory: '/private',
    startReason: 'acquire',
  });
  expect(messages).toEqual([
    ['https://example.test/', 'POST', [255]],
    ['https://example.test/', 'GET', []],
  ]);
  await runtime.stop({ reason: 'reference-released' });
});

it('identifies portable I/O errors by code and diagnostic', async () => {
  const {isServiceIoError} = await import('../src/services.ts');
  expect(isServiceIoError(Object.assign(new Error('certificate'), {code:'tls-error'}))).toBe(true);
  expect(isServiceIoError({code:'tls-error'})).toBe(false);
  expect(isServiceIoError({code:'other',message:'author error'})).toBe(false);
});

it('rejects an oversized outbound body before crossing the service bridge', async () => {
  const runtime = createServiceRuntime(defineService({
    interface:{schemaVersion:1,methods:{},events:{}},start:async context=>{
      await expect(context.http.request({url:'https://example.test',method:'POST',body:new Uint8Array(1024*1024+1)})).rejects.toMatchObject({code:'size-limit'});
      return {methods:{},stop:()=>{}};
    },
  }),{emit:()=>{},callback:async()=>{throw new Error('Unexpected callback');},io:async()=>{throw new Error('Oversized data crossed the bridge');}});
  await runtime.start({id:'http',generation:'1',config:{},dataDirectory:'/private',startReason:'acquire'});
  await runtime.stop({reason:'reference-released'});
});
