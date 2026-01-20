#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebSocketsClient.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPUpdate.h>
#include <esp_task_wdt.h>

// ===================================================== //
// FIRMWARE VERSION - SEMANTIC VERSIONING6  6285994      //
// ===================================================== //
const String FIRMWARE_VERSION = "3.2.0";
const String BUILD_DATE = __DATE__;
const String BUILD_TIME = __TIME__;
const int FIRMWARE_BUILD = 3200;  // Integer for version comparison

// =====================================================
// OTA CONFIGURATION - GITHUB RAW CONTENT
// =====================================================
const char *GITHUB_FIRMWARE_URL = "https://raw.githubusercontent.com/brayton-anderson/pool_pay/main/firmware.bin";
const char *GITHUB_VERSION_URL = "https://raw.githubusercontent.com/brayton-anderson/pool_pay/main/version.json";
const unsigned long OTA_CHECK_INTERVAL = 3600000;         // Check every 1 hour
const unsigned long OTA_FORCE_CHECK_INTERVAL = 86400000;  // Force check daily
const int OTA_MAX_RETRIES = 3;
const bool OTA_AUTO_UPDATE = true;              // Auto-update when WiFi available
const unsigned long OTA_CHECK_ON_BOOT = 60000;  // Check 60s after boot

#define WDT_TIMEOUT 30
#define IS_SPI false
#define OLED_MOSI   2
#define OLED_CLK   15
#define OLED_DC    4
#define OLED_CS    23
#define OLED_RESET 22

// if 1 core doesn't work, try with 2
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
// =====================================================
// CONFIGURATION                                        
// =====================================================

// WiFi Credentials (OPTIONAL - System works without WiFi)
struct WiFiNetwork {
  const char *ssid;
  const char *password;
};

WiFiNetwork wifiNetworks[] = {
  { "MOBILE_HOTSPOT", "hotspot123"},
  { "HUAWEI-2.4G-9gZF", "JxMcMV7UMXJ2JnjF" },
  { "SmartTV", "" }
};

const int NUM_WIFI_NETWORKS = 3;

// Bitmask of all cores
esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WDT_TIMEOUT,
        .idle_core_mask = (1 << CONFIG_FREERTOS_NUMBER_OF_CORES) - 1,   
        .trigger_panic = true,
    };

// Supabase Configuration (OPTIONAL - for cloud sync)
const char *SUPABASE_URL = "https://idwlaptigigxzqrjxmam.supabase.co";
const char *SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Imlkd2xhcHRpZ2lneHpxcmp4bWFtIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjQ0NDE5OTYsImV4cCI6MjA4MDAxNzk5Nn0.CZH92B5VEmpFpRIQzWzYVRU7rYqRDv9FEsrRz3IEL7E";
const String SUPABASE_PROJECT_ID = "idwlaptigigxzqrjxmam";

// Device Configuration
const String DEVICE_ID = "POOL_002";
const int TABLE_NUMBER = 2;
const float GAME_PRICE = 20.0;  // KES per game

// M-Pesa Configuration (PRIMARY payment method)
const String MPESA_PAYBILL = "4004519";
const String MPESA_ACCOUNT = "POOL3";

// Pin Configuration
const int SERVO_PIN = 21;
const int BUTTON_PIN = 19;
const int LED_PIN = 2;

// SIM800L Configuration
const int GSM_RX_PIN = 16;
const int GSM_TX_PIN = 17;
const int GSM_BAUD = 9600;

// I2C Configuration

const int I2C_SDA = 23;
const int I2C_SCL = 22;
const uint8_t OLED_ADDRESS = 0x3C;

// Servo Configuration
const int SERVO_LOCKED = 0;
const int SERVO_UNLOCKED = 180;
const int SERVO_RELEASE_TIME = 3000;

// Display Configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// =====================================================
// TIMING CONFIGURATION
// =====================================================
const unsigned long WIFI_RECONNECT_INTERVAL = 90000;
const unsigned long WIFI_CONNECT_TIMEOUT = 15000;
const unsigned long GSM_CHECK_INTERVAL = 1000;
const unsigned long SYNC_INTERVAL = 3000;
const unsigned long DEBOUNCE_DELAY = 50;
const unsigned long GSM_INIT_TIMEOUT = 30000;
const unsigned long WATCHDOG_TIMEOUT = 120000;
const unsigned long PERSISTENCE_INTERVAL = 10000;

// ===================================================== //
// GLOBAL OBJECTS                                        //
// ===================================================== //

Servo ballServo;
#ifdef IS_SPI == true
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, OLED_MOSI, OLED_CLK, OLED_DC, OLED_RESET, OLED_CS);
#else
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
#endif

WebSocketsClient webSocket;
HTTPClient http;
HardwareSerial gsmSerial(2);
Preferences preferences;

// =====================================================
// STATE MANAGEMENT - OFFLINE-FIRST + OTA
// =====================================================
struct OTAState {
  String currentVersion = FIRMWARE_VERSION;
  String availableVersion = "";
  int availableBuild = 0;
  bool updateAvailable = false;
  bool updateInProgress = false;
  bool updateSuccess = false;
  bool updateFailed = false;
  String updateError = "";
  int updateAttempts = 0;
  unsigned long lastOTACheck = 0;
  unsigned long lastOTAAttempt = 0;
  int totalUpdates = 0;
  bool autoUpdateEnabled = OTA_AUTO_UPDATE;
  size_t updateSize = 0;
  int updateProgress = 0;
  unsigned long updateStartTime = 0;
  String lastUpdateDate = "";
};

struct SystemState {
  // PRIMARY Connection (GSM is king!)
  bool gsmReady = false;

  // OPTIONAL Connections
  bool wifiConnected = false;
  bool supabaseConnected = false;
  bool realtimeConnected = false;
  int currentWiFiNetwork = -1;

  // CORE Table State (Always available, persisted to flash)
  int gamesRemaining = 0;
  int gamesRemainingLastSynced = 0;
  String status = "offline";
  unsigned long lastUsed = 0;

  // Button State
  bool buttonPressed = false;
  bool lastButtonState = HIGH;
  unsigned long lastDebounceTime = 0;


  unsigned long lastWiFiAttempt = 0;
  unsigned long lastGSMCheck = 0;
  unsigned long lastSync = 0;
  unsigned long lastWatchdog = 0;
  unsigned long lastPersistence = 0;
  unsigned long bootTime = 0;

  // Statistics (Persisted)
  int wifiReconnects = 0;
  int gsmResets = 0;
  int paymentsProcessed = 0;
  int gamesPlayed = 0;
  float totalRevenue = 0.0;

  // Sync State
  bool needsSync = false;
  bool syncInProgress = false;
  int pendingGamesToSync = 0;

  // Display
  bool displayNeedsUpdate = true;

  // Realtime
  int realtimeRef = 1;
  bool subscribed = false;

  // Recovery flags
  bool needsGSMRecovery = false;

  // Operation Mode
  bool offlineMode = true;

  // OTA State
  OTAState ota;
} state;

// =====================================================
// FUNCTION DECLARATIONS
// =====================================================
void checkForOTAUpdate();
void performOTAUpdate();
String getOTAStatus();
void checkSerialCommands();

// =====================================================
// PERSISTENT STORAGE - SURVIVES POWER LOSS
// =====================================================

void loadPersistedState() {
  preferences.begin("poolpay", false);

  state.gamesRemaining = preferences.getInt("games", 0);
  state.paymentsProcessed = preferences.getInt("payments", 0);
  state.gamesPlayed = preferences.getInt("played", 0);
  state.totalRevenue = preferences.getFloat("revenue", 0.0);
  state.gamesRemainingLastSynced = preferences.getInt("lastSync", 0);

  // Load OTA state
  state.ota.totalUpdates = preferences.getInt("otaUpdates", 0);
  state.ota.lastUpdateDate = preferences.getString("otaLastDate", "Never");
  state.ota.autoUpdateEnabled = preferences.getBool("otaAutoUpdate", OTA_AUTO_UPDATE);

  Serial.println("📂 Loaded persisted state:");
  Serial.printf(" Games: %d\n", state.gamesRemaining);
  Serial.printf(" Payments: %d\n", state.paymentsProcessed);
  Serial.printf(" Revenue: KES %.2f\n", state.totalRevenue);
  Serial.printf(" OTA Updates: %d\n", state.ota.totalUpdates);
  Serial.printf(" Last Update: %s\n", state.ota.lastUpdateDate.c_str());

  preferences.end();
}

void persistState() {
  preferences.begin("poolpay", false);

  preferences.putInt("games", state.gamesRemaining);
  preferences.putInt("payments", state.paymentsProcessed);
  preferences.putInt("played", state.gamesPlayed);
  preferences.putFloat("revenue", state.totalRevenue);
  preferences.putInt("lastSync", state.gamesRemainingLastSynced);

  // Persist OTA state
  preferences.putInt("otaUpdates", state.ota.totalUpdates);
  preferences.putString("otaLastDate", state.ota.lastUpdateDate);
  preferences.putBool("otaAutoUpdate", state.ota.autoUpdateEnabled);

  preferences.end();
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  printBootScreen();

  state.bootTime = millis();

  // Load persisted state FIRST
  loadPersistedState();

  // Hardware initialization
  initializeHardware();

  // Initialize OLED
  if (!initDisplay()) {
    Serial.println("❌ CRITICAL: OLED failed!");
    blinkSOSPattern();
  }

  // Initialize Servo
  initServo();

  // PRIMARY: Initialize GSM
  initGSM();

  // OPTIONAL: Try WiFi
  if (NUM_WIFI_NETWORKS > 0) {
    Serial.println("📶 Attempting WiFi (optional)...");
    connectWiFiNonBlocking();
  }

  // If WiFi connected, try cloud services
  if (state.wifiConnected) {
    initCloudServices();
  }

  Serial.println("\n╔═══════════════════════════════════════╗");
  Serial.println("║ 🚀 SYSTEM OPERATIONAL - READY! ║");
  Serial.println("║ ✅ Offline Mode: ENABLED ║");
  Serial.println("║ ✅ GSM Payments: ACTIVE ║");
  Serial.printf("║ ✅ Games Available: %-3d ║\n", state.gamesRemaining);
  Serial.printf("║ 📦 Firmware: v%-17s║\n", FIRMWARE_VERSION.c_str());
  Serial.printf("║ 🔄 OTA: %-23s║\n", state.ota.autoUpdateEnabled ? "ENABLED" : "DISABLED");
  Serial.println("╚═══════════════════════════════════════╝\n");

  showMessage("SYSTEM READY", "v" + FIRMWARE_VERSION, 2000);
  updateDisplay();

  // Schedule OTA check after boot
  state.ota.lastOTACheck = millis() - OTA_CHECK_INTERVAL + OTA_CHECK_ON_BOOT;
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
  unsigned long now = millis();

  // PRIORITY 1: GSM PAYMENTS (MONEY!)
  checkForNewSMS();
  // if (true) {
  //   checkForNewSMS();
  //   state.lastGSMCheck = now;
  // }

  // PRIORITY 2: BUTTON HANDLING
  checkButton();

  // PRIORITY 3: PERSISTENCE
  if (now - state.lastPersistence > PERSISTENCE_INTERVAL) {
    persistState();
    state.lastPersistence = now;
  }

  // PRIORITY 4: WIFI (Non-blocking)
  if (!state.wifiConnected && (now - state.lastWiFiAttempt > WIFI_RECONNECT_INTERVAL)) {
    connectWiFiNonBlocking();
    state.lastWiFiAttempt = now;
  }

  // PRIORITY 5: OTA UPDATE CHECK
  if (state.wifiConnected && !state.ota.updateInProgress) {
    if (now - state.ota.lastOTACheck > OTA_CHECK_INTERVAL) {
      checkForOTAUpdate();
      state.ota.lastOTACheck = now;
    }
  }

  // PRIORITY 6: WEBSOCKET
  if (state.realtimeConnected) {
    webSocket.loop();
  }

  // PRIORITY 7: CLOUD SYNC
  if (state.wifiConnected && state.needsSync && !state.syncInProgress) {
    if (now - state.lastSync > SYNC_INTERVAL) {
      syncToCloud();
      state.lastSync = now;
    }
  }

  // PRIORITY 8: SYSTEM HEALTH
  if (now - state.lastWatchdog > WATCHDOG_TIMEOUT) {
    performSystemHealthCheck();
    state.lastWatchdog = now;
  }

  // PRIORITY 9: DISPLAY UPDATE
  if (state.displayNeedsUpdate) {
    updateDisplay();
    state.displayNeedsUpdate = false;
  }

  // PRIORITY 10: SERIAL COMMANDS
  checkSerialCommands();

  // RECOVERY
  if (state.needsGSMRecovery) {
    recoverGSM();
  }

  delay(10);
}

// =====================================================
// OTA UPDATE SYSTEM - GITHUB-POWERED BEAST MODE 🔥
// =====================================================

void checkForOTAUpdate() {
  if (!state.wifiConnected || state.ota.updateInProgress) return;

  Serial.println("\n🔍 Checking for firmware updates...");
  showProgress("Checking OTA...");

  HTTPClient http;
  http.begin(GITHUB_VERSION_URL);
  http.setTimeout(15000);
  http.addHeader("Cache-Control", "no-cache");
  http.addHeader("Pragma", "no-cache");

  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.printf("❌ Version check failed (HTTP %d)\n", httpCode);
    http.end();
    state.displayNeedsUpdate = true;
    return;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.println("❌ Failed to parse version.json");
    state.displayNeedsUpdate = true;
    return;
  }

  String availableVersion = doc["version"].as<String>();
  int availableBuild = doc["build"].as<int>();
  String releaseNotes = doc["notes"].as<String>();
  bool forcedUpdate = doc["forced"] | false;

  Serial.printf("📦 Current: v%s (Build %d)\n", FIRMWARE_VERSION.c_str(), FIRMWARE_BUILD);
  Serial.printf("📦 Available: v%s (Build %d)\n", availableVersion.c_str(), availableBuild);

  if (availableBuild <= FIRMWARE_BUILD) {
    Serial.println("✅ Already up to date!");
    state.ota.updateAvailable = false;
    state.displayNeedsUpdate = true;
    return;
  }

  Serial.println("🆕 NEW VERSION AVAILABLE!");
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.printf("Version: %s\n", availableVersion.c_str());
  Serial.printf("Release Notes: %s\n", releaseNotes.c_str());
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━");

  state.ota.updateAvailable = true;
  state.ota.availableVersion = availableVersion;
  state.ota.availableBuild = availableBuild;

  showMessage("UPDATE!", "v" + availableVersion, 2000);

  // Auto-update if enabled or forced
  if (state.ota.autoUpdateEnabled || forcedUpdate) {
    Serial.println("🚀 Auto-update enabled - Starting OTA...");
    delay(2000);
    performOTAUpdate();
  } else {
    Serial.println("ℹ️ Auto-update disabled - Type 'update' to install");
    state.displayNeedsUpdate = true;
  }
}

void performOTAUpdate() {
  if (!state.wifiConnected) {
    Serial.println("❌ WiFi required for OTA");
    showMessage("UPDATE FAILED", "No WiFi", 2000);
    state.displayNeedsUpdate = true;
    return;
  }

  if (state.ota.updateAttempts >= OTA_MAX_RETRIES) {
    Serial.println("❌ Max OTA retries reached");
    state.ota.updateFailed = true;
    state.ota.updateError = "Max retries";
    showMessage("UPDATE FAILED", "Max retries", 2000);
    state.displayNeedsUpdate = true;
    return;
  }

  state.ota.updateInProgress = true;
  state.ota.updateAttempts++;
  state.ota.updateStartTime = millis();
  state.ota.updateProgress = 0;

  Serial.println("\n╔═══════════════════════════════════════╗");
  Serial.println("║ 🚀 OTA UPDATE INITIATED ║");
  Serial.println("╚═══════════════════════════════════════╝");
  Serial.printf("Version: %s → %s\n", FIRMWARE_VERSION.c_str(), state.ota.availableVersion.c_str());
  Serial.printf("Build: %d → %d\n", FIRMWARE_BUILD, state.ota.availableBuild);
  Serial.printf("Attempt: %d/%d\n\n", state.ota.updateAttempts, OTA_MAX_RETRIES);

  showMessage("UPDATING", "DO NOT POWER OFF!", 2000);

  // Persist state before update (CRITICAL!)
  Serial.println("💾 Persisting state...");
  persistState();
  delay(500);

  // Lock balls
  lockBalls();

  // Disable watchdog
  esp_task_wdt_deinit();

  // Configure HTTPUpdate
  httpUpdate.setLedPin(LED_PIN, HIGH);
  httpUpdate.rebootOnUpdate(false);

  // Update callbacks
  httpUpdate.onStart([]() {
    Serial.println("🔄 Download started...");
    showMessage("DOWNLOADING", "Please wait...", 100);
  });

  httpUpdate.onEnd([]() {
    Serial.println("\n✅ Download complete!");
    showMessage("COMPLETE!", "Rebooting...", 2000);
  });

  httpUpdate.onProgress([](int current, int total) {
    static int lastPercent = -1;
    int percent = (current * 100) / total;

    if (percent != lastPercent && percent % 5 == 0) {
      Serial.printf("📥 Progress: %d%% (%d/%d bytes)\n", percent, current, total);

      // Update OLED display
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.println("UPDATING FIRMWARE");
      display.setCursor(0, 24);
      display.printf("v%s -> v%s", FIRMWARE_VERSION.c_str(), state.ota.availableVersion.c_str());

      // Progress bar
      display.setCursor(0, 44);
      display.printf("%d%%", percent);
      int barWidth = (SCREEN_WIDTH * percent) / 100;
      display.drawRect(20, 44, SCREEN_WIDTH - 40, 8, SSD1306_WHITE);
      if (barWidth > 2) {
        display.fillRect(22, 48, barWidth - 48, 4, SSD1306_WHITE);
      }
      display.display();

      lastPercent = percent;
    }

    state.ota.updateProgress = percent;
    state.ota.updateSize = total;

    // Blink LED
    digitalWrite(LED_PIN, (current / 4096) % 2);
  });

  httpUpdate.onError([](int error) {
    Serial.printf("❌ OTA Error (%d): ", error);

    switch (error) {
      case HTTP_UPDATE_FAILED:
        Serial.println("Update failed");
        state.ota.updateError = "Update failed";
        break;
      case HTTP_UPDATE_NO_UPDATES:
        Serial.println("No update available");
        state.ota.updateError = "No update";
        break;
      default:
        Serial.printf("Unknown error: %d\n", error);
        state.ota.updateError = "Error: " + String(error);
        break;
    }

    showMessage("UPDATE FAILED", state.ota.updateError, 3000);
    state.ota.updateFailed = true;
    state.ota.updateInProgress = false;
    state.displayNeedsUpdate = true;
  });

  // Perform the update
  Serial.println("🌐 Connecting to GitHub...");
  Serial.printf("URL: %s\n", GITHUB_FIRMWARE_URL);

  WiFiClient client;
  t_httpUpdate_return ret = httpUpdate.update(client, GITHUB_FIRMWARE_URL);

  // Handle result
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("❌ Update failed: %s\n", httpUpdate.getLastErrorString().c_str());
      state.ota.updateFailed = true;
      state.ota.updateError = httpUpdate.getLastErrorString();
      state.ota.updateInProgress = false;
      blinkError();
      esp_task_wdt_init(&twdt_config);
      state.displayNeedsUpdate = true;
      break;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("ℹ️ No update available");
      state.ota.updateInProgress = false;
      state.displayNeedsUpdate = true;
      break;

    case HTTP_UPDATE_OK:
      Serial.println("\n╔═══════════════════════════════════════╗");
      Serial.println("║ ✅ UPDATE SUCCESSFUL!                 ║");
      Serial.println("╚═══════════════════════════════════════╝");
      Serial.printf("New version: v%s\n", state.ota.availableVersion.c_str());
      Serial.printf("Update time: %lu ms\n", millis() - state.ota.updateStartTime);

      // Record successful update
      state.ota.updateSuccess = true;
      state.ota.totalUpdates++;
      state.ota.lastUpdateDate = String(BUILD_DATE) + " " + String(BUILD_TIME);
      state.ota.currentVersion = state.ota.availableVersion;
      persistState();

      showMessage("SUCCESS!", "Rebooting...", 2000);

      // Success blink pattern
      for (int i = 0; i < 10; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(100);
        digitalWrite(LED_PIN, LOW);
        delay(100);
      }

      Serial.println("🔄 Rebooting in 3 seconds...");
      delay(3000);

      ESP.restart();
      break;
  }
}


String getOTAStatus() {
  String status = "\n╔═══════════════════════════════════════╗\n";
  status += "║ OTA UPDATE STATUS                     ║\n";
  status += "╚═══════════════════════════════════════╝\n";

  status += "Current Version: v" + FIRMWARE_VERSION + "\n";
  status += "Build Number: " + String(FIRMWARE_BUILD) + "\n";
  status += "Build Date: " + String(BUILD_DATE) + " " + String(BUILD_TIME) + "\n";
  status += "Total Updates: " + String(state.ota.totalUpdates) + "\n";
  status += "Last Update: " + state.ota.lastUpdateDate + "\n";
  status += "Auto-Update: " + String(state.ota.autoUpdateEnabled ? "ENABLED" : "DISABLED") + "\n\n";

  if (state.ota.updateAvailable) {
    status += "🆕 UPDATE AVAILABLE!\n";
    status += "Version: v" + state.ota.availableVersion + "\n";
    status += "Build: " + String(state.ota.availableBuild) + "\n";
  } else {
    status += "✅ Up to date!\n";
  }

  if (state.ota.updateInProgress) {
    status += "\n🔄 UPDATE IN PROGRESS\n";
    status += "Progress: " + String(state.ota.updateProgress) + "%\n";
    status += "Size: " + String(state.ota.updateSize / 1024) + " KB\n";
  }

  if (state.ota.updateFailed) {
    status += "\n❌ LAST UPDATE FAILED\n";
    status += "Error: " + state.ota.updateError + "\n";
    status += "Attempts: " + String(state.ota.updateAttempts) + "/" + String(OTA_MAX_RETRIES) + "\n";
  }

  status += "═══════════════════════════════════════\n";

  return status;
}

// =====================================================
// HARDWARE INITIALIZATION
// =====================================================

void initializeHardware() {
  Serial.println("🔧 Initializing hardware...");
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(LED_PIN, LOW);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  Serial.println("✅ Hardware initialized");
}

// =====================================================
// GSM MODULE
// =====================================================

void initGSM() {
  Serial.println("\n📱 Initializing SIM800L GSM module (PRIMARY)...");
  showProgress("Init GSM...");

  gsmSerial.begin(GSM_BAUD, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  delay(3000);

  while (gsmSerial.available()) gsmSerial.read();

  Serial.println("⚡ Powering up GSM module...");

  unsigned long startTime = millis();
  bool gsmOK = false;
  int attempts = 0;

  while (!gsmOK && attempts < 5 && (millis() - startTime < GSM_INIT_TIMEOUT)) {
    attempts++;
    Serial.printf(" Attempt %d/5...\n", attempts);
    gsmSerial.println("AT");
    delay(1000);
    if (waitForGSMResponse("OK", 3000)) {
      gsmOK = true;
      Serial.println("✅ GSM responding!");
      break;
    }
    delay(2000);
  }

  if (!gsmOK) {
    Serial.println("❌ GSM FAILED - CRITICAL ERROR!");
    showMessage("GSM FAILED", "Check power", 5000);
    state.gsmReady = false;
    state.needsGSMRecovery = true;
    blinkSOSPattern();
    return;
  }

  delay(1000);
  gsmSerial.println("AT+CSQ");
  delay(1000);
  String signalResponse = readGSMResponse();
  Serial.println("📶 Signal: " + signalResponse);

  gsmSerial.println("AT+CMGF=1");
  delay(1000);
  gsmSerial.println("AT+CNMI=2,2,0,0,0");
  delay(1000);
  gsmSerial.println("AT+CMGD=1,4");
  delay(2000);

  state.gsmReady = true;
  Serial.println("✅ GSM OPERATIONAL - Monitoring M-Pesa SMS");
  showMessage("GSM READY", "Payments Active", 2000);
}

void recoverGSM() {
  Serial.println("🔄 GSM RECOVERY INITIATED...");
  state.gsmResets++;
  state.needsGSMRecovery = false;
  gsmSerial.end();
  delay(5000);
  initGSM();
}

bool waitForGSMResponse(String expected, unsigned long timeout) {
  unsigned long start = millis();
  String response = "";
  while (millis() - start < timeout) {
    while (gsmSerial.available()) {
      char c = gsmSerial.read();
      response += c;
      Serial.print(c);
    }
    if (response.indexOf(expected) != -1) return true;
    delay(100);
  }
  return false;
}

String readGSMResponse() {
  String response = "";
  unsigned long start = millis();
  while (millis() - start < 2000) {
    while (gsmSerial.available()) {
      char c = gsmSerial.read();
      response += c;
    }
    delay(10);
  }
  return response;
}

// =====================================================
// SMS PROCESSING
// =====================================================

void checkForNewSMS() {
  //Serial.println("🔧 Initializing ffffff...");
  //if (!gsmSerial.available()) return;
  //Serial.println("🔧 Initializing rrrrrr...");
  String smsData = "";
  while (gsmSerial.available()) {
    char c = gsmSerial.read();
    smsData += c;
    delay(2);
  }

  if (smsData.length() == 0) return;

  Serial.println("📨 SMS RECEIVED:");
  Serial.println(smsData);

  // M-Pesa detection (works with multiple formats!)
  if ((smsData.indexOf("Confirmed") != -1 || smsData.indexOf("confirmed") != -1) && (smsData.indexOf("received") != -1) && (smsData.indexOf("KSH") != -1 || smsData.indexOf("Ksh") != -1)) {
    processMPesaSMS(smsData);
  }
}

void processMPesaSMS(String smsContent) {
  Serial.println("\n💰 M-PESA PAYMENT DETECTED!");
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━");
  showMessage("PAYMENT", "Processing...", 800);

  float amount = extractAmountFromSMS(smsContent);
  Serial.printf("💵 Amount: KES %.2f\n", amount);

  if (amount <= 0) {
    Serial.println("❌ Invalid amount");
    showMessage("Error", "Invalid amount", 2000);
    return;
  }

  Serial.printf("💵 Amount: KES %.2f\n", amount);
  int games = (int)(amount / GAME_PRICE);

  if (games <= 0) {
    Serial.println("⚠️ Amount too small");
    showMessage("Too Small", "Min KES 20", 2000);
    return;
  }

  Serial.printf("🎮 Games to credit: %d\n", games);

  state.gamesRemaining += games;
  state.paymentsProcessed++;
  state.totalRevenue += amount;
  state.pendingGamesToSync += games;
  state.needsSync = true;
  state.displayNeedsUpdate = true;

  persistState();
  blinkLED(games);
  showMessage("PAID!", String(games) + " games added", 3000);
  logPayment(amount, games, smsContent);

  Serial.println("✅ PAYMENT PROCESSED!");
  Serial.printf("📊 Total games now: %d\n", state.gamesRemaining);

  if (state.wifiConnected && state.supabaseConnected) {
    syncToCloud();
  }
}

String extractFirstKSH(String msg) {
  int idx = msg.indexOf("KSH");
  if (idx == -1) return "";

  idx += 3;  // skip "KSH"

  String num = "";
  for (int i = idx; i < msg.length(); i++) {
    if (isDigit(msg[i]) || msg[i] == '.') {
      num += msg[i];
    } else {
      break;
    }
  }
  return num;
}


String extractNthKSH(String msg, int n) {
  int count = 0;
  int idx = 0;

  while (count < n) {
    idx = msg.indexOf("KSH", idx);
    if (idx == -1) return "";
    idx += 3;
    count++;
  }

  String num = "";
  for (int i = idx; i < msg.length(); i++) {
    if (isDigit(msg[i]) || msg[i] == '.') {
      num += msg[i];
    } else {
      break;
    }
  }
  return num;
}


String extractBetween(String text, String start, String end) {
  int s = (start == "") ? 0 : text.indexOf(start);
  if (s == -1) return "";
  if (start != "") s += start.length();
  int e = text.indexOf(end, s);
  if (e == -1) return text.substring(s);
  return text.substring(s, e);
}

String extractAfter(String text, String key, String alt, String prefix) {
  int idx = text.indexOf(key);
  if (idx == -1) {
    idx = text.indexOf(alt);
    if (idx == -1) return "";
  }
  idx = text.indexOf(prefix, idx);
  if (idx == -1) return "";
  idx += prefix.length();
  while (idx < text.length() && (text[idx] == ' ' || text[idx] == ':')) idx++;
  String num = "";
  for (int i = idx; i < text.length(); i++) {
    if (isDigit(text[i]) || text[i] == '.' || text[i] == ',')
      num += text[i];
    else
      break;
  }
  return num;
}

float extractAmountFromSMS(String sms) {
  // Handle both "KSH" and "Ksh"
  int kshIndex = sms.indexOf("KSH");
  if (kshIndex == -1) {
    kshIndex = sms.indexOf("Ksh");
  }

  if (kshIndex == -1) return 0;

  // Start after "KSH" or "Ksh"
  int startIndex = kshIndex + 3;
  String amountStr = "";

  // Extract digits and decimal point
  for (int i = startIndex; i < sms.length(); i++) {
    char c = sms.charAt(i);
    if (isDigit(c) || c == '.') {
      amountStr += c;
    } else if (amountStr.length() > 0) {
      // Stop when we hit first non-digit after starting
      break;
    }
  }

  Serial.printf("🔍 Extracted amount string: '%s'\n", amountStr.c_str());

  return amountStr.length() > 0 ? amountStr.toFloat() : 0;
}

void logPayment(float amount, int games, String smsContent) {
  Serial.println("\n╔══════════ PAYMENT LOG ══════════╗");
  Serial.printf("║ Time: %lu ms               ║\n", millis());
  Serial.printf("║ Amount: KES %.2f            ║\n", amount);
  Serial.printf("║ Games: %d                   ║\n", games);
  Serial.printf("║ Total: %d games             ║\n", state.gamesRemaining);
  Serial.printf("║ Mode: %-21s║\n", state.wifiConnected ? "ONLINE" : "OFFLINE");
  Serial.println("╚═════════════════════════════════╝\n");
  // Extract and log sender info
  int fromIndex = smsContent.indexOf("from ");
  if (fromIndex != -1) {
    String sender = smsContent.substring(fromIndex + 5, fromIndex + 17);
    Serial.printf("📱 From: %s\n", sender.c_str());
  }

  Serial.println();
}

// =====================================================
// WIFI
// =====================================================

void connectWiFiNonBlocking() {
  if (state.wifiConnected) return;

  Serial.println("📶 Trying WiFi (optional)...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  static int lastTriedNetwork = -1;
  int networkToTry = (lastTriedNetwork + 1) % NUM_WIFI_NETWORKS;
  lastTriedNetwork = networkToTry;

  Serial.printf(" Network: %s\n", wifiNetworks[networkToTry].ssid);
  WiFi.begin(wifiNetworks[networkToTry].ssid, wifiNetworks[networkToTry].password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start < WIFI_CONNECT_TIMEOUT)) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    state.wifiConnected = true;
    state.currentWiFiNetwork = networkToTry;
    state.offlineMode = false;

    Serial.println("✅ WiFi connected!");
    Serial.printf(" IP: %s\n", WiFi.localIP().toString().c_str());

    showMessage("WiFi OK", "Cloud enabled", 1500);
    initCloudServices();

    if (state.needsSync) syncToCloud();
  } else {
    Serial.println("⚠️ WiFi failed - continuing offline");
  }
}

// =====================================================
// CLOUD SERVICES
// =====================================================

void initCloudServices() {
  Serial.println("☁️ Initializing cloud services...");
  testSupabaseConnection();

  if (state.supabaseConnected) {
    setupRealtimeWebSocket();
    if (state.needsSync) {
      syncToCloud();
    } else {
      syncFromCloud();
    }
  }
}

void testSupabaseConnection() {
  if (!state.wifiConnected) return;

  Serial.println("🔗 Testing Supabase...");
  String url = String(SUPABASE_URL) + "/rest/v1/pool_tables?device_id=eq." + DEVICE_ID + "&table_number=eq." + String(TABLE_NUMBER) + "&limit=1";

  http.begin(url);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.setTimeout(10000);

  int httpCode = http.GET();

  if (httpCode == 200) {
    state.supabaseConnected = true;
    Serial.println("✅ Supabase connected!");
  } else {
    state.supabaseConnected = false;
    Serial.printf("⚠️ Supabase unavailable (Code: %d)\n", httpCode);
  }

  http.end();
}

void syncToCloud() {
  if (!state.supabaseConnected || state.syncInProgress) return;

  state.syncInProgress = true;
  Serial.println("☁️ Syncing LOCAL → CLOUD...");

  String url = String(SUPABASE_URL) + "/rest/v1/pool_tables?device_id=eq." + DEVICE_ID + "&table_number=eq." + String(TABLE_NUMBER);

  http.begin(url);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");

  DynamicJsonDocument doc(256);
  doc["games_remaining"] = state.gamesRemaining;
  doc["status"] = "online";
  doc["last_used"] = "now()";

  String jsonString;
  serializeJson(doc, jsonString);

  int httpCode = http.PATCH(jsonString);

  if (httpCode == 204 || httpCode == 200) {
    Serial.println("✅ Cloud synced successfully!");
    state.gamesRemainingLastSynced = state.gamesRemaining;
    state.needsSync = false;
    state.pendingGamesToSync = 0;
  } else {
    Serial.printf("⚠️ Sync failed (Code: %d) - will retry\n", httpCode);
  }

  http.end();
  state.syncInProgress = false;
}

void syncFromCloud() {
  if (!state.supabaseConnected) return;

  Serial.println("☁️ Checking CLOUD → LOCAL...");
  String url = String(SUPABASE_URL) + "/rest/v1/pool_tables?device_id=eq." + DEVICE_ID + "&table_number=eq." + String(TABLE_NUMBER);

  http.begin(url);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);

  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);

    if (!error && doc.size() > 0) {
      JsonObject table = doc[0];
      int cloudGames = table["games_remaining"];

      if (cloudGames > state.gamesRemaining) {
        int diff = cloudGames - state.gamesRemaining;
        Serial.printf("☁️ Cloud has %d more games - syncing down\n", diff);
        state.gamesRemaining = cloudGames;
        state.gamesRemainingLastSynced = cloudGames;
        persistState();
        blinkLED(diff);
        showMessage("Cloud Sync", String(diff) + " games", 2000);
        state.displayNeedsUpdate = true;
      }
    }
  }

  http.end();
}

// =====================================================
// REALTIME WEBSOCKET
// =====================================================

void setupRealtimeWebSocket() {
  if (!state.wifiConnected) return;
  Serial.println("🔴 WebSocket (optional)...");
  String host = String(SUPABASE_PROJECT_ID) + ".supabase.co";
  String path = String("/realtime/v1/websocket?apikey=") + SUPABASE_ANON_KEY + "&vsn=1.0.0";
  webSocket.beginSSL(host.c_str(), 443, path.c_str());
  webSocket.onEvent(onWebSocketEvent);
  webSocket.setReconnectInterval(5000);
}

void onWebSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("🔴 WebSocket disconnected");
      state.realtimeConnected = false;
      break;
    case WStype_CONNECTED:
      Serial.println("✅ WebSocket connected");
      state.realtimeConnected = true;
      subscribeToRealtime();
      break;
    case WStype_TEXT:
      handleRealtimeMessage(String((char *)payload));
      break;
  }
}

void subscribeToRealtime() {
  DynamicJsonDocument doc(512);
  doc["event"] = "phx_join";
  doc["topic"] = "realtime:public:pool_tables";
  doc["payload"]["config"]["broadcast"]["self"] = true;
  doc["ref"] = String(state.realtimeRef++);
  String jsonString;
  serializeJson(doc, jsonString);
  webSocket.sendTXT(jsonString);
}

void handleRealtimeMessage(String message) {
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, message)) return;
  String event = doc["event"].as<String>();
  if (event == "phx_reply") {
    String status = doc["payload"]["status"].as<String>();
    if (status == "ok") {
      state.subscribed = true;
      Serial.println("✅ Realtime subscribed!");
    }
  }
}

// =====================================================
// DISPLAY FUNCTIONS
// =====================================================

bool initDisplay() {
  Serial.println("📺 Initializing OLED...");
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) return false;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(" PoolPay PRO");
  display.println(" OFFLINE-FIRST");
  display.printf(" TABLE %d", TABLE_NUMBER);
  display.display();

  Serial.println("✅ Display ready");
  return true;
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.printf("TABLE%d", TABLE_NUMBER);

  // Status indicators
  if (state.gsmReady) {
    display.fillCircle(100, 6, 4, SSD1306_WHITE);
  } else {
    display.drawCircle(100, 6, 4, SSD1306_WHITE);
  }

  if (state.wifiConnected) {
    display.fillCircle(110, 6, 4, SSD1306_WHITE);
  } else {
    display.drawCircle(110, 6, 4, SSD1306_WHITE);
  }

  if (state.supabaseConnected && !state.needsSync) {
    display.fillCircle(120, 6, 4, SSD1306_WHITE);
  } else if (state.needsSync) {
    display.drawCircle(120, 6, 4, SSD1306_WHITE);
  }

  display.setTextSize(2);
  display.setCursor(0, 20);
  display.printf("%d", state.gamesRemaining);

  display.setTextSize(1);
  display.setCursor(0, 48);
  display.print("games");

  if (state.offlineMode) {
    display.setCursor(40, 20);
    display.print("OFFLINE");
  }

  if (state.gamesRemaining > 0) {
    display.setCursor(40, 40);
    display.print("PRESS>");
  } else {
    display.setCursor(40, 40);
    display.print("PAY KES20");
  }

  display.display();
}

void showMessage(String line1, String line2, int duration) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;

  display.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 16);
  display.println(line1);

  if (line2.length() > 0) {
    display.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 40);
    display.println(line2);
  }

  display.display();
  delay(duration);
  state.displayNeedsUpdate = true;
}

void showProgress(String message) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 24);
  display.println(message);
  display.display();
}

// =====================================================
// SERVO CONTROL
// =====================================================

void initServo() {
  Serial.println("🔧 Initializing servo...");
  ballServo.attach(SERVO_PIN);
  lockBalls();
  Serial.println("✅ Servo ready");
}

void lockBalls() {
  ballServo.write(SERVO_LOCKED);
}

void releaseBalls() {
  Serial.println("\n🎱 RELEASING BALLS...");
  Serial.println("━━━━━━━━━━━━━━━━━━━━");

  showMessage("RELEASING", "Enjoy!", 800);

  ballServo.write(SERVO_UNLOCKED);
  delay(SERVO_RELEASE_TIME);
  ballServo.write(SERVO_LOCKED);

  state.lastUsed = millis();
  state.gamesPlayed++;

  Serial.printf("✅ Game started!\n");
  Serial.printf("📊 Games remaining: %d\n", state.gamesRemaining);
  Serial.println("━━━━━━━━━━━━━━━━━━━━\n");

  showMessage("GAME ON!", String(state.gamesRemaining - 1) + " left", 2000);
  state.displayNeedsUpdate = true;
}

// =====================================================
// BUTTON HANDLING
// =====================================================

void checkButton() {
  int reading = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  if (reading != state.lastButtonState) {
    state.lastDebounceTime = now;
  }

  if ((now - state.lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading == LOW && !state.buttonPressed) {
      state.buttonPressed = true;
      handleButtonPress();
    } else if (reading == HIGH) {
      state.buttonPressed = false;
    }
  }

  state.lastButtonState = reading;
}

void handleButtonPress() {
  Serial.println("\n🔘 BUTTON PRESSED!");

  if (state.gamesRemaining <= 0) {
    Serial.println("⚠️ No games available");
    showMessage("PAY FIRST!", "SMS to " + MPESA_PAYBILL, 3000);
    blinkError();
    state.displayNeedsUpdate = true;
    return;
  }

  releaseBalls();

  state.gamesRemaining = max(0, state.gamesRemaining - 1);
  state.needsSync = true;
  state.displayNeedsUpdate = true;

  persistState();

  if (state.wifiConnected && state.supabaseConnected && !state.syncInProgress) {
    syncToCloud();
  }
}

// =====================================================
// SYSTEM HEALTH CHECK
// =====================================================

void performSystemHealthCheck() {
  Serial.println("\n╔═══════════════════════════════════════╗");
  Serial.println("║ SYSTEM HEALTH REPORT ║");
  Serial.println("╚═══════════════════════════════════════╝");

  unsigned long uptime = (millis() - state.bootTime) / 1000;
  Serial.printf("⏱️ Uptime: %lu sec (%.1fh)\n", uptime, uptime / 3600.0);

  Serial.println("\n🔌 CONNECTIVITY:");
  Serial.printf(" GSM: %s\n", state.gsmReady ? "✅ READY" : "❌ DOWN");
  Serial.printf(" WiFi: %s\n", state.wifiConnected ? "✅ ONLINE" : "⚠️ OFFLINE");
  Serial.printf(" Cloud: %s\n", state.supabaseConnected ? "✅ SYNCED" : "⚠️ NOT SYNCED");

  Serial.println("\n💰 BUSINESS METRICS:");
  Serial.printf(" Games Available: %d\n", state.gamesRemaining);
  Serial.printf(" Payments Today: %d\n", state.paymentsProcessed);
  Serial.printf(" Games Played: %d\n", state.gamesPlayed);
  Serial.printf(" Total Revenue: KES %.2f\n", state.totalRevenue);

  Serial.println("\n📦 FIRMWARE:");
  Serial.printf(" Version: v%s\n", FIRMWARE_VERSION.c_str());
  Serial.printf(" Build: %d\n", FIRMWARE_BUILD);
  Serial.printf(" OTA Updates: %d\n", state.ota.totalUpdates);
  if (state.ota.updateAvailable) {
    Serial.printf(" 🆕 Update Available: v%s\n", state.ota.availableVersion.c_str());
  }

  Serial.println("\n🔧 SYSTEM STATS:");
  Serial.printf(" WiFi Reconnects: %d\n", state.wifiReconnects);
  Serial.printf(" GSM Resets: %d\n", state.gsmResets);
  Serial.printf(" Free Heap: %d bytes\n", ESP.getFreeHeap());

  Serial.println("═══════════════════════════════════════\n");

  if (!state.gsmReady) {
    Serial.println("🚨 GSM DOWN - INITIATING RECOVERY!");
    state.needsGSMRecovery = true;
  }

  if (ESP.getFreeHeap() < 10000) {
    Serial.println("⚠️ LOW MEMORY WARNING!");
  }
}

// =====================================================
// VISUAL FEEDBACK
// =====================================================

void blinkLED(int times) {
  for (int i = 0; i < min(times, 10); i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(150);
    digitalWrite(LED_PIN, LOW);
    delay(150);
  }
}

void blinkError() {
  for (int i = 0; i < 5; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }
}

void blinkSOSPattern() {
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      digitalWrite(LED_PIN, HIGH);
      delay(200);
      digitalWrite(LED_PIN, LOW);
      delay(200);
    }
    delay(400);
    for (int j = 0; j < 3; j++) {
      digitalWrite(LED_PIN, HIGH);
      delay(500);
      digitalWrite(LED_PIN, LOW);
      delay(200);
    }
    delay(400);
    for (int j = 0; j < 3; j++) {
      digitalWrite(LED_PIN, HIGH);
      delay(200);
      digitalWrite(LED_PIN, LOW);
      delay(200);
    }
    delay(2000);
  }
}

// =====================================================
// UTILITY FUNCTIONS
// =====================================================

void printBootScreen() {
  Serial.println("\n\n");
  Serial.println("╔═══════════════════════════════════════╗");
  Serial.println("║ PoolPay ULTIMATE v3.2 - OTA EDITION ║");
  Serial.println("║ GSM PRIMARY • WiFi OPTIONAL         ║");
  Serial.println("║ AUTO-UPDATE • 100% UPTIME           ║");
  Serial.println("╚═══════════════════════════════════════╝");
  Serial.println();
  Serial.printf("Device ID: %s\n", DEVICE_ID.c_str());
  Serial.printf("Table Number: %d\n", TABLE_NUMBER);
  Serial.printf("Game Price: KES %.2f\n", GAME_PRICE);
  Serial.printf("M-Pesa Paybill: %s\n", MPESA_PAYBILL.c_str());
  Serial.println();
  Serial.println("📦 FIRMWARE:");
  Serial.printf(" Version: v%s\n", FIRMWARE_VERSION.c_str());
  Serial.printf(" Build: %d\n", FIRMWARE_BUILD);
  Serial.printf(" Date: %s %s\n", BUILD_DATE, BUILD_TIME);
  Serial.printf(" OTA Updates: %d\n", state.ota.totalUpdates);
  Serial.printf(" Auto-Update: %s\n", OTA_AUTO_UPDATE ? "ENABLED" : "DISABLED");
  Serial.println();
  Serial.println("💡 SYSTEM FEATURES:");
  Serial.println(" • Payments via GSM SMS (no WiFi needed)");
  Serial.println(" • Games work offline (no connection needed)");
  Serial.println(" • Cloud sync when available (optional)");
  Serial.println(" • OTA updates via GitHub (wireless)");
  Serial.println(" • Survives power loss (persisted to flash)");
  Serial.println();
  Serial.println("🚀 BOOTING SYSTEMS...\n");
}

// =====================================================
// SERIAL COMMANDS
// =====================================================

void checkSerialCommands() {
  if (!Serial.available()) return;

  String command = Serial.readStringUntil('\n');
  command.trim();
  command.toLowerCase();

  if (command == "status" || command == "health") {
    performSystemHealthCheck();
  } else if (command == "ota" || command == "firmware") {
    Serial.print(getOTAStatus());
  } else if (command == "checkupdate" || command == "checkota") {
    Serial.println("🔍 Manual OTA check...");
    if (state.wifiConnected) {
      checkForOTAUpdate();
    } else {
      Serial.println("❌ WiFi required for OTA check");
    }
  } else if (command == "update" || command == "upgrade") {
    Serial.println("🚀 Manual OTA update...");
    if (state.wifiConnected) {
      if (state.ota.updateAvailable) {
        performOTAUpdate();
      } else {
        Serial.println("ℹ️ Checking for updates first...");
        checkForOTAUpdate();
        if (state.ota.updateAvailable) {
          performOTAUpdate();
        }
      }
    } else {
      Serial.println("❌ WiFi required for OTA");
    }
  } else if (command == "otaauto on" || command == "autoupdate on") {
    state.ota.autoUpdateEnabled = true;
    persistState();
    Serial.println("✅ Auto-update ENABLED");
  } else if (command == "otaauto off" || command == "autoupdate off") {
    state.ota.autoUpdateEnabled = false;
    persistState();
    Serial.println("⚠️ Auto-update DISABLED");
  } else if (command == "sync") {
    Serial.println("🔄 Manual sync triggered");
    if (state.wifiConnected) {
      syncToCloud();
    } else {
      Serial.println("⚠️ WiFi not connected - cannot sync");
    }
  } else if (command == "syncdown") {
    Serial.println("🔄 Sync from cloud");
    if (state.wifiConnected) {
      syncFromCloud();
    } else {
      Serial.println("⚠️ WiFi not connected");
    }
  } else if (command == "persist") {
    Serial.println("💾 Saving to flash...");
    persistState();
    Serial.println("✅ Saved!");
  } else if (command == "load") {
    Serial.println("📂 Loading from flash...");
    loadPersistedState();
    state.displayNeedsUpdate = true;
    Serial.println("✅ Loaded!");
  } else if (command == "test") {
    showMessage("Test OK", "v" + FIRMWARE_VERSION, 2000);
  } else if (command == "release") {
    if (state.gamesRemaining > 0) {
      releaseBalls();
      state.gamesRemaining--;
      persistState();
      state.displayNeedsUpdate = true;
    } else {
      Serial.println("⚠️ No games available");
    }
  } else if (command == "wifi") {
    Serial.println("🔄 WiFi reconnect triggered");
    connectWiFiNonBlocking();
  } else if (command == "gsm") {
    Serial.println("🔄 GSM reset triggered");
    recoverGSM();
  } else if (command.startsWith("add ")) {
    int gamesToAdd = command.substring(4).toInt();
    if (gamesToAdd > 0) {
      state.gamesRemaining += gamesToAdd;
      state.needsSync = true;
      persistState();
      Serial.printf("✅ Added %d games (offline)\n", gamesToAdd);
      state.displayNeedsUpdate = true;
    }
  } else if (command.startsWith("setgames ")) {
    int games = command.substring(9).toInt();
    state.gamesRemaining = games;
    state.needsSync = true;
    persistState();
    Serial.printf("✅ Set to %d games\n", games);
    state.displayNeedsUpdate = true;
  } else if (command == "offline") {
    state.offlineMode = true;
    state.wifiConnected = false;
    WiFi.disconnect();
    Serial.println("📡 Forced offline mode");
    state.displayNeedsUpdate = true;
  } else if (command == "online") {
    state.offlineMode = false;
    Serial.println("🌐 Attempting online mode...");
    connectWiFiNonBlocking();
  } else if (command == "reboot") {
    Serial.println("🔄 REBOOTING...");
    persistState();
    delay(1000);
    ESP.restart();
  } else if (command == "help") {
    printHelpMenu();
  } else {
    Serial.println("❓ Unknown command. Type 'help'");
  }
}

void printHelpMenu() {
  Serial.println("\n╔═══════════════════════════════════════╗");
  Serial.println("║ DIAGNOSTIC COMMANDS ║");
  Serial.println("╚═══════════════════════════════════════╝");
  Serial.println("\n📦 FIRMWARE (OTA):");
  Serial.println("ota - Show OTA status");
  Serial.println("checkupdate - Check for updates");
  Serial.println("update - Install available update");
  Serial.println("otaauto on/off - Enable/disable auto-update");

  Serial.println("\n🎮 GAMES:");
  Serial.println("release - Manual ball release");
  Serial.println("add X - Add X games (offline)");
  Serial.println("setgames X - Set games to X");

  Serial.println("\n☁️ SYNC:");
  Serial.println("sync - Push local to cloud");
  Serial.println("syncdown - Pull cloud to local");
  Serial.println("persist - Save state to flash");
  Serial.println("load - Load state from flash");

  Serial.println("\n📡 CONNECTIVITY:");
  Serial.println("wifi - Reconnect WiFi");
  Serial.println("gsm - Reset GSM");
  Serial.println("offline - Force offline mode");
  Serial.println("online - Force online mode");

  Serial.println("\n═══════════════════════════════════════\n");
}
