// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifndef MUON_PREPARE_LAUNCHER_CONFIG_H
#define MUON_PREPARE_LAUNCHER_CONFIG_H

#define MUON_LAUNCHER_CONFIG_FILE_NAME "muon-launcher.ini"
#define MUON_LAUNCHER_DEFAULT_CATALOG_REFRESH_INTERVAL_SECONDS 604800ULL

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
  /** Whether a catalog update was requested for the next launcher run. */
  int update_requested;
  /** Catalog update request Unix timestamp. */
  unsigned long long update_requested_at_unix;
} MuonLauncherConfig;

void muon_launcher_config_init_defaults(MuonLauncherConfig *config);
void muon_launcher_config_free(MuonLauncherConfig *config);
int muon_launcher_config_read(const char *runtime_dir,
                               MuonLauncherConfig *config);
/**
 * Reads launcher settings, applying default_version_policy when versionPolicy
 * is not explicitly present.
 */
int muon_launcher_config_read_with_default(const char *runtime_dir,
                                            const char *default_version_policy,
                                            MuonLauncherConfig *config);
int muon_launcher_config_write(const char *runtime_dir,
                                const MuonLauncherConfig *config);
int muon_launcher_config_validate(const MuonLauncherConfig *config);
/**
 * Reads the embedded muon.json launcher.defaultVersionPolicy from this
 * executable.
 *
 * The returned policy is heap-allocated and must be released with free().
 */
int muon_launcher_get_embedded_default_version_policy(char **policy);
/**
 * Reads the embedded muon.json launcher.appId from this executable.
 *
 * The returned app ID is heap-allocated and must be released with free().
 * When the embedded config has no appId, *app_id is set to NULL.
 */
int muon_launcher_get_embedded_app_id(char **app_id);

#endif
