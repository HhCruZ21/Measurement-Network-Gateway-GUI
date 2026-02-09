#include "utils.h"

gboolean is_valid_ipv4(const char *ip)
{
    if (!ip || !*ip)
        return FALSE;

    int a, b, c, d;
    char tail;

    /* Strict format: a.b.c.d and nothing else */
    if (sscanf(ip, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4)
        return FALSE;

    if (a < 0 || a > 255)
        return FALSE;
    if (b < 0 || b > 255)
        return FALSE;
    if (c < 0 || c > 255)
        return FALSE;
    if (d < 0 || d > 255)
        return FALSE;

    return TRUE;
}

void set_enabled(GtkWidget *w, gboolean e)
{
    gtk_widget_set_sensitive(w, e);
}

void load_css(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
                                    /* Command feedback */
                                    "entry.cmd-success {\n"
                                    "  border: 2px solid #2ecc71;\n"
                                    "  box-shadow: none;\n"
                                    "}\n"
                                    "entry.cmd-error {\n"
                                    "  border: 2px solid #e74c3c;\n"
                                    "  box-shadow: none;\n"
                                    "}\n"
                                    ".text-green { color: #2ecc71; }\n"
                                    ".text-red   { color: #e74c3c; }\n"

                                    /* Clean blue focus (NO theme bleed) */
                                    "entry:focus:not(.cmd-success):not(.cmd-error) {\n"
                                    "  border: 2px solid #3399ff;\n"
                                    "  outline: none;\n"
                                    "  box-shadow: none;\n"
                                    "  background-clip: padding-box;\n"
                                    "}\n",

                                    -1, NULL);

    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}
