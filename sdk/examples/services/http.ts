import {
  defineService,
  defineServiceInterface,
  type ServiceHttpRequest,
} from 'muon-plugin-sdk/services';

/** Exercises HTTP through the service's own grants, including binary bodies and non-success statuses. */
export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: {
      bytes: { parameters: ['string', 'bytes'], returns: 'bytes', errors: [] },
      describe: {
        parameters: ['string', 'bytes'],
        returns: 'string',
        errors: [],
      },
    },
    events: {},
  }),
  start: (context) => ({
    methods: {
      bytes: async (call, options, body) =>
        (
          await context.http.request({
            ...(JSON.parse(options) as ServiceHttpRequest),
            body,
            responseType: 'bytes',
            signal: call.signal,
          })
        ).body,
      describe: async (call, options, body) => {
        const reply = await context.http.request({
          ...(JSON.parse(options) as ServiceHttpRequest),
          body,
          responseType: 'text',
          signal: call.signal,
        });
        return JSON.stringify({
          status: reply.status,
          headers: reply.headers,
          text: reply.body,
        });
      },
    },
    stop: () => {},
  }),
});
