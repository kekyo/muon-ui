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
    readonly list: () => Promise<readonly unknown[]>;
    readonly snapshot: (processId: number) => Promise<unknown>;
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
    readonly stat: (path: string) => Promise<unknown>;
    readonly writeFile: (path: string, data: Buffer) => Promise<void>;
  }

  export interface RemoteKeyboard {
    readonly press: (
      key: string,
      options?: { readonly modifiers?: readonly string[] },
    ) => Promise<void>;
  }

  export interface RemoteMouse {
    readonly click: (
      point: { readonly x: number; readonly y: number },
      options?: { readonly button?: string },
    ) => Promise<void>;
  }

  export interface RemoteScreenshot {
    readonly bounds: ScreenRect;
    readonly clipped: boolean;
    readonly image: Buffer;
    readonly visibleBounds: ScreenRect;
  }

  export interface RemoteAgent {
    readonly applications: RemoteApplications;
    readonly capabilities: () => Promise<RemoteAgentCapabilities>;
    readonly files: RemoteFileSystem;
    readonly keyboard: RemoteKeyboard;
    readonly mouse: RemoteMouse;
    readonly processes: RemoteProcesses;
    readonly release: () => void;
    readonly screenshot: () => Promise<RemoteScreenshot>;
    readonly windows: () => Promise<readonly unknown[]>;
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
}
