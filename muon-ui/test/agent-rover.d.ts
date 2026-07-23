// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

declare module "agent-rover" {
  export interface ScreenRect {
    readonly height: number;
    readonly width: number;
    readonly x: number;
    readonly y: number;
  }

  export interface ScreenPoint {
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

  export interface RemoteManagedProcessLaunchOptions {
    readonly arguments?: readonly string[];
    readonly captureStderr?: boolean;
    readonly captureStdout?: boolean;
    readonly createNoWindow?: boolean;
    readonly environment?: Readonly<Record<string, string>>;
    readonly killTreeOnRelease?: boolean;
    readonly path: string;
    readonly workingDirectory?: string;
  }

  export interface RemoteManagedProcessSnapshot {
    readonly processes: readonly RemoteProcessSnapshot[];
    readonly root: RemoteProcessSnapshot;
    readonly running: boolean;
  }

  export interface RemoteManagedProcess extends RemoteApplicationProcess {
    readonly kill: () => Promise<void>;
    readonly processes: () => Promise<readonly RemoteProcessSnapshot[]>;
    readonly releaseAsync: () => Promise<void>;
    readonly rootSnapshot: () => Promise<RemoteProcessSnapshot>;
    readonly snapshot: () => Promise<RemoteManagedProcessSnapshot>;
    readonly stderrText: () => Promise<string>;
    readonly stdoutText: () => Promise<string>;
    readonly waitForExit: (
      options?: RemoteWaitOptions,
    ) => Promise<RemoteManagedProcessSnapshot>;
    readonly waitForNoWindow: (
      query?: RemoteWindowQuery,
      options?: RemoteWaitOptions,
    ) => Promise<void>;
    readonly waitForWindow: (
      query: RemoteWindowQuery,
      options?: RemoteWaitOptions,
    ) => Promise<unknown>;
    readonly windows: (
      query?: RemoteWindowQuery,
    ) => Promise<readonly unknown[]>;
  }

  export interface RemoteWaitOptions {
    readonly intervalMs?: number;
    readonly message?: string;
    readonly timeoutMs?: number;
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

  export type RemoteFileType = "directory" | "file" | "other";

  export interface RemoteFileStat {
    readonly createdAt: string;
    readonly modifiedAt: string;
    readonly size: number;
    readonly type: RemoteFileType;
  }

  export interface RemoteDirectoryEntry extends RemoteFileStat {
    readonly name: string;
  }

  export type RemoteDirectorySyncMode = "mirror" | "update";

  export type RemoteDirectoryChecksum = "sha256";

  export type RemoteLockedFilePolicy =
    | "fail"
    | "retry"
    | "killRelatedProcessesAndRetry";

  export interface RemoteDirectorySyncOptions {
    readonly checksum?: RemoteDirectoryChecksum;
    readonly deleteExtraneous?: boolean;
    readonly exclude?: readonly string[];
    readonly include?: readonly string[];
    readonly localPath: string;
    readonly mode?: RemoteDirectorySyncMode;
    readonly onLockedFile?: RemoteLockedFilePolicy;
    readonly relatedProcessPaths?: readonly string[];
    readonly remotePath: string;
  }

  export interface RemoteDirectorySyncResult {
    readonly bytesUploaded: number;
    readonly createdDirectories: number;
    readonly deletedDirectories: number;
    readonly deletedFiles: number;
    readonly skippedFiles: number;
    readonly uploadedFiles: number;
  }

  export interface RemoteDirectoryDownloadOptions {
    readonly exclude?: readonly string[];
    readonly ignoreMissing?: boolean;
    readonly include?: readonly string[];
    readonly localPath: string;
    readonly remotePath: string;
  }

  export interface RemoteDirectoryDownloadResult {
    readonly bytesDownloaded: number;
    readonly createdDirectories: number;
    readonly downloadedFiles: number;
  }

  export interface RemoteFileSystem {
    readonly downloadDirectory: (
      options: RemoteDirectoryDownloadOptions,
    ) => Promise<RemoteDirectoryDownloadResult>;
    readonly exists: (path: string) => Promise<boolean>;
    readonly mkdir: (
      path: string,
      options?: { readonly recursive?: boolean },
    ) => Promise<void>;
    readonly mkdtemp: (prefix: string) => Promise<string>;
    readonly readFile: (path: string) => Promise<Buffer>;
    readonly readdir: (
      path: string,
    ) => Promise<readonly RemoteDirectoryEntry[]>;
    readonly remove: (
      path: string,
      options?: { readonly recursive?: boolean },
    ) => Promise<void>;
    readonly rename: (from: string, to: string) => Promise<void>;
    readonly stat: (path: string) => Promise<RemoteFileStat>;
    readonly syncDirectory: (
      options: RemoteDirectorySyncOptions,
    ) => Promise<RemoteDirectorySyncResult>;
    readonly writeFile: (path: string, data: Buffer) => Promise<void>;
  }

  export interface RemoteProcesses {
    readonly exists: (processId: number) => Promise<boolean>;
    readonly kill: (processId: number) => Promise<void>;
    readonly launchManaged: (
      options: RemoteManagedProcessLaunchOptions,
    ) => Promise<RemoteManagedProcess>;
    readonly list: (options?: {
      readonly name?: string;
    }) => Promise<readonly RemoteProcessSnapshot[]>;
    readonly snapshot: (processId: number) => Promise<RemoteProcessSnapshot>;
    readonly waitForExit: (
      processId: number,
      options?: RemoteWaitOptions,
    ) => Promise<RemoteProcessSnapshot>;
  }

  export interface RemoteProcessSnapshot {
    readonly createdAt: string | null;
    readonly exitCode: number | null;
    readonly id: number;
    readonly name: string;
    readonly parentProcessId: number | null;
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
