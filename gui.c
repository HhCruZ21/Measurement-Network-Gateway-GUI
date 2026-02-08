#include <gtk/gtk.h>
#include <string.h>
#include <ctype.h>

/* ---------- Connect with backend ---------- */

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

/* ---------- Sensor Model ---------- */

#define PORT 50012
#define SENSOR_COUNT 5
#define CMD_HISTORY_SIZE 5
#define MAX_SAMPLES 1024

static int sock_fd = -1;
static pthread_t net_thread;
static volatile int net_running = 0;

static char connected_ip[64] = {0};

/* ---------- Store command history ---------- */

static char *cmd_history[CMD_HISTORY_SIZE];
static int cmd_hist_count = 0;
static int cmd_hist_index = -1;

const char *sensor_ids[SENSOR_COUNT] = {
    "TEMP", "ADC0", "ADC1", "SW", "PB"};

const char *sensor_labels[SENSOR_COUNT] = {
    "Temp", "ADC 0", "ADC 1", "Switches", "Push Buttons"};

static double sample_data[SENSOR_COUNT][MAX_SAMPLES];
static int sample_count[SENSOR_COUNT] = {0};

/* Plot colors (Matplotlib default palette) */
static const double plot_colors[SENSOR_COUNT][3] = {
    {0x1F / 255.0, 0x77 / 255.0, 0xB4 / 255.0}, // Blue
    {0xFF / 255.0, 0x7F / 255.0, 0x0E / 255.0}, // Orange
    {0x2C / 255.0, 0xA0 / 255.0, 0x2C / 255.0}, // Green
    {0xD6 / 255.0, 0x27 / 255.0, 0x28 / 255.0}, // Red
    {0x94 / 255.0, 0x67 / 255.0, 0xBD / 255.0}  // Purple
};


static const char *canonical_sensor(const char *s)
{
    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        if (g_ascii_strcasecmp(s, sensor_ids[i]) == 0 ||
            g_ascii_strcasecmp(s, sensor_labels[i]) == 0)
            return sensor_ids[i];
    }
    return NULL;
}

/* ---------- State ---------- */

GtkWidget *graph_area;

typedef enum
{
    STATE_DISCONNECTED,
    STATE_CONNECTED,
    STATE_RUNNING
} AppState;

static AppState state = STATE_DISCONNECTED;

/* ---------- Widgets ---------- */

GtkWidget *connect_entry, *connect_btn, *disconnect_btn;
GtkWidget *start_btn, *stop_btn;
GtkWidget *checkboxes[SENSOR_COUNT];
GtkWidget *combo;
GtkWidget *hz_entry, *config_btn;
GtkWidget *cmd_entry, *cmd_status;

GHashTable *sensor_freq;

/* ---------- CSS ---------- */

static void load_css(void)
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

/* ---------- Utilities ---------- */

static void set_enabled(GtkWidget *w, gboolean e)
{
    gtk_widget_set_sensitive(w, e);
}

static int checked_count()
{
    int c = 0;
    for (int i = 0; i < SENSOR_COUNT; i++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checkboxes[i])))
            c++;
    return c;
}

/* ---------- Focus handling ---------- */

static gboolean entry_focus_out(GtkWidget *w, GdkEvent *e, gpointer d)
{
    gtk_editable_select_region(GTK_EDITABLE(w), -1, -1);
    gtk_editable_set_position(GTK_EDITABLE(w), -1);
    return FALSE;
}

/* ---------- Dropdown ---------- */

static void update_dropdown()
{
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checkboxes[i])))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),
                                      sensor_ids[i], sensor_labels[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

static void combo_changed(GtkComboBox *box, gpointer d)
{
    const char *id = gtk_combo_box_get_active_id(box);
    if (!id)
        return;

    const char *val = g_hash_table_lookup(sensor_freq, id);
    gtk_entry_set_text(GTK_ENTRY(hz_entry), val ? val : "");
}

static gboolean is_sensor_selected(int idx)
{
    return gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(checkboxes[idx]));
}

static gboolean cmd_key_press(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    if (cmd_hist_count == 0)
        return FALSE;

    if (e->keyval == GDK_KEY_Up)
    {
        if (cmd_hist_index > 0)
            cmd_hist_index--;

        gtk_entry_set_text(GTK_ENTRY(w),
                           cmd_history[cmd_hist_index]);
        return TRUE;
    }
    else if (e->keyval == GDK_KEY_Down)
    {
        if (cmd_hist_index < cmd_hist_count - 1)
        {
            cmd_hist_index++;
            gtk_entry_set_text(GTK_ENTRY(w),
                               cmd_history[cmd_hist_index]);
        }
        else
        {
            cmd_hist_index = cmd_hist_count;
            gtk_entry_set_text(GTK_ENTRY(w), "");
        }
        return TRUE;
    }
    return FALSE;
}

/* ---------- Checkbox logic ---------- */

static void checkbox_changed(GtkToggleButton *btn, gpointer d)
{
    if (checked_count() < 2)
    {
        gtk_toggle_button_set_active(btn, TRUE);
        return;
    }
    update_dropdown();

    if (graph_area)
        gtk_widget_queue_draw(graph_area);
}

/* ---------- Hz ---------- */

static void hz_changed(GtkEditable *e, gpointer d)
{
    set_enabled(config_btn,
                strlen(gtk_entry_get_text(GTK_ENTRY(hz_entry))) > 0);
}

static void configure_clicked(GtkButton *b, gpointer d)
{
    if (sock_fd < 0)
        return;

    const char *id =
        gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    const char *freq =
        gtk_entry_get_text(GTK_ENTRY(hz_entry));

    if (!id || !*freq)
        return;

    /* validate numeric rate */
    for (int i = 0; freq[i]; i++)
    {
        if (!isdigit((unsigned char)freq[i]))
            return;
    }

    int rate = atoi(freq);
    if (rate <= 0)
        return;

    /* send to server */
    char net_cmd[64];
    snprintf(net_cmd, sizeof(net_cmd),
             "CONFIGURE %s %d\n", id, rate);
    send(sock_fd, net_cmd, strlen(net_cmd), 0);

    printf("Sent: %s", net_cmd);

    /* update local model */
    g_hash_table_replace(sensor_freq,
                         g_strdup(id),
                         g_strdup(freq));
}

/* ---------- Command Line ---------- */

typedef struct
{
    GtkWidget *entry;
    GtkWidget *label;
} CmdClearCtx;

static gboolean clear_cmd_feedback(gpointer data)
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

static void cmd_enter(GtkEntry *e, gpointer d)
{
    char buf[128];
    strncpy(buf, gtk_entry_get_text(e), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    char *tok1 = strtok(buf, " ");
    char *tok2 = strtok(NULL, " ");
    char *tok3 = strtok(NULL, " ");
    char *extra = strtok(NULL, " ");

    gboolean valid = FALSE;
    const char *id = NULL;

    if (tok1 && tok2 && tok3 && !extra &&
        g_ascii_strcasecmp(tok1, "CONFIGURE") == 0 &&
        (id = canonical_sensor(tok2)))
    {
        /* validate numeric rate */
        for (int i = 0; tok3[i]; i++)
        {
            if (!isdigit((unsigned char)tok3[i]))
                goto done;
        }

        int rate = atoi(tok3);
        if (rate > 0)
        {
            valid = TRUE;

            /* update local model */
            g_hash_table_replace(sensor_freq,
                                 g_strdup(id),
                                 g_strdup(tok3));

            /* update Hz entry if same sensor selected */
            const char *active =
                gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
            if (active && strcmp(active, id) == 0)
                gtk_entry_set_text(GTK_ENTRY(hz_entry), tok3);

            /* send to server */
            if (sock_fd >= 0)
            {
                char net_cmd[64];
                snprintf(net_cmd, sizeof(net_cmd),
                         "CONFIGURE %s %d\n", id, rate);
                send(sock_fd, net_cmd, strlen(net_cmd), 0);
                printf("Sent: %s", net_cmd);
            }
        }
    }

done:;
    GtkStyleContext *ec = gtk_widget_get_style_context(GTK_WIDGET(e));
    GtkStyleContext *lc = gtk_widget_get_style_context(cmd_status);

    gtk_style_context_remove_class(ec, "cmd-success");
    gtk_style_context_remove_class(ec, "cmd-error");
    gtk_style_context_remove_class(lc, "text-green");
    gtk_style_context_remove_class(lc, "text-red");

    if (valid)
    {
        gtk_style_context_add_class(ec, "cmd-success");
        gtk_style_context_add_class(lc, "text-green");
        gtk_label_set_text(GTK_LABEL(cmd_status), "command executed");

        /* Success icon */
        gtk_entry_set_icon_from_icon_name(
            GTK_ENTRY(e),
            GTK_ENTRY_ICON_PRIMARY,
            "emblem-ok-symbolic");

        gtk_widget_set_sensitive(GTK_WIDGET(e), FALSE);

        if (cmd_hist_count < CMD_HISTORY_SIZE)
        {
            cmd_history[cmd_hist_count++] = g_strdup(gtk_entry_get_text(e));
        }
        else
        {
            g_free(cmd_history[0]);
            memmove(&cmd_history[0], &cmd_history[1],
                    (CMD_HISTORY_SIZE - 1) * sizeof(char *));
            cmd_history[CMD_HISTORY_SIZE - 1] =
                g_strdup(gtk_entry_get_text(e));
        }
        cmd_hist_index = cmd_hist_count;
    }
    else
    {
        gtk_style_context_add_class(ec, "cmd-error");
        gtk_style_context_add_class(lc, "text-red");
        gtk_label_set_text(GTK_LABEL(cmd_status), "command execution failed");

        /* Error icon */
        gtk_entry_set_icon_from_icon_name(
            GTK_ENTRY(e),
            GTK_ENTRY_ICON_PRIMARY,
            "dialog-error-symbolic");
        gtk_widget_set_sensitive(GTK_WIDGET(e), FALSE);
    }

    CmdClearCtx *ctx = g_malloc(sizeof(CmdClearCtx));
    ctx->entry = GTK_WIDGET(e);
    ctx->label = cmd_status;

    g_timeout_add(5000, clear_cmd_feedback, ctx);
}

/* ---------- State Machine ---------- */

static gboolean is_valid_ipv4(const char *ip)
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

static void apply_state()
{
    gboolean connected = (state != STATE_DISCONNECTED);
    gboolean running = (state == STATE_RUNNING);

    const char *ip = gtk_entry_get_text(GTK_ENTRY(connect_entry));
    gboolean ip_ok = is_valid_ipv4(ip);

    GtkStyleContext *ctx =
        gtk_widget_get_style_context(connect_entry);

    gtk_style_context_remove_class(ctx, "cmd-error");

    if (*ip && !ip_ok)
        gtk_style_context_add_class(ctx, "cmd-error");

    set_enabled(connect_btn, !connected && ip_ok);

    set_enabled(connect_entry, !connected);

    set_enabled(disconnect_btn, connected && !running);
    set_enabled(start_btn, connected && !running);
    set_enabled(stop_btn, running);

    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        set_enabled(checkboxes[i], running);
        if (!running)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkboxes[i]), FALSE);
    }

    set_enabled(combo, running);
    set_enabled(hz_entry, running);
    set_enabled(config_btn,
                running && strlen(gtk_entry_get_text(GTK_ENTRY(hz_entry))) > 0);
    set_enabled(cmd_entry, running);
}

/* ---------- Buttons ---------- */

static void connect_clicked(GtkButton *b, gpointer d)
{
    const char *ip = gtk_entry_get_text(GTK_ENTRY(connect_entry));
    if (!ip || !*ip)
        return;

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0)
    {
        perror("socket");
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
    {
        perror("inet_pton");
        close(sock_fd);
        sock_fd = -1;
        return;
    }

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("connect");
        close(sock_fd);
        sock_fd = -1;
        return;
    }

    printf("Connected to server %s\n", ip);

    strncpy(connected_ip, ip, sizeof(connected_ip) - 1);
    connected_ip[sizeof(connected_ip) - 1] = '\0';

    state = STATE_CONNECTED;
    apply_state();
}

static gboolean on_window_delete(GtkWidget *widget,
                                 GdkEvent *event,
                                 gpointer user_data)
{
    /* If not connected, allow close immediately */
    if (state == STATE_DISCONNECTED)
    {
        gtk_main_quit();
        return TRUE;
    }

    /* Build message */
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Client connected to IP %s.\n\nAre you sure you want to close?",
             connected_ip[0] ? connected_ip : "unknown");

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(widget),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO,
        "%s",
        msg);

    gtk_window_set_title(GTK_WINDOW(dialog), "Confirm Exit");

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response != GTK_RESPONSE_YES)
    {
        /* User cancelled close */
        return TRUE;
    }

    /* User confirmed close */

    if (state == STATE_RUNNING)
    {
        /* Stop streaming first */
        send(sock_fd, "STOP\n", 5, 0);
        printf("Sent STOP (on exit)\n");
    }

    if (sock_fd >= 0)
    {
        close(sock_fd);
        sock_fd = -1;
        printf("Client socket closed (on exit)\n");
    }

    gtk_main_quit();

    /* Allow GTK to close window */
    return TRUE;
}

static void disconnect_clicked(GtkButton *b, gpointer d)
{
    if (sock_fd >= 0)
    {
        close(sock_fd);
        sock_fd = -1;
        printf("Disconnected from server\n");
    }

    state = STATE_DISCONNECTED;
    apply_state();
}

static void start_clicked(GtkButton *b, gpointer d)
{
    if (sock_fd < 0)
        return;

    send(sock_fd, "START\n", 6, 0);
    printf("Sent START\n");

    state = STATE_RUNNING;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkboxes[0]), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkboxes[2]), TRUE);
    update_dropdown();
    apply_state();
}

static void stop_clicked(GtkButton *b, gpointer d)
{
    if (sock_fd < 0)
        return;

    send(sock_fd, "STOP\n", 5, 0);
    printf("Sent STOP\n");

    state = STATE_CONNECTED;
    apply_state();
}

static gboolean draw_grid(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    int width = alloc.width;
    int height = alloc.height;

    const int grid_spacing = 70;  // bigger grid
    const int bottom_margin = 35; // space for x-axis label
    const int left_margin = 45;   // space for y-axis
    const int arrow_size = 10;    // axis arrow size

    /* ================== Faint Grid ================== */
    cairo_set_source_rgba(cr, 0.7, 0.7, 0.7, 0.1);
    cairo_set_line_width(cr, 1.0);

    /* Vertical grid lines */
    for (int x = left_margin; x <= width; x += grid_spacing)
    {
        cairo_move_to(cr, x + 0.5, 0);
        cairo_line_to(cr, x + 0.5, height - bottom_margin);
    }

    /* Horizontal grid lines */
    for (int y = 0; y <= height - bottom_margin; y += grid_spacing)
    {
        cairo_move_to(cr, left_margin, y + 0.5);
        cairo_line_to(cr, width, y + 0.5);
    }

    cairo_stroke(cr);

    /* ================== Theme-aware axis color ================== */
    GtkStyleContext *context =
        gtk_widget_get_style_context(widget);

    GdkRGBA fg;
    gtk_style_context_get_color(
        context,
        gtk_style_context_get_state(context),
        &fg);

    cairo_set_source_rgba(cr,
                          fg.red,
                          fg.green,
                          fg.blue,
                          fg.alpha);

    /* ================== Axes ================== */

    /* ================== Legend Placeholder ================== */
    /* ================== Dynamic Legend ================== */

    const int legend_x = width - 200;
    int legend_y = 24;
    const int box_size = 12;
    const int row_spacing = 20;

    cairo_set_font_size(cr, 12);

    /* Legend title */
    cairo_set_source_rgba(cr,
                          fg.red,
                          fg.green,
                          fg.blue,
                          fg.alpha);

    cairo_move_to(cr, legend_x, legend_y);
    cairo_show_text(cr, "Legend:");
    legend_y += row_spacing;

    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        if (!is_sensor_selected(i))
            continue;

        /* --- Color square --- */
        cairo_set_source_rgb(cr,
                             plot_colors[i][0],
                             plot_colors[i][1],
                             plot_colors[i][2]);

        cairo_rectangle(cr,
                        legend_x,
                        legend_y - box_size + 2,
                        box_size,
                        box_size);
        cairo_fill(cr);

        /* --- Label text (theme color, NOT colored) --- */
        cairo_set_source_rgba(cr,
                              fg.red,
                              fg.green,
                              fg.blue,
                              fg.alpha);

        cairo_move_to(cr,
                      legend_x + box_size + 8,
                      legend_y + 2);

        cairo_show_text(cr, sensor_labels[i]);

        legend_y += row_spacing;
    }

    /* ================== Signal Plot Skeleton ================== */
    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        if (!is_sensor_selected(i))
            continue;

        /*
         * REAL DATA WILL BE DRAWN HERE LATER
         *
         * When backend arrives:
         *   - use plot_colors[i]
         *   - draw only for selected sensors
         *
         * This conditional is the important part.
         */
    }

    cairo_set_line_width(cr, 2.5);

    /* Y-axis */
    cairo_move_to(cr, left_margin + 0.5, arrow_size);
    cairo_line_to(cr, left_margin + 0.5, height - bottom_margin);

    /* X-axis */
    cairo_move_to(cr, left_margin, height - bottom_margin + 0.5);
    cairo_line_to(cr, width - arrow_size, height - bottom_margin + 0.5);

    cairo_stroke(cr);

    /* ================== Axis Arrows ================== */
    cairo_set_line_width(cr, 2.5);

    /* X-axis arrow (right) */
    cairo_move_to(cr, width - arrow_size, height - bottom_margin);
    cairo_line_to(cr, width, height - bottom_margin + 0.5);
    cairo_line_to(cr, width - arrow_size, height - bottom_margin + arrow_size);
    cairo_stroke(cr);

    /* Y-axis arrow (up) */
    cairo_move_to(cr, left_margin - arrow_size, arrow_size);
    cairo_line_to(cr, left_margin + 0.5, 0);
    cairo_line_to(cr, left_margin + arrow_size, arrow_size);
    cairo_stroke(cr);

    /* ================== X-axis Label ================== */
    const char *xlabel = "Time in microseconds (\xCE\xBCs)"; // µs

    cairo_select_font_face(cr, "Sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    cairo_set_source_rgba(cr,
                          fg.red,
                          fg.green,
                          fg.blue,
                          fg.alpha);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, xlabel, &ext);

    double x = (width - ext.width) / 2.0 - ext.x_bearing;
    double y = height - 10;

    cairo_move_to(cr, x, y);
    cairo_show_text(cr, xlabel);

    return FALSE;
}

/* ---------- UI ---------- */

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    load_css();

    sensor_freq =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Measurement Network Gateway - GUI");
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size(GTK_WINDOW(win), 1200, 800);

    g_signal_connect(win, "delete-event",
                     G_CALLBACK(on_window_delete), NULL);

    GtkWidget *main_v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(main_v), 16);
    gtk_container_add(GTK_CONTAINER(win), main_v);

    /* Section A */
    GtkWidget *secA = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(main_v), secA, FALSE, FALSE, 0);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(secA), left, FALSE, FALSE, 0);

    connect_entry = gtk_entry_new();
    GtkWidget *cnk_label = gtk_label_new("Enter Server IP:");
    gtk_widget_set_halign(cnk_label, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(left), cnk_label, FALSE, FALSE, 6);

    gtk_entry_set_width_chars(GTK_ENTRY(connect_entry), 20);
    gtk_box_pack_start(GTK_BOX(left), connect_entry, FALSE, FALSE, 0);
    g_signal_connect(connect_entry, "focus-out-event",
                     G_CALLBACK(entry_focus_out), NULL);

    GtkWidget *space_conn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(left), space_conn, FALSE, FALSE, 0);
    connect_btn = gtk_button_new_with_label("Connect");
    disconnect_btn = gtk_button_new_with_label("Disconnect");
    /* Visual hierarchy: Connect = primary, Disconnect = caution */
    gtk_style_context_add_class(
        gtk_widget_get_style_context(connect_btn),
        "suggested-action");

    gtk_style_context_add_class(
        gtk_widget_get_style_context(disconnect_btn),
        "destructive-action");
    gtk_box_pack_start(GTK_BOX(left), connect_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), disconnect_btn, FALSE, FALSE, 0);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    /* This box expands and takes all remaining space */
    gtk_box_pack_start(GTK_BOX(secA), right, TRUE, TRUE, 0);

    /* Filler to push content to the right */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(right), spacer, TRUE, TRUE, 0);

    GtkWidget *chk_label = gtk_label_new("SENSORS:");
    gtk_widget_set_halign(chk_label, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(right), chk_label, FALSE, FALSE, 6);

    /* Checkboxes packed AFTER spacer = hard right aligned */
    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        checkboxes[i] =
            gtk_check_button_new_with_label(sensor_labels[i]);
        gtk_box_pack_start(GTK_BOX(right), checkboxes[i], FALSE, FALSE, 0);
        g_signal_connect(checkboxes[i], "toggled",
                         G_CALLBACK(checkbox_changed), NULL);
    }

    /* Section B */
    /* ---------- Section B : Graph ---------- */
    GtkWidget *secB = gtk_frame_new("Plot");
    gtk_widget_set_vexpand(secB, TRUE);
    gtk_box_pack_start(GTK_BOX(main_v), secB, TRUE, TRUE, 0);

    graph_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(graph_area, TRUE);
    gtk_widget_set_vexpand(graph_area, TRUE);
    gtk_container_add(GTK_CONTAINER(secB), graph_area);

    g_signal_connect(graph_area, "draw",
                     G_CALLBACK(draw_grid), NULL);

    /* Section C */
    GtkWidget *secC = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(main_v), secC, FALSE, FALSE, 12);

    /* ---- Left controls (Start / Stop) ---- */
    GtkWidget *secC_left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(secC), secC_left, FALSE, FALSE, 0);

    start_btn = gtk_button_new_with_label("Start");
    stop_btn = gtk_button_new_with_label("Stop");

    /* Make Start the primary action */
    gtk_style_context_add_class(
        gtk_widget_get_style_context(start_btn),
        "suggested-action");

    /* Make Stop visually distinct but not aggressive */
    gtk_style_context_add_class(
        gtk_widget_get_style_context(stop_btn),
        "destructive-action");

    gtk_box_pack_start(GTK_BOX(secC_left), start_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(secC_left), stop_btn, FALSE, FALSE, 0);

    /* ---- Expanding spacer ---- */
    GtkWidget *secC_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(secC), secC_spacer, TRUE, TRUE, 0);

    /* ---- Right controls (dropdown + Hz + Configure) ---- */
    GtkWidget *secC_right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(secC), secC_right, FALSE, FALSE, 0);

    combo = gtk_combo_box_text_new();
    gtk_box_pack_start(GTK_BOX(secC_right), combo, FALSE, FALSE, 0);
    g_signal_connect(combo, "changed", G_CALLBACK(combo_changed), NULL);

    GtkWidget *hz_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    hz_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(hz_box), hz_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hz_box), gtk_label_new("Hz"), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(secC_right), hz_box, FALSE, FALSE, 0);

    g_signal_connect(hz_entry, "changed", G_CALLBACK(hz_changed), NULL);
    g_signal_connect(hz_entry, "focus-out-event",
                     G_CALLBACK(entry_focus_out), NULL);

    GtkWidget *space_cfg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(secC_right), space_cfg, FALSE, FALSE, 0);
    config_btn = gtk_button_new_with_label("Configure");
    gtk_box_pack_start(GTK_BOX(secC_right), config_btn, FALSE, FALSE, 0);
    g_signal_connect(config_btn, "clicked",
                     G_CALLBACK(configure_clicked), NULL);

    /* Section D */
    cmd_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(cmd_entry),
                                   "CONFIGURE SENSOR_ID FREQ_IN_HZ");
    gtk_box_pack_start(GTK_BOX(main_v), cmd_entry, FALSE, FALSE, 0);
    /* Command line default icon (idle state) */
    gtk_entry_set_icon_from_icon_name(
        GTK_ENTRY(cmd_entry),
        GTK_ENTRY_ICON_PRIMARY,
        "utilities-terminal-symbolic");

    g_signal_connect(cmd_entry, "activate",
                     G_CALLBACK(cmd_enter), NULL);
    g_signal_connect(cmd_entry, "focus-out-event",
                     G_CALLBACK(entry_focus_out), NULL);
    g_signal_connect(cmd_entry, "key-press-event",
                     G_CALLBACK(cmd_key_press), NULL);

    cmd_status = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(main_v), cmd_status, FALSE, FALSE, 0);

    g_signal_connect(connect_btn, "clicked", G_CALLBACK(connect_clicked), NULL);
    g_signal_connect(disconnect_btn, "clicked", G_CALLBACK(disconnect_clicked), NULL);
    g_signal_connect(start_btn, "clicked", G_CALLBACK(start_clicked), NULL);
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(stop_clicked), NULL);
    g_signal_connect(connect_entry, "changed", G_CALLBACK(apply_state), NULL);

    apply_state();
    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
