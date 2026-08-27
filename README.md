# Smart School Simulation Project

This project is a smart school bell and announcement system built for an ESP32-based controller. It uses a real-time clock, keypad, LCD, relay, buzzer, Wi-Fi, MQTT, and a small web dashboard to manage class bell timing, automatic scheduling, manual bell control, emergency announcements, and event logging.

The firmware is designed to run in the Wokwi simulator and can also be adapted for physical ESP32 hardware.

---

## 1. Setup for this project

### Required tools

- ESP-IDF installed and configured on your computer
- A supported ESP32 development board or Wokwi simulator
- VS Code with the ESP-IDF extension or a terminal configured for ESP-IDF
- Optional: Wokwi extension for simulation

### Project configuration

The project is already set up as an ESP-IDF CMake project:

- Root `CMakeLists.txt` sets the project name and includes the ESP-IDF project helper
- `main/CMakeLists.txt` registers the firmware source files and required libraries
- `main/idf_component.yml` declares the MQTT component dependency

### Hardware and simulation files

At the project root:

- `diagram.json` contains the Wokwi circuit diagram
- `wokwi.toml` configures the simulation firmware and forwarded local web port
- `partitions.csv` defines flash partition layout for the ESP32

### Typical ESP-IDF setup steps

1. Open a terminal in the project root.
2. Set up the ESP-IDF environment.
3. Configure the target board:

```bash
idf.py set-target esp32
```

4. Build the project:

```bash
idf.py build
```

5. Flash to an ESP32 device:

```bash
idf.py -p <PORT> flash
```

6. Open the serial monitor:

```bash
idf.py monitor
```

If you are using Wokwi, the project is already configured to simulate the firmware from the `build` folder.

### Important runtime assumptions

- The system uses an RTC module (DS1307-style device) for clock reading.
- The LCD is connected over I2C.
- The relay output can be used for external bell activation.
- Wi-Fi is treated as optional. The bell system continues operating even if Wi-Fi is not available.
- MQTT and web reporting work only when the network is connected.

---

## 2. Structure of this project

### Root folder

```text
Smart_school_simulation/
├── CMakeLists.txt
├── diagram.json
├── partitions.csv
├── wokwi.toml
├── main/
├── managed_components/
└── README.md
```

### Root files

- `CMakeLists.txt` - main ESP-IDF project definition
- `diagram.json` - simulation hardware layout for Wokwi
- `wokwi.toml` - simulation startup configuration and port forwarding
- `partitions.csv` - storage partition settings for the firmware

### `main/` folder

This is the main firmware source folder.

#### Core application logic

- `main.c` - main application loop, hardware setup, RTC processing, scheduled bell checks, keypad handling, watchdog management, and the main runtime system

#### Bell and scheduling modules

- `schedule_guard.c` / `schedule_guard.h` - validates RTC time changes and prevents missed or duplicate bell triggers
- `event_log.c` / `event_log.h` - persistent event history for alarms, resets, security events, and announcement activity
- `access_control.c` / `access_control.h` - administrator PIN validation and lockout logic
- `system_watchdog.c` / `system_watchdog.h` - prevents main task hangs and resets logic issues

#### Announcement system

- `announcement_manager.c` / `announcement_manager.h` - tracks live and emergency announcement states
- `announcement_controls.c` / `announcement_controls.h` - handles push-to-talk and emergency trigger buttons
- `announcement_output.c` / `announcement_output.h` - controls the PA announcement output state

#### Connectivity and web features

- `wifi_manager.c` / `wifi_manager.h` - Wi-Fi station management and status tracking
- `mqtt_manager.c` / `mqtt_manager.h` - MQTT connection and status publishing
- `web_server.c` / `web_server.h` - embedded HTTP dashboard and admin web interface
- `web_auth.c` / `web_auth.h` - browser session authentication for admin access
- `web_timetable_bridge.c` / `web_timetable_bridge.h` - bridge between web requests and timetable storage
- `web_event_bridge.c` / `web_event_bridge.h` - bridge between web requests and event logs

### Feature behavior from the codebase

The firmware implements:

- Automatic bell scheduling based on a timetable
- Manual bell trigger with a button
- Announcement priority handling for live and emergency PA output
- Bell blocking while announcements are active
- Web dashboard showing time, Wi-Fi, MQTT, NVS, and schedule status
- Event recording for resets, alarms, time jumps, access control, and announcements
- Administrator login and PIN-change flow
- Watchdog protection for reliability

---

## 3. How to use this project

### 1. Start the system

Power on the ESP32 or run the Wokwi simulation. The firmware initializes:

- RTC timing
- LCD display
- buzzer and relay output
- Wi-Fi connection
- MQTT reporting
- the web server when Wi-Fi is connected
- the bell schedule and NVS storage

### 2. Normal operation

Once the device is running:

- The LCD shows the current time and status.
- The relay and buzzer can trigger when the bell schedule matches the configured timetable.
- `AUTO` mode controls whether scheduled bells are active.
- A manual button can ring the bell immediately.
- A separate emergency or PA button can temporarily block the bell and run announcements.

### 3. Bell schedule and timetable

The timetable is stored in non-volatile memory and managed inside the application. Typical tasks are:

- add a bell time
- remove a bell time
- view the timetable
- reset to the default timetable if needed

The schedule guard prevents duplicate triggers when the RTC jumps or recovers from a glitch.

### 4. Admin access

The project includes an administrator menu and PIN protection. The code comments indicate that the default PIN is initially `1234` unless it has been changed.

Typical admin actions include:

- login with the admin PIN
- change the PIN
- change the ring duration
- view logs
- clear logs
- modify timetable entries
- set or inspect system time/date

### 5. Web dashboard

When the ESP32 has an IP address and the web server starts successfully, you can access the dashboard through the local browser. The project uses a simple status page with information such as:

- current time
- next bell
- automatic mode
- alarm status
- Wi-Fi and IP state
- MQTT diagnostics
- timetable count
- event log count
- ring duration
- NVS storage state

This is useful for monitoring the school bell system without using the physical keypad.

### 6. MQTT and status reporting

The firmware publishes a status snapshot over MQTT when the network is available. This allows external monitoring systems to observe:

- current time
- next scheduled bell
- alarm state
- announcement state
- bell blocking status
- timetable and log counts

### 7. Common workflow

A typical usage flow is:

1. Power on the device.
2. Wait for the RTC to stabilize.
3. Verify the current time and date.
4. Load or edit the school timetable.
5. Enable automatic bell mode.
6. Monitor via the LCD or web dashboard.
7. Use manual bell or PA controls when needed.
8. Review event logs if a schedule or alarm issue occurs.

---

## Summary

This project is a smart campus bell management system for an ESP32. It combines hardware control, scheduling logic, event logging, Wi-Fi/MQTT integration, and a simple web dashboard to create a reliable automated school-bell solution.

For most users, the workflow is:

- setup the ESP-IDF environment,
- flash the firmware to the device,
- run the Wokwi simulation or hardware system,
- configure the timetable and admin access,
- monitor the bell state through the display or browser dashboard.

---

## Quick command examples

Build:

```bash
idf.py build
```

Flash:

```bash
idf.py -p COM3 flash
```

Monitor serial output:

```bash
idf.py monitor
```

If the project is running in Wokwi, launch the simulation from the Wokwi tooling instead of flashing hardware.
