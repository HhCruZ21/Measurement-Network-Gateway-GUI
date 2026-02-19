/******************************************************************************
 * File: gui.c
 * Author: Haizon Helet Cruz
 * Date: 2026-02-13
 *
 * Description:
 * Main GUI implementation for the Measurement Network Gateway application.
 *
 * This file contains:
 *  - GTK-based graphical user interface construction
 *  - TCP client networking logic for communication with backend server
 *  - Multithreaded receive loop for sensor streaming
 *  - Command-line interface parsing and validation
 *  - Real-time plotting using Cairo
 *  - State machine handling (DISCONNECTED / CONNECTED / RUNNING)
 *  - Sensor configuration management and dynamic time-window scaling
 *  - Command history and user feedback system
 *
 * The GUI supports:
 *  - Connecting to a remote measurement device via IPv4
 *  - Starting/stopping streaming
 *  - Configuring per-sensor sampling frequency (10–1000 Hz)
 *  - Live multi-sensor plotting with dynamic legend and scaling
 *  - Safe disconnect, shutdown, and exit handling
 *
 ******************************************************************************/

#include <string.h>
#include <ctype.h>

/* ---------- Connect with backend ---------- */

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>
#include <pthread.h>
#include <inttypes.h>

/* ---------- Sensor Model ---------- */

#include "utils.h"

#define VISIBLE_SAMPLES 300
#define MIN_WINDOW_US 50000ULL   // 50 ms
#define MAX_WINDOW_US 5000000ULL // 5 s

void push_sample(int sid, double value, uint64_t ts);
static void *net_rx_thread();
static int recv_all(int fd, void *buf, size_t len);
static void set_connect_status(const char *msg, const char *color);
static void connect_clicked();
static void disconnect_clicked(GtkButton *b, gpointer d);
static void start_clicked();
static void stop_clicked();
static gboolean update_secB_info(gpointer data);
static int checked_count();

GtkWidget *main_window = NULL;

static pthread_mutex_t sample_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t sample_ts[SENSOR_COUNT][MAX_SAMPLES];

static uint64_t server_t0 = 0;

static gboolean suppress_checkbox_cb = FALSE;

static int sock_fd = -1;
static pthread_t net_thread;
static volatile int net_running = 0;

static char connected_ip[64] = {0};

/* ---------- Store command history ---------- */

static char *cmd_history[CMD_HISTORY_SIZE];
static int cmd_hist_count = 0;
static int cmd_hist_index = -1;

/* ---------- Per-sensor Y scaling ---------- */
static const double sensor_y_max[SENSOR_COUNT] = {
    32768.0, // Temp
    4095.0,  // ADC 0
    4095.0,  // ADC 1
    255.0,   // Switches
    31.0     // Push buttons
};

const char *sensor_ids[SENSOR_COUNT] = {
    "TEMP", "ADC0", "ADC1", "SW", "PB"};

const char *sensor_labels[SENSOR_COUNT] = {
    "Temp", "ADC 0", "ADC 1", "Switches", "Push Buttons"};

static double sample_data[SENSOR_COUNT][MAX_SAMPLES];
static int sample_count[SENSOR_COUNT] = {0};
static int sample_head[SENSOR_COUNT] = {0};
static guint connect_status_timeout_id = 0;

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
static AppState state = STATE_DISCONNECTED;

/* ---------- Widgets ---------- */

GtkWidget *connect_entry, *connect_btn, *disconnect_btn, *shutdown_btn;
GtkWidget *start_btn, *stop_btn;
GtkWidget *connect_status_label;

GtkWidget *checkboxes[SENSOR_COUNT];
GtkWidget *combo;
GtkWidget *hz_entry, *config_btn;
GtkWidget *cmd_entry, *cmd_status;
GtkWidget *secB_info_view;
GtkTextBuffer *secB_info_buffer;
GtkWidget *info_scroll;

GHashTable *sensor_freq;

/**
 * @brief Resets all plotting buffers and timestamp reference.
 *
 * Clears sample counts, head indices, and resets the
 * server timestamp origin used for relative plotting.
 *
 * @return void
 */
static void reset_plot_state(void)
{
    server_t0 = 0;

    for (int s = 0; s < SENSOR_COUNT; s++)
    {
        sample_count[s] = 0;
        sample_head[s] = 0;
    }
}

/**
 * @brief Applies UI state based on current application state.
 *
 * Enables/disables buttons, entries, and controls depending on:
 * - Connection state
 * - Running state
 * - IP validity
 *
 * @return void
 */
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
    set_enabled(shutdown_btn, connected && !running);
    set_enabled(start_btn, connected && !running);
    set_enabled(stop_btn, running);

    if (!running)
    {
        /* Not running → disable all */
        for (int i = 0; i < SENSOR_COUNT; i++)
            set_enabled(checkboxes[i], FALSE);
    }
    else
    {
        /* Running → enforce 2-sensor rule */
        int selected = checked_count();

        for (int i = 0; i < SENSOR_COUNT; i++)
        {
            gboolean active =
                gtk_toggle_button_get_active(
                    GTK_TOGGLE_BUTTON(checkboxes[i]));

            if (selected >= 2 && !active)
                set_enabled(checkboxes[i], FALSE);
            else
                set_enabled(checkboxes[i], TRUE);
        }
    }

    set_enabled(combo, running);
    set_enabled(hz_entry, running);
    set_enabled(config_btn,
                running && strlen(gtk_entry_get_text(GTK_ENTRY(hz_entry))) > 0);
    set_enabled(cmd_entry, TRUE);
}

static void update_info_text_colors(GtkWidget *widget)
{
    GtkStyleContext *ctx =
        gtk_widget_get_style_context(secB_info_view);

    GdkRGBA fg;
    gtk_style_context_get_color(ctx,
                                GTK_STATE_FLAG_NORMAL,
                                &fg);

    GdkRGBA time_color = fg;
    time_color.alpha = 0.65;

    GtkTextTagTable *table =
        gtk_text_buffer_get_tag_table(secB_info_buffer);

    GtkTextTag *temp_tag =
        gtk_text_tag_table_lookup(table, "temp_tag");

    GtkTextTag *adc_tag =
        gtk_text_tag_table_lookup(table, "adc_tag");

    GtkTextTag *time_tag =
        gtk_text_tag_table_lookup(table, "time_tag");

    if (!temp_tag)
    {
        temp_tag = gtk_text_buffer_create_tag(
            secB_info_buffer,
            "temp_tag",
            NULL);
    }

    if (!adc_tag)
    {
        adc_tag = gtk_text_buffer_create_tag(
            secB_info_buffer,
            "adc_tag",
            NULL);
    }

    if (!time_tag)
    {
        time_tag = gtk_text_buffer_create_tag(
            secB_info_buffer,
            "time_tag",
            NULL);
    }

    /* Update tag colors instead of recreating */
    g_object_set(temp_tag,
                 "foreground-rgba", &fg,
                 NULL);

    g_object_set(adc_tag,
                 "foreground-rgba", &fg,
                 NULL);

    g_object_set(time_tag,
                 "foreground-rgba", &time_color,
                 NULL);
}

/**
 * @brief Handles unexpected connection loss from network thread.
 *
 * Stops network thread, closes socket, resets buffers,
 * updates UI state to DISCONNECTED.
 *
 * @param data Unused GTK callback data
 * @return gboolean G_SOURCE_REMOVE (run once)
 */
static gboolean handle_connection_lost(gpointer data)
{
    (void)data;

    if (net_running)
        net_running = 0;

    if (sock_fd >= 0)
    {
        close(sock_fd);
        sock_fd = -1;
    }

    reset_plot_state();

    state = STATE_DISCONNECTED;

    set_connect_status("Connection lost", "red");
    apply_state();

    printf("[GUI] Connection lost → auto-disconnected\n");

    return G_SOURCE_REMOVE; // run once
}

/**
 * @brief Counts how many sensor checkboxes are active.
 *
 * @return int Number of selected sensors
 */
static int checked_count()
{
    int c = 0;
    for (int i = 0; i < SENSOR_COUNT; i++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checkboxes[i])))
            c++;
    return c;
}

/**
 * @brief Handles Shutdown button click.
 *
 * Prompts user for confirmation, sends STOP (if running),
 * sends SHUTDOWN command to server, cleans up network,
 * and exits the application.
 *
 * @param b Button widget (unused)
 * @param d User data (unused)
 * @return void
 */
static void shutdown_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;

    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(main_window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO,
        "Are you sure you want to shutdown the application?");

    gtk_window_set_title(GTK_WINDOW(dialog), "Confirm Shutdown");

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response != GTK_RESPONSE_YES)
        return;

    /* Stop streaming if running */
    if (state == STATE_RUNNING && sock_fd >= 0)
    {
        send(sock_fd, "STOP\n", 5, 0);
        printf("Sent STOP (before shutdown)\n");
    }

    send(sock_fd, "SHUTDOWN\n", 9, 0);
    printf("Sent SHUTDOWN\n");

    if (net_running)
    {
        net_running = 0;
        shutdown(sock_fd, SHUT_RDWR);
        pthread_join(net_thread, NULL);
    }

    if (sock_fd >= 0)
    {
        close(sock_fd);
        sock_fd = -1;
    }

    state = STATE_DISCONNECTED;
    apply_state();
    gtk_main_quit();
}

/**
 * @brief Triggers graph redraw.
 *
 * Queues a GTK draw event for the plotting area.
 *
 * @return gboolean G_SOURCE_CONTINUE
 */
static gboolean redraw_graph()
{
    gtk_widget_queue_draw(graph_area);
    return G_SOURCE_CONTINUE;
}

/**
 * @brief Updates sensor frequency configuration received from server.
 *
 * Adjusts internal frequency table and dynamically recalculates
 * time window for ADC0 based on sampling rate.
 *
 * @param data Pointer to RatesMsg structure
 * @return gboolean G_SOURCE_REMOVE
 */
static gboolean handle_rates_update(gpointer data)
{
    RatesMsg *msg = (RatesMsg *)data;
    server_t0 = 0;

    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", msg->rates[i].rate_hz);

        g_hash_table_replace(sensor_freq,
                             g_strdup(sensor_ids[msg->rates[i].sensor_id]),
                             g_strdup(buf));

        /* Dynamic time window for ADC0 */
        if (msg->rates[i].sensor_id == adc_zero_sid &&
            msg->rates[i].rate_hz > 0)
        {
            double sample_period_us =
                1e6 / msg->rates[i].rate_hz;

            time_window_us =
                (uint64_t)(VISIBLE_SAMPLES * sample_period_us);

            if (time_window_us < MIN_WINDOW_US)
                time_window_us = MIN_WINDOW_US;
            if (time_window_us > MAX_WINDOW_US)
                time_window_us = MAX_WINDOW_US;

            printf("[GUI] Time window set to %.2f ms\n",
                   time_window_us / 1000.0);
        }
    }

    /* Update Hz entry for active sensor */
    const char *active =
        gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));

    if (active)
    {
        const char *val =
            g_hash_table_lookup(sensor_freq, active);
        gtk_entry_set_text(GTK_ENTRY(hz_entry),
                           val ? val : "");
    }

    if (!msg)
        return G_SOURCE_REMOVE;

    g_free(msg);
    return G_SOURCE_REMOVE;
}

/**
 * @brief Network receive thread.
 *
 * Continuously receives:
 * - RATES messages
 * - Streaming sensor batches
 *
 * Dispatches GUI updates via g_idle_add().
 *
 * @return void* NULL on exit
 */
static void *net_rx_thread()
{
    const size_t batch_size = 1440;
    sensor_data_t batch[batch_size / sizeof(sensor_data_t)];

    char hdr[6];

    while (net_running)
    {
        // Otherwise assume streaming batch
        uint32_t net_size;
        if (recv_all(sock_fd, &net_size, sizeof(net_size)) < 0)
            break;

        uint32_t payload_size = ntohl(net_size);

        if (payload_size == 0 || payload_size > sizeof(batch))
        {
            printf("Invalid payload size: %u\n", payload_size);
            break;
        }

        if (recv_all(sock_fd, batch, payload_size) < 0)
            break;

        int samples = payload_size / sizeof(sensor_data_t);

        for (int i = 0; i < samples; i++)
        {
            sensor_data_t *pkt = &batch[i];

            if (pkt->sensor_id < SENSOR_COUNT)
            {
                push_sample(pkt->sensor_id,
                            pkt->sensor_value,
                            pkt->timestamp);
            }
        }
    }

    g_idle_add(handle_connection_lost, NULL);
    return NULL;
}

/**
 * @brief Clears text selection and moves cursor to end when focus leaves entry.
 *
 * @param w GTK widget
 * @return gboolean FALSE (propagate event)
 */
static gboolean entry_focus_out(GtkWidget *w)
{
    gtk_editable_select_region(GTK_EDITABLE(w), -1, -1);
    gtk_editable_set_position(GTK_EDITABLE(w), -1);
    return FALSE;
}

/**
 * @brief Updates sensor dropdown based on selected checkboxes.
 *
 * Rebuilds combo box entries dynamically.
 *
 * @return void
 */
static void update_dropdown()
{
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));

    int added = 0;
    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checkboxes[i])))
        {
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo),
                                      sensor_ids[i], sensor_labels[i]);
            added++;
        }
    }

    if (added > 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

/**
 * @brief Inserts a new sensor sample into circular buffer.
 *
 * Handles timestamp reset detection and maintains
 * relative timestamp origin.
 *
 * @param sid Sensor ID
 * @param value Sensor value
 * @param ts Absolute timestamp (microseconds)
 * @return void
 */
void push_sample(int sid, double value, uint64_t ts)
{
    pthread_mutex_lock(&sample_lock);

    if (server_t0 == 0)
        server_t0 = ts;

    /* Convert to relative time */
    uint64_t rel_ts = ts - server_t0;

    int head = sample_head[sid];

    sample_data[sid][head] = value;
    sample_ts[sid][head] = rel_ts;

    if (sample_count[sid] < MAX_SAMPLES)
        sample_count[sid]++;

    sample_head[sid] = (head + 1) % MAX_SAMPLES;

    pthread_mutex_unlock(&sample_lock);

    /* Trigger redraw */
    g_idle_add((GSourceFunc)gtk_widget_queue_draw, graph_area);

    /* ---- NEW: Log last 10 Temp + ADC1 with timestamp ---- */
    /* ---- Log only once per timestamp (on ADC1 arrival) ---- */
    if (sid == 2) // Use ADC1 as commit trigger
    {
        pthread_mutex_lock(&sample_lock);

        double temp = 0.0;
        double adc1 = 0.0;
        uint64_t rel_ts = 0;

        /* Latest TEMP */
        if (sample_count[0] > 0)
        {
            int idx0 = (sample_head[0] - 1 + MAX_SAMPLES) % MAX_SAMPLES;
            temp = sample_data[0][idx0];
        }

        /* Latest ADC1 */
        if (sample_count[2] > 0)
        {
            int idx2 = (sample_head[2] - 1 + MAX_SAMPLES) % MAX_SAMPLES;
            adc1 = sample_data[2][idx2];
            rel_ts = sample_ts[2][idx2];
        }

        pthread_mutex_unlock(&sample_lock);

        info_line *info = g_malloc(sizeof(info_line));

        info->temp = temp;
        info->adc_v = (adc1 / 4095.0) * 3.3;
        info->ts_us = rel_ts / 1000.0;

        g_idle_add(update_secB_info, info);
    }
}

/**
 * @brief Handles sensor dropdown change.
 *
 * Updates frequency entry field with stored sensor rate.
 *
 * @param box GTK combo box
 * @return void
 */
static void combo_changed(GtkComboBox *box)
{
    const char *id = gtk_combo_box_get_active_id(box);
    if (!id)
        return;

    const char *val = g_hash_table_lookup(sensor_freq, id);
    gtk_entry_set_text(GTK_ENTRY(hz_entry), val ? val : "");
}

/**
 * @brief Checks if a sensor checkbox is active.
 *
 * @param idx Sensor index
 * @return gboolean TRUE if selected
 */
static gboolean is_sensor_selected(int idx)
{
    return gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(checkboxes[idx]));
}

/**
 * @brief Handles Up/Down arrow navigation for command history.
 *
 * @param w Entry widget
 * @param e Key event
 * @return gboolean TRUE if handled
 */
static gboolean cmd_key_press(GtkWidget *w, GdkEventKey *e)
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

/**
 * @brief Handles sensor checkbox toggle event.
 *
 * Prevents deselecting below minimum during running state.
 * Triggers dropdown update and graph redraw.
 *
 * @param btn Toggle button
 * @return void
 */
static void checkbox_changed(GtkToggleButton *btn)
{
    if (suppress_checkbox_cb)
        return;

    int selected = 0;

    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        if (gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(checkboxes[i])))
        {
            selected++;
        }
    }

    /* If more than 2 selected, revert this toggle */
    if (selected > 2)
    {
        suppress_checkbox_cb = TRUE;
        gtk_toggle_button_set_active(btn, FALSE);
        suppress_checkbox_cb = FALSE;
        return;
    }

    /* If exactly 2 selected → disable all unchecked */
    if (selected == 2)
    {
        for (int i = 0; i < SENSOR_COUNT; i++)
        {
            if (!gtk_toggle_button_get_active(
                    GTK_TOGGLE_BUTTON(checkboxes[i])))
            {
                gtk_widget_set_sensitive(checkboxes[i], FALSE);
            }
        }
    }
    else
    {
        /* If less than 2 → enable all */
        for (int i = 0; i < SENSOR_COUNT; i++)
        {
            gtk_widget_set_sensitive(checkboxes[i], TRUE);
        }
    }

    update_dropdown();

    if (graph_area)
        gtk_widget_queue_draw(graph_area);
}

/**
 * @brief Validates frequency entry field.
 *
 * Ensures numeric value within 10–1000 Hz.
 * Updates visual error styling.
 *
 * @return void
 */
static void hz_changed()
{
    const char *txt = gtk_entry_get_text(GTK_ENTRY(hz_entry));

    gboolean valid = TRUE;

    if (!txt || !*txt)
        valid = FALSE;
    else
    {
        /* numeric check */
        for (int i = 0; txt[i]; i++)
        {
            if (!isdigit((unsigned char)txt[i]))
            {
                valid = FALSE;
                break;
            }
        }

        if (valid)
        {
            int val = atoi(txt);
            if (val < 10 || val > 1000)
                valid = FALSE;
        }
    }

    GtkStyleContext *ctx =
        gtk_widget_get_style_context(hz_entry);

    gtk_style_context_remove_class(ctx, "cmd-error");

    if (!valid && txt && *txt)
        gtk_style_context_add_class(ctx, "cmd-error");

    set_enabled(config_btn, valid);
}

/**
 * @brief Sends CONFIGURE command to server.
 *
 * Validates selected sensor and frequency,
 * updates local frequency model.
 *
 * @return void
 */
static void configure_clicked()
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
    if (rate < 10 || rate > 1000)
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

/**
 * @brief Opens CLI help text in external terminal.
 *
 * Spawns x-terminal-emulator with HELP_TEXT content.
 *
 * @return void
 */
static void open_help_terminal(void)
{
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "cat << 'EOF'\n%s\nEOF\n"
             "echo\n"
             "read -p 'Press Enter to close...'\n",
             HELP_TEXT);

    char *argv[] = {
        "x-terminal-emulator",
        "-e",
        "bash",
        "-c",
        cmd,
        NULL};

    g_spawn_async(NULL, argv, NULL,
                  G_SPAWN_SEARCH_PATH,
                  NULL, NULL, NULL, NULL);
}

/**
 * @brief Parses and executes command entered in CLI entry field.
 *
 * Supports:
 * CONNECT, DISCONNECT, START, STOP,
 * SHUTDOWN, CONFIGURE, HELP
 *
 * Performs syntax validation and state checks.
 *
 * @param e Entry widget
 * @return void
 */
static void cmd_enter(GtkEntry *e)
{
    char buf[128];

    strncpy(buf, gtk_entry_get_text(e), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    char *raw = g_strdup(gtk_entry_get_text(e));
    g_strstrip(raw);

    if (g_ascii_strcasecmp(raw, "HELP") == 0)
    {
        open_help_terminal();

        /* ---- ADD THIS BLOCK ---- */
        if (cmd_hist_count < CMD_HISTORY_SIZE)
        {
            cmd_history[cmd_hist_count++] = g_strdup("HELP");
        }
        else
        {
            g_free(cmd_history[0]);
            memmove(&cmd_history[0], &cmd_history[1],
                    (CMD_HISTORY_SIZE - 1) * sizeof(char *));
            cmd_history[CMD_HISTORY_SIZE - 1] = g_strdup("HELP");
        }
        cmd_hist_index = cmd_hist_count;

        gtk_entry_set_icon_from_icon_name(
            GTK_ENTRY(e),
            GTK_ENTRY_ICON_PRIMARY,
            "help-browser-symbolic");

        gtk_label_set_text(GTK_LABEL(cmd_status),
                           "Help opened in terminal");

        gtk_widget_set_sensitive(GTK_WIDGET(e), FALSE);

        g_free(raw);

        CmdClearCtx *ctx = g_malloc(sizeof(CmdClearCtx));
        ctx->entry = GTK_WIDGET(e);
        ctx->label = cmd_status;
        g_timeout_add(3000, clear_cmd_feedback, ctx);

        return;
    }

    g_free(raw);

    char *tok1 = strtok(buf, " ");
    char *tok2 = strtok(NULL, " ");
    char *tok3 = strtok(NULL, " ");
    char *extra = strtok(NULL, " ");

    gboolean valid = FALSE;
    CmdError err = CMD_ERR_SYNTAX;
    const char *id = NULL;

    if (!tok1)
    {
        err = CMD_ERR_SYNTAX;
        goto done;
    }

    /* ================= CONNECT ================= */
    if (g_ascii_strcasecmp(tok1, "CONNECT") == 0)
    {
        if (!tok2 || tok3 || extra)
        {
            err = CMD_ERR_SYNTAX;
            goto done;
        }

        /* Validate IP */
        if (!is_valid_ipv4(tok2))
        {
            err = CMD_ERR_SYNTAX;
            goto done;
        }

        /* Prevent reconnect if already connected */
        if (state != STATE_DISCONNECTED)
        {
            err = CMD_ERR_SYNTAX;
            goto done;
        }

        /* Set IP into textbox */
        gtk_entry_set_text(GTK_ENTRY(connect_entry), tok2);

        /* Trigger same workflow as button click */
        connect_clicked();

        valid = TRUE;
        err = CMD_OK;

        goto done;
    }

    /* ================= DISCONNECT ================= */
    if (g_ascii_strcasecmp(tok1, "DISCONNECT") == 0)
    {
        if (tok2 || tok3 || extra)
        {
            err = CMD_ERR_SYNTAX;
            goto done;
        }

        if (state == STATE_DISCONNECTED)
        {
            err = CMD_ERR_NOT_CONNECTED; // add this enum if not exists
            goto done;
        }

        if (state == STATE_RUNNING)
        {
            err = CMD_ERR_RUNNING; // <-- add this enum
            goto done;
        }

        /* Trigger same workflow as button click */
        disconnect_clicked(NULL, NULL);

        valid = TRUE;
        err = CMD_OK;

        goto done;
    }

    /* ================= SHUTDOWN ================= */
    if (g_ascii_strcasecmp(tok1, "SHUTDOWN") == 0)
    {
        if (tok2 || tok3 || extra)
        {
            err = CMD_ERR_SYNTAX;
            goto done;
        }

        if (state == STATE_DISCONNECTED)
        {
            err = CMD_ERR_NOT_CONNECTED;
            goto done;
        }

        if (state == STATE_RUNNING)
        {
            err = CMD_ERR_RUNNING;
            goto done;
        }

        /* Trigger same workflow as button click */
        shutdown_clicked(NULL, NULL);

        valid = TRUE;
        err = CMD_OK;

        goto done;
    }

    /* ================= START ================= */
    if (g_ascii_strcasecmp(tok1, "START") == 0)
    {
        if (tok2 || tok3 || extra)
        {
            err = CMD_ERR_SYNTAX;
            goto done;
        }

        if (state == STATE_DISCONNECTED)
        {
            err = CMD_ERR_NOT_CONNECTED;
            goto done;
        }

        if (state == STATE_RUNNING)
        {
            err = CMD_ERR_ALREADY_RUNNING; // add this enum
            goto done;
        }

        /* Trigger same workflow as button click */
        start_clicked();

        valid = TRUE;
        err = CMD_OK;

        goto done;
    }

    /* ================= STOP ================= */
    if (g_ascii_strcasecmp(tok1, "STOP") == 0)
    {
        if (tok2 || tok3 || extra)
        {
            err = CMD_ERR_SYNTAX;
            goto done;
        }

        if (state == STATE_DISCONNECTED)
        {
            err = CMD_ERR_NOT_CONNECTED;
            goto done;
        }

        if (state != STATE_RUNNING)
        {
            err = CMD_ERR_NOT_RUNNING; // add this enum
            goto done;
        }

        /* Trigger same workflow as button click */
        stop_clicked();

        valid = TRUE;
        err = CMD_OK;

        goto done;
    }

    /* ================= CONFIGURE ================= */
    if (g_ascii_strcasecmp(tok1, "CONFIGURE") == 0)
    {
        if (!tok2 || !tok3 || extra)
        {
            err = CMD_ERR_SYNTAX;
            goto done;
        }

        id = canonical_sensor(tok2);
        if (!id)
        {
            err = CMD_ERR_SENSOR;
            goto done;
        }

        for (int i = 0; tok3[i]; i++)
        {
            if (!isdigit((unsigned char)tok3[i]))
            {
                err = CMD_ERR_FREQ_RANGE;
                goto done;
            }
        }

        int rate = atoi(tok3);
        if (rate < 10 || rate > 1000)
        {
            err = CMD_ERR_FREQ_RANGE;
            goto done;
        }

        valid = TRUE;
        err = CMD_OK;

        g_hash_table_replace(sensor_freq,
                             g_strdup(id),
                             g_strdup(tok3));

        const char *active =
            gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
        if (active && strcmp(active, id) == 0)
            gtk_entry_set_text(GTK_ENTRY(hz_entry), tok3);

        if (sock_fd >= 0)
        {
            char net_cmd[64];
            snprintf(net_cmd, sizeof(net_cmd),
                     "CONFIGURE %s %d\n", id, rate);
            send(sock_fd, net_cmd, strlen(net_cmd), 0);
            printf("Sent: %s", net_cmd);
        }

        goto done;
    }

    /* Unknown command */
    err = CMD_ERR_SYNTAX;
    goto done;

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
        gtk_label_set_text(GTK_LABEL(cmd_status), "Command executed");

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
        switch (err)
        {
        case CMD_ERR_FREQ_RANGE:
            gtk_label_set_text(GTK_LABEL(cmd_status),
                               "Valid frequency is between 10 and 1000 Hz.");
            break;

        case CMD_ERR_NOT_CONNECTED:
            gtk_label_set_text(GTK_LABEL(cmd_status),
                               "Cannot disconnect: no active connection.");
            break;

        case CMD_ERR_RUNNING:
            gtk_label_set_text(GTK_LABEL(cmd_status),
                               "Cannot disconnect: GUI is running, stop and disconnect.");
            break;

        case CMD_ERR_ALREADY_RUNNING:
            gtk_label_set_text(GTK_LABEL(cmd_status),
                               "Already running.");
            break;

        case CMD_ERR_NOT_RUNNING:
            gtk_label_set_text(GTK_LABEL(cmd_status),
                               "Cannot stop: not currently running.");
            break;

        default:
            gtk_label_set_text(GTK_LABEL(cmd_status),
                               "Invalid command. Use HELP for available commands.");
            break;
        }

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

/**
 * @brief Clears connection status label after timeout.
 *
 * @param data Pointer to label widget
 * @return gboolean G_SOURCE_REMOVE
 */
static gboolean clear_connect_status(gpointer data)
{
    GtkWidget *label = GTK_WIDGET(data);
    gtk_label_set_text(GTK_LABEL(label), "");
    connect_status_timeout_id = 0;
    return G_SOURCE_REMOVE; // run once
}

/**
 * @brief Displays colored connection status message.
 *
 * Automatically clears after timeout.
 *
 * @param msg Message string
 * @param color GTK markup color string
 * @return void
 */
static void set_connect_status(const char *msg, const char *color)
{
    if (!msg || !*msg)
    {
        gtk_label_set_text(GTK_LABEL(connect_status_label), "");
        return;
    }
    char markup[256];

    /* Cancel previous timeout if any */
    if (connect_status_timeout_id)
    {
        g_source_remove(connect_status_timeout_id);
        connect_status_timeout_id = 0;
    }

    /* Set text */
    snprintf(markup, sizeof(markup),
             "<span foreground=\"%s\"><b>%s</b></span>",
             color, msg);
    gtk_label_set_markup(GTK_LABEL(connect_status_label), markup);

    /* Arm auto-clear (only if non-empty message) */
    if (msg && *msg)
    {
        connect_status_timeout_id =
            g_timeout_add(5000, clear_connect_status,
                          connect_status_label);
    }
}

/**
 * @brief Handles Connect button click.
 *
 * Establishes TCP connection with timeout,
 * starts network receive thread,
 * initializes GUI state.
 *
 * @return void
 */
static void connect_clicked()
{
    const char *ip = gtk_entry_get_text(GTK_ENTRY(connect_entry));
    if (!ip || !*ip)
        return;

    set_connect_status("", "black");

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0)
    {
        perror("socket");
        set_connect_status("Socket creation failed!", "red");
        return;
    }

    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
    {
        perror("inet_pton");
        set_connect_status("IP not found!", "red");
        close(sock_fd);
        sock_fd = -1;
        return;
    }

    int res = connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr));

    if (res < 0)
    {
        if (errno != EINPROGRESS)
        {
            perror("connect");
            set_connect_status("Connect failed", "red");
            close(sock_fd);
            sock_fd = -1;
            return;
        }

        /* Wait with timeout using select */
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock_fd, &wfds);

        struct timeval tv;
        tv.tv_sec = 3; // 3 second timeout
        tv.tv_usec = 0;

        res = select(sock_fd + 1, NULL, &wfds, NULL, &tv);

        if (res <= 0)
        {
            set_connect_status("Connection timeout", "red");
            close(sock_fd);
            sock_fd = -1;
            return;
        }

        /* Check for socket error */
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &so_error, &len);

        if (so_error != 0)
        {
            set_connect_status("Connection refused/unreachable", "red");
            close(sock_fd);
            sock_fd = -1;
            return;
        }
    }

    /* Restore blocking mode after successful connect */
    int flags2 = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags2 & ~O_NONBLOCK);

    printf("Connected to server %s\n", ip);
    set_connect_status("Connection successful", "green");

    net_running = 1;
    pthread_create(&net_thread, NULL, net_rx_thread, NULL);

    reset_plot_state();

    strncpy(connected_ip, ip, sizeof(connected_ip) - 1);
    connected_ip[sizeof(connected_ip) - 1] = '\0';

    state = STATE_CONNECTED;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkboxes[0]), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkboxes[2]), TRUE);

    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checkboxes[i])))
            gtk_widget_set_sensitive(checkboxes[i], FALSE);
    }
    apply_state();
}

/**
 * @brief Handles window close event.
 *
 * Prompts confirmation if connected,
 * gracefully stops streaming and network thread.
 *
 * @param widget Window widget
 * @param event GDK event
 * @param user_data Unused
 * @return gboolean TRUE to stop default handler
 */
static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{

    (void)event;
    (void)user_data;
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

    if (net_running)
    {
        net_running = 0;
        shutdown(sock_fd, SHUT_RDWR);   // unblock recv
        pthread_join(net_thread, NULL); // wait for thread to exit
    }

    if (sock_fd >= 0)
    {
        close(sock_fd);
        sock_fd = -1;
        printf("Client socket closed (on exit)\n");
    }
    reset_plot_state();

    gtk_main_quit();

    /* Allow GTK to close window */
    return TRUE;
}

/**
 * @brief Handles Disconnect button click.
 *
 * Stops network thread, closes socket,
 * resets GUI state.
 *
 * @param b Button widget (unused)
 * @param d User data (unused)
 * @return void
 */
static void disconnect_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (net_running)
    {
        net_running = 0;
        shutdown(sock_fd, SHUT_RDWR);
        pthread_join(net_thread, NULL);
    }

    if (sock_fd >= 0)
    {
        close(sock_fd);
        sock_fd = -1;
        printf("Disconnected from server\n");
    }
    reset_plot_state();

    state = STATE_DISCONNECTED;
    set_connect_status("", "black");
    apply_state();
}

/**
 * @brief Sends START command to server.
 *
 * Transitions GUI state to RUNNING.
 *
 * @return void
 */
static void start_clicked()
{
    if (sock_fd < 0)
        return;

    if (state == STATE_RUNNING)
        return;

    ssize_t n = send(sock_fd, "START\n", 6, 0);
    if (n <= 0)
    {
        printf("Failed to send START\n");
        return;
    }

    printf("Sent START\n");

    /* ---- Force default 300 Hz for all sensors locally ---- */
    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        g_hash_table_replace(sensor_freq,
                             g_strdup(sensor_ids[i]),
                             g_strdup("300"));
    }

    /* ---- Recalculate time window based on 300 Hz ---- */
    double sample_period_us = 1000000.0 / 300.0;

    time_window_us = (uint64_t)(VISIBLE_SAMPLES * sample_period_us);

    if (time_window_us < MIN_WINDOW_US)
        time_window_us = MIN_WINDOW_US;

    if (time_window_us > MAX_WINDOW_US)
        time_window_us = MAX_WINDOW_US;

    printf("[GUI] Time window set to %.2f ms\n",
           time_window_us / 1000.0);

    state = STATE_RUNNING;

    gtk_widget_set_sensitive(secB_info_view, FALSE);

    update_dropdown();
    apply_state();
}

/**
 * @brief Receives exactly len bytes from socket.
 *
 * Blocks until full payload received or error occurs.
 *
 * @param fd Socket file descriptor
 * @param buf Buffer to fill
 * @param len Number of bytes to receive
 * @return int 0 on success, -1 on failure
 */
static int recv_all(int fd, void *buf, size_t len)
{
    size_t off = 0;
    char *p = buf;

    while (off < len)
    {
        ssize_t n = recv(fd, p + off, len - off, 0);
        if (n <= 0)
            return -1;
        off += n;
    }
    return 0;
}

/**
 * @brief Sends STOP command to server.
 *
 * Transitions GUI state to CONNECTED.
 *
 * @return void
 */
static void stop_clicked()
{
    if (sock_fd < 0)
        return;

    send(sock_fd, "STOP\n", 5, 0);
    printf("Sent STOP\n");

    state = STATE_CONNECTED;
    gtk_widget_set_sensitive(secB_info_view, TRUE);

    apply_state();
}

/**
 * @brief Adjusts background color for legend visibility.
 *
 * Lightens dark themes and darkens light themes
 * for improved contrast.
 *
 * @param bg Background color
 * @return GdkRGBA Adjusted color
 */
static GdkRGBA
adjust_bg_for_legend(GdkRGBA bg)
{
    GdkRGBA out = bg;

    /* Perceived luminance (sRGB) */
    double lum = 0.2126 * bg.red +
                 0.7152 * bg.green +
                 0.0722 * bg.blue;

    if (lum < 0.5)
    {
        /* Dark theme → lighten */
        out.red = bg.red + (1.0 - bg.red) * 0.15;
        out.green = bg.green + (1.0 - bg.green) * 0.15;
        out.blue = bg.blue + (1.0 - bg.blue) * 0.15;
    }
    else
    {
        /* Light theme → darken */
        out.red = bg.red * 0.92;
        out.green = bg.green * 0.92;
        out.blue = bg.blue * 0.92;
    }

    out.alpha = 1.0;
    return out;
}

static gboolean update_secB_info(gpointer data)
{
    info_line *info = (info_line *)data;

    GtkPolicyType hpol, vpol;

    gtk_scrolled_window_get_policy(
        GTK_SCROLLED_WINDOW(info_scroll),
        &hpol, &vpol);
    int max_lines = 10;
    /* Remove first line if already 10 lines */
    gint line_count =
        gtk_text_buffer_get_line_count(secB_info_buffer);

    if (line_count >= max_lines)
    {
        GtkTextIter line_start, line_end;

        gtk_text_buffer_get_iter_at_line(
            secB_info_buffer, &line_start, 0);

        gtk_text_buffer_get_iter_at_line(
            secB_info_buffer, &line_end, 1);

        gtk_text_buffer_delete(
            secB_info_buffer, &line_start, &line_end);
    }

    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(secB_info_buffer, &iter);

    char buf[128];

    /* Temp */
    snprintf(buf, sizeof(buf),
             "Temp: %-10.2f    |    ",
             info->temp);

    gtk_text_buffer_insert_with_tags_by_name(
        secB_info_buffer, &iter,
        buf, -1,
        "temp_tag", NULL);

    /* ADC */
    snprintf(buf, sizeof(buf),
             "ADC1: %6.2f V    |    ",
             info->adc_v);

    gtk_text_buffer_insert_with_tags_by_name(
        secB_info_buffer, &iter,
        buf, -1,
        "adc_tag", NULL);

    /* Time */
    snprintf(buf, sizeof(buf),
             "t = %.3f µs\n",
             info->ts_us);

    gtk_text_buffer_insert_with_tags_by_name(
        secB_info_buffer, &iter,
        buf, -1,
        "time_tag", NULL);

    g_free(info);

    return FALSE;
}

/**
 * @brief Draw callback for plotting area.
 *
 * Renders:
 * - Grid
 * - Axes
 * - Axis labels
 * - Ticks
 * - Dynamic legend
 * - Real-time sensor traces
 *
 * Uses circular buffers and time-window clipping.
 *
 * @param widget Drawing area widget
 * @param cr Cairo context
 * @return gboolean FALSE
 */
static gboolean draw_grid(GtkWidget *widget, cairo_t *cr)
{
    uint64_t t_max = 0;

    static uint64_t visible_ts[MAX_SAMPLES];
    static double visible_val[SENSOR_COUNT][MAX_SAMPLES];
    static int visible_count[SENSOR_COUNT];

    pthread_mutex_lock(&sample_lock);

    for (int s = 0; s < SENSOR_COUNT; s++)
    {
        if (sample_count[s] > 0)
        {
            int last = (sample_head[s] - 1 + MAX_SAMPLES) % MAX_SAMPLES;
            uint64_t ts = sample_ts[s][last];
            if (ts > t_max)
                t_max = ts;
        }
    }

    pthread_mutex_unlock(&sample_lock);

    uint64_t t_min =
        (t_max > time_window_us) ? (t_max - time_window_us) : 0;

    for (int s = 0; s < SENSOR_COUNT; s++)
        visible_count[s] = 0;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    int plot_w, plot_h;

    int width = alloc.width;
    int height = alloc.height;

    const int grid_spacing = 70;        // bigger grid
    const int bottom_margin = 60;       // ticks ↔ x-axis label
    const int left_margin = 60;         // ticks ↔ y-axis line
    const int outer_bottom_margin = 12; // space below x-axis label
    const int outer_left_margin = 15;   // space left of y-axis label
    const int arrow_size = 10;          // axis arrow size

    plot_w = width - left_margin - 10;
    plot_h = height - bottom_margin - 10;

    if (t_max <= t_min)
        t_max = t_min + 1;

    /* ================== Faint Grid ================== */
    cairo_set_source_rgba(cr, 0.7, 0.7, 0.7, 0.1);
    cairo_set_line_width(cr, 1.0);

    /* Vertical grid lines */
    for (int x = left_margin; x <= left_margin + plot_w; x += grid_spacing)
    {
        cairo_move_to(cr, x + 0.5, 0);
        cairo_line_to(cr, x + 0.5, height - bottom_margin);
    }

    /* Horizontal grid lines */
    /* Horizontal grid lines + Y labels */
    int grid_count = plot_h / grid_spacing;

    for (int i = 0; i <= grid_count; i++)
    {
        double y = (height - bottom_margin) - i * grid_spacing;

        cairo_move_to(cr, left_margin, y + 0.5);
        cairo_line_to(cr, left_margin + plot_w, y + 0.5);
    }
    cairo_stroke(cr);

    /* ================== Theme-aware axis color ================== */
    GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
    GtkStyleContext *context =
        gtk_widget_get_style_context(toplevel);

    GtkStateFlags state = gtk_style_context_get_state(context);

    GdkRGBA fg;
    GdkRGBA bg;

    gtk_style_context_get_color(context, state, &fg);
    gtk_style_context_get_background_color(context, state, &bg);

    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha);

    /* ================== Normalized Y-axis ticks (0.0 – 1.0) ================== */
    cairo_set_font_size(cr, 11);

    for (int i = 0; i <= grid_count; i++)
    {
        double y = (height - bottom_margin) - i * grid_spacing;
        double value = Y_AXIS_MAX * (double)i / grid_count;

        char label[16];
        snprintf(label, sizeof(label), "%.1f", value);

        cairo_text_extents_t ext;
        cairo_text_extents(cr, label, &ext);

        cairo_move_to(cr,
                      left_margin - ext.width - 6,
                      y + ext.height / 2);
        cairo_show_text(cr, label);
    }

    /* Y-axis labels */

    cairo_stroke(cr);

    GdkRGBA legend_bg = adjust_bg_for_legend(bg);

    cairo_set_source_rgba(cr,
                          fg.red,
                          fg.green,
                          fg.blue,
                          fg.alpha);

    /* ================== Axes ================== */

    /* ================== Legend Placeholder ================== */

    /* ================== Signal Plot ================== */

    for (int s = 0; s < SENSOR_COUNT; s++)
    {
        if (!is_sensor_selected(s))
            continue;

        if (sample_count[s] < 2)
            continue;

        int head = sample_head[s];
        int count = sample_count[s];
        int start = (head - count + MAX_SAMPLES) % MAX_SAMPLES;

        visible_count[s] = 0;

        for (int i = 0; i < count; i++)
        {
            int idx = (start + i) % MAX_SAMPLES;
            uint64_t ts = sample_ts[s][idx];

            if (ts < t_min)
                continue;

            visible_ts[visible_count[s]] = ts;
            visible_val[s][visible_count[s]] = sample_data[s][idx];
            visible_count[s]++;
        }

        if (visible_count[s] < 2)
            continue;

        cairo_set_source_rgb(cr,
                             plot_colors[s][0],
                             plot_colors[s][1],
                             plot_colors[s][2]);

        cairo_set_line_width(cr, 2.0);

        gboolean started = FALSE;
        int n = visible_count[s];

        for (int i = 0; i < n; i++)
        {
            double x = left_margin +
                       plot_w *
                           (double)(visible_ts[i] - t_min) /
                           (double)(time_window_us);

            if (x < left_margin)
                continue;
            if (x > left_margin + plot_w)
                break;

            double v = visible_val[s][i];

            /* ADC-style scaling (0–4095) */
            double norm = v / sensor_y_max[s];

            /* Clamp to [0, 1] to avoid visual artifacts */
            if (norm < 0.0)
                norm = 0.0;
            else if (norm > 1.0)
                norm = 1.0;

            double y = (height - bottom_margin) -
                       (plot_h * norm);

            if (!started)
            {
                cairo_move_to(cr, x, y);
                started = TRUE;
            }
            else
            {
                cairo_line_to(cr, x, y);
            }
        }

        cairo_stroke(cr);
    }

    /* ================== Dynamic Legend ================== */

    /* Count active legend items */
    int legend_items = 0;
    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        if (is_sensor_selected(i))
            legend_items++;
    }

    double max_text_width = 0.0;
    cairo_set_font_size(cr, 12);

    /* Include title width */
    cairo_text_extents_t ext;
    cairo_text_extents(cr, "Legend:", &ext);
    max_text_width = ext.width;

    for (int i = 0; i < SENSOR_COUNT; i++)
    {
        if (!is_sensor_selected(i))
            continue;

        cairo_text_extents(cr, sensor_labels[i], &ext);
        if (ext.width > max_text_width)
            max_text_width = ext.width;
    }

    const int legend_padding = 10;
    const int box_size = 12;
    const int text_offset = box_size + 8;

    double legend_width =
        legend_padding * 2 +
        text_offset +
        max_text_width;

    const int legend_x = left_margin + plot_w - 190;

    int legend_y = 24;
    const int row_spacing = 20;

    /* Legend height = padding + title + rows */
    int legend_height =
        legend_padding * 2 +
        row_spacing * (1 + legend_items); // 1 = "Legend:" title

    cairo_set_font_size(cr, 12);

    /* Legend title */
    /* Clip legend to plot area */
    cairo_save(cr);
    cairo_rectangle(cr,
                    left_margin,
                    0,
                    plot_w,
                    plot_h);
    cairo_clip(cr);

    /* Legend background box (auto-sized) */
    /* Legend background (theme-aware) */
    cairo_set_source_rgba(cr,
                          legend_bg.red,
                          legend_bg.green,
                          legend_bg.blue,
                          1.0);

    cairo_rectangle(cr,
                    legend_x - legend_padding,
                    legend_y - row_spacing + 4,
                    legend_width,
                    legend_height);
    cairo_fill(cr);

    cairo_set_source_rgba(cr,
                          fg.red,
                          fg.green,
                          fg.blue,
                          1.0);

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

        /* Legend text (theme foreground color) */
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
    cairo_restore(cr);

    /* Reset color for axes (theme foreground) */
    cairo_set_source_rgba(cr,
                          fg.red,
                          fg.green,
                          fg.blue,
                          fg.alpha);

    cairo_set_line_width(cr, 2.5);

    /* Y-axis */
    cairo_move_to(cr, left_margin + 0.5, arrow_size);
    cairo_line_to(cr, left_margin + 0.5, height - bottom_margin);

    /* X-axis */
    cairo_move_to(cr, left_margin, height - bottom_margin + 0.5);
    cairo_line_to(cr,
                  left_margin + plot_w,
                  height - bottom_margin + 0.5);

    cairo_stroke(cr);

    /* ================== Axis Arrows ================== */
    cairo_set_line_width(cr, 2.5);

    /* X-axis arrow (right) */
    cairo_move_to(cr, left_margin + plot_w, height - bottom_margin);
    cairo_line_to(cr, left_margin + plot_w + arrow_size, height - bottom_margin + 0.5);
    cairo_line_to(cr, left_margin + plot_w, height - bottom_margin + arrow_size);

    cairo_stroke(cr);

    /* Y-axis arrow (up) */
    cairo_move_to(cr, left_margin - arrow_size, arrow_size);
    cairo_line_to(cr, left_margin + 0.5, 0);
    cairo_line_to(cr, left_margin + arrow_size, arrow_size);
    cairo_stroke(cr);

    /* ================== X-axis Ticks ================== */

    cairo_set_font_size(cr, 11);

    int tick_count = plot_w / grid_spacing;
    if (tick_count < 1)
        tick_count = 1;

    for (int i = 0; i <= tick_count; i++)
    {
        double x = left_margin + i * grid_spacing;
        uint64_t t = t_min + (time_window_us * i) / tick_count;

        /* Tick mark */
        cairo_move_to(cr, x + 0.5, height - bottom_margin);
        cairo_line_to(cr, x + 0.5, height - bottom_margin + 6);
        cairo_stroke(cr);

        /* Label */
        char label[32];

        /* Absolute monotonic time in milliseconds (reduced magnitude) */
        uint64_t abs_ms = t / 1000;

        /* Drop high digits to avoid clutter (keep last 5 digits) */
        abs_ms %= 100000;

        snprintf(label, sizeof(label), "%" PRIu64, abs_ms);

        cairo_text_extents(cr, label, &ext);

        cairo_move_to(cr,
                      x - ext.width / 2,
                      height - bottom_margin + 20);
        cairo_show_text(cr, label);
    }

    /* ================== X-axis Label ================== */
    const char *xlabel = "Time (ms)";

    cairo_select_font_face(cr, "Sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    cairo_set_source_rgba(cr,
                          fg.red,
                          fg.green,
                          fg.blue,
                          fg.alpha);

    cairo_text_extents(cr, xlabel, &ext);

    double x = (width - ext.width) / 2.0 - ext.x_bearing;
    double y = height - outer_bottom_margin;

    cairo_move_to(cr, x, y);
    cairo_show_text(cr, xlabel);

    /* ================== Y-axis Label ================== */
    const char *ylabel = "Value (V)";

    cairo_save(cr);
    cairo_translate(cr, outer_left_margin + 2, height / 2);

    cairo_rotate(cr, -G_PI / 2);

    cairo_set_font_size(cr, 14);
    cairo_set_source_rgba(cr,
                          fg.red,
                          fg.green,
                          fg.blue,
                          fg.alpha);

    cairo_text_extents_t yext;
    cairo_text_extents(cr, ylabel, &yext);

    cairo_move_to(cr, -yext.width / 2, 0);
    cairo_show_text(cr, ylabel);

    cairo_restore(cr);

    return FALSE;
}

/**
 * @brief Application entry point.
 *
 * Initializes GTK, builds UI layout,
 * connects signals, and starts main loop.
 *
 * @param argc Argument count
 * @param argv Argument vector
 * @return int Exit status
 */
int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    load_css();

    sensor_freq =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(main_window), "Measurement Network Gateway - GUI");
    gtk_window_set_position(GTK_WINDOW(main_window), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size(GTK_WINDOW(main_window), 1200, 800);

    g_signal_connect(main_window, "delete-event",
                     G_CALLBACK(on_window_delete), NULL);

    GtkWidget *main_v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(main_v), 16);
    gtk_container_add(GTK_CONTAINER(main_window), main_v);

    /* Section A */
    GtkWidget *secA = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(main_v), secA, FALSE, FALSE, 0);

    /* ---- Top row: buttons + sensors ---- */
    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(secA), top_row, FALSE, FALSE, 0);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(top_row), left, FALSE, FALSE, 0);

    GtkWidget *cnk_label = gtk_label_new("Enter Server IP:");
    gtk_widget_set_halign(cnk_label, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(left), cnk_label, FALSE, FALSE, 6);

    /* IP entry (inline with label and buttons) */
    connect_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(connect_entry), 20);
    gtk_box_pack_start(GTK_BOX(left), connect_entry, FALSE, FALSE, 0);
    g_signal_connect(connect_entry, "focus-out-event",
                     G_CALLBACK(entry_focus_out), NULL);

    /* ---- connection status (below entire row) ---- */
    GtkWidget *ip_column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(secA), ip_column, FALSE, FALSE, 0);
    gtk_widget_set_margin_start(ip_column, 120);

    /* Connection status label (below IP entry) */
    connect_status_label = gtk_label_new("");
    gtk_widget_set_halign(connect_status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(ip_column),
                       connect_status_label,
                       FALSE, FALSE, 0);

    GtkWidget *space_conn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(left), space_conn, FALSE, FALSE, 0);
    connect_btn = gtk_button_new_with_label("Connect");
    disconnect_btn = gtk_button_new_with_label("Disconnect");
    shutdown_btn = gtk_button_new_with_label("Shutdown");
    /* Visual hierarchy: Connect = primary, Disconnect = caution */
    gtk_style_context_add_class(
        gtk_widget_get_style_context(connect_btn),
        "suggested-action");

    gtk_style_context_add_class(
        gtk_widget_get_style_context(disconnect_btn),
        "destructive-action");

    gtk_style_context_add_class(
        gtk_widget_get_style_context(shutdown_btn),
        "destructive-action");

    gtk_box_pack_start(GTK_BOX(left), connect_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), disconnect_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), shutdown_btn, FALSE, FALSE, 0);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(top_row), right, TRUE, TRUE, 0);

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

    /* Redraw plot when GTK theme / style changes */
    g_signal_connect(main_window, "style-updated",
                     G_CALLBACK(gtk_widget_queue_draw),
                     graph_area);

    g_signal_connect(main_window,
                     "style-updated",
                     G_CALLBACK(update_info_text_colors),
                     NULL);

    /* ---- Section B Info Box ---- */
    info_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(info_scroll, -1, 120);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(info_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);

    secB_info_view = gtk_text_view_new();
    PangoFontDescription *font =
        pango_font_description_from_string("Monospace 11");

    gtk_widget_override_font(secB_info_view, font);
    pango_font_description_free(font);

    gtk_text_view_set_editable(GTK_TEXT_VIEW(secB_info_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(secB_info_view), FALSE);

    secB_info_buffer =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(secB_info_view));

    update_info_text_colors(secB_info_view);

    /* Wrap info box in frame with title */
    GtkWidget *info_frame = gtk_frame_new("Temp & ADC 1");
    gtk_widget_set_hexpand(info_frame, TRUE);
    gtk_widget_set_vexpand(info_frame, FALSE);

    gtk_frame_set_shadow_type(GTK_FRAME(info_frame),
                              GTK_SHADOW_ETCHED_IN);
    gtk_container_add(GTK_CONTAINER(info_scroll), secB_info_view);
    gtk_container_add(GTK_CONTAINER(info_frame), info_scroll);

    gtk_box_pack_start(GTK_BOX(main_v),
                       info_frame,
                       FALSE, FALSE, 4);

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
                                   "Type commands here, use help for command info.");
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
    g_signal_connect(shutdown_btn, "clicked", G_CALLBACK(shutdown_clicked), NULL);
    g_signal_connect(start_btn, "clicked", G_CALLBACK(start_clicked), NULL);
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(stop_clicked), NULL);
    g_signal_connect(connect_entry, "changed", G_CALLBACK(apply_state), NULL);

    apply_state();
    gtk_widget_show_all(main_window);
    g_timeout_add(33, redraw_graph, NULL);
    gtk_main();
    return 0;
}
