import {
  defineService,
  defineServiceInterface,
} from 'muon-plugin-sdk/services';

/** A service-owned HTTP and WebSocket echo listener on its configured loopback port. */
export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: {},
    events: {},
  }),
  start: async (context) => {
    const listener = await context.server.listen({
      name: 'echo',
      port: Number(context.config.port),
      allowedOrigins: ['https://example.test'],
      bearerToken: context.config.bearerToken,
      fetch: (request) => ({
        status: 200,
        headers: { 'x-service-generation': context.generation },
        body: request.body.length ? request.body : context.generation,
      }),
      websocket: {
        accept: () => ({}),
        message: async (socket, message) => await socket.send(message),
      },
    });
    return { methods: {}, stop: async () => await listener.close() };
  },
});
