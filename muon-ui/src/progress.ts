// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

/**
 * Progress event used by internal muon build, pack, and prepare operations.
 *
 * @internal
 */
export interface MuonProgressEvent {
  /**
   * Stable phase identifier.
   *
   * @internal
   */
  phase: string;

  /**
   * Human-readable single-line status.
   *
   * @internal
   */
  status: string;

  /**
   * Current progress value.
   *
   * @internal
   */
  current?: number;

  /**
   * Total progress value.
   *
   * @internal
   */
  total?: number;

  /**
   * True when current and total form bounded progress.
   *
   * @internal
   */
  determinate?: boolean;
}

/**
 * Receives internal muon progress events.
 *
 * @internal
 */
export type MuonProgressCallback = (event: MuonProgressEvent) => void;

/**
 * Renders internal muon progress events to stderr.
 *
 * @internal
 */
export interface MuonProgressRenderer {
  /**
   * Render a progress event.
   *
   * @internal
   */
  report(event: MuonProgressEvent): void;

  /**
   * Complete the active status line.
   *
   * @internal
   */
  flush(): void;
}

const spinnerFrames = ["-", "\\", "|", "/"] as const;
const spinnerIntervalMilliseconds = 500;
const terminalLineStart = "\r";
const terminalClearLine = "\x1b[K";

const clampProgressValue = (value: number, total: number): number =>
  Math.max(0, Math.min(value, total));

const formatProgressEvent = (event: MuonProgressEvent): string => {
  const status = event.status.trimEnd();
  const current = event.current ?? 0;
  const total = event.total ?? 0;
  if (event.determinate === true && total > 0) {
    const percentage = Math.floor(
      (clampProgressValue(current, total) / total) * 100,
    );
    return `${status} ${percentage}%`;
  }
  if (event.phase === "installing" && current > 0) {
    return `${status} ${Math.trunc(current)} files`;
  }
  return status;
};

interface ActiveProgressLine {
  key: string;
  text: string;
}

const getProgressLineKey = (event: MuonProgressEvent): string =>
  `${event.phase}\0${event.status.trimEnd()}`;

const createSpinnerProgressRenderer = (): MuonProgressRenderer => {
  let activeLine: ActiveProgressLine | undefined = undefined;
  let frameIndex = 0;
  let timer: NodeJS.Timeout | undefined = undefined;

  const writeRaw = (chunk: string): void => {
    process.stderr.write(chunk);
  };

  const stopTimer = (): void => {
    if (timer !== undefined) {
      clearInterval(timer);
      timer = undefined;
    }
  };

  const renderLine = (): void => {
    if (activeLine === undefined) {
      return;
    }
    const frame = spinnerFrames[frameIndex] ?? spinnerFrames[0];
    writeRaw(
      `${terminalLineStart}${frame} ${activeLine.text}${terminalClearLine}`,
    );
    frameIndex = (frameIndex + 1) % spinnerFrames.length;
  };

  const ensureTimer = (): void => {
    if (timer === undefined) {
      timer = setInterval(renderLine, spinnerIntervalMilliseconds);
      timer.unref();
    }
  };

  const finishLine = (): void => {
    if (activeLine === undefined) {
      return;
    }
    stopTimer();
    const text = activeLine.text;
    activeLine = undefined;
    frameIndex = 0;
    writeRaw(`${terminalLineStart}${text}${terminalClearLine}\n`);
  };

  return {
    report: (event): void => {
      const nextLine = {
        key: getProgressLineKey(event),
        text: formatProgressEvent(event),
      };
      if (activeLine === undefined) {
        activeLine = nextLine;
        renderLine();
        ensureTimer();
        return;
      }
      if (activeLine.key !== nextLine.key) {
        renderLine();
        activeLine = nextLine;
        renderLine();
      } else {
        activeLine = nextLine;
      }
      ensureTimer();
    },
    flush: finishLine,
  };
};

/**
 * Creates a stderr progress renderer for the current terminal mode.
 *
 * @internal
 */
export const createMuonProgressRenderer = (): MuonProgressRenderer =>
  createSpinnerProgressRenderer();
