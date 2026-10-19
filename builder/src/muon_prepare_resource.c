// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

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

typedef struct {
  const char *asset_directory;
  int output_opened;
} ResourceIoContext;

static eg_result resource_open_read(void *context, const char *path, void **handle) {
  ResourceIoContext *io = context;
  int relative = path[0] != '/';
#ifdef _WIN32
  relative = relative && path[0] != '\\' && !(path[0] != '\0' && path[1] == ':');
#endif
  char *resolved = io->asset_directory != NULL && relative
      ? muon_path_join(io->asset_directory, path) : muon_duplicate_string(path);
  *handle = NULL;
  if (resolved == NULL) return EG_ERROR_OUT_OF_MEMORY;
  FILE *file = muon_fopen(resolved, "rb");
  free(resolved);
  if (file == NULL) return EG_ERROR_IO;
  *handle = file;
  return EG_OK;
}

static eg_result resource_open_write(void *context, const char *path, void **handle) {
  ResourceIoContext *io = context;
  *handle = muon_fopen(path, "wb");
  if (*handle == NULL) return EG_ERROR_IO;
  io->output_opened = 1;
  return EG_OK;
}

static eg_result resource_read(void *context, void *handle, void *buffer,
                                size_t size, size_t *read_size) {
  (void)context;
  *read_size = fread(buffer, 1, size, (FILE *)handle);
  return ferror((FILE *)handle) ? EG_ERROR_IO : EG_OK;
}

static eg_result resource_write(void *context, void *handle, const void *buffer, size_t size) {
  (void)context;
  return fwrite(buffer, 1, size, (FILE *)handle) == size ? EG_OK : EG_ERROR_IO;
}

static eg_result resource_close(void *context, void *handle) {
  (void)context;
  return fclose((FILE *)handle) == 0 ? EG_OK : EG_ERROR_IO;
}

static void print_resource_usage(void) {
  fprintf(stderr,
          "Usage: muon-builder resource --input <pe> --updates-json <json> "
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
  ResourceIoContext io_context = {0};
  const eg_io_ops io = {&io_context, resource_open_read, resource_open_write,
                       resource_read, resource_write, resource_close};

  if (read_resource_options(argc, argv, arg_start, &options) != 0) {
    print_resource_usage();
    return 1;
  }

  result = eg_load_json_file_with_io(&io, options.updates_json_path, &document);
  if (result != EG_OK) {
    return report_engraver_error("Loading resource update JSON", result);
  }

  // Engraver's default path joining uses POSIX separators. Its public document
  // view and I/O callbacks let us resolve assets from the JSON directory without
  // rewriting an explicitly extended Windows input or changing the process cwd.
  char *asset_directory = muon_parent_directory(options.updates_json_path);
  if (asset_directory == NULL) {
    eg_release_json(document);
    return report_engraver_error("Resolving resource assets", EG_ERROR_OUT_OF_MEMORY);
  }
  eg_json_document asset_document = *document;
  asset_document.base_dir = ".";
  io_context.asset_directory = asset_directory;
  result = eg_json_document_to_update_with_io(&io, &asset_document, &update);
  io_context.asset_directory = NULL;
  free(asset_directory);
  if (result != EG_OK) {
    eg_release_json(document);
    return report_engraver_error("Converting resource update JSON", result);
  }

  result = eg_pe_open_file_with_io(&io, options.input_path, &file);
  if (result != EG_OK) {
    eg_resource_update_destroy(update);
    eg_release_json(document);
    return report_engraver_error("Opening PE input", result);
  }

  result = eg_pe_write_file_with_io(&io, file, update, options.output_path);
  eg_pe_close(file);
  eg_resource_update_destroy(update);
  eg_release_json(document);
  if (result != EG_OK) {
    if (io_context.output_opened) muon_remove_recursive(options.output_path);
    return report_engraver_error("Writing PE output", result);
  }

  if (!options.quiet) {
    muon_log_message("Updated PE resources: %s\n", options.output_path);
  }
  return 0;
}
