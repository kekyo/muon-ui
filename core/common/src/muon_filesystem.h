/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace muon_internal {

/** Selects the path spelling accepted by a native filesystem operation. */
enum class FilesystemIoUsage {
  /** Ordinary file operations; extend when needed. */
  File,
  /** Directory creation and enumeration; always extend on Windows. */
  Tree,
};

/**
 * Decodes an application path without interpreting it or resolving its base.
 * @param value UTF-8 bytes, including relative paths and exact native spelling.
 * @return Native filesystem path; invalid Unicode conversion throws.
 */
std::filesystem::path FilesystemPathFromUtf8(std::string_view value);

/**
 * Encodes a native path without changing its namespace or components.
 * @param path Native path or wide command-line argument.
 * @return UTF-8 bytes, independent of the Windows ANSI code page.
 */
std::string FilesystemPathToUtf8(const std::filesystem::path& path);

/**
 * Resolves a logical path against its owning configuration or executable base.
 * @param base Directory for relative input, which may use exact Windows syntax.
 * @param path Logical path; an absolute input replaces the base.
 * @return Absolute normalized Windows path, or a lexically normalized path elsewhere.
 * @remarks Explicit Windows input keeps its namespace and exact components.
 * Relative dot segments under an explicit base cannot ascend above its root.
 */
std::filesystem::path ResolveFilesystemPath(const std::filesystem::path& base,
                                            const std::filesystem::path& path);

/**
 * Resolves a path at the boundary of a filesystem operation.
 * @param path Logical native path; URI decoding is the caller's responsibility.
 * @param usage Receiving operation's path requirements.
 * @return On Windows, an absolute path extended when required; unchanged elsewhere.
 * @throws std::system_error If Windows rejects normalization or the path length.
 * @remarks Do not use this conversion for CEF settings, URLs or arbitrary argv.
 */
std::filesystem::path FilesystemPathForIo(const std::filesystem::path& path,
                                         FilesystemIoUsage usage);

/**
 * Reads the target type, following links and supporting Windows extended paths.
 * @param path Logical path to inspect.
 * @param error Receives the native failure, cleared on success or missing paths.
 * @return Object type, not_found for missing paths, or none on other failures.
 */
std::filesystem::file_type GetFilesystemType(const std::filesystem::path& path,
                                            std::error_code& error);

/**
 * Resolves an existing object, including links, without losing exact names.
 * @param path Logical path to resolve.
 * @param error Receives the native failure, cleared on success.
 * @return Canonical native path, or empty on failure. Windows retains its prefix.
 */
std::filesystem::path CanonicalFilesystemPath(const std::filesystem::path& path,
                                              std::error_code& error);

/**
 * Creates missing directories while accepting an existing directory.
 * @param path Logical directory path.
 * @param error Receives the native failure, cleared on success.
 * @return True when the directory exists after the operation.
 */
bool CreateFilesystemDirectories(const std::filesystem::path& path,
                                 std::error_code& error);

}  // namespace muon_internal
