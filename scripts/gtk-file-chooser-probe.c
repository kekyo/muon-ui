#include <atk/atk.h>
#include <gtk/gtk.h>

#include <stdio.h>
#include <string.h>

static const char* read_arg_value(int argc,
                                  char** argv,
                                  const char* key,
                                  const char* fallback) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (strcmp(argv[index], key) == 0) {
      return argv[index + 1];
    }
  }
  return fallback;
}

static int has_arg(int argc, char** argv, const char* key) {
  for (int index = 1; index < argc; ++index) {
    if (strcmp(argv[index], key) == 0) {
      return 1;
    }
  }
  return 0;
}

static void set_accessible_metadata(GtkWidget* widget,
                                    const char* name,
                                    const char* description,
                                    const char* id) {
  if (widget == NULL) {
    return;
  }
  AtkObject* accessible = gtk_widget_get_accessible(widget);
  if (accessible == NULL) {
    return;
  }
  atk_object_set_name(accessible, name);
  atk_object_set_description(accessible, description);
  atk_object_set_accessible_id(accessible, id);
}

static gboolean close_dialog(gpointer data) {
  gtk_dialog_response(GTK_DIALOG(data), GTK_RESPONSE_CANCEL);
  return G_SOURCE_REMOVE;
}

int main(int argc, char** argv) {
  const char* title = read_arg_value(
      argc, argv, "--title", "Muon GTK Standalone Probe Dialog");
  const char* mode = read_arg_value(argc, argv, "--mode", "null-parent");
  const int accessibility = has_arg(argc, argv, "--accessibility");

  if (!gtk_init_check(&argc, &argv)) {
    fprintf(stderr, "gtk_init_check failed\n");
    return 2;
  }

  GtkWindow* parent = NULL;
  if (strcmp(mode, "transient") == 0) {
    GtkWidget* parent_widget = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(parent_widget), "Muon GTK Probe Parent");
    gtk_widget_show_all(parent_widget);
    parent = GTK_WINDOW(parent_widget);
  }

  GtkWidget* dialog = gtk_file_chooser_dialog_new(
      title, parent, GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
      GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
  if (dialog == NULL) {
    fprintf(stderr, "gtk_file_chooser_dialog_new failed\n");
    return 3;
  }

  if (accessibility) {
    set_accessible_metadata(
        dialog,
        "Muon GTK probe dialog accessible name",
        "Muon GTK probe dialog accessible description",
        "muon-gtk-probe-dialog");
    set_accessible_metadata(
        gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                           GTK_RESPONSE_ACCEPT),
        "Muon GTK probe accept button accessible name",
        "Muon GTK probe accept button accessible description",
        "muon-gtk-probe-accept");
    set_accessible_metadata(
        gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                           GTK_RESPONSE_CANCEL),
        "Muon GTK probe cancel button accessible name",
        "Muon GTK probe cancel button accessible description",
        "muon-gtk-probe-cancel");
  }

  gtk_widget_show_all(dialog);
  g_timeout_add_seconds(20, close_dialog, dialog);
  printf("ready\n");
  fflush(stdout);
  int response = gtk_dialog_run(GTK_DIALOG(dialog));
  printf("response=%d\n", response);
  fflush(stdout);
  gtk_widget_destroy(dialog);
  return 0;
}
