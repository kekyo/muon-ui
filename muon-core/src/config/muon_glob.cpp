/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "config/muon_glob.h"

#include <cstddef>
#include <utility>

static bool IsMuonGlobSeparator(char value, const std::string& separators) {
  return separators.find(value) != std::string::npos;
}

static MuonGlobToken CreateMuonGlobLiteralToken(char value) {
  MuonGlobToken token;
  token.kind = MuonGlobToken::literal;
  token.literal_value = value;
  return token;
}

static MuonGlobToken CreateMuonGlobWildcardToken() {
  MuonGlobToken token;
  token.kind = MuonGlobToken::wildcard;
  return token;
}

static MuonGlobToken CreateMuonGlobDeepWildcardToken() {
  MuonGlobToken token;
  token.kind = MuonGlobToken::deep_wildcard;
  return token;
}

static bool IsMuonGlobDeepWildcardBoundaryBefore(
    const std::vector<MuonGlobToken>& tokens,
    const std::string& separators) {
  if (tokens.empty()) {
    return true;
  }
  const auto& token = tokens.back();
  return token.kind == MuonGlobToken::literal &&
         IsMuonGlobSeparator(token.literal_value, separators);
}

static bool IsMuonGlobDeepWildcardBoundaryAfter(
    const std::string& pattern,
    size_t next_index,
    const std::string& separators,
    std::string* error_message) {
  if (next_index >= pattern.size()) {
    return true;
  }
  auto value = pattern[next_index];
  if (value == '\\') {
    if (next_index + 1 >= pattern.size()) {
      *error_message = "escape must be followed by a character";
      return false;
    }
    value = pattern[next_index + 1];
  }
  if (IsMuonGlobSeparator(value, separators)) {
    return true;
  }
  *error_message = "** must be surrounded by separators or pattern edges";
  return false;
}

static bool MatchMuonGlobFrom(const MuonGlob& glob,
                              const std::string& target,
                              size_t token_index,
                              size_t target_index,
                              std::vector<int>* memo) {
  const auto target_slots = target.size() + 1;
  const auto memo_index = token_index * target_slots + target_index;
  if ((*memo)[memo_index] != -1) {
    return (*memo)[memo_index] == 1;
  }

  auto matched = false;
  if (token_index >= glob.tokens.size()) {
    matched = target_index == target.size();
  } else {
    const auto& token = glob.tokens[token_index];
    if (token.kind == MuonGlobToken::literal) {
      matched = target_index < target.size() &&
                target[target_index] == token.literal_value &&
                MatchMuonGlobFrom(glob, target, token_index + 1,
                                  target_index + 1, memo);
    } else if (token.kind == MuonGlobToken::wildcard) {
      auto next_target_index = target_index;
      while (true) {
        if (MatchMuonGlobFrom(glob, target, token_index + 1,
                              next_target_index, memo)) {
          matched = true;
          break;
        }
        if (next_target_index >= target.size() ||
            IsMuonGlobSeparator(target[next_target_index], glob.separators)) {
          break;
        }
        ++next_target_index;
      }
    } else {
      for (auto next_target_index = target_index;
           next_target_index <= target.size();
           ++next_target_index) {
        if (MatchMuonGlobFrom(glob, target, token_index + 1,
                              next_target_index, memo)) {
          matched = true;
          break;
        }
      }
    }
  }

  (*memo)[memo_index] = matched ? 1 : 0;
  return matched;
}

bool CompileMuonGlob(const std::string& pattern,
                     const std::string& separators,
                     MuonGlob* glob,
                     std::string* error_message) {
  if (glob == nullptr || error_message == nullptr) {
    return false;
  }
  if (pattern.empty()) {
    *error_message = "pattern must not be empty";
    return false;
  }

  MuonGlob compiled;
  compiled.separators = separators;
  for (auto index = size_t{0}; index < pattern.size();) {
    const auto value = pattern[index];
    if (value == '\\') {
      if (index + 1 >= pattern.size()) {
        *error_message = "escape must be followed by a character";
        return false;
      }
      compiled.tokens.push_back(
          CreateMuonGlobLiteralToken(pattern[index + 1]));
      index += 2;
      continue;
    }
    if (value != '*') {
      compiled.tokens.push_back(CreateMuonGlobLiteralToken(value));
      ++index;
      continue;
    }

    const auto first_star_index = index;
    while (index < pattern.size() && pattern[index] == '*') {
      ++index;
    }
    const auto star_count = index - first_star_index;
    if (star_count > 2) {
      *error_message = "three or more consecutive unescaped * are not allowed";
      return false;
    }
    if (star_count == 1) {
      compiled.tokens.push_back(CreateMuonGlobWildcardToken());
      continue;
    }

    if (!IsMuonGlobDeepWildcardBoundaryBefore(compiled.tokens, separators)) {
      *error_message = "** must be surrounded by separators or pattern edges";
      return false;
    }
    if (!IsMuonGlobDeepWildcardBoundaryAfter(pattern, index, separators,
                                            error_message)) {
      return false;
    }
    compiled.tokens.push_back(CreateMuonGlobDeepWildcardToken());
  }

  *glob = std::move(compiled);
  error_message->clear();
  return true;
}

bool IsMuonGlobMatch(const MuonGlob& glob, const std::string& target) {
  auto memo = std::vector<int>((glob.tokens.size() + 1) * (target.size() + 1),
                               -1);
  return MatchMuonGlobFrom(glob, target, 0, 0, &memo);
}
