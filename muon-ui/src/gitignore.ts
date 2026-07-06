// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";

const muonGitignoreEntries = [".muon/", "dist-muon/", "artifacts/"] as const;

type MuonGitignoreEntry = (typeof muonGitignoreEntries)[number];

const muonGitignoreEntryAliases: Record<MuonGitignoreEntry, readonly string[]> =
  {
    ".muon/": [".muon/", "/.muon/", ".muon", "/.muon"],
    "dist-muon/": [
      "dist-muon/",
      "/dist-muon/",
      "dist-muon",
      "/dist-muon",
      "dist-muon/*",
      "/dist-muon/*",
    ],
    "artifacts/": ["artifacts/", "/artifacts/", "artifacts", "/artifacts"],
  };

/**
 * Result of ensuring muon generated directories are ignored by Git.
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

const hasMuonGitignoreEntry = (
  content: string,
  entry: MuonGitignoreEntry,
): boolean =>
  content
    .split(/\r?\n/)
    .map((line) => line.trim())
    .some((line) => muonGitignoreEntryAliases[entry].includes(line));

/**
 * Adds muon generated directories to a project .gitignore file.
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

  const missingEntries = muonGitignoreEntries.filter(
    (entry) => !hasMuonGitignoreEntry(content, entry),
  );
  if (missingEntries.length === 0) {
    return {
      gitignorePath,
      changed: false,
    };
  }

  const separator = content.length > 0 && !content.endsWith("\n") ? "\n" : "";
  await writeFile(
    gitignorePath,
    `${content}${separator}${missingEntries.join("\n")}\n`,
  );
  return {
    gitignorePath,
    changed: true,
  };
};
