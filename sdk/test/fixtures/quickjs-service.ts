import { defineService, defineServiceInterface } from '../../src/services.ts';

/** Portable fixture shared by host-engine integration tests. */
export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: {
      echo: { parameters: ['string'], returns: 'string', errors: [] },
      echoBytes: { parameters: ['bytes'], returns: 'bytes', errors: [] },
      echoInt64: { parameters: ['int64'], returns: 'int64', errors: [] },
      echoUint64: { parameters: ['uint64'], returns: 'uint64', errors: [] },
      callback: {
        parameters: [
          'string',
          { callback: { parameters: ['string'], returns: 'string' } },
        ],
        returns: 'string',
        errors: [],
      },
      emit: { parameters: ['int32'], returns: 'void', errors: [] },
    },
    events: { progress: 'int32' },
  }),
  start: (context) => ({
    methods: {
      echo: (_call, value) => value,
      echoBytes: (_call, value) => value,
      echoInt64: (_call, value) => value,
      echoUint64: (_call, value) => value,
      callback: async (_call, value, callback) => await callback(value),
      emit: (_call, value) => {
        context.emit('progress', value);
      },
    },
    stop: async (info) => {
      console.info(
        'lifecycle',
        context.generation,
        context.startReason,
        info.reason
      );
      await new Promise<void>((resolve) =>
        setTimeout(resolve, Number(context.config.stopDelayMs ?? '1'))
      );
    },
  }),
});
