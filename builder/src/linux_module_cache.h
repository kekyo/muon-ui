// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#ifndef MUON_LINUX_MODULE_CACHE_H
#define MUON_LINUX_MODULE_CACHE_H

/**
 * Expands runtime paths in a GTK or GdkPixbuf module cache template.
 * @param source Null-terminated template with @MUON_RUNTIME@ path markers.
 * @param runtime_path Absolute final runtime directory before cache escaping.
 * @return Allocated cache contents, or NULL on allocation failure. The caller
 *   owns the returned buffer and must free it.
 * @remarks Escapes quotes, backslashes and control bytes for GTK's quoted
 *   cache strings. The final path can differ from the transaction directory.
 */
char *muon_expand_linux_module_cache(const char *source,
                                     const char *runtime_path);

#endif
