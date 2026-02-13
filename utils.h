#include <gtk/gtk.h>

#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>

#define PORT 50012
#define SENSOR_COUNT 5
#define CMD_HISTORY_SIZE 5
#define MAX_SAMPLES 1024
// #define TIME_WINDOW_US 5e6 // 5 seconds visible
#define Y_AXIS_MAX 5.0

extern uint64_t time_window_us;
extern const char *HELP_TEXT;

typedef enum
{
    temp_sid = 0,
    adc_zero_sid,
    adc_one_sid,
    sw_sid,
    pb_sid,
    snsr_cnt
} sensor_id_t;

typedef struct
{
    sensor_id_t sensor_id;
    unsigned int sensor_value;
    uint64_t timestamp;
} sensor_data_t;

typedef struct
{
    uint32_t sensor_id;
    uint32_t rate_hz;
} sensor_rate_t;

typedef enum
{
    STATE_DISCONNECTED,
    STATE_CONNECTED,
    STATE_RUNNING
} AppState;

typedef struct
{
    GtkWidget *entry;
    GtkWidget *label;
} CmdClearCtx;

typedef enum
{
    CMD_OK = 0,
    CMD_ERR_SYNTAX,
    CMD_ERR_SENSOR,
    CMD_ERR_FREQ_RANGE,
    CMD_ERR_NOT_CONNECTED,
    CMD_ERR_RUNNING,
    CMD_ERR_ALREADY_RUNNING,
    CMD_ERR_NOT_RUNNING
} CmdError;

typedef enum
{
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_START,
    CMD_STOP,
    CMD_SHUTDOWN,
    CMD_CONFIGURE,
    CMD_STATUS,
    CMD_HELP
} CmdType;

typedef struct
{
    sensor_rate_t rates[SENSOR_COUNT];
} RatesMsg;

typedef struct
{
    CmdType type;
    char sensor[8];
    int value;
    char ip[64];
} Cmd;

gboolean is_valid_ipv4(const char *ip);
void set_enabled(GtkWidget *w, gboolean e);
void load_css(void);

gboolean clear_cmd_feedback(gpointer data);