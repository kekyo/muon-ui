// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";

const muonGitignoreEntry = ".muon/";

/**
 * Result of ensuring the Muon staging directory is ignored by Git.
 */
export interface MuonGitignoreResult {
  /**
   * Path of the .gitignore file that was checked or updated.
   */
  gitignorePath: string;

  /**
   * True when the file was created or appended.
   */
  changed: boolean;
}

const isMissingFileError = (error: unknown): boolean =>
  error instanceof Error && (error as NodeJS.ErrnoException).code === "ENOENT";

const hasMuonGitignoreEntry = (content: string): boolean =>
  content
    .split(/\r?\n/)
    .map((line) => line.trim())
    .some(
      (line) =>
        line === muonGitignoreEntry ||
        line === `/${muonGitignoreEntry}` ||
        line === ".muon" ||
        line === "/.muon",
    );

/**
 * Adds the Muon staging directory to a project .gitignore file.
 *
 * @param root Project root containing the .gitignore file.
 * @returns Gitignore update result.
 */
export const ensureMuonGitignoreEntry = async (
  root: string,
): Promise<MuonGitignoreResult> => {
  const gitignorePath = join(root, ".gitignore");
  let content = "";
  try {
    content = await readFile(gitignorePath, "utf8");
  } catch (error) {
    if (!isMissingFileError(error)) {
      throw error;
    }
  }

  if (hasMuonGitignoreEntry(content)) {
    return {
      gitignorePath,
      changed: false,
    };
  }

  const separator = content.length > 0 && !content.endsWith("\n") ? "\n" : "";
  await writeFile(
    gitignorePath,
    `${content}${separator}${muonGitignoreEntry}\n`,
  );
  return {
    gitignorePath,
    changed: true,
  };
};
