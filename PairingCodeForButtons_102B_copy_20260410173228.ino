#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>

// =====================================
// BUTTON SETTINGS
// =====================================
#define BUTTON_ID   1     // CHANGE PER BUTTON: 1,2,3,4,5
#define BUTTON_PIN  2     // CHANGE IF NEEDED
#define LED_R_PIN   7
#define LED_G_PIN   8
#define LED_B_PIN   9

const unsigned long DEBOUNCE_MS                = 40;
const unsigned long LONG_PRESS_MS              = 5000;  // hold for 5 sec to pair
const unsigned long PAIR_SEND_MS               = 700;
const unsigned long HEARTBEAT_MS               = 5000;
const unsigned long REQUIRED_IDLE_BEFORE_PAIR  = 500;   // must be released/high this long before pair hold can begin
const unsigned long PAIR_LOCKOUT_AFTER_FIRE_MS = 8000;  // block pairing after a fire request

// =====================================
// MESSAGE TYPES
// =====================================
enum MessageType : uint8_t {
  MSG_PAIR_REQUEST = 1,
  MSG_PAIR_ACK     = 2,
  MSG_FIRE_REQUEST = 3,
  MSG_FIRE_ACK     = 4
};

struct __attribute__((packed)) Packet {
  uint8_t type;
  uint8_t buttonId;
  uint8_t reserved1;
  uint8_t reserved2;
};

// =====================================
// GLOBALS
// =====================================
Preferences prefs;

uint8_t dispenserMAC[6];
bool paired = false;
bool pairingMode = false;

bool lastRawReading = HIGH;
bool stableButtonState = HIGH;
unsigned long lastDebounceTime = 0;

bool pressActive = false;
unsigned long pressStartTime = 0;
bool longPressTriggered = false;

unsigned long lastPairSend = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastReleaseTime = 0;
unsigned long lastFireRequestTime = 0;

enum LedMode {
  LED_IDLE_UNPAIRED,
  LED_IDLE_PAIRED,
  LED_PAIRING,
  LED_PAIR_SUCCESS,
  LED_FIRE_FLASH
};

LedMode ledMode = LED_IDLE_UNPAIRED;
unsigned long ledTimer = 0;
int ledStep = 0;

// =====================================
// LED HELPERS
// =====================================
void setLED(bool r, bool g, bool b) {
  digitalWrite(LED_R_PIN, r ? HIGH : LOW);
  digitalWrite(LED_G_PIN, g ? HIGH : LOW);
  digitalWrite(LED_B_PIN, b ? HIGH : LOW);
}

void ledOff() {
  setLED(false, false, false);
}

void flashWhiteNow() {
  ledMode = LED_FIRE_FLASH;
  ledTimer = millis();
}

void setButtonColor(bool on) {
  if (!on) {
    ledOff();
    return;
  }

  switch (BUTTON_ID) {
    case 1: setLED(true, false, false);  break; // Red
    case 2: setLED(true, true, false);   break; // Orange approx
    case 3: setLED(true, true, false);   break; // Yellow approx
    case 4: setLED(false, true, false);  break; // Green
    case 5: setLED(false, false, true);  break; // Blue
    default: setLED(true, true, true);   break; // White fallback
  }
}

void updateLED() {
  unsigned long now = millis();

  switch (ledMode) {
    case LED_IDLE_UNPAIRED:
      if ((now / 250) % 8 == 0) setLED(true, false, false);
      else ledOff();
      break;

    case LED_IDLE_PAIRED:
      if ((now - lastHeartbeat) < 120) setButtonColor(true);
      else setButtonColor(false);
      break;

    case LED_PAIRING:
      if ((now / 250) % 2 == 0) setLED(false, false, true);
      else ledOff();
      break;

    case LED_PAIR_SUCCESS:
      if (now - ledTimer > 150) {
        ledTimer = now;
        ledStep++;
      }

      if (ledStep >= 6) {
        ledStep = 0;
        ledMode = paired ? LED_IDLE_PAIRED : LED_IDLE_UNPAIRED;
        ledOff();
      } else {
        bool on = (ledStep % 2 == 0);
        setLED(false, on, false);
      }
      break;

    case LED_FIRE_FLASH:
      if (now - ledTimer < 120) {
        setLED(true, true, true);
      } else {
        ledMode = paired ? LED_IDLE_PAIRED : LED_IDLE_UNPAIRED;
        ledOff();
      }
      break;
  }
}

// =====================================
// STORAGE
// =====================================
void saveDispenserMAC(const uint8_t *mac) {
  prefs.begin("pairing", false);
  prefs.putBytes("dispMAC", mac, 6);
  prefs.putBool("paired", true);
  prefs.end();
}

bool loadDispenserMAC() {
  prefs.begin("pairing", true);
  bool wasPaired = prefs.getBool("paired", false);

  if (wasPaired) {
    size_t len = prefs.getBytes("dispMAC", dispenserMAC, 6);
    paired = (len == 6);
  } else {
    paired = false;
  }

  prefs.end();
  return paired;
}

// =====================================
// ESPNOW HELPERS
// =====================================
bool addPeer(const uint8_t *mac) {
  if (mac == nullptr) return false;

  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  return esp_now_add_peer(&peerInfo) == ESP_OK;
}

bool sendPacket(const uint8_t *destMac, MessageType type) {
  if (destMac == nullptr) return false;

  Packet pkt;
  pkt.type = static_cast<uint8_t>(type);
  pkt.buttonId = BUTTON_ID;
  pkt.reserved1 = 0;
  pkt.reserved2 = 0;

  esp_err_t result = esp_now_send(destMac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
  if (result != ESP_OK) {
    Serial.print("esp_now_send failed. Error: ");
    Serial.println(result);
    return false;
  }

  return true;
}

void sendPairRequestBroadcast() {
  uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  if (addPeer(broadcastMAC)) {
    sendPacket(broadcastMAC, MSG_PAIR_REQUEST);
  }
}

void sendFireRequest() {
  if (!paired) {
    Serial.println("Short press detected, but button is not paired.");
    return;
  }

  if (!addPeer(dispenserMAC)) {
    Serial.println("Failed to add dispenser peer.");
    return;
  }

  Serial.println("Sending FIRE request...");
  flashWhiteNow();
  sendPacket(dispenserMAC, MSG_FIRE_REQUEST);
  lastFireRequestTime = millis();

  // Hard reset press tracking so a stale held state cannot drift into pairing
  pressActive = false;
  longPressTriggered = false;
}

// =====================================
// CALLBACKS
// =====================================
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.print("Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}

void onDataRecv(const esp_now_recv_info_t *recvInfo, const uint8_t *incomingData, int len) {
  if (recvInfo == nullptr || incomingData == nullptr) return;
  if (len != sizeof(Packet)) return;

  Packet pkt;
  memcpy(&pkt, incomingData, sizeof(pkt));

  const uint8_t *srcMac = recvInfo->src_addr;

  if (pkt.type == MSG_PAIR_ACK) {
    memcpy(dispenserMAC, srcMac, 6);
    saveDispenserMAC(dispenserMAC);
    paired = true;
    pairingMode = false;

    ledMode = LED_PAIR_SUCCESS;
    ledTimer = millis();
    ledStep = 0;

    Serial.println("Pairing successful.");
  }

  if (pkt.type == MSG_FIRE_ACK) {
    Serial.println("Fire acknowledged by dispenser.");
  }
}

// =====================================
// BUTTON HANDLING
// =====================================
void handleButton() {
  bool rawReading = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  if (rawReading != lastRawReading) {
    lastDebounceTime = now;
  }

  if ((now - lastDebounceTime) > DEBOUNCE_MS) {
    if (rawReading != stableButtonState) {
      stableButtonState = rawReading;

      // PRESSED
      if (stableButtonState == LOW) {
        pressActive = true;
        pressStartTime = now;
        longPressTriggered = false;
        Serial.println("Button press started.");
      }

      // RELEASED
      else {
        if (pressActive) {
          unsigned long pressDuration = now - pressStartTime;

          Serial.print("Button released. Duration = ");
          Serial.println(pressDuration);

          if (!longPressTriggered && pressDuration < LONG_PRESS_MS) {
            sendFireRequest();
          }
        }

        pressActive = false;
        longPressTriggered = false;
        lastReleaseTime = now;
      }
    }
  }

  bool hadCleanIdleBeforePress =
      (pressStartTime >= lastReleaseTime) &&
      ((pressStartTime - lastReleaseTime) >= REQUIRED_IDLE_BEFORE_PAIR);

  bool pairingLockoutActive =
      ((now - lastFireRequestTime) < PAIR_LOCKOUT_AFTER_FIRE_MS);

  if (pressActive && !longPressTriggered && !pairingMode) {
    unsigned long heldTime = now - pressStartTime;

    if (!pairingLockoutActive && hadCleanIdleBeforePress && heldTime >= LONG_PRESS_MS) {
      longPressTriggered = true;
      pairingMode = true;
      ledMode = LED_PAIRING;
      Serial.println("Entered pairing mode.");
    }
  }

  lastRawReading = rawReading;
}

// =====================================
// SETUP / LOOP
// =====================================
void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  ledOff();

  delay(100);
  lastRawReading = digitalRead(BUTTON_PIN);
  stableButtonState = lastRawReading;
  lastReleaseTime = millis();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed.");
    while (true) {
      setLED(true, false, false);
      delay(150);
      ledOff();
      delay(150);
    }
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  paired = loadDispenserMAC();
  pairingMode = false;
  pressActive = false;
  longPressTriggered = false;
  lastFireRequestTime = 0;

  ledMode = paired ? LED_IDLE_PAIRED : LED_IDLE_UNPAIRED;

  Serial.println("Button started.");
  Serial.printf("BUTTON_ID: %d\n", BUTTON_ID);
  Serial.printf("Paired: %s\n", paired ? "YES" : "NO");
  Serial.printf("Initial button state: %d\n", stableButtonState);
}

void loop() {
  handleButton();

  unsigned long now = millis();

  if (pairingMode) {
    if (now - lastPairSend >= PAIR_SEND_MS) {
      lastPairSend = now;
      sendPairRequestBroadcast();
      Serial.println("Broadcasting pair request...");
    }
  }

  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
  }

  updateLED();
}