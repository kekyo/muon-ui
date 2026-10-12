// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { appendFileSync } from "node:fs";

const markerPath = process.env.MUON_NODE_TEST_START_MARKER;
if (markerPath !== undefined && markerPath !== "") {
  appendFileSync(markerPath, `${String(process.pid)}\n`, "utf8");
}
