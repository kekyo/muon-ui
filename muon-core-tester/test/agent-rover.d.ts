// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

declare module "agent-rover" {
  export interface ScreenRect {
    readonly height: number;
    readonly width: number;
    readonly x: number;
    readonly y: number;
  }

  export interface RemoteAgentCapabilities {
    readonly features: readonly string[];
    readonly platform: "windows";
    readonly protocolVersion: string;
  }

  export interface RemoteApplicationLaunchOptions {
    readonly arguments?: readonly string[];
    readonly createNoWindow?: boolean;
    readonly environment?: Readonly<Record<string, string>>;
    readonly path: string;
    readonly stderrPath?: string;
    readonly stdoutPath?: string;
    readonly workingDirectory?: string;
  }

  export interface RemoteApplicationProcess {
    readonly id: number;
    readonly name: string;
  }

  export interface RemoteApplications {
    readonly launch: (
      options: RemoteApplicationLaunchOptions,
    ) => Promise<RemoteApplicationProcess>;
  }

  export interface RemoteProcesses {
    readonly kill: (processId: number) => Promise<void>;
    readonly list: (options?: {
      readonly name?: string;
    }) => Promise<readonly RemoteProcessSnapshot[]>;
    readonly snapshot: (processId: number) => Promise<RemoteProcessSnapshot>;
    readonly waitForExit: (
      processId: number,
      options?: {
        readonly intervalMs?: number;
        readonly message?: string;
        readonly timeoutMs?: number;
      },
    ) => Promise<RemoteProcessSnapshot>;
  }

  export interface RemoteProcessSnapshot {
    readonly exitCode: number | null;
    readonly id: number;
    readonly name: string;
    readonly path: string;
    readonly running: boolean;
  }

  export interface RemoteFileSystem {
    readonly exists: (path: string) => Promise<boolean>;
    readonly mkdir: (
      path: string,
      options?: { readonly recursive?: boolean },
    ) => Promise<void>;
    readonly mkdtemp: (prefix: string) => Promise<string>;
    readonly readFile: (path: string) => Promise<Buffer>;
    readonly readdir: (path: string) => Promise<
      readonly Array<{
        readonly name: string;
        readonly size: number;
        readonly type: "directory" | "file" | "other";
      }>
    >;
    readonly remove: (
      path: string,
      options?: { readonly recursive?: boolean },
    ) => Promise<void>;
    readonly rename: (from: string, to: string) => Promise<void>;
    readonly stat: (path: string) => Promise<unknown>;
    readonly writeFile: (path: string, data: Buffer) => Promise<void>;
  }

  export interface RemoteKeyboard {
    readonly press: (
      key: string,
      options?: { readonly modifiers?: readonly KeyboardModifier[] },
    ) => Promise<void>;
    readonly down: (key: string) => Promise<void>;
    readonly pasteText: (
      text: string,
      options?: {
        readonly restoreClipboard?: boolean;
        readonly restoreDelayMs?: number;
      },
    ) => Promise<void>;
    readonly type: (text: string) => Promise<void>;
    readonly up: (key: string) => Promise<void>;
  }

  export interface RemoteMouse {
    readonly click: (
      point: { readonly x: number; readonly y: number },
      options?: { readonly button?: string },
    ) => Promise<void>;
    readonly drag: (
      from: { readonly x: number; readonly y: number },
      to: { readonly x: number; readonly y: number },
      options?: {
        readonly button?: MouseButton;
        readonly modifiers?: readonly KeyboardModifier[];
      },
    ) => Promise<void>;
    readonly move: (point: {
      readonly x: number;
      readonly y: number;
    }) => Promise<void>;
    readonly wheel: (options: {
      readonly deltaX?: number;
      readonly deltaY?: number;
      readonly point?: { readonly x: number; readonly y: number } | null;
    }) => Promise<void>;
  }

  export type KeyboardModifier = "Alt" | "Control" | "Meta" | "Shift";

  export type MouseButton = "left" | "middle" | "right";

  export interface RemoteScreenshot {
    readonly bounds: ScreenRect;
    readonly clipped: boolean;
    readonly image: Buffer;
    readonly visibleBounds: ScreenRect;
  }

  export interface EventLogEntry {
    readonly id: number;
    readonly level: string;
    readonly message: string;
    readonly provider: string;
    readonly timestamp: string;
  }

  export interface EventLogQuery {
    readonly level?: string;
    readonly maxEntries?: number;
    readonly provider?: string;
    readonly since?: string;
  }

  export interface RemoteDiagnosticsWindow extends AppWindow {
    readonly children?: readonly RemoteDiagnosticsWindow[];
  }

  export interface RemoteDiagnosticsCaptureOptions {
    readonly eventLogs?: EventLogQuery;
    readonly includeDescendants?: boolean;
    readonly maxDescendantDepth?: number;
    readonly screenshot?: {
      readonly bounds?: ScreenRect;
    };
  }

  export interface RemoteDiagnosticsCapture {
    readonly activeWindow: AppWindow | null;
    readonly bounds: ScreenRect;
    readonly capturedAt: string;
    readonly eventLogs: readonly EventLogEntry[];
    readonly inputOperations: readonly unknown[];
    readonly monitors: readonly unknown[];
    readonly protocolOperations: readonly unknown[];
    readonly screenshot: RemoteScreenshot;
    readonly windows: readonly RemoteDiagnosticsWindow[];
  }

  export interface RemoteDiagnosticsArtifact {
    readonly contentType: string;
    readonly kind: string;
    readonly path: string;
  }

  export interface RemoteDiagnosticsSaveResult {
    readonly artifacts: readonly RemoteDiagnosticsArtifact[];
    readonly directory: string;
    readonly manifestPath: string;
    readonly screenshotPath: string;
  }

  export interface SaveDiagnosticsOptions {
    readonly agent?: RemoteAgent;
    readonly capture?: RemoteDiagnosticsCapture;
    readonly captureOptions?: RemoteDiagnosticsCaptureOptions;
  }

  export interface RemoteDiagnostics {
    readonly capture: (
      options?: RemoteDiagnosticsCaptureOptions,
    ) => Promise<RemoteDiagnosticsCapture>;
  }

  export interface AppWindowProcess {
    readonly id: number;
    readonly name: string;
  }

  export interface AppWindow {
    readonly active: boolean;
    readonly bounds: ScreenRect;
    readonly className: string;
    readonly controlId: number;
    readonly enabled: boolean;
    readonly focused: boolean;
    readonly id: string;
    readonly maximized: boolean;
    readonly minimized: boolean;
    readonly process: AppWindowProcess;
    readonly title: string;
    readonly visible: boolean;
    readonly activate: () => Promise<AppWindow>;
    readonly children: () => Promise<readonly AppWindow[]>;
    readonly close: () => Promise<void>;
    readonly descendants: (options?: {
      readonly maxDepth?: number;
    }) => Promise<readonly AppWindow[]>;
    readonly findDescendants: (
      query: RemoteWindowQuery,
    ) => Promise<readonly AppWindow[]>;
    readonly focus: () => Promise<AppWindow>;
    readonly maximize: () => Promise<AppWindow>;
    readonly minimize: () => Promise<AppWindow>;
    readonly refresh: () => Promise<AppWindow>;
    readonly restore: () => Promise<AppWindow>;
    readonly screenshot: () => Promise<RemoteScreenshot>;
    readonly setBounds: (bounds: ScreenRect) => Promise<AppWindow>;
    readonly waitForClosed: (options?: RemoteWaitOptions) => Promise<void>;
    readonly waitForHidden: (options?: RemoteWaitOptions) => Promise<void>;
    readonly waitForStableBounds: (
      options?: RemoteWaitOptions & { readonly stableIterations?: number },
    ) => Promise<AppWindow>;
    readonly waitForVisible: (
      options?: RemoteWaitOptions,
    ) => Promise<AppWindow>;
  }

  export interface RemoteWindowQuery {
    readonly active?: boolean;
    readonly className?: string;
    readonly controlId?: number;
    readonly focused?: boolean;
    readonly includeDescendants?: boolean;
    readonly processId?: number;
    readonly processName?: string;
    readonly strict?: boolean;
    readonly title?: string;
    readonly titleRegex?: RegExp;
    readonly visible?: boolean;
  }

  export interface RemoteWaitOptions {
    readonly intervalMs?: number;
    readonly message?: string;
    readonly timeoutMs?: number;
  }

  export interface RemoteAgent {
    readonly applications: RemoteApplications;
    readonly capabilities: () => Promise<RemoteAgentCapabilities>;
    readonly diagnostics: RemoteDiagnostics;
    readonly files: RemoteFileSystem;
    readonly keyboard: RemoteKeyboard;
    readonly mouse: RemoteMouse;
    readonly processes: RemoteProcesses;
    readonly release: () => void;
    readonly screenshot: () => Promise<RemoteScreenshot>;
    readonly waitForNoWindow: (
      query: RemoteWindowQuery,
      options?: RemoteWaitOptions,
    ) => Promise<void>;
    readonly waitForWindow: (
      query: RemoteWindowQuery,
      options?: RemoteWaitOptions,
    ) => Promise<AppWindow>;
    readonly windows: () => Promise<readonly AppWindow[]>;
  }

  export interface ConnectRemoteAgentOptions {
    readonly authToken?: string;
    readonly host: string;
    readonly port: number;
    readonly timeoutMs?: number;
  }

  export const connectRemoteAgent: (
    options: ConnectRemoteAgentOptions,
  ) => Promise<RemoteAgent>;

  export const saveDiagnostics: (
    directory: string,
    options: SaveDiagnosticsOptions,
  ) => Promise<RemoteDiagnosticsSaveResult>;
}
