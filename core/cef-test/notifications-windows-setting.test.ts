// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { expect, it } from "vitest";

it("waits for definite notification settings without toggling or accepting indefinite states", async () => {
  const directory = fileURLToPath(new URL("./native/", import.meta.url));
  const execute = promisify(execFile);
  await execute("make", ["notification-toggle-test"], { cwd: directory });
  const result = await execute(".build/notification-toggle-test", [], {
    cwd: directory,
  });
  expect(result.stdout).toBe("Notification setting transitions passed\n");
});
