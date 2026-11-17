/* OHOS native plugins must live in the HAP linker namespace, never el2.
 * Keep this helper independent of Wine so the same code can be probed there. */
#ifndef WINEHUA_GSTREAMER_LOADER_H
#define WINEHUA_GSTREAMER_LOADER_H
#include <gst/gst.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include "winehua_gst_inventory.h"

static const char *winehua_gst_native_dir(void)
{
    const char *dir = g_getenv("WINEHUA_GST_PLUGIN_DIR");
    return dir && *dir ? dir : NULL;
}

/* Must run BEFORE gst_init_check: no directory-wide scan of HAP native libs,
 * no inherited registry blacklist and no external scanner subprocess. */
static gboolean winehua_gst_prepare(void)
{
    const char *root, *arch;
    gchar *fallback = NULL, *name, *registry;
#if defined(__aarch64__)
    arch = "aarch64";
#elif defined(__x86_64__)
    arch = "x86_64";
#else
    arch = "i386";
#endif
    if (winehua_gst_native_dir())
    {
        g_setenv("GST_PLUGIN_PATH", "", TRUE);
        g_setenv("GST_PLUGIN_PATH_1_0", "", TRUE);
        g_setenv("GST_PLUGIN_SYSTEM_PATH", "", TRUE);
        g_setenv("GST_PLUGIN_SYSTEM_PATH_1_0", "", TRUE);
        g_setenv("GST_REGISTRY_FORK", "no", TRUE);
        g_setenv("GST_REGISTRY_UPDATE", "no", TRUE);
    }
    root = g_getenv("WINE_GST_REGISTRY_DIR");
    if (!root || !*root)
        root = fallback = g_build_filename(g_get_user_cache_dir(), "wine", "gstreamer-1.0", NULL);
    if (g_mkdir_with_parents(root, 0700))
    {
        fprintf(stderr, "winegstreamer: cannot create registry directory %s\n", root);
        g_free(fallback);
        return FALSE;
    }
    name = g_strdup_printf("registry.%s.%s.bin", arch, WINEHUA_GST_INVENTORY_SHA256);
    registry = g_build_filename(root, name, NULL);
    g_setenv("GST_REGISTRY", registry, TRUE);
    g_setenv("GST_REGISTRY_1_0", registry, TRUE);
    fprintf(stderr, "winegstreamer: native plugin directory=%s registry=%s %s\n",
            winehua_gst_native_dir() ? winehua_gst_native_dir() : "Box64 scan path", registry, winehua_gst_inventory_identity);
    g_free(registry);
    g_free(name);
    g_free(fallback);
    return TRUE;
}

/* Proton's WMA converter ranks above libav and expects Steam's Fossilize
 * media cache. Local OHOS games have no such cache: let the actual decoders
 * handle them. Keep the Proton path available when explicitly configured. */
static gboolean winehua_gst_use_media_converter(void)
{
    const char * const keys[] = {"STEAM_COMPAT_TRANSCODED_MEDIA_PATH",
        "MEDIACONV_AUDIO_DUMP_FILE", "MEDIACONV_AUDIO_TRANSCODED_FILE",
        "MEDIACONV_VIDEO_DUMP_FILE", "MEDIACONV_VIDEO_TRANSCODED_FILE"};
    guint i;
    if (!winehua_gst_native_dir()) return TRUE;
    for (i = 0; i < G_N_ELEMENTS(keys); ++i)
    {
        const char *value = g_getenv(keys[i]);
        if (value && *value) return TRUE;
    }
    fprintf(stderr, "winegstreamer: using native decoders (Steam media conversion cache is not configured)\n");
    return FALSE;
}

static gboolean winehua_gst_load_plugins(void)
{
    const char *dir = winehua_gst_native_dir();
    guint i, loaded = 0, elements = 0;
    if (!dir) return TRUE;
    for (i = 0; i < G_N_ELEMENTS(winehua_gst_plugins); ++i)
    {
        gchar *path = g_build_filename(dir, winehua_gst_plugins[i], NULL);
        GError *error = NULL;
        GstPlugin *plugin = gst_plugin_load_file(path, &error);
        if (plugin)
        {
            ++loaded;
            gst_object_unref(plugin);
        }
        else
            fprintf(stderr, "winegstreamer: plugin %s failed (%s): %s\n",
                    winehua_gst_plugins[i], path, error ? error->message : "unknown loader error");
        g_clear_error(&error);
        g_free(path);
    }
    for (i = 0; i < G_N_ELEMENTS(winehua_gst_required_elements); ++i)
    {
        GstElement *element = gst_element_factory_make(winehua_gst_required_elements[i], NULL);
        if (element)
        {
            ++elements;
            gst_object_unref(element);
        }
        else
            fprintf(stderr, "winegstreamer: required element %s cannot be created\n",
                    winehua_gst_required_elements[i]);
    }
    fprintf(stderr, "winegstreamer: native plugins loaded %u/%u; required elements %u/%u\n",
            loaded, (unsigned)G_N_ELEMENTS(winehua_gst_plugins),
            elements, (unsigned)G_N_ELEMENTS(winehua_gst_required_elements));
    return loaded == G_N_ELEMENTS(winehua_gst_plugins) && elements == G_N_ELEMENTS(winehua_gst_required_elements);
}
#endif
