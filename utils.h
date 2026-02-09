#include <gtk/gtk.h>

#include <unistd.h>
#include <stdint.h>

#define PORT 50012
#define SENSOR_COUNT 5
#define CMD_HISTORY_SIZE 5
#define MAX_SAMPLES 1024

#define TIME_WINDOW_US 5e6 // 5 seconds visible
#define Y_AXIS_MAX 5.0

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

gboolean is_valid_ipv4(const char *ip);
void set_enabled(GtkWidget *w, gboolean e);
void load_css(void);