# Measurement Network Gateway – GUI

Author: Haizon Helet Cruz  
Date: 2026-02-13  

---

## Overview

The **Measurement Network Gateway GUI** is a GTK-based desktop application written in C that connects to a remote measurement device over TCP and visualizes real-time sensor data.

The application provides:

- TCP client connection to remote device
- Real-time multi-sensor plotting using Cairo
- Dynamic sampling frequency configuration (10–1000 Hz)
- Integrated CLI with strict validation
- Safe disconnect and shutdown handling
- Multithreaded network receive architecture
- Circular buffer data management
- State-machine driven UI logic

This project demonstrates:

- Embedded-oriented network client design
- GTK GUI development in C
- POSIX multithreading
- Real-time signal visualization
- Robust command parsing and validation
- Defensive error handling

---

## Folder Structure

All files are located inside:

```
gui/
│
├── gui.c        # Main GUI, networking, plotting, and state machine
├── utils.h      # Shared definitions, enums, structures
├── utils.c      # Helper functions, CLI help, CSS loader
├── README.md    # Project documentation
```

---

## System Architecture

```
+-----------------------+
| GTK Main Thread       |
| - UI Controls         |
| - State Machine       |
| - Plot Rendering      |
+----------+------------+
           |
           v
+-----------------------+
| Network RX Thread     |
| - TCP recv loop       |
| - RATES handling      |
| - Streaming batches   |
+----------+------------+
           |
           v
+-----------------------+
| Circular Buffers      |
| - Per-sensor storage  |
| - Timestamp tracking  |
+----------+------------+
           |
           v
+-----------------------+
| Cairo Renderer        |
| - Grid + Axes         |
| - Dynamic legend      |
| - Time window scaling |
+-----------------------+
```

---

## Application States

The GUI operates using a strict state machine:

- `STATE_DISCONNECTED`
- `STATE_CONNECTED`
- `STATE_RUNNING`

Valid transitions:

| From | Action | To |
|------|--------|----|
| DISCONNECTED | CONNECT | CONNECTED |
| CONNECTED | START | RUNNING |
| RUNNING | STOP | CONNECTED |
| CONNECTED | DISCONNECT | DISCONNECTED |
| CONNECTED | SHUTDOWN | Exit |

Invalid transitions are blocked both at UI and CLI level.

---

## Supported CLI Commands

Commands are case-insensitive.

### CONNECT
```
CONNECT <IP_ADDRESS>
```

### DISCONNECT
```
DISCONNECT
```

### START
```
START
```

### STOP
```
STOP
```

### SHUTDOWN
```
SHUTDOWN
```

### CONFIGURE
```
CONFIGURE <SENSOR_ID> <FREQ_HZ>
```

Valid sensor IDs:

- TEMP
- ADC0
- ADC1
- SW
- PB

Valid frequency range:

```
10 – 1000 Hz
```

---

## Networking Protocol

### TCP Port
```
50012
```

### Server → Client Message Types

#### 1) RATES Message
```
"RATES\n"
+ sensor_rate_t[SENSOR_COUNT]
```

#### 2) Streaming Batch
```
uint32_t payload_size (network byte order)
+ sensor_data_t array
```

---

## Sensor Data Model

```c
typedef struct {
    sensor_id_t sensor_id;
    unsigned int sensor_value;
    uint64_t timestamp;
} sensor_data_t;
```

Each sensor uses:

- Independent circular buffer (size: 1024 samples)
- Independent timestamp tracking
- Independent Y-axis scaling

---

## Plot Features

- Real-time streaming visualization
- Dynamic time window scaling
- Absolute monotonic time axis (ms)
- Theme-aware legend background
- Normalized Y-axis rendering
- Multiple simultaneous sensor traces
- ~30 FPS redraw rate

Time window automatically adjusts based on ADC0 sampling rate:

```
time_window = VISIBLE_SAMPLES × sample_period
```

Clamped between:

```
50 ms – 5 seconds
```

---

## Dependencies

Required:

- GTK 3
- Cairo
- POSIX Threads
- GCC (C11 or later)

Ubuntu install:

```bash
sudo apt install libgtk-3-dev
```

---

## Build Instructions

From inside the `gui/` directory:

```bash
make
```

---

## Run

```bash
./gui_app
```

---

## Error Handling

The application handles:

- Socket creation failure
- Connection timeout
- Invalid IPv4 input
- Invalid command syntax
- Invalid frequency range
- Network thread termination
- Timestamp reset detection
- Graceful shutdown during streaming

---

## Design Decisions

### Multithreading
Networking runs in a dedicated POSIX thread.  
GUI updates are safely dispatched using `g_idle_add()`.

### Circular Buffers
Each sensor uses a fixed-size ring buffer to eliminate runtime allocations during streaming.

### Defensive Validation
All CLI commands and frequency inputs are strictly validated before execution.

### State-Driven UI
Buttons and inputs are enabled/disabled automatically based on application state.

---

## Limitations

- IPv4 only
- Fixed port (50012)
- No authentication
- No TLS encryption
- No persistent configuration
- No auto-reconnect

---

## Possible Improvements

- IPv6 support
- TLS encryption
- Auto-reconnect logic
- Config file persistence
- CSV export
- Zoomable graph
- Logging subsystem
- Authentication layer

---

## Intended Use

This project is suitable for:

- Embedded systems portfolios
- FPGA/SoC frontend clients
- Industrial sensor gateways
- Real-time data acquisition demonstrations
- Internship/job applications in embedded or systems engineering

---

## License

For academic and portfolio use.
