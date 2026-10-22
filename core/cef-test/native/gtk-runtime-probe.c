// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

static gboolean has_key(const char *schema_id, const char *key) {
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    GSettingsSchema *schema = source == NULL ? NULL :
        g_settings_schema_source_lookup(source, schema_id, TRUE);
    gboolean present = schema != NULL && g_settings_schema_has_key(schema, key);
    if (schema != NULL) g_settings_schema_unref(schema);
    return present;
}

static gboolean check_input_module(const char *id, const char *filename) {
    GtkIMContext *context = gtk_im_multicontext_new();
    gtk_im_multicontext_set_context_id(GTK_IM_MULTICONTEXT(context), id);
    gchar *preedit = NULL;
    gtk_im_context_get_preedit_string(context, &preedit, NULL, NULL);
    g_free(preedit);
    FILE *maps = fopen("/proc/self/maps", "r");
    gboolean loaded = FALSE;
    char line[4096];
    const char *root = g_getenv("MUON_LINUX_MODULE_ROOT");
    if (maps != NULL) {
        while (fgets(line, sizeof(line), maps) != NULL) {
            if (root != NULL && strstr(line, root) != NULL && strstr(line, filename) != NULL)
                loaded = TRUE;
        }
        fclose(maps);
    }
    printf("input: %s bundled=%d\n", id, loaded);
    g_object_unref(context);
    return loaded;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "new";
    const char *interface_id = "org.gnome.desktop.interface";
    const char *legacy_id = "org.gnome.settings-daemon.plugins.xsettings";
    gboolean current = has_key(interface_id, "font-antialiasing");
    gboolean legacy = has_key(legacy_id, "antialiasing");
    printf("schemas: current=%d legacy=%d\n", current, legacy);
    fflush(stdout);
    if ((strcmp(mode, "new") == 0 && (!current || legacy)) ||
        (strcmp(mode, "old") == 0 && (current || !legacy)) ||
        (strcmp(mode, "none") == 0 && (current || legacy))) return 2;

    // An in-memory backend confines writes to this process, never the user's dconf.
    if (g_strcmp0(g_getenv("GSETTINGS_BACKEND"), "memory") != 0) return 3;
    GSettings *fonts = NULL;
    if (current || legacy) {
        fonts = g_settings_new(current ? interface_id : legacy_id);
        if (!g_settings_set_string(fonts, current ? "font-antialiasing" : "antialiasing", "rgba") ||
            !g_settings_set_string(fonts, current ? "font-hinting" : "hinting", "full") ||
            !g_settings_set_string(fonts, current ? "font-rgba-order" : "rgba-order", "bgr")) return 4;
    }
    if (!gtk_init_check(NULL, NULL)) return 5;
    const char *backend = G_OBJECT_TYPE_NAME(gdk_display_get_default());
    printf("backend: %s\n", backend);
    gint antialias = -1;
    gchar *hintstyle = NULL;
    gchar *rgba = NULL;
    g_object_get(gtk_settings_get_default(), "gtk-xft-antialias", &antialias,
                 "gtk-xft-hintstyle", &hintstyle, "gtk-xft-rgba", &rgba, NULL);
    printf("fonts: antialias=%d hintstyle=%s rgba=%s\n", antialias, hintstyle, rgba);
    gboolean valid = TRUE;
    if (strcmp(backend, "GdkWaylandDisplay") == 0 && fonts != NULL && strcmp(mode, "portal") != 0)
        valid = antialias == 1 && g_strcmp0(hintstyle, "hintfull") == 0 && g_strcmp0(rgba, "bgr") == 0;
    g_free(hintstyle);
    g_free(rgba);
    if (fonts != NULL) g_object_unref(fonts);
    GtkWidget *chooser = gtk_file_chooser_widget_new(GTK_FILE_CHOOSER_ACTION_OPEN);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_container_add(GTK_CONTAINER(window), chooser);
    gtk_widget_show_all(window);
    printf("file chooser: shown\n");
    gtk_widget_destroy(window);
    if (argc > 2) {
        GError *error = NULL;
        GdkPixbuf *image = gdk_pixbuf_new_from_file(argv[2], &error);
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
        if (argc > 3) {
            gchar *filename = g_strdup_printf("/im-%s.so", argv[3]);
            valid = check_input_module(argv[3], filename) && valid;
            g_free(filename);
        }
    }
    return valid ? 0 : 6;
}
