/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin.h"

#include "plugins/builtin/muon_builtin_bootstrap.h"
#include "plugins/builtin/muon_builtin_environments.h"
#include "plugins/builtin/muon_builtin_executor.h"
#include "plugins/builtin/muon_builtin_fs.h"

static const muon_plugin_namespace* const builtin_namespaces[] = {
    &kMuonBuiltinFsNamespace,
    &kMuonBuiltinEnvironmentsNamespace,
    &kMuonBuiltinExecutorNamespace,
    &kMuonBuiltinBootstrapNamespace,
    nullptr,
};

static const muon_plugin_metadata builtin_metadata = {
    builtin_namespaces,
};

const muon_plugin_metadata* GetMuonBuiltinPluginMetadata() {
  return &builtin_metadata;
}
