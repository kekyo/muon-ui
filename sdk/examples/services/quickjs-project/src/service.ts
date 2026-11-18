import { fromByteArray } from 'base64-js';
import { defineService, defineServiceInterface } from 'muon-plugin-sdk/services';

/** A string echo and a Base64 representation of binary input. */
export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: {
      echo: { parameters: ['string'], returns: 'string', errors: [] },
      base64: { parameters: ['bytes'], returns: 'string', errors: [] },
      notify: { parameters: ['string'], returns: 'void', errors: [] },
    },
    events: { progress: 'string' },
  }),
  start: (context) => ({
    methods: {
      echo: (_call, value) => value,
      base64: (_call, bytes) => fromByteArray(bytes),
      notify: (_call, value) => context.emit('progress', value),
    },
    stop: async () => {},
  }),
});
