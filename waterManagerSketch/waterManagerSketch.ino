#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "config.h"

// ============================================================
// VERSION
// ============================================================

#define FIRMWARE_VERSION "4.0"

// ============================================================
// TIMING
// ============================================================

const unsigned long WIFI_TIMEOUT = 15000UL;

const unsigned long DUCKDNS_UPDATE_INTERVAL = 10UL * 60UL * 1000UL;

const unsigned long SENSOR_INTERVAL = 1000UL;

const unsigned long WATER_DEBOUNCE_TIME = 3000UL;

const unsigned long LEVEL_DEBOUNCE_TIME = 3000UL;

const unsigned long TELEGRAM_COOLDOWN = 30000UL;

// ============================================================
// WATER SENSOR PINS
// ============================================================

// Water presence sensor
//
// DRIVE pin briefly supplies voltage to the sensing circuit.
// SENSE pin reads the result.
//
// IMPORTANT:
// GPIO34 is input-only and has NO internal pull-up/pull-down.
// Your external sensor circuit must provide the required biasing.

#define WATER_DRIVE_PIN 25
#define WATER_SENSE_PIN 34

// ============================================================
// WATER LEVEL PINS
// ============================================================

#define LEVEL1_PIN 32
#define LEVEL2_PIN 33
#define LEVEL3_PIN 35
#define LEVEL4_PIN 36
#define LEVEL5_PIN 39

// ============================================================
// OBJECTS
// ============================================================

WebServer server(80);

Preferences preferences;

// ============================================================
// STATE
// ============================================================

bool wifiConnected = false;

bool waterPresence = false;
bool lastRawWaterPresence = false;

unsigned long waterCandidateSince = 0;

int currentWaterLevel = 0;
int lastRawWaterLevel = 0;

unsigned long levelCandidateSince = 0;

String currentIPv4 = "";
String currentIPv6 = "";

String duckDNSStatus = "Not updated";
String lastDuckDNSIPv6 = "";

String telegramStatus = "Not tested";

unsigned long lastDuckDNSUpdate = 0;
unsigned long lastSensorRead = 0;

unsigned long lastTelegramSent = 0;

bool otaRunning = false;

unsigned long bootTime = 0;

// ============================================================
// LOGGING
// ============================================================

#define MAX_LOGS 50

String logs[MAX_LOGS];
int logCount = 0;

void addLog(String message) {

  String entry =
      "[" + String(millis() / 1000) + "s] " + message;

  if (logCount < MAX_LOGS) {
    logs[logCount++] = entry;
  } else {

    for (int i = 0; i < MAX_LOGS - 1; i++) {
      logs[i] = logs[i + 1];
    }

    logs[MAX_LOGS - 1] = entry;
  }

  Serial.println(entry);
}

// ============================================================
// URL ENCODE
// ============================================================

String urlEncode(const String &text) {

  String encoded = "";

  const char *hex = "0123456789ABCDEF";

  for (size_t i = 0; i < text.length(); i++) {

    char c = text.charAt(i);

    if (
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' ||
        c == '_' ||
        c == '.' ||
        c == '~'
    ) {
      encoded += c;
    } else {

      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }

  return encoded;
}

// ============================================================
// AUTHENTICATION
// ============================================================

bool authenticate() {

  if (!server.authenticate(WEB_USERNAME, WEB_PASSWORD)) {

    server.requestAuthentication();

    return false;
  }

  return true;
}

// ============================================================
// IPV6
// ============================================================

String getIPv6Address() {

  if (!WiFi.STA.hasGlobalIPv6()) {
    return "";
  }

  IPAddress ipv6 = WiFi.STA.globalIPv6();

  return ipv6.toString();
}

// ============================================================
// WIFI
// ============================================================

bool connectToWiFi() {

  String ssid =
      preferences.getString("ssid", "");

  String password =
      preferences.getString("password", "");

  if (ssid.length() == 0) {

    addLog("No saved Wi-Fi credentials");

    return false;
  }

  addLog("Connecting to Wi-Fi: " + ssid);

  WiFi.mode(WIFI_STA);

  // Enable IPv6 SLAAC
  WiFi.STA.enableIPv6(true);

  WiFi.begin(
      ssid.c_str(),
      password.c_str()
  );

  unsigned long start = millis();

  while (
      WiFi.status() != WL_CONNECTED &&
      millis() - start < WIFI_TIMEOUT
  ) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {

    addLog("Wi-Fi connection failed");

    wifiConnected = false;

    return false;
  }

  wifiConnected = true;

  currentIPv4 =
      WiFi.localIP().toString();

  addLog(
      "Wi-Fi connected. IPv4: " +
      currentIPv4
  );

  // Wait briefly for IPv6 address
  start = millis();

  while (
      !WiFi.STA.hasGlobalIPv6() &&
      millis() - start < 5000
  ) {

    delay(250);
  }

  currentIPv6 = getIPv6Address();

  if (currentIPv6.length() > 0) {

    addLog(
        "Global IPv6: " +
        currentIPv6
    );

  } else {

    addLog("Global IPv6 not available");
  }

  return true;
}

// ============================================================
// SETUP ACCESS POINT
// ============================================================

void startSetupAP() {

  WiFi.mode(WIFI_AP);

  WiFi.softAP(
      AP_SSID,
      AP_PASSWORD
  );

  IPAddress apIP =
      WiFi.softAPIP();

  addLog(
      "Setup AP started: " +
      String(AP_SSID)
  );

  addLog(
      "AP IP: " +
      apIP.toString()
  );
}

// ============================================================
// DUCKDNS
// ============================================================

bool updateDuckDNS() {

  if (!wifiConnected) {

    duckDNSStatus =
        "Wi-Fi disconnected";

    return false;
  }

  String ipv6 =
      getIPv6Address();

  if (ipv6.length() == 0) {

    duckDNSStatus =
        "No global IPv6";

    addLog(
        "DuckDNS skipped: no IPv6"
    );

    return false;
  }

  // No need to update if address has not changed
  if (
      ipv6 == lastDuckDNSIPv6 &&
      lastDuckDNSUpdate != 0
  ) {

    duckDNSStatus =
        "IPv6 unchanged";

    return true;
  }

  addLog(
      "Updating DuckDNS: " +
      ipv6
  );

  WiFiClientSecure client;

  // DuckDNS HTTPS certificate validation would require
  // maintaining a CA certificate. For this local device,
  // use HTTPS transport with certificate verification disabled.
  client.setInsecure();

  HTTPClient http;

  String url =
      "https://www.duckdns.org/update"
      "?domains=" +
      String(DUCKDNS_DOMAIN) +
      "&token=" +
      String(DUCKDNS_TOKEN) +
      "&ipv6=" +
      ipv6 +
      "&verbose=true";

  if (!http.begin(client, url)) {

    duckDNSStatus =
        "HTTPS connection failed";

    addLog(
        "DuckDNS HTTP begin failed"
    );

    return false;
  }

  int httpCode =
      http.GET();

  String response =
      http.getString();

  http.end();

  if (
      httpCode == HTTP_CODE_OK &&
      response.indexOf("OK") >= 0
  ) {

    lastDuckDNSIPv6 = ipv6;

    lastDuckDNSUpdate = millis();

    duckDNSStatus =
        "Updated successfully";

    addLog(
        "DuckDNS updated successfully"
    );

    return true;
  }

  duckDNSStatus =
      "Update failed: HTTP " +
      String(httpCode);

  addLog(
      "DuckDNS update failed: " +
      String(httpCode) +
      " " +
      response
  );

  return false;
}

// ============================================================
// TELEGRAM
// ============================================================

bool sendTelegram(
    const String &message,
    bool ignoreCooldown = false
) {

  if (!wifiConnected) {

    telegramStatus =
        "Wi-Fi disconnected";

    addLog(
        "Telegram skipped: Wi-Fi disconnected"
    );

    return false;
  }

  if (
      !ignoreCooldown &&
      lastTelegramSent != 0 &&
      millis() - lastTelegramSent <
          TELEGRAM_COOLDOWN
  ) {

    addLog(
        "Telegram skipped: cooldown"
    );

    return false;
  }

  WiFiClientSecure client;

  // HTTPS transport.
  // Telegram's API itself requires HTTPS.
  client.setInsecure();

  HTTPClient http;

  String url =
      "https://api.telegram.org/bot" +
      String(TELEGRAM_BOT_TOKEN) +
      "/sendMessage"
      "?chat_id=" +
      urlEncode(String(TELEGRAM_CHAT_ID)) +
      "&text=" +
      urlEncode(message);

  if (!http.begin(client, url)) {

    telegramStatus =
        "HTTPS connection failed";

    addLog(
        "Telegram HTTP begin failed"
    );

    return false;
  }

  int httpCode =
      http.GET();

  String response =
      http.getString();

  http.end();

  if (
      httpCode == HTTP_CODE_OK &&
      response.indexOf("\"ok\":true") >= 0
  ) {

    lastTelegramSent = millis();

    telegramStatus =
        "Last message sent successfully";

    addLog(
        "Telegram message sent"
    );

    return true;
  }

  telegramStatus =
      "Send failed: HTTP " +
      String(httpCode);

  addLog(
      "Telegram failed: HTTP " +
      String(httpCode)
  );

  return false;
}

// ============================================================
// WATER PRESENCE SENSOR
// ============================================================

bool readWaterPresenceRaw() {

  // Keep electrodes normally unpowered
  digitalWrite(
      WATER_DRIVE_PIN,
      LOW
  );

  delayMicroseconds(100);

  // Briefly energize the sensing circuit
  digitalWrite(
      WATER_DRIVE_PIN,
      HIGH
  );

  delay(5);

  bool wet =
      digitalRead(WATER_SENSE_PIN) == HIGH;

  digitalWrite(
      WATER_DRIVE_PIN,
      LOW
  );

  return wet;
}

// ============================================================
// LEVEL SENSOR
// ============================================================

int readWaterLevelRaw() {

  bool level1 =
      digitalRead(LEVEL1_PIN);

  bool level2 =
      digitalRead(LEVEL2_PIN);

  bool level3 =
      digitalRead(LEVEL3_PIN);

  bool level4 =
      digitalRead(LEVEL4_PIN);

  bool level5 =
      digitalRead(LEVEL5_PIN);

  // Highest active level wins

  if (level5)
    return 5;

  if (level4)
    return 4;

  if (level3)
    return 3;

  if (level2)
    return 2;

  if (level1)
    return 1;

  return 0;
}

// ============================================================
// WATER LEVEL TEXT
// ============================================================

String waterLevelText(int level) {

  switch (level) {

    case 0:
      return "EMPTY";

    case 1:
      return "LEVEL 1";

    case 2:
      return "LEVEL 2";

    case 3:
      return "LEVEL 3";

    case 4:
      return "LEVEL 4";

    case 5:
      return "FULL";
  }

  return "UNKNOWN";
}

// ============================================================
// WATER PRESENCE CHANGE
// ============================================================

void handleWaterPresenceChange(
    bool newState
) {

  waterPresence = newState;

  if (newState) {

    addLog(
        "Water presence detected"
    );

    sendTelegram(
        "💧 WATER DETECTED"
    );

  } else {

    addLog(
        "Water presence cleared"
    );

    sendTelegram(
        "🔵 WATER CLEARED"
    );
  }
}

// ============================================================
// LEVEL CHANGE
// ============================================================

void handleWaterLevelChange(
    int newLevel
) {

  currentWaterLevel = newLevel;

  String message;

  if (newLevel == 0) {

    message =
        "🔵 WATER LEVEL: EMPTY";

  } else if (newLevel == 5) {

    message =
        "🔴 WATER LEVEL: FULL";

  } else {

    message =
        "💧 WATER LEVEL: " +
        String(newLevel) +
        "/5";
  }

  addLog(
      "Water level changed: " +
      waterLevelText(newLevel)
  );

  sendTelegram(message);
}

// ============================================================
// SENSOR PROCESSING
// ============================================================

void updateSensors() {

  if (
      millis() - lastSensorRead <
      SENSOR_INTERVAL
  ) {
    return;
  }

  lastSensorRead = millis();

  // ----------------------------------------------------------
  // WATER PRESENCE
  // ----------------------------------------------------------

  bool rawWater =
      readWaterPresenceRaw();

  if (rawWater != lastRawWaterPresence) {

    lastRawWaterPresence =
        rawWater;

    waterCandidateSince =
        millis();
  }

  if (
      rawWater != waterPresence &&
      millis() - waterCandidateSince >=
          WATER_DEBOUNCE_TIME
  ) {

    handleWaterPresenceChange(
        rawWater
    );
  }

  // ----------------------------------------------------------
  // WATER LEVEL
  // ----------------------------------------------------------

  int rawLevel =
      readWaterLevelRaw();

  if (rawLevel != lastRawWaterLevel) {

    lastRawWaterLevel =
        rawLevel;

    levelCandidateSince =
        millis();
  }

  if (
      rawLevel != currentWaterLevel &&
      millis() - levelCandidateSince >=
          LEVEL_DEBOUNCE_TIME
  ) {

    handleWaterLevelChange(
        rawLevel
    );
  }
}

// ============================================================
// UPTIME
// ============================================================

String getUptime() {

  unsigned long seconds =
      (millis() - bootTime) / 1000;

  unsigned long days =
      seconds / 86400;

  seconds %= 86400;

  unsigned long hours =
      seconds / 3600;

  seconds %= 3600;

  unsigned long minutes =
      seconds / 60;

  seconds %= 60;

  char buffer[64];

  snprintf(
      buffer,
      sizeof(buffer),
      "%lu days %02lu:%02lu:%02lu",
      days,
      hours,
      minutes,
      seconds
  );

  return String(buffer);
}

// ============================================================
// HTML HELPERS
// ============================================================

String htmlHeader(
    const String &title
) {

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport"
      content="width=device-width,initial-scale=1">

<title>)rawliteral";

  html += title;

  html += R"rawliteral(</title>

<style>

body {
  font-family: Arial, sans-serif;
  margin: 0;
  padding: 20px;
  background: #f2f4f7;
  color: #222;
}

.container {
  max-width: 1000px;
  margin: auto;
}

.card {
  background: white;
  padding: 18px;
  margin-bottom: 15px;
  border-radius: 12px;
  box-shadow: 0 2px 8px rgba(0,0,0,.08);
}

h1 {
  margin-top: 0;
}

h2 {
  margin-top: 0;
}

.grid {
  display: grid;
  grid-template-columns:
    repeat(auto-fit,minmax(220px,1fr));
  gap: 15px;
}

.value {
  font-size: 24px;
  font-weight: bold;
  margin-top: 8px;
}

.good {
  color: #198754;
}

.warning {
  color: #d97706;
}

.danger {
  color: #dc2626;
}

.blue {
  color: #2563eb;
}

button {
  padding: 11px 16px;
  border: 0;
  border-radius: 8px;
  cursor: pointer;
  margin: 4px;
  font-size: 15px;
}

input {
  width: 100%;
  box-sizing: border-box;
  padding: 10px;
  margin-top: 5px;
  margin-bottom: 12px;
  border: 1px solid #ccc;
  border-radius: 7px;
}

pre {
  white-space: pre-wrap;
  word-break: break-word;
  background: #111;
  color: #eee;
  padding: 12px;
  border-radius: 8px;
  max-height: 400px;
  overflow-y: auto;
}

a {
  text-decoration: none;
}

</style>
</head>

<body>

<div class="container">

)rawliteral";

  return html;
}

// ============================================================
// MAIN DASHBOARD
// ============================================================

void handleRoot() {

  if (!authenticate())
    return;

  String html =
      htmlHeader("ESP32 Water Monitor");

  html += R"rawliteral(

<div class="card">

<h1>ESP32 Water Monitor</h1>

<p>
Firmware: <b>)rawliteral";

  html += FIRMWARE_VERSION;

  html += R"rawliteral(</b>
</p>

</div>

<div class="grid">

<div class="card">

<h2>Water Presence</h2>

<div class="value">)rawliteral";

  if (waterPresence) {

    html +=
        "<span class=\"danger\">WATER DETECTED</span>";

  } else {

    html +=
        "<span class=\"blue\">DRY</span>";
  }

  html += R"rawliteral(</div>

</div>

<div class="card">

<h2>Water Level</h2>

<div class="value">)rawliteral";

  html += waterLevelText(
      currentWaterLevel
  );

  html += " (" +
          String(currentWaterLevel) +
          "/5)";

  html += R"rawliteral(</div>

</div>

</div>

<div class="card">

<h2>Level Sensors</h2>

<div class="grid">

)rawliteral";

  for (int i = 1; i <= 5; i++) {

    bool active = false;

    switch (i) {

      case 1:
        active = digitalRead(LEVEL1_PIN);
        break;

      case 2:
        active = digitalRead(LEVEL2_PIN);
        break;

      case 3:
        active = digitalRead(LEVEL3_PIN);
        break;

      case 4:
        active = digitalRead(LEVEL4_PIN);
        break;

      case 5:
        active = digitalRead(LEVEL5_PIN);
        break;
    }

    html +=
        "<div class=\"card\">";

    html +=
        "<h2>Level " +
        String(i) +
        "</h2>";

    if (active) {

      html +=
          "<div class=\"value good\">WET</div>";

    } else {

      html +=
          "<div class=\"value\">DRY</div>";
    }

    html += "</div>";
  }

  html += R"rawliteral(

</div>

</div>

<div class="card">

<h2>Network</h2>

<p>
IPv4:
<b>)rawliteral";

  html += currentIPv4;

  html += R"rawliteral(</b>
</p>

<p>
IPv6:
<b>)rawliteral";

  html +=
      currentIPv6.length()
          ? currentIPv6
          : "Unavailable";

  html += R"rawliteral(</b>
</p>

<p>
RSSI:
<b>)rawliteral";

  html +=
      String(WiFi.RSSI());

  html += R"rawliteral( dBm</b>
</p>

<p>
DuckDNS:
<b>)rawliteral";

  html +=
      String(DUCKDNS_DOMAIN) +
      ".duckdns.org";

  html += R"rawliteral(</b>
</p>

<p>
DuckDNS status:
<b>)rawliteral";

  html += duckDNSStatus;

  html += R"rawliteral(</b>
</p>

</div>

<div class="card">

<h2>Telegram</h2>

<p>
Status:
<b>)rawliteral";

  html += telegramStatus;

  html += R"rawliteral(</b>
</p>

<a href="/telegram-test">
<button>Test Telegram</button>
</a>

</div>

<div class="card">

<h2>System</h2>

<p>
Uptime:
<b>)rawliteral";

  html += getUptime();

  html += R"rawliteral(</b>
</p>

<p>
Free heap:
<b>)rawliteral";

  html +=
      String(ESP.getFreeHeap());

  html += R"rawliteral( bytes</b>
</p>

<p>
Chip:
<b>)rawliteral";

  html += ESP.getChipModel();

  html += R"rawliteral(</b>
</p>

<p>
CPU:
<b>)rawliteral";

  html +=
      String(ESP.getCpuFreqMHz()) +
      " MHz";

  html += R"rawliteral(</b>
</p>

</div>

<div class="card">

<h2>Actions</h2>

<a href="/wifi">
<button>Wi-Fi Settings</button>
</a>

<a href="/update">
<button>Firmware Update</button>
</a>

<a href="/restart"
   onclick="return confirm('Restart ESP32?');">

<button>Restart</button>

</a>

</div>

<div class="card">

<h2>Event Logs</h2>

<pre>
)rawliteral";

  for (int i = 0; i < logCount; i++) {

    html += logs[i];

    html += "\n";
  }

  html += R"rawliteral(
</pre>

</div>

</div>

<script>

setTimeout(function() {
  location.reload();
}, 10000);

</script>

</body>
</html>
)rawliteral";

  server.send(
      200,
      "text/html",
      html
  );
}

// ============================================================
// WIFI PAGE
// ============================================================

void handleWiFiPage() {

  if (!authenticate())
    return;

  String savedSSID =
      preferences.getString(
          "ssid",
          ""
      );

  String html =
      htmlHeader("Wi-Fi Settings");

  html += R"rawliteral(

<div class="card">

<h1>Wi-Fi Settings</h1>

<form method="POST"
      action="/savewifi">

<label>Wi-Fi SSID</label>

<input
  type="text"
  name="ssid"
  value=")rawliteral";

  html += savedSSID;

  html += R"rawliteral("
  required
>

<label>Wi-Fi Password</label>

<input
  type="password"
  name="password"
  placeholder="Enter Wi-Fi password"
>

<button type="submit">
Save & Restart
</button>

</form>

<p>
After saving, the ESP32 will restart and connect
using the new credentials.
</p>

<a href="/">
Back to Dashboard
</a>

</div>

</div>

</body>
</html>

)rawliteral";

  server.send(
      200,
      "text/html",
      html
  );
}

// ============================================================
// SAVE WIFI
// ============================================================

void handleSaveWiFi() {

  if (!authenticate())
    return;

  if (
      !server.hasArg("ssid") ||
      !server.hasArg("password")
  ) {

    server.send(
        400,
        "text/plain",
        "Missing Wi-Fi credentials"
    );

    return;
  }

  String ssid =
      server.arg("ssid");

  String password =
      server.arg("password");

  ssid.trim();

  if (ssid.length() == 0) {

    server.send(
        400,
        "text/plain",
        "SSID cannot be empty"
    );

    return;
  }

  preferences.putString(
      "ssid",
      ssid
  );

  preferences.putString(
      "password",
      password
  );

  addLog(
      "Wi-Fi credentials saved"
  );

  server.send(
      200,
      "text/html",
      "<html><body>"
      "<h2>Wi-Fi credentials saved.</h2>"
      "<p>Restarting...</p>"
      "</body></html>"
  );

  delay(1000);

  ESP.restart();
}

// ============================================================
// RESTART
// ============================================================

void handleRestart() {

  if (!authenticate())
    return;

  server.send(
      200,
      "text/html",
      "<html><body>"
      "<h2>ESP32 restarting...</h2>"
      "</body></html>"
  );

  delay(500);

  ESP.restart();
}

// ============================================================
// TELEGRAM TEST
// ============================================================

void handleTelegramTest() {

  if (!authenticate())
    return;

  bool success =
      sendTelegram(
          "🟢 ESP32 Water Monitor\n"
          "Telegram test successful.\n"
          "Firmware: " +
          String(FIRMWARE_VERSION),
          true
      );

  if (success) {

    server.send(
        200,
        "text/html",
        "<html><body>"
        "<h2>Telegram test sent successfully.</h2>"
        "<a href='/'>Back</a>"
        "</body></html>"
    );

  } else {

    server.send(
        500,
        "text/html",
        "<html><body>"
        "<h2>Telegram test failed.</h2>"
        "<p>Check the event log.</p>"
        "<a href='/'>Back</a>"
        "</body></html>"
    );
  }
}

// ============================================================
// OTA PAGE
// ============================================================

void handleUpdatePage() {

  if (!authenticate())
    return;

  String html =
      htmlHeader("Firmware Update");

  html += R"rawliteral(

<div class="card">

<h1>Firmware Update</h1>

<p>
Current firmware:
<b>)rawliteral";

  html += FIRMWARE_VERSION;

  html += R"rawliteral(</b>
</p>

<form
  method="POST"
  action="/update"
  enctype="multipart/form-data"
>

<input
  type="file"
  name="firmware"
  accept=".bin"
  required
>

<br><br>

<button type="submit">
Upload Firmware
</button>

</form>

<p>
Select the compiled ESP32 <b>.bin</b> file.
</p>

<a href="/">
Back to Dashboard
</a>

</div>

</div>

</body>
</html>

)rawliteral";

  server.send(
      200,
      "text/html",
      html
  );
}

// ============================================================
// OTA UPLOAD
// ============================================================

void handleFirmwareUpload() {

  HTTPUpload &upload =
      server.upload();

  if (
      upload.status ==
      UPLOAD_FILE_START
  ) {

    otaRunning = true;

    addLog(
        "OTA upload started: " +
        upload.filename
    );

    if (
        !Update.begin(
            UPDATE_SIZE_UNKNOWN
        )
    ) {

      Update.printError(Serial);

      addLog(
          "OTA Update.begin failed"
      );
    }

  } else if (
      upload.status ==
      UPLOAD_FILE_WRITE
  ) {

    if (
        Update.write(
            upload.buf,
            upload.currentSize
        ) != upload.currentSize
    ) {

      Update.printError(Serial);

      addLog(
          "OTA write failed"
      );
    }

  } else if (
      upload.status ==
      UPLOAD_FILE_END
  ) {

    if (Update.end(true)) {

      addLog(
          "OTA upload completed"
      );

    } else {

      Update.printError(Serial);

      addLog(
          "OTA finalization failed"
      );
    }

    otaRunning = false;

  } else if (
      upload.status ==
      UPLOAD_FILE_ABORTED
  ) {

    Update.abort();

    otaRunning = false;

    addLog(
        "OTA upload aborted"
    );
  }
}

// ============================================================
// OTA HANDLER
// ============================================================

void handleFirmwareUpdate() {

  if (!authenticate())
    return;

  if (otaRunning) {

    server.send(
        500,
        "text/plain",
        "OTA already running"
    );

    return;
  }

  server.send(
      200,
      "text/html",
      "<html><body>"
      "<h2>Firmware updated.</h2>"
      "<p>Restarting ESP32...</p>"
      "</body></html>"
  );

  delay(1000);

  ESP.restart();
}

// ============================================================
// 404
// ============================================================

void handleNotFound() {

  if (!authenticate())
    return;

  server.send(
      404,
      "text/plain",
      "404 - Not Found"
  );
}

// ============================================================
// WEB SERVER
// ============================================================

void setupWebServer() {

  server.on(
      "/",
      HTTP_GET,
      handleRoot
  );

  server.on(
      "/wifi",
      HTTP_GET,
      handleWiFiPage
  );

  server.on(
      "/savewifi",
      HTTP_POST,
      handleSaveWiFi
  );

  server.on(
      "/restart",
      HTTP_GET,
      handleRestart
  );

  server.on(
      "/telegram-test",
      HTTP_GET,
      handleTelegramTest
  );

  server.on(
      "/update",
      HTTP_GET,
      handleUpdatePage
  );

  server.on(
      "/update",
      HTTP_POST,
      handleFirmwareUpdate,
      handleFirmwareUpload
  );

  server.onNotFound(
      handleNotFound
  );

  server.begin();

  addLog(
      "Web server started on port 80"
  );
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(500);

  bootTime = millis();

  addLog(
      "================================"
  );

  addLog(
      "ESP32 Water Monitor V" +
      String(FIRMWARE_VERSION)
  );

  addLog(
      "Booting..."
  );

  // ----------------------------------------------------------
  // PREFERENCES
  // ----------------------------------------------------------

  preferences.begin(
      "watermon",
      false
  );

  // ----------------------------------------------------------
  // SENSOR PINS
  // ----------------------------------------------------------

  pinMode(
      WATER_DRIVE_PIN,
      OUTPUT
  );

  digitalWrite(
      WATER_DRIVE_PIN,
      LOW
  );

  pinMode(
      WATER_SENSE_PIN,
      INPUT
  );

  pinMode(
      LEVEL1_PIN,
      INPUT
  );

  pinMode(
      LEVEL2_PIN,
      INPUT
  );

  pinMode(
      LEVEL3_PIN,
      INPUT
  );

  pinMode(
      LEVEL4_PIN,
      INPUT
  );

  pinMode(
      LEVEL5_PIN,
      INPUT
  );

  addLog(
      "Sensor pins initialized"
  );

  // ----------------------------------------------------------
  // WIFI
  // ----------------------------------------------------------

  if (!connectToWiFi()) {

    startSetupAP();

  } else {

    // Initial DuckDNS update
    updateDuckDNS();

    // Initial Telegram notification
    sendTelegram(
        "🟢 ESP32 Water Monitor started\n"
        "Firmware: " +
        String(FIRMWARE_VERSION) +
        "\nIPv4: " +
        currentIPv4 +
        "\nIPv6: " +
        (
          currentIPv6.length()
            ? currentIPv6
            : "Unavailable"
        ),
        true
    );
  }

  // ----------------------------------------------------------
  // WEB SERVER
  // ----------------------------------------------------------

  setupWebServer();

  // ----------------------------------------------------------
  // INITIAL SENSOR STATE
  // ----------------------------------------------------------

  waterPresence =
      readWaterPresenceRaw();

  lastRawWaterPresence =
      waterPresence;

  currentWaterLevel =
      readWaterLevelRaw();

  lastRawWaterLevel =
      currentWaterLevel;

  addLog(
      "Initial water presence: " +
      String(
          waterPresence
            ? "WET"
            : "DRY"
      )
  );

  addLog(
      "Initial water level: " +
      waterLevelText(
          currentWaterLevel
      )
  );

  addLog(
      "System ready"
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  server.handleClient();

  // ----------------------------------------------------------
  // SENSOR MONITORING
  // ----------------------------------------------------------

  updateSensors();

  // ----------------------------------------------------------
  // WIFI RECONNECT
  // ----------------------------------------------------------

  if (
      WiFi.getMode() == WIFI_STA &&
      WiFi.status() != WL_CONNECTED
  ) {

    if (wifiConnected) {

      wifiConnected = false;

      addLog(
          "Wi-Fi disconnected"
      );
    }

  } else if (
      WiFi.getMode() == WIFI_STA &&
      WiFi.status() == WL_CONNECTED
  ) {

    if (!wifiConnected) {

      wifiConnected = true;

      currentIPv4 =
          WiFi.localIP().toString();

      currentIPv6 =
          getIPv6Address();

      addLog(
          "Wi-Fi reconnected"
      );

      updateDuckDNS();
    }
  }

  // ----------------------------------------------------------
  // DUCKDNS PERIODIC UPDATE
  // ----------------------------------------------------------

  if (
      wifiConnected &&
      millis() - lastDuckDNSUpdate >=
          DUCKDNS_UPDATE_INTERVAL
  ) {

    updateDuckDNS();
  }

  delay(5);
}