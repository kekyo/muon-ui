// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  defineService,
  defineServiceInterface,
} from "muon-plugin-sdk/services";
import {
  echo,
  echoBytes,
  echoInt64,
  echoUint64,
  callback,
  fail,
  makeEcho,
  wrapped,
} from "fixture:api";

/** Common TS service using the C99 plugin fixture on desktop and Android. */
export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: {
      echo: { parameters: ["string"], returns: "string", errors: [] },
      echoBytes: { parameters: ["bytes"], returns: "bytes", errors: [] },
      echoInt64: { parameters: ["int64"], returns: "int64", errors: [] },
      echoUint64: { parameters: ["uint64"], returns: "uint64", errors: [] },
      callback: {
        parameters: [
          "string",
          { callback: { parameters: ["string"], returns: "string" } },
        ],
        returns: "string",
        errors: [],
      },
      proxy: { parameters: ["string"], returns: "string", errors: [] },
      failure: { parameters: [], returns: "string", errors: [] },
      emit: { parameters: ["string"], returns: "void", errors: [] },
    },
    events: { progress: "string" },
  }),
  start: async (context) => {
    await echo(context.id);
    return {
      methods: {
        echo: async (_call, value) => await wrapped(value),
        echoBytes: async (_call, value) => await echoBytes(value),
        echoInt64: async (_call, value) => await echoInt64(value),
        echoUint64: async (_call, value) => await echoUint64(value),
        callback: async (_call, value, notify) =>
          await callback(
            value,
            async (text: string) => await notify(await echo(text)),
          ),
        proxy: async (_call, value) => {
          const native = await makeEcho();
          try {
            return await native(value);
          } finally {
            native.release();
          }
        },
        failure: async () => {
          try {
            await fail();
            return "unexpected success";
          } catch (error) {
            return String(error);
          }
        },
        emit: async (_call, value) => {
          context.emit("progress", await echo(value));
        },
      },
      stop: async () => {
        await echo("stopped");
      },
    };
  },
});
