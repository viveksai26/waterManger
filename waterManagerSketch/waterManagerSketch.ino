#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>

// ============================================================
// ESP32 Water Monitor - V2
// Features:
//   - Persistent Wi-Fi credentials
//   - Wi-Fi scanning
//   - Setup Access Point if Wi-Fi connection fails
//   - Web dashboard
//   - Wi-Fi configuration
//   - Browser OTA firmware update
//   - System logs
//   - Restart
// ============================================================

WebServer server(80);
Preferences preferences;

// ============================================================
// Configuration
// ============================================================

const char* AP_SSID = "ESP32-WaterMonitor";
const char* AP_PASSWORD = "01234567890";

const char* WEB_USERNAME = "admin";
const char* WEB_PASSWORD = "admin1234";

// ============================================================
// Wi-Fi credentials
// ============================================================

String wifiSSID = "";
String wifiPassword = "";

bool setupMode = false;

// ============================================================
// Logs
// ============================================================

const int MAX_LOGS = 50;

String logs[MAX_LOGS];
int logCount = 0;

void addLog(String message) {

  String entry =
    "[" + String(millis() / 1000) + "s] " + message;

  Serial.println(entry);

  if (logCount < MAX_LOGS) {

    logs[logCount] = entry;
    logCount++;

  } else {

    for (int i = 0; i < MAX_LOGS - 1; i++) {
      logs[i] = logs[i + 1];
    }

    logs[MAX_LOGS - 1] = entry;
  }
}

// ============================================================
// Authentication
// ============================================================

bool authenticate() {

  if (!server.authenticate(WEB_USERNAME, WEB_PASSWORD)) {

    server.requestAuthentication();

    return false;
  }

  return true;
}

// ============================================================
// Load Wi-Fi credentials
// ============================================================

void loadWiFiCredentials() {

  preferences.begin("wifi", true);

  wifiSSID = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");

  preferences.end();

  if (wifiSSID.length() > 0) {

    addLog("Saved Wi-Fi credentials loaded");

  } else {

    addLog("No saved Wi-Fi credentials");
  }
}

// ============================================================
// Save Wi-Fi credentials
// ============================================================

void saveWiFiCredentials(String ssid, String password) {

  preferences.begin("wifi", false);

  preferences.putString("ssid", ssid);
  preferences.putString("password", password);

  preferences.end();

  wifiSSID = ssid;
  wifiPassword = password;

  addLog("Wi-Fi credentials saved");
}

// ============================================================
// Start setup Access Point
// ============================================================

void startSetupMode() {

  setupMode = true;

  WiFi.mode(WIFI_AP);

  WiFi.softAP(AP_SSID, AP_PASSWORD);

  IPAddress ip = WiFi.softAPIP();

  addLog("Setup mode started");
  addLog("AP SSID: " + String(AP_SSID));
  addLog("AP IP: " + ip.toString());
}

// ============================================================
// Connect to Wi-Fi
// ============================================================

bool connectToWiFi() {

  if (wifiSSID.length() == 0) {

    addLog("No Wi-Fi configured");

    return false;
  }

  setupMode = false;

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    wifiSSID.c_str(),
    wifiPassword.c_str()
  );

  addLog("Connecting to Wi-Fi: " + wifiSSID);

  int attempts = 0;

  while (
    WiFi.status() != WL_CONNECTED &&
    attempts < 30
  ) {

    delay(500);

    Serial.print(".");

    attempts++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {

    addLog("Wi-Fi connected");

    addLog(
      "IP address: " +
      WiFi.localIP().toString()
    );

    addLog(
      "Signal: " +
      String(WiFi.RSSI()) +
      " dBm"
    );

    return true;
  }

  addLog("Wi-Fi connection failed");

  return false;
}

// ============================================================
// HTML helpers
// ============================================================

String getUptime() {

  unsigned long seconds = millis() / 1000;

  unsigned long days = seconds / 86400;

  seconds %= 86400;

  unsigned long hours = seconds / 3600;

  seconds %= 3600;

  unsigned long minutes = seconds / 60;

  seconds %= 60;

  return
    String(days) + "d " +
    String(hours) + "h " +
    String(minutes) + "m " +
    String(seconds) + "s";
}

String getIPAddress() {

  if (setupMode) {

    return WiFi.softAPIP().toString();

  } else {

    return WiFi.localIP().toString();
  }
}

String getSSID() {

  if (setupMode) {

    return String(AP_SSID);

  } else {

    return WiFi.SSID();
  }
}

String getRSSI() {

  if (setupMode) {

    return "Setup mode";

  } else {

    return String(WiFi.RSSI()) + " dBm";
  }
}

// ============================================================
// Dashboard
// ============================================================

void handleRoot() {

  if (!authenticate()) {
    return;
  }

  String html = R"rawliteral(
<!DOCTYPE html>

<html>

<head>

<meta name="viewport"
      content="width=device-width, initial-scale=1">

<title>ESP32 Water Monitor</title>

<style>

body {
  font-family: Arial, sans-serif;
  margin: 0;
  padding: 20px;
  background: #f3f3f3;
}

.container {
  max-width: 850px;
  margin: auto;
}

.card {
  background: white;
  padding: 20px;
  margin-bottom: 15px;
  border-radius: 12px;
  box-shadow: 0 2px 8px rgba(0,0,0,0.08);
}

h1 {
  margin-top: 0;
}

.status {
  font-size: 20px;
  font-weight: bold;
}

.online {
  color: green;
}

.setup {
  color: orange;
}

.info {
  display: grid;
  grid-template-columns: 150px 1fr;
  gap: 8px;
}

.logs {
  background: #111;
  color: #00ff88;
  padding: 15px;
  border-radius: 8px;
  font-family: monospace;
  white-space: pre-wrap;
  max-height: 400px;
  overflow-y: auto;
}

button {
  padding: 11px 16px;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  font-size: 15px;
  margin: 4px;
}

.primary {
  background: #1976d2;
  color: white;
}

.danger {
  background: #d32f2f;
  color: white;
}

.secondary {
  background: #555;
  color: white;
}

a {
  text-decoration: none;
}

</style>

</head>

<body>

<div class="container">

<h1>ESP32 Water Monitor</h1>

<div class="card">

<div class="status %STATUS_CLASS%">
%STATUS%

</div>

</div>

<div class="card">

<h2>Wi-Fi</h2>

<div class="info">

<b>SSID</b>
<span>%SSID%</span>

<b>IP Address</b>
<span>%IP%</span>

<b>Signal</b>
<span>%RSSI%</span>

</div>

<br>

<a href="/wifi">
<button class="primary">
Wi-Fi Settings
</button>
</a>

</div>

<div class="card">

<h2>System</h2>

<div class="info">

<b>Chip</b>
<span>ESP32</span>

<b>Uptime</b>
<span>%UPTIME%</span>

<b>Free Heap</b>
<span>%HEAP% bytes</span>

<b>Firmware</b>
<span>V2.0</span>

</div>

</div>

<div class="card">

<h2>Logs</h2>

<div class="logs">%LOGS%</div>

</div>

<div class="card">

<h2>Maintenance</h2>

<a href="/update">
<button class="primary">
OTA Firmware Update
</button>
</a>

<button class="danger"
        onclick="restartESP()">
Restart ESP32
</button>

</div>

</div>

<script>

function restartESP() {

  if (confirm("Restart ESP32?")) {

    fetch("/restart");

    alert("ESP32 is restarting...");

  }

}

</script>

</body>

</html>
)rawliteral";

  String status;
  String statusClass;

  if (setupMode) {

    status = "🟠 SETUP MODE";
    statusClass = "setup";

  } else if (WiFi.status() == WL_CONNECTED) {

    status = "🟢 ESP32 ONLINE";
    statusClass = "online";

  } else {

    status = "🔴 OFFLINE";
    statusClass = "setup";
  }

  html.replace("%STATUS%", status);
  html.replace("%STATUS_CLASS%", statusClass);

  html.replace("%SSID%", getSSID());
  html.replace("%IP%", getIPAddress());
  html.replace("%RSSI%", getRSSI());

  html.replace("%UPTIME%", getUptime());

  html.replace(
    "%HEAP%",
    String(ESP.getFreeHeap())
  );

  String logText;

  for (int i = 0; i < logCount; i++) {

    logText += logs[i];

    if (i < logCount - 1) {
      logText += "\n";
    }
  }

  html.replace("%LOGS%", logText);

  server.send(
    200,
    "text/html",
    html
  );
}

// ============================================================
// Wi-Fi settings page
// ============================================================

void handleWiFiPage() {

  if (!authenticate()) {
    return;
  }

  int networkCount = WiFi.scanNetworks();

  String html = R"rawliteral(
<!DOCTYPE html>

<html>

<head>

<meta name="viewport"
      content="width=device-width, initial-scale=1">

<title>Wi-Fi Settings</title>

<style>

body {
  font-family: Arial;
  margin: 20px;
  background: #f3f3f3;
}

.container {
  max-width: 700px;
  margin: auto;
}

.card {
  background: white;
  padding: 20px;
  border-radius: 12px;
}

input, select {
  width: 100%;
  padding: 12px;
  margin: 8px 0 15px;
  box-sizing: border-box;
  font-size: 16px;
}

button {
  padding: 12px 18px;
  border: none;
  border-radius: 8px;
  background: #1976d2;
  color: white;
  font-size: 16px;
}

</style>

</head>

<body>

<div class="container">

<div class="card">

<h2>Wi-Fi Settings</h2>

<form method="POST"
      action="/savewifi">

<label>Wi-Fi Network</label>

<select name="ssid">

<option value="">
-- Select Network --
</option>

%NETWORKS%

</select>

<label>Password</label>

<input
  type="password"
  name="password"
  placeholder="Wi-Fi password">

<button type="submit">
Save & Connect
</button>

</form>

<br>

<a href="/">
Back to Dashboard
</a>

</div>

</div>

</body>

</html>
)rawliteral";

  String networks;

  for (int i = 0; i < networkCount; i++) {

    String ssid = WiFi.SSID(i);

    if (ssid.length() == 0) {
      continue;
    }

    networks += "<option value=\"";
    networks += ssid;
    networks += "\">";
    networks += ssid;
    networks += " (";
    networks += String(WiFi.RSSI(i));
    networks += " dBm)";
    networks += "</option>";
  }

  html.replace(
    "%NETWORKS%",
    networks
  );

  WiFi.scanDelete();

  server.send(
    200,
    "text/html",
    html
  );
}

// ============================================================
// Save Wi-Fi
// ============================================================

void handleSaveWiFi() {

  if (!authenticate()) {
    return;
  }

  String ssid =
    server.arg("ssid");

  String password =
    server.arg("password");

  if (ssid.length() == 0) {

    server.send(
      400,
      "text/plain",
      "SSID cannot be empty"
    );

    return;
  }

  saveWiFiCredentials(
    ssid,
    password
  );

  String html = R"rawliteral(
<!DOCTYPE html>

<html>

<head>

<meta name="viewport"
      content="width=device-width, initial-scale=1">

<title>Wi-Fi</title>

</head>

<body>

<h2>Wi-Fi saved</h2>

<p>
ESP32 will restart and attempt to connect.
</p>

</body>

</html>
)rawliteral";

  server.send(
    200,
    "text/html",
    html
  );

  delay(1500);

  ESP.restart();
}

// ============================================================
// Restart
// ============================================================

void handleRestart() {

  if (!authenticate()) {
    return;
  }

  server.send(
    200,
    "text/plain",
    "Restarting..."
  );

  delay(500);

  ESP.restart();
}

// ============================================================
// OTA page
// ============================================================

void handleUpdatePage() {

  if (!authenticate()) {
    return;
  }

  String html = R"rawliteral(
<!DOCTYPE html>

<html>

<head>

<meta name="viewport"
      content="width=device-width, initial-scale=1">

<title>OTA Update</title>

<style>

body {
  font-family: Arial;
  margin: 20px;
  background: #f3f3f3;
}

.container {
  max-width: 600px;
  margin: auto;
}

.card {
  background: white;
  padding: 20px;
  border-radius: 12px;
}

input {
  width: 100%;
  margin: 15px 0;
}

button {
  padding: 12px 20px;
  background: #1976d2;
  color: white;
  border: none;
  border-radius: 8px;
}

</style>

</head>

<body>

<div class="container">

<div class="card">

<h2>OTA Firmware Update</h2>

<p>
Select the compiled ESP32 firmware (.bin)
</p>

<form
  method="POST"
  action="/update"
  enctype="multipart/form-data">

<input
  type="file"
  name="firmware"
  accept=".bin"
  required>

<br>

<button type="submit">
Upload Firmware
</button>

</form>

<br>

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
// OTA upload handler
// ============================================================

void handleUpdateUpload() {

  HTTPUpload& upload =
    server.upload();

  if (upload.status == UPLOAD_FILE_START) {

    if (!authenticate()) {
      return;
    }

    addLog(
      "OTA update started: " +
      upload.filename
    );

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {

      Update.printError(Serial);

      addLog("OTA begin failed");
    }
  }

  else if (
    upload.status == UPLOAD_FILE_WRITE
  ) {

    if (
      Update.write(
        upload.buf,
        upload.currentSize
      ) != upload.currentSize
    ) {

      Update.printError(Serial);
    }
  }

  else if (
    upload.status == UPLOAD_FILE_END
  ) {

    if (Update.end(true)) {

      addLog(
        "OTA update successful"
      );

    } else {

      Update.printError(Serial);

      addLog(
        "OTA update failed"
      );
    }
  }
}

// ============================================================
// OTA result
// ============================================================

void handleUpdateResult() {

  if (!authenticate()) {
    return;
  }

  if (Update.hasError()) {

    server.send(
      500,
      "text/plain",
      "OTA update failed"
    );

  } else {

    server.send(
      200,
      "text/html",
      "<h2>Update successful</h2>"
      "<p>ESP32 is restarting...</p>"
    );

    delay(1000);

    ESP.restart();
  }
}

// ============================================================
// Setup
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  addLog("================================");
  addLog("ESP32 Water Monitor V2");
  addLog("Booting...");
  addLog("================================");

  // Load saved Wi-Fi
  loadWiFiCredentials();

  // Try Wi-Fi
  bool connected =
    connectToWiFi();

  // If failed, start setup AP
  if (!connected) {

    startSetupMode();
  }

  // Web routes
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
    "/update",
    HTTP_GET,
    handleUpdatePage
  );

  server.on(
    "/update",
    HTTP_POST,
    handleUpdateResult,
    handleUpdateUpload
  );

  server.begin();

  addLog("Web server started");

  if (setupMode) {

    addLog(
      "Connect to Wi-Fi: " +
      String(AP_SSID)
    );

    addLog(
      "Setup IP: " +
      WiFi.softAPIP().toString()
    );

  } else {

    addLog(
      "Dashboard: http://" +
      WiFi.localIP().toString()
    );
  }
}

// ============================================================
// Main loop
// ============================================================

void loop() {

  server.handleClient();

  delay(2);
}