// Run native Wayland tests inside an isolated Weston compositor on the test X11 display.
import { spawn } from "node:child_process";
import { mkdtemp, readFile, rm, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout as delay } from "node:timers/promises";

const [westonExecutable, command, ...args] = process.argv.slice(2);
if (westonExecutable === undefined || command === undefined) {
  throw new Error("Expected a Weston executable and test command");
}
const directory = await mkdtemp(join(tmpdir(), "muon-wayland-test-"));
const socket = "wayland-muon-test";
const logPath = join(directory, "weston.log");
const environment = {
  ...process.env,
  XDG_RUNTIME_DIR: directory,
  WAYLAND_DISPLAY: socket,
  GDK_BACKEND: "wayland",
  GSETTINGS_BACKEND: "memory",
  GIO_USE_VFS: "local",
};
const compositor = spawn(
  westonExecutable,
  [
    "--backend=x11",
    "--renderer=pixman",
    "--no-config",
    "--idle-time=0",
    `--socket=${socket}`,
    `--log=${logPath}`,
  ],
  { env: environment, stdio: "inherit" },
);
let compositorExited = false;
let compositorError;
const compositorDone = new Promise((resolve) => {
  compositor.once("error", (error) => {
    compositorError = error;
    compositorExited = true;
    resolve();
  });
  compositor.once("exit", () => {
    compositorExited = true;
    resolve();
  });
});
try {
  const deadline = Date.now() + 30000;
  while (true) {
    if (compositorExited)
      throw (
        compositorError ??
        new Error("Weston exited before creating its display")
      );
    try {
      if ((await stat(join(directory, socket))).isSocket()) break;
    } catch (error) {
      if (error.code !== "ENOENT") throw error;
    }
    if (Date.now() >= deadline)
      throw new Error("Timed out waiting for the Wayland display socket");
    await delay(25);
  }
  process.exitCode = await new Promise((resolve, reject) => {
    const child = spawn(command, args, {
      env: { ...environment, DISPLAY: "" },
      stdio: "inherit",
    });
    child.once("error", reject);
    child.once("exit", (code) => resolve(code ?? 1));
  });
} finally {
  if (!compositorExited) compositor.kill("SIGTERM");
  const forceStop = setTimeout(() => compositor.kill("SIGKILL"), 5000);
  await compositorDone;
  clearTimeout(forceStop);
  if (process.exitCode) process.stderr.write(await readFile(logPath, "utf8"));
  await rm(directory, { recursive: true, force: true });
}
