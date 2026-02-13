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

static const char *HELP_TEXT =
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
    "  CONNECT abc\n"
    "  START              (not connected)\n"
    "  STOP               (not running)\n"
    "  CONFIGURE TEMP 9        (frequency too low)\n"
    "  CONFIGURE ADC1 1001     (frequency too high)\n"
    "  CONFIGURE XYZ 100       (invalid sensor)\n"
    "  CONFIGURE TEMP abc      (non-numeric frequency)\n"
    "\n"
    "\033[1;36mNOTES:\033[0m\n"
    "\n"
    "  - Commands are case-insensitive\n"
    "  - Cannot CONNECT while already connected\n"
    "  - Cannot DISCONNECT while plotting is running\n"
    "  - START requires active connection\n"
    "  - STOP requires running state\n"
    "  - Streaming must be running to apply configuration\n"
    "\n"
    "\033[2mPress Ctrl+C to close this window.\033[0m\n";

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