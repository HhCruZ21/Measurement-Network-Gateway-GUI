#include "utils.h"

const char *HELP_TEXT =
    "\033[1mMeasurement Network Gateway – CLI Help\033[0m\n"
    "\n"
    "\033[1;36mVALID COMMANDS:\033[0m\n"
    "\n"
    "  \033[1;32mCONNECT <IP_ADDRESS>\033[0m\n"
    "\n"
    "    Establish TCP connection to server.\n"
    "    IP_ADDRESS must be valid IPv4 format.\n"
    "\n"
    "    \033[33mExample:\033[0m\n"
    "      CONNECT 192.168.1.10\n"
    "\n"
    "  \033[1;32mDISCONNECT\033[0m\n"
    "\n"
    "    Close active connection.\n"
    "    Plotting must be stopped before disconnecting.\n"
    "\n"
    "    \033[33mExample:\033[0m\n"
    "      DISCONNECT\n"
    "\n"
    "  \033[1;32mSTART\033[0m\n"
    "\n"
    "    Start data streaming and plotting.\n"
    "    Only valid when connected.\n"
    "\n"
    "    \033[33mExample:\033[0m\n"
    "      START\n"
    "\n"
    "  \033[1;32mSTOP\033[0m\n"
    "\n"
    "    Stop data streaming and plotting.\n"
    "    Only valid when currently running.\n"
    "\n"
    "    \033[33mExample:\033[0m\n"
    "      STOP\n"
    "\n"
    "  \033[1;32mSHUTDOWN\033[0m\n"
    "\n"
    "    Shutdown remote device and close application.\n"
    "    Must not be running.\n"
    "\n"
    "    \033[33mExample:\033[0m\n"
    "      SHUTDOWN\n"
    "\n"
    "  \033[1;32mCONFIGURE <SENSOR_ID> <FREQ_HZ>\033[0m\n"
    "\n"
    "    SENSOR_ID:\n"
    "      TEMP   - Temperature sensor\n"
    "      ADC0   - ADC channel 0\n"
    "      ADC1   - ADC channel 1\n"
    "      SW     - Switch inputs\n"
    "      PB     - Push buttons\n"
    "\n"
    "    FREQ_HZ:\n"
    "      Integer value between 10 and 1000\n"
    "\n"
    "    \033[33mExamples:\033[0m\n"
    "      CONFIGURE TEMP 50\n"
    "      CONFIGURE ADC0 200\n"
    "\n"
    "\033[1;31mINVALID EXAMPLES:\033[0m\n"
    "\n"
    "  START              (not connected)\n"
    "  STOP               (not running)\n"
    "  SHUTDOWN           (while running)\n"
    "  CONFIGURE TEMP 9        (frequency too low)\n"
    "  CONFIGURE ADC1 1001     (frequency too high)\n"
    "\n"
    "\033[1;36mNOTES:\033[0m\n"
    "\n"
    "  - Commands are case-insensitive\n"
    "  - Cannot CONNECT while already connected\n"
    "  - Cannot DISCONNECT while plotting is running\n"
    "  - START requires active connection\n"
    "  - STOP requires running state\n"
    "  - SHUTDOWN requires connected and not running\n"
    "  - Streaming must be running to apply configuration\n"
    "\n"
    "\033[2mPress Ctrl+C to close this window.\033[0m\n";

uint64_t time_window_us = 5000000ULL; // default: 5s fallback

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

gboolean clear_cmd_feedback(gpointer data)
{
    CmdClearCtx *ctx = data;

    GtkStyleContext *ec = gtk_widget_get_style_context(ctx->entry);
    GtkStyleContext *lc = gtk_widget_get_style_context(ctx->label);

    gtk_style_context_remove_class(ec, "cmd-success");
    gtk_style_context_remove_class(ec, "cmd-error");
    gtk_style_context_remove_class(lc, "text-green");
    gtk_style_context_remove_class(lc, "text-red");

    gtk_label_set_text(GTK_LABEL(ctx->label), "");

    /* RESET command icon to idle */
    gtk_entry_set_icon_from_icon_name(
        GTK_ENTRY(ctx->entry),
        GTK_ENTRY_ICON_PRIMARY,
        "utilities-terminal-symbolic");

    gtk_entry_set_text(GTK_ENTRY(ctx->entry), "");
    gtk_widget_set_sensitive(ctx->entry, TRUE);

    g_free(ctx);
    return FALSE;
}
