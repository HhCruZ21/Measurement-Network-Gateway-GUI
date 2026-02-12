#include <gtk/gtk.h>

#include <unistd.h>
#include <stdint.h>

#define PORT 50012
#define SENSOR_COUNT 5
#define CMD_HISTORY_SIZE 5
#define MAX_SAMPLES 1024
// #define TIME_WINDOW_US 5e6 // 5 seconds visible
#define Y_AXIS_MAX 5.0

extern uint64_t time_window_us;

static const char *HELP_TEXT =
    "Measurement Network Gateway – CLI Help\n"
    "\n"
    "VALID COMMANDS:\n"
    "\n"
    "  CONFIGURE <SENSOR_ID> <FREQ_HZ>\n"
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
    "EXAMPLES:\n"
    "\n"
    "  CONFIGURE TEMP 50\n"
    "  CONFIGURE ADC0 200\n"
    "\n"
    "INVALID EXAMPLES:\n"
    "\n"
    "  CONFIGURE TEMP 9        (frequency too low)\n"
    "  CONFIGURE ADC1 1001     (frequency too high)\n"
    "  CONFIGURE XYZ 100       (invalid sensor)\n"
    "  CONFIGURE TEMP abc      (non-numeric frequency)\n"
    "\n"
    "NOTES:\n"
    "\n"
    "  - Commands are case-insensitive\n"
    "  - Streaming must be running to apply configuration\n"
    "\n"
    "Press Ctrl+C to close this window.\n";

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
    CMD_ERR_FREQ_RANGE
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

typedef struct {
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