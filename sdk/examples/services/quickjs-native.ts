import {
  defineService,
  defineServiceInterface,
} from 'muon-plugin-sdk/services';
import { increment } from 'muon:counter';

/** Calls the C or C++ counter plugin created by muon-plugin init. */
export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: { next: { parameters: ['int32'], returns: 'int32', errors: [] } },
    events: {},
  }),
  start: () => ({
    methods: { next: async (_call, value) => await increment(value) },
    stop: async () => {},
  }),
});
