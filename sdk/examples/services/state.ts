import {
  defineService,
  defineServiceInterface,
  isServiceIoError,
} from 'muon-plugin-sdk/services';

/** Loads the packaged default on first use and saves the latest bytes on shutdown. */
export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: {
      get: { parameters: [], returns: 'bytes', errors: [] },
      set: { parameters: ['bytes'], returns: 'void', errors: [] },
    },
    events: {},
  }),
  start: async (context) => {
    let state: Uint8Array;
    try {
      state = await context.fs.readBytes('state.bin');
    } catch (error) {
      if (!isServiceIoError(error) || error.code !== 'not-found') throw error;
      state = await context.fs.readBytes('initial', { area: 'assets' });
    }
    return {
      methods: {
        get: () => state,
        set: (_call, value) => {
          state = value.slice();
        },
      },
      stop: async (info) =>
        await context.fs.writeBytes('state.bin', state, {
          signal: info.signal,
        }),
    };
  },
});
