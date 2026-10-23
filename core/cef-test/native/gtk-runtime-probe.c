// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

static gboolean check_input_module(const char *id, const char *filename) {
    GtkIMContext *context = gtk_im_multicontext_new();
    gtk_im_multicontext_set_context_id(GTK_IM_MULTICONTEXT(context), id);
    gchar *preedit = NULL;
    gtk_im_context_get_preedit_string(context, &preedit, NULL, NULL);
    g_free(preedit);
    FILE *maps = fopen("/proc/self/maps", "r");
    gboolean loaded = FALSE;
    char line[4096];
    if (maps != NULL) {
        while (fgets(line, sizeof(line), maps) != NULL) {
            if ((strstr(line, " /usr/lib/") != NULL || strstr(line, " /lib/") != NULL) && strstr(line, filename) != NULL)
                loaded = TRUE;
        }
        fclose(maps);
    }
    printf("input: %s system=%d\n", id, loaded);
    g_object_unref(context);
    return loaded;
}

typedef struct {
    GMainLoop *loop;
    const char *filename;
    gboolean selected;
    guint timeout;
} file_selection;

static void selection_changed(GtkFileChooser *chooser, gpointer data) {
    file_selection *selection = data;
    gchar *filename = gtk_file_chooser_get_filename(chooser);
    selection->selected = g_strcmp0(filename, selection->filename) == 0;
    g_free(filename);
    if (selection->selected) g_main_loop_quit(selection->loop);
}

static gboolean selection_timeout(gpointer data) {
    file_selection *selection = data;
    selection->timeout = 0;
    g_main_loop_quit(selection->loop);
    return G_SOURCE_REMOVE;
}

static gboolean select_file(GtkFileChooser *chooser, const char *filename) {
    file_selection selection = {g_main_loop_new(NULL, FALSE), filename, FALSE, 0};
    gulong handler = g_signal_connect(chooser, "selection-changed", G_CALLBACK(selection_changed), &selection);
    // Folder loading is asynchronous. Wait for the requested selection;
    // the timeout only bounds a broken chooser, never signals success.
    selection.timeout = g_timeout_add_seconds(60, selection_timeout, &selection);
    gtk_file_chooser_select_filename(chooser, filename);
    selection_changed(chooser, &selection);
    if (!selection.selected) g_main_loop_run(selection.loop);
    if (selection.timeout != 0) g_source_remove(selection.timeout);
    g_signal_handler_disconnect(chooser, handler);
    g_main_loop_unref(selection.loop);
    printf("file chooser: selected=%d\n", selection.selected);
    return selection.selected;
}

int main(int argc, char **argv) {
    // An in-memory backend confines writes to this process, never the user's dconf.
    if (g_strcmp0(g_getenv("GSETTINGS_BACKEND"), "memory") != 0) return 3;
    if (!gtk_init_check(NULL, NULL)) return 5;
    const char *backend = G_OBJECT_TYPE_NAME(gdk_display_get_default());
    printf("backend: %s\n", backend);
    gboolean valid = TRUE;
    GtkWidget *chooser = gtk_file_chooser_widget_new(GTK_FILE_CHOOSER_ACTION_OPEN);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), chooser);
    gtk_widget_show_all(window);
    printf("file chooser: shown\n");
    if (argc > 1) valid = select_file(GTK_FILE_CHOOSER(chooser), argv[1]) && valid;
    gtk_widget_destroy(window);
    if (argc > 1) {
        GError *error = NULL;
        GdkPixbuf *image = gdk_pixbuf_new_from_file(argv[1], &error);
        if (image == NULL) {
            fprintf(stderr, "image: %s\n", error->message);
            g_error_free(error);
            return 7;
        }
        const guchar *pixels = gdk_pixbuf_get_pixels(image);
        valid = valid && gdk_pixbuf_get_width(image) == 3 && gdk_pixbuf_get_height(image) == 2 &&
            pixels[0] == 255 && pixels[1] == 0 && pixels[2] == 0;
        printf("image: decoded=%d\n", valid);
        g_object_unref(image);
        // Fcitx4 and Fcitx5 use the same GType name. Exercise each user's
        // selected input method in its own process, as applications do.
        if (argc > 2) {
            gchar *filename = g_strdup_printf("/im-%s.so", argv[2]);
            valid = check_input_module(argv[2], filename) && valid;
            g_free(filename);
        }
    }
    return valid ? 0 : 6;
}
