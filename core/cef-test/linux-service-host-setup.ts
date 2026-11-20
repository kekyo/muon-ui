// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFileSync } from "node:child_process";
import { mkdirSync } from "node:fs";

/** Build the shared Linux service host and run its native tests before starting workers. */
export const setup = (): void => {
  const cwd = new URL("../service-host/", import.meta.url);
  mkdirSync(new URL(".build/", cwd), { recursive: true });
  execFileSync("flock", [".build/driver-build.lock", "make", "all", "test"], {
    cwd,
    stdio: "inherit",
  });
};
