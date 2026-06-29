// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "engraver.h"
#include "muon_prepare_resource.h"

typedef struct {
  const char *input_path;
  const char *updates_json_path;
  const char *output_path;
  int quiet;
} ResourceOptions;

static void print_resource_usage(void) {
  fprintf(stderr,
          "Usage: muon-prepare resource --input <pe> --updates-json <json> "
          "--output <pe> [options]\n"
          "\n"
          "Options:\n"
          "  --input <pe>           Input PE executable path.\n"
          "  --updates-json <json>  Engraver resource update JSON path.\n"
          "  --output <pe>          Output PE executable path.\n"
          "  -q, --quiet            Suppress informational output.\n"
          "  -h, --help             Show this help.\n");
}

static int read_resource_options(int argc, char **argv, int arg_start,
                                 ResourceOptions *options) {
  int index;
  memset(options, 0, sizeof(*options));

  for (index = arg_start; index < argc; index += 1) {
    const char *arg = argv[index];
    if (strcmp(arg, "--input") == 0) {
      if (index + 1 >= argc) {
        muon_print_error("--input requires a value.\n");
        return -1;
      }
      options->input_path = argv[++index];
    } else if (strcmp(arg, "--updates-json") == 0) {
      if (index + 1 >= argc) {
        muon_print_error("--updates-json requires a value.\n");
        return -1;
      }
      options->updates_json_path = argv[++index];
    } else if (strcmp(arg, "--output") == 0) {
      if (index + 1 >= argc) {
        muon_print_error("--output requires a value.\n");
        return -1;
      }
      options->output_path = argv[++index];
    } else if (strcmp(arg, "--quiet") == 0 || strcmp(arg, "-q") == 0) {
      options->quiet = 1;
    } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_resource_usage();
      exit(0);
    } else {
      muon_print_error("Unknown resource option: %s\n", arg);
      return -1;
    }
  }

  if (options->input_path == NULL || options->updates_json_path == NULL ||
      options->output_path == NULL) {
    muon_print_error(
        "resource requires --input, --updates-json, and --output.\n");
    return -1;
  }
  if (strcmp(options->input_path, options->output_path) == 0) {
    muon_print_error("resource --input and --output must be different paths.\n");
    return -1;
  }

  return 0;
}

static int report_engraver_error(const char *operation, eg_result result) {
  muon_print_error("%s failed: %s\n", operation, eg_result_string(result));
  return 1;
}

int muon_prepare_resource_main(int argc, char **argv, int arg_start) {
  ResourceOptions options;
  eg_json_document *document = NULL;
  eg_resource_update *update = NULL;
  eg_pe_file *file = NULL;
  eg_result result;

  if (read_resource_options(argc, argv, arg_start, &options) != 0) {
    print_resource_usage();
    return 1;
  }

  result = eg_load_json_file(options.updates_json_path, &document);
  if (result != EG_OK) {
    return report_engraver_error("Loading resource update JSON", result);
  }

  result = eg_json_document_to_update(document, &update);
  if (result != EG_OK) {
    eg_release_json(document);
    return report_engraver_error("Converting resource update JSON", result);
  }

  result = eg_pe_open_file(options.input_path, &file);
  if (result != EG_OK) {
    eg_resource_update_destroy(update);
    eg_release_json(document);
    return report_engraver_error("Opening PE input", result);
  }

  result = eg_pe_write_file(file, update, options.output_path);
  eg_pe_close(file);
  eg_resource_update_destroy(update);
  eg_release_json(document);
  if (result != EG_OK) {
    return report_engraver_error("Writing PE output", result);
  }

  if (!options.quiet) {
    muon_log_message("Updated PE resources: %s\n", options.output_path);
  }
  return 0;
}
