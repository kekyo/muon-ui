// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifndef MUON_PREPARE_BOOTSTRAP_CONFIG_H
#define MUON_PREPARE_BOOTSTRAP_CONFIG_H

#define MUON_BOOTSTRAP_CONFIG_FILE_NAME "muon-bootstrap.ini"
#define MUON_BOOTSTRAP_DEFAULT_CATALOG_REFRESH_INTERVAL_SECONDS 604800ULL

typedef struct {
  /** Effective CEF version selection policy. */
  char *cef_version_policy;
  /** Whether cef_version_policy was explicitly present in the ini file. */
  int has_cef_version_policy;
  /** Effective exact CEF version used by the exact policy. */
  char *cef_exact_version;
  /** Whether cef_exact_version was explicitly present in the ini file. */
  int has_cef_exact_version;
  /** Effective catalog refresh interval in seconds. */
  unsigned long long catalog_refresh_interval_seconds;
  /** Whether catalog_refresh_interval_seconds was explicitly present in the ini file. */
  int has_catalog_refresh_interval_seconds;
  /** Last successful CEF catalog update Unix timestamp. */
  unsigned long long last_catalog_update_unix;
  /** Whether a catalog update was requested for the next bootstrap run. */
  int update_requested;
  /** Catalog update request Unix timestamp. */
  unsigned long long update_requested_at_unix;
} MuonBootstrapConfig;

void muon_bootstrap_config_init_defaults(MuonBootstrapConfig *config);
void muon_bootstrap_config_free(MuonBootstrapConfig *config);
int muon_bootstrap_config_read(const char *runtime_dir,
                               MuonBootstrapConfig *config);
/**
 * Reads bootstrap settings, applying default_version_policy when versionPolicy
 * is not explicitly present.
 */
int muon_bootstrap_config_read_with_default(const char *runtime_dir,
                                            const char *default_version_policy,
                                            MuonBootstrapConfig *config);
int muon_bootstrap_config_write(const char *runtime_dir,
                                const MuonBootstrapConfig *config);
int muon_bootstrap_config_validate(const MuonBootstrapConfig *config);
/**
 * Reads the embedded muon.json bootstrap.defaultVersionPolicy from this
 * executable.
 *
 * The returned policy is heap-allocated and must be released with free().
 */
int muon_bootstrap_get_embedded_default_version_policy(char **policy);
/**
 * Reads the embedded muon.json bootstrap.appId from this executable.
 *
 * The returned app ID is heap-allocated and must be released with free().
 * When the embedded config has no appId, *app_id is set to NULL.
 */
int muon_bootstrap_get_embedded_app_id(char **app_id);

#endif
