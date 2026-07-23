/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <string>
#include <vector>

/**
 * One compiled token from a muon allow glob.
 */
struct MuonGlobToken {
  /**
   * Token matching behavior.
   */
  enum Kind {
    /**
     * Matches one exact byte.
     */
    literal,
    /**
     * Matches zero or more non-separator bytes.
     */
    wildcard,
    /**
     * Matches zero or more bytes, including separators.
     */
    deep_wildcard,
  };

  Kind kind = literal;
  char literal_value = '\0';
};

/**
 * Compiled muon allow glob.
 */
struct MuonGlob {
  /**
   * Pattern tokens after validation and escape processing.
   */
  std::vector<MuonGlobToken> tokens;
  /**
   * Characters that a single wildcard cannot consume.
   */
  std::string separators;
};

/**
 * Compiles and validates a muon allow glob pattern.
 *
 * @param pattern Raw glob pattern from muon.json.
 * @param separators Characters that `*` cannot consume.
 * @param glob Receives the compiled glob.
 * @param error_message Receives a validation error on failure.
 * @return true when the pattern is valid and compiled.
 */
bool CompileMuonGlob(const std::string& pattern,
                     const std::string& separators,
                     MuonGlob* glob,
                     std::string* error_message);

/**
 * Returns whether a target string fully matches a compiled muon glob.
 *
 * @param glob Compiled glob pattern.
 * @param target Target string to test.
 * @return true when the full target string matches.
 */
bool IsMuonGlobMatch(const MuonGlob& glob, const std::string& target);
