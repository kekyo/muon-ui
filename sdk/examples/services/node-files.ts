import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import {
  defineService,
  defineServiceInterface,
} from 'muon-plugin-sdk/services';

/** A Node service using a packaged asset and awaiting a private file write on stop. */
export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: {
      greet: { parameters: ['string'], returns: 'string', errors: [] },
    },
    events: {},
  }),
  start: async (context) => {
    const prefix =
      context.config.prefix ??
      `${(await readFile(new URL('./node-greeting.txt', import.meta.url), 'utf8')).trimEnd()} `;
    let lastName = '';
    return {
      methods: {
        greet: (_call, name) => {
          lastName = name;
          return prefix + name;
        },
      },
      // The shared runtime drains active calls before awaiting this file write.
      stop: async () => {
        await writeFile(join(context.dataDirectory, 'last-name.txt'), lastName);
      },
    };
  },
});
