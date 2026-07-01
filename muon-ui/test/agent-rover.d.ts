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

  export interface RemoteFileSystem {
    readonly exists: (path: string) => Promise<boolean>;
    readonly mkdir: (
      path: string,
      options?: { readonly recursive?: boolean },
    ) => Promise<void>;
    readonly readFile: (path: string) => Promise<Buffer>;
    readonly remove: (
      path: string,
      options?: { readonly recursive?: boolean },
    ) => Promise<void>;
    readonly writeFile: (path: string, data: Buffer) => Promise<void>;
  }

  export interface RemoteProcesses {
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

  export interface RemoteScreenshot {
    readonly bounds: ScreenRect;
    readonly clipped: boolean;
    readonly image: Buffer;
    readonly visibleBounds: ScreenRect;
  }

  export interface RemoteDiagnosticsCaptureOptions {
    readonly includeDescendants?: boolean;
    readonly maxDescendantDepth?: number;
    readonly screenshot?: {
      readonly bounds?: ScreenRect;
    };
  }

  export interface SaveDiagnosticsOptions {
    readonly agent?: RemoteAgent;
    readonly captureOptions?: RemoteDiagnosticsCaptureOptions;
  }

  export interface RemoteAgent {
    readonly applications: RemoteApplications;
    readonly capabilities: () => Promise<RemoteAgentCapabilities>;
    readonly files: RemoteFileSystem;
    readonly processes: RemoteProcesses;
    readonly release: () => void;
    readonly screenshot: () => Promise<RemoteScreenshot>;
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
  ) => Promise<unknown>;
}
