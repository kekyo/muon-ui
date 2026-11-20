import {
  defineService,
  defineServiceInterface,
} from 'muon-plugin-sdk/services';

/** A portable service with a method and an independently subscribed event. */
export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: {
      echo: { parameters: ['string'], returns: 'string', errors: [] },
      notify: { parameters: ['string'], returns: 'void', errors: [] },
    },
    events: { progress: 'string' },
  }),
  start: (context) => ({
    methods: {
      echo: (_call, value) => value,
      notify: (_call, value) => {
        context.emit('progress', value);
      },
    },
    stop: async () => {},
  }),
});
