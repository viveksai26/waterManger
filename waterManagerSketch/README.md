# ESP32 Water Manager

ESP32 firmware that monitors the **Manjeera water supply** and the **tank water level**. It sends Telegram alerts and serves a password-protected web dashboard.

---

## Features

### Monitoring
- **Manjeera supply detection.** A probe pair on GPIO 25/26 reports when supply water is flowing.
- **4-level tank sensing.** Probes for L1–L4 (GPIO 32, 33, 16, 17) plus a common probe (GPIO 27) give the levels EMPTY → L1 → L2 → L3 → FULL.
- **Noise filtering.** Each sensor is sampled 15 times per read, and 12 or more HIGH readings count as WET. A new state must then hold for 3 seconds before it's accepted.
- **Pulsed probes.** Probe voltage is applied only for a few milliseconds per read, which reduces probe corrosion.

### Telegram
- **Supply alerts** with times:
  - `💧 Manjeera WATER DETECTED — Started at 06:42`
  - `🔵 Manjeera WATER CLEARED — Stopped at 07:30, Duration: 48 min`
- **Tank level alerts:**
  - FULL and EMPTY are sent immediately.
  - Levels 1–3 respect a 30 s cooldown. An alert that couldn't be sent is queued and retried rather than dropped.
  - The FULL alert includes the fill time, for example `Filled L1 → FULL in 52 min`.
- **Daily summary** at 08:00: supplies and tank fills from the last 24 h, the current tank level, and uptime.
- **Commands:** `/status`, `/history`, `/restart`. Every message ends with a tappable `/status`.
- **Replies work.** Replying `/status` to an old message works the same as typing it.

### History (saved in flash, survives restarts)
- The last **20 Manjeera supplies**: start, end and duration.
- The last **10 tank fills**: start level, the time FULL was reached, and how long it took.

### Web dashboard
- Supply status, tank level, each sensor, network, Telegram status, current time, uptime, supply history, tank fills and the event log.
- The page refreshes itself every 10 seconds.
- Wi-Fi settings, firmware upload (OTA) and restart, all behind HTTP basic auth.

### Reliability
- **Clock sync (NTP)** in India Standard Time (IST) by default. Logs and alerts use real times.
- **Watchdog.** If the main loop is stuck for 60 s, the ESP32 restarts. The next startup message says it was a watchdog restart.
- **Wi-Fi auto-reconnect.** After reconnecting, it also re-registers mDNS, DuckDNS and the Telegram commands.
- **Setup access point.** If no Wi-Fi is saved or the connection fails, the ESP32 starts its own Wi-Fi network for configuration.

### Remote access
- **mDNS:** `http://watermanager.local` on your local network.
- **DuckDNS:** `<domain>.duckdns.org` is kept updated with the device's **IPv6** address every 10 minutes.

---

## Hardware

| Part | Notes |
|---|---|
| ESP32 dev board (ESP32-WROOM) | Tested with board **ESP32 Dev Module** |
| Stainless steel probes / wires | 2 for the supply, 5 for the tank (common + L1–L4) |
| 5 V USB power supply | |

### Wiring

**Manjeera supply sensor**

| ESP32 pin | Connects to |
|---|---|
| GPIO **25** (drive) | Probe A in the supply line |
| GPIO **26** (sense) | Probe B in the supply line |

**Tank level sensors**

| ESP32 pin | Connects to |
|---|---|
| GPIO **27** (common) | Probe at the **bottom** of the tank (always under water when there is any water) |
| GPIO **32** | L1 probe (lowest) |
| GPIO **33** | L2 probe |
| GPIO **16** | L3 probe |
| GPIO **17** | L4 probe (FULL) |

The sense pins use the ESP32's **internal pull-down** resistors, so no external resistors are needed. When water bridges the drive/common probe and a sense probe, the sense pin reads HIGH (WET).

> GPIO 16/17 are used by PSRAM on WROVER modules. Use a WROOM module, or move L3/L4 to other free pins and update the `#define`s.

---

## Software setup

### 1. Install the tools
1. Install the **Arduino IDE 2.x**.
2. In **Boards Manager**, install **esp32 by Espressif Systems**, version **3.x**. Version 3.3.x is tested; the watchdog code needs core 3.x.
3. Select **Tools → Board → ESP32 Dev Module**. The defaults are fine: 4 MB flash, Default partition scheme.

No extra libraries are needed. Everything used comes with the ESP32 core.

### 2. Create a Telegram bot
1. In Telegram, open **@BotFather** and send `/newbot`. Follow the steps and copy the **bot token**.
2. Send any message to your new bot.
3. Open `https://api.telegram.org/bot<TOKEN>/getUpdates` in a browser and copy the `"chat":{"id": ...}` value. That is your **chat ID**.
   - For a group, add the bot to the group, send a message there, and use the group's (negative) chat ID.

### 3. Set up DuckDNS (optional, for remote access)
1. Sign in at <https://www.duckdns.org>, create a subdomain and copy the **token**.
2. Remote access works over **IPv6**. Your ISP and router must support IPv6, and the router must allow incoming connections to the ESP32 on port 80.

### 4. Create `config.h`
Copy `config.example.h` to `config.h` in the sketch folder and fill in your values:

```cpp
#define DUCKDNS_DOMAIN "watermanager"        // subdomain only, no .duckdns.org
#define DUCKDNS_TOKEN  "your-duckdns-token"

#define WEB_USERNAME   "admin"               // dashboard login
#define WEB_PASSWORD   "a-strong-password"

#define AP_SSID        "ESP32-WaterMonitor"  // setup Wi-Fi network
#define AP_PASSWORD    "at-least-8-chars"

#define TELEGRAM_BOT_TOKEN "123456:ABC..."
#define TELEGRAM_CHAT_ID   "123456789"

// Optional overrides
// #define TIMEZONE "IST-5:30"        // POSIX TZ string
// #define DAILY_SUMMARY_HOUR 8       // 0-23, local time
```

`config.h` is listed in `.gitignore`. **Never commit it**, because it contains your secrets.

### 5. First flash (USB)
1. Connect the ESP32 over USB and select its COM port.
2. Click **Upload**.
3. Open **Serial Monitor** at **115200 baud** to watch the boot log.

### 6. Connect to Wi-Fi
On the first boot there are no saved Wi-Fi credentials, so the ESP32 starts a **setup access point**:

1. On your phone or laptop, connect to the Wi-Fi network named by `AP_SSID`, using the password `AP_PASSWORD`.
2. Open `http://192.168.4.1` and log in with `WEB_USERNAME` / `WEB_PASSWORD`.
3. Go to **Wi-Fi Settings**, enter your home Wi-Fi name and password, and save. The ESP32 restarts and connects.
4. You'll receive the Telegram message **"🟢 ESP32 Water Monitor started"**, which includes the IP address.

To change Wi-Fi later, use **Wi-Fi Settings** on the dashboard. If the saved network becomes unreachable at boot, the setup AP starts again.

---

## Using it

### Web dashboard
- Local network: `http://watermanager.local` or `http://<IPv4 from the Telegram startup message>`
- Remote: `http://<domain>.duckdns.org` (IPv6 only)

Log in with `WEB_USERNAME` / `WEB_PASSWORD`.

### Telegram commands

| Command | What it does |
|---|---|
| `/status` | Supply state, last supply, tank level, L1–L4, network, firmware, time, uptime |
| `/history` | Last 10 Manjeera supplies plus recent tank fills |
| `/restart` | Restarts the ESP32 |
| `/start` | Shows the command keyboard |

Commands also work as a **reply** to any message and as `/status@YourBot` in groups. Only the configured `TELEGRAM_CHAT_ID` is accepted, and messages from other chats are ignored.

### Alerts you will get

| Event | Message |
|---|---|
| Boot | 🟢 started, with firmware, IP and time (plus ⚠️ if the restart was caused by the watchdog) |
| Supply starts / stops | 💧 WATER DETECTED / 🔵 WATER CLEARED, with times and duration |
| Tank FULL | 🔴 FULL, with fill time |
| Tank EMPTY | 🔵 EMPTY |
| Tank level 1–3 | 💧 Tank WATER LEVEL n/4 (delayed alerts show `(at HH:MM)`) |
| Every day at 08:00 | ☀️ Daily summary |

### How history is recorded
- **A supply** starts when water is detected and ends when it clears.
  - If the ESP32 restarts while water is flowing, the same supply continues.
  - If water stopped while the ESP32 was off, that entry shows *"ended during restart"*.
- **A fill** starts on the first rise in tank level and is recorded when the tank reaches FULL. A drop in level cancels the fill. A fill that is in progress during a restart is not recorded.

---

## Updating the firmware

### Over Wi-Fi (OTA)
1. **Bump the version.** Increase `FIRMWARE_VERSION` at the top of `waterManagerSketch.ino` on every change, so you can confirm the new build is running.
2. In the Arduino IDE, use **Sketch → Export Compiled Binary**. The file `waterManagerSketch.ino.bin` is created in `build/esp32.esp32.esp32/`.
3. On the dashboard, open **Firmware Update**, choose the `.bin` file and click **Upload Firmware**.
4. The ESP32 restarts. Check the Telegram startup message or the dashboard for the new version.

### Over USB
Click **Upload** in the Arduino IDE as in the first flash. Saved Wi-Fi, history and settings are kept, unless you choose **Erase All Flash** in the Tools menu.

> Firmware space is currently about 87% used, and OTA needs the new binary to fit in one app partition (1.25 MB).

---

## Configuration reference

Edit these in `waterManagerSketch.ino`. The ones marked * can also be set in `config.h`.

| Setting | Default | Meaning |
|---|---|---|
| `FIRMWARE_VERSION` | — | Shown on the dashboard and in Telegram |
| `MDNS_HOSTNAME` | `watermanager` | `http://<name>.local` |
| `TIMEZONE`* | `IST-5:30` | POSIX timezone string |
| `DAILY_SUMMARY_HOUR`* | `8` | Hour to send the daily summary |
| `WATCHDOG_TIMEOUT_MS` | 60000 | Restart if the loop is stuck this long |
| `HISTORY_SIZE` / `HISTORY_SHOWN` | 20 / 10 | Supplies stored / shown |
| `FILL_HISTORY_SIZE` | 10 | Tank fills stored |
| `SENSOR_INTERVAL` | 1000 ms | How often the sensors are read |
| `WATER_DEBOUNCE_TIME` / `LEVEL_DEBOUNCE_TIME` | 3000 ms | How long a new state must hold before it's accepted |
| `SENSOR_SAMPLE_COUNT` / `SENSOR_WET_REQUIRED` | 15 / 12 | Readings taken / HIGH readings needed for WET |
| `TELEGRAM_COOLDOWN` | 30 s | Minimum gap between non-urgent alerts |
| `TELEGRAM_POLL_INTERVAL` | 5 s | How often to check for commands |
| `DUCKDNS_UPDATE_INTERVAL` | 10 min | DuckDNS refresh |

---

## Troubleshooting

| Problem | Check |
|---|---|
| No Telegram messages | Bot token and chat ID in `config.h`; you sent the bot at least one message; **Telegram** card / event log on the dashboard; use **Telegram Test** |
| Commands not answered | The chat ID must match exactly (a group ID is negative); check the log for "Telegram command received" |
| Time shows "not synced" | Internet access and NTP (UDP port 123) not blocked; sync retries in the background |
| Sensor flickers WET/DRY | Clean the probes, check wiring and grounding, raise `SENSOR_WET_REQUIRED` or the debounce times |
| Level stuck at a value | Make sure the common probe (GPIO 27) is at the very bottom; check each Lx wire on the dashboard |
| `watermanager.local` not found | Some Android devices and networks don't support mDNS; use the IP address |
| DuckDNS "No global IPv6" | Your router/ISP isn't giving the ESP32 an IPv6 address |
| ⚠️ Restarted by watchdog | Usually a network call that hung; check the Wi-Fi signal. Occasional ones are harmless |
| OTA upload fails | Use the `.bin` from **Export Compiled Binary** (not `bootloader` or `merged`); make sure it fits the partition |

---

## Project files

```
waterManagerSketch/
├── waterManagerSketch.ino   firmware
├── config.example.h         template for config.h
├── config.h                 your secrets (not committed)
├── .gitignore
└── README.md
```
