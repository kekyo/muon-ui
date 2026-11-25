// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFileSync } from "node:child_process";
import { mkdirSync } from "node:fs";

/** Build the Linux service host before any UI test can copy it. */
export const setup = (): void => {
  // A clean SDK build can refresh the host's native dependencies. Keep build
  // time separate from the runtime behavior deadline and build only this host.
  const cwd = new URL("../../core/service-host/", import.meta.url);
  mkdirSync(new URL(".build/", cwd), { recursive: true });
  execFileSync(
    "flock",
    [".build/driver-build.lock", "make", ".build/muon-service-host"],
    {
      cwd,
      stdio: "inherit",
    },
  );
};
