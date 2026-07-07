// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { execFile } from "node:child_process";
import { randomUUID } from "node:crypto";
import {
  chmod,
  mkdir,
  mkdtemp,
  readdir,
  readFile,
  rm,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, join, resolve, win32 } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";

import {
  connectRemoteAgent,
  saveDiagnostics,
  type RemoteAgent,
} from "agent-rover";
import { afterAll, afterEach, describe, expect, it, vi } from "vitest";

import type { MuonBuildTarget } from "../src/build.js";
import {
  createMuonBootstrapEmbeddedConfigSlot,
  createMuonEmbeddedConfigSlot,
} from "../src/embed-config.js";
import { packMuonApp } from "../src/pack.js";
import {
  getMuonTargetDescriptor,
  getMuonTargetRuntimeAppId,
} from "../src/targets.js";

const execFileAsync = promisify(execFile);
const defaultWindowsE2ePort = 39397;
const defaultWindowsE2eWorkDirectory = String.raw`C:\muon-e2e`;
const windowsPowerShellPath = String.raw`C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe`;
const localCleanupDirectories: string[] = [];

interface WindowsE2eEnvironment {
  host: string;
  port: number;
  token: string;
  workDirectory: string;
}

interface WindowsCommandResult {
  exitCode: number | null;
  stderr: string;
  stdout: string;
}

interface WindowsNsisInstallIdentity {
  displayName: string;
  installDirectoryName: string;
  runtimeAppId: string;
}

interface MuonPackCliJsonResult {
  artifacts: readonly {
    path: string;
    target: string;
    type: string;
  }[];
}

const readRequiredEnvironmentValue = (
  env: Readonly<Record<string, string | undefined>>,
  name: string,
): string | undefined => {
  const value = env[name];
  if (value === undefined) {
    return undefined;
  }
  const trimmed = value.trim();
  return trimmed.length === 0 ? undefined : trimmed;
};

const readOptionalWindowsE2ePort = (
  env: Readonly<Record<string, string | undefined>>,
): number => {
  const value = env.AGENT_ROVER_WIN11_PORT;
  if (value === undefined || value.trim() === "") {
    return defaultWindowsE2ePort;
  }
  const trimmed = value.trim();
  if (!/^[0-9]+$/u.test(trimmed)) {
    throw new Error("AGENT_ROVER_WIN11_PORT must be an integer.");
  }
  return Number(trimmed);
};

const readOptionalWindowsE2eWorkDirectory = (
  env: Readonly<Record<string, string | undefined>>,
): string => {
  const value = env.AGENT_ROVER_WIN11_WORK_DIR;
  return value === undefined || value.trim() === ""
    ? defaultWindowsE2eWorkDirectory
    : value.trim();
};

const parseWindowsE2eEnvironment = (
  env: Readonly<Record<string, string | undefined>>,
):
  | { environment: WindowsE2eEnvironment; status: "configured" }
  | { reason: string; status: "skip" } => {
  const host = readRequiredEnvironmentValue(env, "AGENT_ROVER_WIN11_HOST");
  if (host === undefined) {
    return {
      reason: "AGENT_ROVER_WIN11_HOST is not set",
      status: "skip",
    };
  }

  const token = readRequiredEnvironmentValue(env, "AGENT_ROVER_WIN11_TOKEN");
  if (token === undefined) {
    return {
      reason: "AGENT_ROVER_WIN11_TOKEN is not set",
      status: "skip",
    };
  }

  return {
    environment: {
      host,
      port: readOptionalWindowsE2ePort(env),
      token,
      workDirectory: readOptionalWindowsE2eWorkDirectory(env),
    },
    status: "configured",
  };
};

const createTemporaryDirectory = async (prefix: string): Promise<string> => {
  const directory = await mkdtemp(join(tmpdir(), prefix));
  localCleanupDirectories.push(directory);
  return directory;
};

const expectMakensisAvailable = async (): Promise<void> => {
  try {
    await execFileAsync("makensis", ["-VERSION"]);
  } catch (error) {
    throw new Error(
      `makensis is required on PATH for Windows NSIS e2e: ${String(error)}`,
    );
  }
};

const writeExecutable = async (
  path: string,
  content: Buffer | string,
): Promise<void> => {
  await mkdir(join(path, ".."), { recursive: true });
  await writeFile(path, content);
  await chmod(path, 0o755);
};

const joinWindowsPath = (...paths: string[]): string => win32.join(...paths);

const quotePowerShellString = (value: string): string =>
  `'${value.replaceAll("'", "''")}'`;

const writeRemoteFile = async (
  agent: RemoteAgent,
  path: string,
  content: Buffer | string,
): Promise<void> => {
  await agent.files.mkdir(win32.dirname(path), { recursive: true });
  await agent.files.writeFile(
    path,
    Buffer.isBuffer(content) ? content : Buffer.from(content),
  );
};

const writeRemoteTextFile = async (
  agent: RemoteAgent,
  path: string,
  content: string,
): Promise<void> => {
  await writeRemoteFile(agent, path, content);
};

const copyLocalFileToRemote = async (
  agent: RemoteAgent,
  localPath: string,
  remotePath: string,
): Promise<void> => {
  await writeRemoteFile(agent, remotePath, await readFile(localPath));
};

const copyLocalDirectoryToRemote = async (
  agent: RemoteAgent,
  localDirectory: string,
  remoteDirectory: string,
): Promise<void> => {
  await agent.files.mkdir(remoteDirectory, { recursive: true });
  const entries = await readdir(localDirectory, { withFileTypes: true });
  entries.sort((left, right) => left.name.localeCompare(right.name));
  for (const entry of entries) {
    const localPath = join(localDirectory, entry.name);
    const remotePath = joinWindowsPath(remoteDirectory, entry.name);
    if (entry.isDirectory()) {
      await copyLocalDirectoryToRemote(agent, localPath, remotePath);
    } else if (entry.isFile()) {
      await copyLocalFileToRemote(agent, localPath, remotePath);
    }
  }
};

const wait = async (milliseconds: number): Promise<void> => {
  await new Promise<void>((resolvePromise) => {
    setTimeout(resolvePromise, milliseconds);
  });
};

const readRemoteUtf8IfExists = async (
  agent: RemoteAgent,
  path: string,
): Promise<string> => {
  if (!(await agent.files.exists(path))) {
    return "";
  }

  let lastError: unknown = undefined;
  for (let attempt = 0; attempt < 20; attempt += 1) {
    try {
      return (await agent.files.readFile(path)).toString("utf8");
    } catch (error) {
      lastError = error;
      await wait(250);
    }
  }
  if (lastError instanceof Error) {
    throw lastError;
  }
  throw new Error(`Failed to read remote file: ${path}`);
};

const runWindowsProcess = async (
  agent: RemoteAgent,
  remoteDirectory: string,
  label: string,
  commandPath: string,
  commandArguments: readonly string[],
  timeoutMs: number,
): Promise<WindowsCommandResult> => {
  const stdoutPath = joinWindowsPath(remoteDirectory, `${label}.stdout.log`);
  const stderrPath = joinWindowsPath(remoteDirectory, `${label}.stderr.log`);
  const processInfo = await agent.applications.launch({
    arguments: commandArguments,
    createNoWindow: true,
    path: commandPath,
    stderrPath,
    stdoutPath,
    workingDirectory: remoteDirectory,
  });
  const snapshot = await agent.processes.waitForExit(processInfo.id, {
    intervalMs: 250,
    timeoutMs,
  });
  return {
    exitCode: snapshot.exitCode,
    stderr: await readRemoteUtf8IfExists(agent, stderrPath),
    stdout: await readRemoteUtf8IfExists(agent, stdoutPath),
  };
};

const runWindowsPowerShell = async (
  agent: RemoteAgent,
  remoteDirectory: string,
  label: string,
  script: string,
  timeoutMs: number,
): Promise<WindowsCommandResult> => {
  const scriptPath = joinWindowsPath(remoteDirectory, `${label}.ps1`);
  await agent.files.writeFile(
    scriptPath,
    Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), Buffer.from(script)]),
  );
  return await runWindowsProcess(
    agent,
    remoteDirectory,
    label,
    windowsPowerShellPath,
    ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", scriptPath],
    timeoutMs,
  );
};

const isWindowsCommandSucceeded = (result: WindowsCommandResult): boolean =>
  result.exitCode === 0 ||
  (result.exitCode === null && result.stderr.trim() === "");

const expectWindowsCommandSucceeded = (
  result: WindowsCommandResult,
  label: string,
): void => {
  expect(
    isWindowsCommandSucceeded(result),
    `${label} failed.\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`,
  ).toBe(true);
};

const writeRemoteSharpStub = async (
  agent: RemoteAgent,
  remoteNodeModulesDirectory: string,
): Promise<void> => {
  const sharpDirectory = joinWindowsPath(remoteNodeModulesDirectory, "sharp");
  await writeRemoteTextFile(
    agent,
    joinWindowsPath(sharpDirectory, "package.json"),
    `${JSON.stringify({ name: "sharp", version: "0.0.0", main: "index.js" }, null, 2)}\n`,
  );
  await writeRemoteTextFile(
    agent,
    joinWindowsPath(sharpDirectory, "index.js"),
    [
      "module.exports = (input) => {",
      "  const source = Buffer.isBuffer(input) ? Buffer.from(input) : Buffer.from(input ?? []);",
      "  let width = 256;",
      "  let height = 256;",
      "  const chain = {",
      "    metadata: async () => ({ format: 'png' }),",
      "    resize: (nextWidth, nextHeight) => {",
      "      width = Number(nextWidth) || width;",
      "      height = Number(nextHeight) || height;",
      "      return chain;",
      "    },",
      "    ensureAlpha: () => chain,",
      "    png: () => chain,",
      "    raw: () => chain,",
      "    toBuffer: async (options) => {",
      "      if (options?.resolveWithObject === true) {",
      "        return {",
      "          data: Buffer.alloc(width * height * 4),",
      "          info: { width, height, channels: 4 },",
      "        };",
      "      }",
      "      return source;",
      "    },",
      "  };",
      "  return chain;",
      "};",
      "",
    ].join("\n"),
  );
};

const stageRemoteMuonPackCli = async (
  agent: RemoteAgent,
  remoteDirectory: string,
): Promise<void> => {
  const remoteNodeModulesDirectory = joinWindowsPath(
    remoteDirectory,
    "node_modules",
  );
  const remoteMuonUiDirectory = joinWindowsPath(
    remoteNodeModulesDirectory,
    "muon-ui",
  );
  await writeRemoteTextFile(
    agent,
    joinWindowsPath(remoteMuonUiDirectory, "package.json"),
    `${JSON.stringify(
      {
        name: "muon-ui",
        version: "0.0.0",
        type: "module",
        bin: { muon: "./dist/cli.cjs" },
      },
      null,
      2,
    )}\n`,
  );
  await copyLocalDirectoryToRemote(
    agent,
    resolve("dist"),
    joinWindowsPath(remoteMuonUiDirectory, "dist"),
  );
  for (const packageName of ["adm-zip", "commander", "tar-vern"] as const) {
    await copyLocalDirectoryToRemote(
      agent,
      resolve("..", "node_modules", packageName),
      joinWindowsPath(remoteNodeModulesDirectory, packageName),
    );
  }
  await writeRemoteSharpStub(agent, remoteNodeModulesDirectory);
};

const createRemotePlainAssetsProject = async (
  agent: RemoteAgent,
  remoteDirectory: string,
  packageName: string,
): Promise<void> => {
  await writeRemoteTextFile(
    agent,
    joinWindowsPath(remoteDirectory, "package.json"),
    `${JSON.stringify(
      {
        author: "muon Tester",
        description: "muon pack Windows e2e sample",
        name: packageName,
        version: "1.2.3",
      },
      null,
      2,
    )}\n`,
  );
  await writeRemoteTextFile(
    agent,
    joinWindowsPath(remoteDirectory, "assets", "index.html"),
    "<!doctype html><title>windows pack e2e</title>",
  );
  await writeRemoteTextFile(
    agent,
    joinWindowsPath(remoteDirectory, "muon.json"),
    `${JSON.stringify({ network: { allow: ["asset://main/**"] } }, null, 2)}\n`,
  );
};

const createRemoteFakeLinuxPackageDist = async (
  agent: RemoteAgent,
  remoteDirectory: string,
): Promise<string> => {
  const target: MuonBuildTarget = "linux-amd64";
  const descriptor = getMuonTargetDescriptor(target);
  const packageDirectory = joinWindowsPath(remoteDirectory, "package-dist");
  const runtimeDirectory = joinWindowsPath(packageDirectory, "runtime", target);
  const nativeDirectory = joinWindowsPath(packageDirectory, "native", target);
  await writeRemoteFile(
    agent,
    joinWindowsPath(runtimeDirectory, descriptor.runtimeExecutableName),
    Buffer.concat([
      Buffer.from("core prefix\n"),
      createMuonEmbeddedConfigSlot(),
      Buffer.from("\ncore suffix\n"),
    ]),
  );
  for (const fileName of descriptor.runtimeFiles) {
    if (fileName !== descriptor.runtimeExecutableName) {
      await writeRemoteTextFile(
        agent,
        joinWindowsPath(runtimeDirectory, fileName),
        `${fileName}\n`,
      );
    }
  }
  await writeRemoteTextFile(
    agent,
    joinWindowsPath(runtimeDirectory, "CREDITS.md"),
    "notices\n",
  );
  await writeRemoteFile(
    agent,
    joinWindowsPath(nativeDirectory, descriptor.bootstrapExecutableName),
    Buffer.concat([
      Buffer.from("bootstrap prefix\n"),
      createMuonBootstrapEmbeddedConfigSlot(),
      Buffer.from("\nbootstrap suffix\n"),
    ]),
  );
  await copyLocalFileToRemote(
    agent,
    resolve("dist", "native", "muon-256.png"),
    joinWindowsPath(packageDirectory, "native", "muon-256.png"),
  );
  return packageDirectory;
};

const createMuonPackCliScript = (packageDirectory: string): string => `
$ErrorActionPreference = 'Stop'
$cliPath = Join-Path $PWD 'node_modules\\muon-ui\\dist\\cli.cjs'
$packageDirectory = ${quotePowerShellString(packageDirectory)}
$arguments = @(
  $cliPath,
  'pack',
  '--target',
  'linux-amd64',
  '--json',
  '--package-directory',
  $packageDirectory
)
& node @arguments
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}
`;

const createFakeMuonPackageDist = async (
  root: string,
  targets: readonly MuonBuildTarget[],
): Promise<string> => {
  const packageDirectory = join(root, "package-dist");
  for (const target of targets) {
    const runtimeDirectory = join(packageDirectory, "runtime", target);
    const nativeDirectory = join(packageDirectory, "native", target);
    await mkdir(runtimeDirectory, { recursive: true });
    await mkdir(nativeDirectory, { recursive: true });
    await writeExecutable(
      join(runtimeDirectory, "muon-core.exe"),
      Buffer.concat([
        Buffer.from("core prefix\n"),
        createMuonEmbeddedConfigSlot(),
        Buffer.from("\ncore suffix\n"),
      ]),
    );
    await writeFile(join(runtimeDirectory, "libmuon-ui.dll"), "ui\n");
    await writeFile(join(runtimeDirectory, "libcardio.dll"), "cardio\n");
    await writeFile(join(runtimeDirectory, "CREDITS.md"), "notices\n");
    await writeExecutable(
      join(nativeDirectory, "muon-bootstrap.exe"),
      Buffer.concat([
        Buffer.from("bootstrap prefix\n"),
        createMuonBootstrapEmbeddedConfigSlot(),
        Buffer.from("\nbootstrap suffix\n"),
      ]),
    );
  }
  return packageDirectory;
};

const createWindowsNsisInstallIdentity = (
  packageName: string,
  appId: string,
  target: MuonBuildTarget,
): WindowsNsisInstallIdentity => {
  const descriptor = getMuonTargetDescriptor(target);
  if (descriptor.os !== "windows") {
    throw new Error(`Unsupported NSIS e2e target: ${target}`);
  }
  return {
    displayName: `${packageName} (${descriptor.arch})`,
    installDirectoryName: `${packageName}-${descriptor.arch}`,
    runtimeAppId: getMuonTargetRuntimeAppId(appId, target),
  };
};

const createPowerShellIdentityObjects = (
  identities: readonly WindowsNsisInstallIdentity[],
): string =>
  identities
    .map(
      (identity) =>
        `[pscustomobject]@{ DisplayName = ${quotePowerShellString(identity.displayName)}; InstallDirectoryName = ${quotePowerShellString(identity.installDirectoryName)}; RuntimeAppId = ${quotePowerShellString(identity.runtimeAppId)} }`,
    )
    .join(",\n");

const writeViteProject = async (
  root: string,
  packageDirectory: string,
  targets: readonly MuonBuildTarget[],
  packageName: string,
): Promise<void> => {
  const vitePluginUrl = pathToFileURL(resolve("dist", "vite.mjs")).href;
  await writeFile(
    join(root, "package.json"),
    `${JSON.stringify(
      {
        author: "muon Tester",
        description: "muon NSIS e2e sample",
        name: packageName,
        type: "module",
        version: "1.2.3",
      },
      null,
      2,
    )}\n`,
  );
  await writeFile(
    join(root, "index.html"),
    '<!doctype html><title>nsis e2e</title><script type="module" src="/src/main.ts"></script>',
  );
  await mkdir(join(root, "src"), { recursive: true });
  await writeFile(
    join(root, "src", "main.ts"),
    "document.body.textContent = 'nsis e2e';\n",
  );
  await writeFile(
    join(root, "muon.json"),
    `${JSON.stringify({ network: { allow: ["asset://main/**"] } }, null, 2)}\n`,
  );
  await writeFile(
    join(root, "vite.config.mjs"),
    [
      `import muon from ${JSON.stringify(vitePluginUrl)};`,
      "export default {",
      "  build: { outDir: 'web-dist' },",
      "  plugins: [",
      `    muon({ build: { targets: ${JSON.stringify(targets)}, packageDirectory: ${JSON.stringify(packageDirectory)} } }),`,
      "  ],",
      "};",
    ].join("\n"),
  );
};

const createRemoteTestDirectory = async (
  agent: RemoteAgent,
  environment: WindowsE2eEnvironment,
  packageName: string,
): Promise<string> => {
  const remoteDirectory = joinWindowsPath(
    environment.workDirectory,
    "muon-ui-nsis",
    packageName,
  );
  if (await agent.files.exists(remoteDirectory)) {
    await agent.files.remove(remoteDirectory, { recursive: true });
  }
  await agent.files.mkdir(remoteDirectory, { recursive: true });
  return remoteDirectory;
};

const removeRemoteDirectoryIfExists = async (
  agent: RemoteAgent,
  directory: string,
): Promise<void> => {
  if (await agent.files.exists(directory)) {
    await agent.files.remove(directory, { recursive: true });
  }
};

const createCleanupScript = (
  identities: readonly WindowsNsisInstallIdentity[],
): string => `
$ErrorActionPreference = 'Stop'
Stop-Process -Name SystemSettings -Force -ErrorAction SilentlyContinue
$identities = @(
${createPowerShellIdentityObjects(identities)}
)
foreach ($identity in $identities) {
  $installDir = Join-Path $env:LOCALAPPDATA ('Programs\\' + $identity.InstallDirectoryName)
  $stateDir = Join-Path $env:LOCALAPPDATA $identity.RuntimeAppId
  $shortcutPath = Join-Path $env:APPDATA ('Microsoft\\Windows\\Start Menu\\Programs\\' + $identity.DisplayName + '.lnk')
  $registryPath = 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\' + $identity.RuntimeAppId
  $uninstaller = Join-Path $installDir 'Uninstall.exe'
  if (Test-Path -LiteralPath $uninstaller) {
    $process = Start-Process -FilePath $uninstaller -ArgumentList '/S' -Wait -PassThru
    if ($process.ExitCode -ne 0) {
      Write-Output ('cleanup uninstaller exited with ' + $process.ExitCode)
    }
  }
  Remove-Item -LiteralPath $registryPath -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $shortcutPath -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $installDir -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $stateDir -Recurse -Force -ErrorAction SilentlyContinue
}
`;

const createInstallStateAssertionScript = (
  identities: readonly WindowsNsisInstallIdentity[],
): string => `
$ErrorActionPreference = 'Stop'
$identities = @(
${createPowerShellIdentityObjects(identities)}
)
$errors = New-Object System.Collections.Generic.List[string]
foreach ($identity in $identities) {
  $installDir = Join-Path $env:LOCALAPPDATA ('Programs\\' + $identity.InstallDirectoryName)
  $shortcutPath = Join-Path $env:APPDATA ('Microsoft\\Windows\\Start Menu\\Programs\\' + $identity.DisplayName + '.lnk')
  $registryPath = 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\' + $identity.RuntimeAppId
  if (-not (Test-Path -LiteralPath $installDir)) {
    $errors.Add('install directory is missing: ' + $installDir)
  }
  if (-not (Test-Path -LiteralPath $shortcutPath)) {
    $errors.Add('start menu shortcut is missing: ' + $shortcutPath)
  }
  if (-not (Test-Path -LiteralPath $registryPath)) {
    $errors.Add('registry key is missing: ' + $registryPath)
  } else {
    $displayName = (Get-ItemProperty -LiteralPath $registryPath).DisplayName
    if ($displayName -ne $identity.DisplayName) {
      $errors.Add('registry DisplayName mismatch: ' + $displayName)
    }
  }
}
if ($errors.Count -gt 0) {
  $errors | ForEach-Object { Write-Error $_ }
  exit 1
}
Write-Output 'installed'
`;

const createInstallerScript = (installerPath: string): string => `
$ErrorActionPreference = 'Stop'
$installerPath = ${quotePowerShellString(installerPath)}
$process = Start-Process -FilePath $installerPath -ArgumentList '/S' -Wait -PassThru
if ($null -ne $process.ExitCode -and $process.ExitCode -ne 0) {
  Write-Error ('installer exited with ' + $process.ExitCode)
  exit $process.ExitCode
}
Write-Output 'installer completed'
`;

const createRemovalStateAssertionScript = (
  removed: WindowsNsisInstallIdentity,
  remaining: WindowsNsisInstallIdentity,
): string => `
$ErrorActionPreference = 'Stop'
$removed = ${createPowerShellIdentityObjects([removed])}
$remaining = ${createPowerShellIdentityObjects([remaining])}
$installDir = Join-Path $env:LOCALAPPDATA ('Programs\\' + $removed.InstallDirectoryName)
$stateDir = Join-Path $env:LOCALAPPDATA $removed.RuntimeAppId
$shortcutPath = Join-Path $env:APPDATA ('Microsoft\\Windows\\Start Menu\\Programs\\' + $removed.DisplayName + '.lnk')
$registryPath = 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\' + $removed.RuntimeAppId
$remainingInstallDir = Join-Path $env:LOCALAPPDATA ('Programs\\' + $remaining.InstallDirectoryName)
$remainingRegistryPath = 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\' + $remaining.RuntimeAppId
$errors = New-Object System.Collections.Generic.List[string]
if (Test-Path -LiteralPath $installDir) {
  $errors.Add('install directory still exists: ' + $installDir)
}
if (Test-Path -LiteralPath $shortcutPath) {
  $errors.Add('start menu shortcut still exists: ' + $shortcutPath)
}
if (Test-Path -LiteralPath $registryPath) {
  $errors.Add('registry key still exists: ' + $registryPath)
}
if (Test-Path -LiteralPath $stateDir) {
  $errors.Add('runtime state directory still exists: ' + $stateDir)
}
if (-not (Test-Path -LiteralPath $remainingInstallDir)) {
  $errors.Add('remaining install directory is missing: ' + $remainingInstallDir)
}
if (-not (Test-Path -LiteralPath $remainingRegistryPath)) {
  $errors.Add('remaining registry key is missing: ' + $remainingRegistryPath)
}
if ($errors.Count -gt 0) {
  $errors | ForEach-Object { Write-Error $_ }
  exit 1
}
Write-Output 'removed'
`;

const createRuntimeStateFixtureScript = (appId: string): string => `
$ErrorActionPreference = 'Stop'
$appId = ${quotePowerShellString(appId)}
$stateDir = Join-Path $env:LOCALAPPDATA $appId
$targetStateDir = Join-Path $stateDir 'runtime'
New-Item -ItemType Directory -Force -Path $targetStateDir | Out-Null
Set-Content -LiteralPath (Join-Path $targetStateDir 'state.txt') -Value 'state' -Encoding UTF8
Write-Output 'runtime state fixture created'
`;

const createSettingsUninstallScript = (
  identity: WindowsNsisInstallIdentity,
): string => `
$ErrorActionPreference = 'Stop'
$displayName = ${quotePowerShellString(identity.displayName)}
$installDirectoryName = ${quotePowerShellString(identity.installDirectoryName)}
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class MuonMouse {
  [DllImport("user32.dll")]
  public static extern bool SetCursorPos(int X, int Y);
  [DllImport("user32.dll")]
  public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);
}
'@
$automationRoot = [System.Windows.Automation.AutomationElement]::RootElement
$conditionTrue = [System.Windows.Automation.Condition]::TrueCondition
$controlTypeButton = [System.Windows.Automation.ControlType]::Button
$controlTypeEdit = [System.Windows.Automation.ControlType]::Edit
$controlTypeMenuItem = [System.Windows.Automation.ControlType]::MenuItem
$invokePatternId = [System.Windows.Automation.InvokePattern]::Pattern
$processIdProperty = [System.Windows.Automation.AutomationElement]::ProcessIdProperty
$treeScopeChildren = [System.Windows.Automation.TreeScope]::Children
$treeScopeDescendants = [System.Windows.Automation.TreeScope]::Descendants
$valuePatternId = [System.Windows.Automation.ValuePattern]::Pattern

function Wait-Until([scriptblock] $operation, [int] $timeoutMs, [string] $message) {
  $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
  while ([DateTime]::UtcNow -lt $deadline) {
    $result = & $operation
    if ($null -ne $result -and $false -ne $result) {
      return $result
    }
    Start-Sleep -Milliseconds 200
  }
  throw $message
}

function Test-VisibleElement($element) {
  if ($null -eq $element) {
    return $false
  }
  $rectangle = $element.Current.BoundingRectangle
  return (-not $element.Current.IsOffscreen) -and $rectangle.Width -gt 1 -and $rectangle.Height -gt 1
}

function Get-ElementCenterX($element) {
  $rectangle = $element.Current.BoundingRectangle
  return $rectangle.X + ($rectangle.Width / 2)
}

function Get-ElementCenterY($element) {
  $rectangle = $element.Current.BoundingRectangle
  return $rectangle.Y + ($rectangle.Height / 2)
}

function Get-SettingsWindow() {
  $processes = @(Get-Process -Name SystemSettings -ErrorAction SilentlyContinue)
  foreach ($process in $processes) {
    $propertyCondition = New-Object System.Windows.Automation.PropertyCondition($processIdProperty, $process.Id)
    $windows = @($automationRoot.FindAll($treeScopeChildren, $propertyCondition))
    foreach ($window in $windows) {
      if (Test-VisibleElement $window) {
        return $window
      }
    }
  }
  $topLevelWindows = @($automationRoot.FindAll($treeScopeChildren, $conditionTrue))
  foreach ($window in $topLevelWindows) {
    if ((Test-VisibleElement $window) -and ($window.Current.Name -match 'Settings|設定')) {
      return $window
    }
  }
  return $null
}

function Get-VisibleDescendants($root) {
  $items = @($root.FindAll($treeScopeDescendants, $conditionTrue))
  return @($items | Where-Object { Test-VisibleElement $_ })
}

function Click-Element($element) {
  if ($null -eq $element) {
    throw 'Cannot click a missing UI element.'
  }
  try {
    $element.SetFocus()
  } catch {
  }
  $rectangle = $element.Current.BoundingRectangle
  $x = [int] ($rectangle.X + ($rectangle.Width / 2))
  $y = [int] ($rectangle.Y + ($rectangle.Height / 2))
  [void] [MuonMouse]::SetCursorPos($x, $y)
  [MuonMouse]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 50
  [MuonMouse]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
}

function Invoke-Element($element) {
  if ($null -eq $element) {
    throw 'Cannot invoke a missing UI element.'
  }
  $pattern = $null
  if ($element.TryGetCurrentPattern($invokePatternId, [ref] $pattern)) {
    $pattern.Invoke()
    return
  }
  Click-Element $element
}

function Test-NamePattern($name, [string[]] $patterns) {
  foreach ($pattern in $patterns) {
    if ($name -match $pattern) {
      return $true
    }
  }
  return $false
}

function Find-SearchAppsEdit($window) {
  $searchPatterns = @('Search apps', 'search.*apps', 'apps.*search', 'アプリ.*検索', '検索.*アプリ')
  $windowRectangle = $window.Current.BoundingRectangle
  $edits = @(Get-VisibleDescendants $window | Where-Object { $_.Current.ControlType -eq $controlTypeEdit })
  $contentEdits = @($edits | Where-Object { $_.Current.BoundingRectangle.X -gt ($windowRectangle.X + 280) })
  $named = @($contentEdits | Where-Object { Test-NamePattern $_.Current.Name $searchPatterns })
  if ($named.Count -gt 0) {
    return $named[0]
  }
  if ($contentEdits.Count -gt 0) {
    $sorted = @($contentEdits | Sort-Object { $_.Current.BoundingRectangle.Y })
    return $sorted[0]
  }
  return $null
}

function Set-EditValue($edit, [string] $value) {
  $edit.SetFocus()
  $pattern = $null
  if ($edit.TryGetCurrentPattern($valuePatternId, [ref] $pattern)) {
    $pattern.SetValue($value)
    return
  }
  [System.Windows.Forms.SendKeys]::SendWait('^a')
  [System.Windows.Forms.SendKeys]::SendWait($value)
}

function Find-AppNameElement($window) {
  $items = Get-VisibleDescendants $window
  $exact = @($items | Where-Object { $_.Current.Name -eq $displayName })
  if ($exact.Count -gt 0) {
    return $exact[0]
  }
  $contains = @($items | Where-Object { $_.Current.Name -like ('*' + $displayName + '*') })
  if ($contains.Count -gt 0) {
    return $contains[0]
  }
  return $null
}

function Find-MoreOptionsButton($window, $appElement) {
  $appCenterX = Get-ElementCenterX $appElement
  $appCenterY = Get-ElementCenterY $appElement
  $buttons = @(Get-VisibleDescendants $window | Where-Object { $_.Current.ControlType -eq $controlTypeButton })
  $named = @($buttons | Where-Object {
    (Test-NamePattern $_.Current.Name @('More options', 'その他のオプション', '詳細', 'オプション')) -and
      ([Math]::Abs((Get-ElementCenterY $_) - $appCenterY) -lt 120)
  } | Sort-Object { Get-ElementCenterX $_ } -Descending)
  if ($named.Count -gt 0) {
    return $named[0]
  }
  $near = @($buttons | Where-Object {
    (Get-ElementCenterX $_) -gt $appCenterX -and
      ([Math]::Abs((Get-ElementCenterY $_) - $appCenterY) -lt 120)
  } | Sort-Object { Get-ElementCenterX $_ } -Descending)
  if ($near.Count -gt 0) {
    return $near[0]
  }
  return $null
}

function Find-NamedControl([string[]] $patterns, $acceptedControlTypes) {
  $items = Get-VisibleDescendants $automationRoot
  $matches = @($items | Where-Object {
    (Test-NamePattern $_.Current.Name $patterns) -and
      ($acceptedControlTypes -contains $_.Current.ControlType)
  })
  if ($matches.Count -gt 0) {
    return $matches[0]
  }
  return $null
}

Stop-Process -Name SystemSettings -Force -ErrorAction SilentlyContinue
Start-Process -FilePath 'explorer.exe' -ArgumentList 'ms-settings:appsfeatures'
$window = Wait-Until { Get-SettingsWindow } 30000 'Timed out waiting for Windows Settings.'
try {
  $window.SetFocus()
} catch {
}
Start-Sleep -Milliseconds 1200
$search = Wait-Until { Find-SearchAppsEdit $window } 30000 'Timed out waiting for the Installed apps search field.'
Set-EditValue $search $displayName
Start-Sleep -Milliseconds 1200
$appElement = Wait-Until { Find-AppNameElement $window } 30000 ('Timed out waiting for installed app in Settings: ' + $displayName)
$moreOptions = Wait-Until { Find-MoreOptionsButton $window $appElement } 30000 'Timed out waiting for the app options button.'
Click-Element $moreOptions
Start-Sleep -Milliseconds 500
$uninstallMenuItem = Wait-Until {
  Find-NamedControl @('^Uninstall$', '^アンインストール$') @($controlTypeMenuItem, $controlTypeButton)
} 15000 'Timed out waiting for the Settings uninstall menu item.'
Invoke-Element $uninstallMenuItem
Start-Sleep -Milliseconds 800
$confirmButton = Wait-Until {
  Find-NamedControl @('^Uninstall$', '^アンインストール$') @($controlTypeButton)
} 15000 'Timed out waiting for the Settings uninstall confirmation button.'
Click-Element $confirmButton
$installDir = Join-Path $env:LOCALAPPDATA ('Programs\\' + $installDirectoryName)
$null = Wait-Until { -not (Test-Path -LiteralPath $installDir) } 90000 'Timed out waiting for the app to be removed.'
Stop-Process -Name SystemSettings -Force -ErrorAction SilentlyContinue
[ordered] @{
  clickedConfirm = $true
  clickedOptions = $true
  clickedUninstall = $true
  foundInSettings = $true
} | ConvertTo-Json -Compress
`;

const saveWindowsDiagnostics = async (
  agent: RemoteAgent,
  packageName: string,
): Promise<void> => {
  const diagnosticsDirectory = resolve(
    "test-results",
    "windows-nsis-e2e",
    packageName,
  );
  await mkdir(diagnosticsDirectory, { recursive: true });
  await saveDiagnostics(diagnosticsDirectory, {
    agent,
    captureOptions: {
      includeDescendants: true,
      maxDescendantDepth: 8,
    },
  });
};

const windowsE2eEnvironment = parseWindowsE2eEnvironment(process.env);
const describeWindowsNsis =
  windowsE2eEnvironment.status === "configured" ? describe : describe.skip;
const suiteName =
  windowsE2eEnvironment.status === "configured"
    ? "NSIS Windows Settings uninstall e2e"
    : `NSIS Windows Settings uninstall e2e (${windowsE2eEnvironment.reason})`;

const windowsAgent =
  windowsE2eEnvironment.status === "configured"
    ? await connectRemoteAgent({
        authToken: windowsE2eEnvironment.environment.token,
        host: windowsE2eEnvironment.environment.host,
        port: windowsE2eEnvironment.environment.port,
        timeoutMs: 30000,
      })
    : undefined;

if (windowsE2eEnvironment.status === "configured") {
  vi.setConfig({
    hookTimeout: 300000,
    testTimeout: 300000,
  });
}

afterEach(async () => {
  for (const directory of localCleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

afterAll(() => {
  windowsAgent?.release();
});

describeWindowsNsis(suiteName, { concurrent: false }, () => {
  it("runs muon pack on Windows and skips deb when dpkg-deb is unavailable", async () => {
    if (
      windowsAgent === undefined ||
      windowsE2eEnvironment.status !== "configured"
    ) {
      throw new Error("Windows e2e environment is not configured.");
    }

    const capabilities = await windowsAgent.capabilities();
    expect(capabilities.platform).toBe("windows");

    const suffix = randomUUID().replaceAll("-", "").slice(0, 12);
    const packageName = `muon-pack-e2e-${suffix}`;
    const remoteDirectory = await createRemoteTestDirectory(
      windowsAgent,
      windowsE2eEnvironment.environment,
      packageName,
    );

    try {
      await stageRemoteMuonPackCli(windowsAgent, remoteDirectory);
      await createRemotePlainAssetsProject(
        windowsAgent,
        remoteDirectory,
        packageName,
      );
      const packageDirectory = await createRemoteFakeLinuxPackageDist(
        windowsAgent,
        remoteDirectory,
      );
      const result = await runWindowsPowerShell(
        windowsAgent,
        remoteDirectory,
        "muon-pack-linux",
        createMuonPackCliScript(packageDirectory),
        120000,
      );
      expect(
        result.exitCode === 0 || result.exitCode === null,
        `muon pack failed.\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`,
      ).toBe(true);
      const parsed = JSON.parse(result.stdout.trim()) as MuonPackCliJsonResult;
      expect(
        parsed.artifacts.map(
          (artifact) =>
            `${artifact.type}:${artifact.target}:${win32.basename(artifact.path)}`,
        ),
      ).toEqual([`tar.gz:linux-amd64:${packageName}-1.2.3-linux-amd64.tar.gz`]);
      expect(result.stderr).toContain(
        "Warning: dpkg-deb is not available; skipping deb package for linux-amd64.",
      );
      const [artifact] = parsed.artifacts;
      expect(artifact).toBeDefined();
      await expect(
        windowsAgent.files.exists(artifact?.path ?? ""),
      ).resolves.toBe(true);
      await expect(
        windowsAgent.files.exists(
          joinWindowsPath(
            remoteDirectory,
            "artifacts",
            `${packageName}-1.2.3-amd64.deb`,
          ),
        ),
      ).resolves.toBe(false);
      await expect(
        windowsAgent.files.exists(
          joinWindowsPath(remoteDirectory, "artifacts", "deb"),
        ),
      ).resolves.toBe(false);
    } finally {
      await removeRemoteDirectoryIfExists(windowsAgent, remoteDirectory);
    }
  });

  it("is visible in Windows Settings and can be uninstalled from it", async () => {
    if (
      windowsAgent === undefined ||
      windowsE2eEnvironment.status !== "configured"
    ) {
      throw new Error("Windows e2e environment is not configured.");
    }

    await expectMakensisAvailable();

    const capabilities = await windowsAgent.capabilities();
    expect(capabilities.platform).toBe("windows");

    const suffix = randomUUID().replaceAll("-", "").slice(0, 12);
    const packageName = `muon-nsis-e2e-${suffix}`;
    const appId = `muon.nsis.e2e.${suffix}`;
    const windowsI686Identity = createWindowsNsisInstallIdentity(
      packageName,
      appId,
      "windows-i686",
    );
    const windowsAmd64Identity = createWindowsNsisInstallIdentity(
      packageName,
      appId,
      "windows-amd64",
    );
    const identities = [windowsI686Identity, windowsAmd64Identity] as const;
    const localRoot = await createTemporaryDirectory("muon-nsis-e2e-");
    const remoteDirectory = await createRemoteTestDirectory(
      windowsAgent,
      windowsE2eEnvironment.environment,
      packageName,
    );
    const packageDirectory = await createFakeMuonPackageDist(localRoot, [
      "windows-i686",
      "windows-amd64",
    ]);
    await writeViteProject(
      localRoot,
      packageDirectory,
      ["windows-i686", "windows-amd64"],
      packageName,
    );

    await runWindowsPowerShell(
      windowsAgent,
      remoteDirectory,
      "cleanup-before",
      createCleanupScript(identities),
      120000,
    );

    try {
      const result = await packMuonApp({
        appId,
        author: "muon Tester",
        environment: process.env,
        packageDirectory,
        packageName,
        packageVersion: "1.2.3",
        root: localRoot,
        targets: ["windows-i686", "windows-amd64"],
        types: ["nsis"],
      });
      const artifacts = (["windows-i686", "windows-amd64"] as const).map(
        (target) => {
          const artifact = result.artifacts.find(
            (entry) => entry.type === "nsis" && entry.target === target,
          );
          expect(artifact).toBeDefined();
          return artifact;
        },
      );

      for (const artifact of artifacts) {
        const remoteInstallerPath = joinWindowsPath(
          remoteDirectory,
          basename(artifact?.path ?? "setup.exe"),
        );
        await windowsAgent.files.writeFile(
          remoteInstallerPath,
          await readFile(artifact?.path ?? ""),
        );

        const installResult = await runWindowsPowerShell(
          windowsAgent,
          remoteDirectory,
          `installer-${basename(artifact?.path ?? "setup.exe")}`,
          createInstallerScript(remoteInstallerPath),
          180000,
        );
        expectWindowsCommandSucceeded(installResult, "NSIS installer");
      }

      const installStateResult = await runWindowsPowerShell(
        windowsAgent,
        remoteDirectory,
        "assert-installed",
        createInstallStateAssertionScript(identities),
        120000,
      );
      expectWindowsCommandSucceeded(
        installStateResult,
        "installed state assertion",
      );

      const runtimeStateFixtureResult = await runWindowsPowerShell(
        windowsAgent,
        remoteDirectory,
        "create-runtime-state-fixture",
        createRuntimeStateFixtureScript(windowsAmd64Identity.runtimeAppId),
        120000,
      );
      expectWindowsCommandSucceeded(
        runtimeStateFixtureResult,
        "runtime state fixture",
      );

      const uninstallResult = await runWindowsPowerShell(
        windowsAgent,
        remoteDirectory,
        "settings-uninstall",
        createSettingsUninstallScript(windowsAmd64Identity),
        180000,
      );
      if (!isWindowsCommandSucceeded(uninstallResult)) {
        await saveWindowsDiagnostics(
          windowsAgent,
          windowsAmd64Identity.displayName,
        );
      }
      expectWindowsCommandSucceeded(
        uninstallResult,
        "Windows Settings uninstall",
      );
      const settingsResult = JSON.parse(uninstallResult.stdout.trim()) as {
        clickedConfirm?: unknown;
        clickedOptions?: unknown;
        clickedUninstall?: unknown;
        foundInSettings?: unknown;
      };
      expect(settingsResult).toEqual({
        clickedConfirm: true,
        clickedOptions: true,
        clickedUninstall: true,
        foundInSettings: true,
      });

      const removalStateResult = await runWindowsPowerShell(
        windowsAgent,
        remoteDirectory,
        "assert-removed",
        createRemovalStateAssertionScript(
          windowsAmd64Identity,
          windowsI686Identity,
        ),
        120000,
      );
      expectWindowsCommandSucceeded(
        removalStateResult,
        "removed state assertion",
      );
    } finally {
      await runWindowsPowerShell(
        windowsAgent,
        remoteDirectory,
        "cleanup-after",
        createCleanupScript(identities),
        120000,
      );
      await removeRemoteDirectoryIfExists(windowsAgent, remoteDirectory);
    }
  });
});
