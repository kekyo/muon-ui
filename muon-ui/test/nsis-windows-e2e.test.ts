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

const createFakeMuonPackageDist = async (
  root: string,
  target: MuonBuildTarget,
): Promise<string> => {
  const packageDirectory = join(root, "package-dist");
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
  return packageDirectory;
};

const writeViteProject = async (
  root: string,
  packageDirectory: string,
  target: MuonBuildTarget,
  packageName: string,
): Promise<void> => {
  const vitePluginUrl = pathToFileURL(resolve("dist", "vite.mjs")).href;
  await writeFile(
    join(root, "package.json"),
    `${JSON.stringify(
      {
        author: "Muon Tester",
        description: "Muon NSIS e2e sample",
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
      `    muon({ build: { targets: [${JSON.stringify(target)}], packageDirectory: ${JSON.stringify(packageDirectory)} } }),`,
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

const createCleanupScript = (packageName: string, appId: string): string => `
$ErrorActionPreference = 'Stop'
$packageName = ${quotePowerShellString(packageName)}
$appId = ${quotePowerShellString(appId)}
$installDir = Join-Path $env:LOCALAPPDATA ('Programs\\' + $packageName)
$stateDir = Join-Path $env:LOCALAPPDATA $appId
$shortcutPath = Join-Path $env:APPDATA ('Microsoft\\Windows\\Start Menu\\Programs\\' + $packageName + '.lnk')
$registryPath = 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\' + $appId
$uninstaller = Join-Path $installDir 'Uninstall.exe'
Stop-Process -Name SystemSettings -Force -ErrorAction SilentlyContinue
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
`;

const createInstallStateAssertionScript = (packageName: string): string => `
$ErrorActionPreference = 'Stop'
$packageName = ${quotePowerShellString(packageName)}
$installDir = Join-Path $env:LOCALAPPDATA ('Programs\\' + $packageName)
$shortcutPath = Join-Path $env:APPDATA ('Microsoft\\Windows\\Start Menu\\Programs\\' + $packageName + '.lnk')
$errors = New-Object System.Collections.Generic.List[string]
if (-not (Test-Path -LiteralPath $installDir)) {
  $errors.Add('install directory is missing: ' + $installDir)
}
if (-not (Test-Path -LiteralPath $shortcutPath)) {
  $errors.Add('start menu shortcut is missing: ' + $shortcutPath)
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
  packageName: string,
  appId: string,
): string => `
$ErrorActionPreference = 'Stop'
$packageName = ${quotePowerShellString(packageName)}
$appId = ${quotePowerShellString(appId)}
$installDir = Join-Path $env:LOCALAPPDATA ('Programs\\' + $packageName)
$stateDir = Join-Path $env:LOCALAPPDATA $appId
$shortcutPath = Join-Path $env:APPDATA ('Microsoft\\Windows\\Start Menu\\Programs\\' + $packageName + '.lnk')
$registryPath = 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\' + $appId
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

const createSettingsUninstallScript = (packageName: string): string => `
$ErrorActionPreference = 'Stop'
$packageName = ${quotePowerShellString(packageName)}
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
  $exact = @($items | Where-Object { $_.Current.Name -eq $packageName })
  if ($exact.Count -gt 0) {
    return $exact[0]
  }
  $contains = @($items | Where-Object { $_.Current.Name -like ('*' + $packageName + '*') })
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
Set-EditValue $search $packageName
Start-Sleep -Milliseconds 1200
$appElement = Wait-Until { Find-AppNameElement $window } 30000 ('Timed out waiting for installed app in Settings: ' + $packageName)
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
$installDir = Join-Path $env:LOCALAPPDATA ('Programs\\' + $packageName)
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
    const localRoot = await createTemporaryDirectory("muon-nsis-e2e-");
    const remoteDirectory = await createRemoteTestDirectory(
      windowsAgent,
      windowsE2eEnvironment.environment,
      packageName,
    );
    const packageDirectory = await createFakeMuonPackageDist(
      localRoot,
      "windows-amd64",
    );
    await writeViteProject(
      localRoot,
      packageDirectory,
      "windows-amd64",
      packageName,
    );

    await runWindowsPowerShell(
      windowsAgent,
      remoteDirectory,
      "cleanup-before",
      createCleanupScript(packageName, appId),
      120000,
    );

    try {
      const result = await packMuonApp({
        appId,
        author: "Muon Tester",
        environment: process.env,
        packageDirectory,
        packageName,
        packageVersion: "1.2.3",
        root: localRoot,
        targets: ["windows-amd64"],
        types: ["nsis"],
      });
      const artifact = result.artifacts.find((entry) => entry.type === "nsis");
      expect(artifact).toBeDefined();

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
        "installer",
        createInstallerScript(remoteInstallerPath),
        180000,
      );
      expectWindowsCommandSucceeded(installResult, "NSIS installer");

      const installStateResult = await runWindowsPowerShell(
        windowsAgent,
        remoteDirectory,
        "assert-installed",
        createInstallStateAssertionScript(packageName),
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
        createRuntimeStateFixtureScript(appId),
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
        createSettingsUninstallScript(packageName),
        180000,
      );
      if (!isWindowsCommandSucceeded(uninstallResult)) {
        await saveWindowsDiagnostics(windowsAgent, packageName);
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
        createRemovalStateAssertionScript(packageName, appId),
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
        createCleanupScript(packageName, appId),
        120000,
      );
      await removeRemoteDirectoryIfExists(windowsAgent, remoteDirectory);
    }
  });
});
