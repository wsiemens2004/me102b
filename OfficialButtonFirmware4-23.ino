// Official Button Firmware
// Version: BUTTON_OFFICIAL_V1
// Target: XIAO ESP32S3 / ESP32 button node
//
// Features:
// - Short press sends FIRE request when paired
// - Hold button for 5 seconds to enter pairing mode
// - Pairing request is broadcast over ESP-NOW
// - Dispenser replies with PAIR_ACK
// - Dispenser MAC is saved in Preferences
// - Paired button gives occasional colored idle pulse
// - White flash on FIRE send
// - Color flash on successful pairing
// - Debounced button handling
// - Lockout prevents accidental re-pairing right after FIRE

#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>

// =====================================
// Button hardware settings
// =====================================
#define BUTTON_ID   1
#define BUTTON_PIN  2
#define LED_R_PIN   7
#define LED_G_PIN   8
#define LED_B_PIN   9

// =====================================
// Timing settings
// =====================================
const unsigned long DEBOUNCE_MS                 = 50;
const unsigned long LONG_PRESS_MS               = 5000;
const unsigned long PAIR_SEND_MS                = 700;
const unsigned long REQUIRED_IDLE_BEFORE_PAIR   = 500;
const unsigned long PAIR_LOCKOUT_AFTER_FIRE_MS  = 8000;

const unsigned long PAIRED_IDLE_FLASH_PERIOD_MS = 20000;
const unsigned long PAIRED_IDLE_FLASH_ON_MS     = 120;

const unsigned long FIRE_FLASH_MS               = 120;
const unsigned long PAIR_SUCCESS_FLASH_MS       = 180;

// =====================================
// ESP-NOW message types
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
// Global state
// =====================================
Preferences prefs;

uint8_t dispenserMAC[6];
bool paired = false;
bool pairingMode = false;

// Raw and debounced button tracking
bool rawButtonReading = HIGH;
bool lastRawButtonReading = HIGH;
bool debouncedButtonState = HIGH;
unsigned long rawLastChangeTime = 0;

// Press tracking
bool pressActive = false;
unsigned long pressStartTime = 0;
bool longPressTriggered = false;
unsigned long lastReleaseTime = 0;
unsigned long lastFireRequestTime = 0;
unsigned long lastPairSend = 0;
unsigned long lastIdlePulseTime = 0;

// =====================================
// LED modes
// =====================================
enum LedMode {
  LED_IDLE_UNPAIRED,
  LED_IDLE_PAIRED,
  LED_PAIRING,
  LED_PAIR_SUCCESS,
  LED_FIRE_FLASH
};

LedMode ledMode = LED_IDLE_UNPAIRED;
unsigned long ledTimer = 0;

// =====================================
// LED helper functions
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

void flashPairSuccessNow() {
  ledMode = LED_PAIR_SUCCESS;
  ledTimer = millis();
}

void setButtonColor(bool on) {
  if (!on) {
    ledOff();
    return;
  }

  switch (BUTTON_ID) {
    case 1: setLED(true,  false, false); break; // red
    case 2: setLED(true,  true,  false); break; // orange approx
    case 3: setLED(true,  true,  false); break; // yellow approx
    case 4: setLED(false, true,  false); break; // green
    case 5: setLED(false, false, true ); break; // blue
    case 6: setLED(true,  false, true ); break; // indigo approx
    case 7: setLED(true,  false, true ); break; // violet approx
    case 8: setLED(true,  true,  true ); break; // white fallback
    default:setLED(true,  true,  true ); break;
  }
}

void updateLED() {
  unsigned long now = millis();

  switch (ledMode) {
    case LED_IDLE_UNPAIRED:
      ledOff();
      break;

    case LED_IDLE_PAIRED:
      if ((now - lastIdlePulseTime) < PAIRED_IDLE_FLASH_ON_MS) {
        setButtonColor(true);
      } else {
        ledOff();
      }
      break;

    case LED_PAIRING:
      if ((now / 250) % 2 == 0) {
        setLED(false, false, true);
      } else {
        ledOff();
      }
      break;

    case LED_PAIR_SUCCESS:
      if ((now - ledTimer) < PAIR_SUCCESS_FLASH_MS) {
        setButtonColor(true);
      } else {
        ledMode = paired ? LED_IDLE_PAIRED : LED_IDLE_UNPAIRED;
        ledOff();
      }
      break;

    case LED_FIRE_FLASH:
      if ((now - ledTimer) < FIRE_FLASH_MS) {
        setLED(true, true, true);
      } else {
        ledMode = paired ? LED_IDLE_PAIRED : LED_IDLE_UNPAIRED;
        ledOff();
      }
      break;
  }
}

// =====================================
// Preferences storage
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
// ESP-NOW helper functions
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
    Serial.println("Button is not paired. Fire request blocked.");
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

  pressActive = false;
  longPressTriggered = false;
}

// =====================================
// ESP-NOW callbacks
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
    flashPairSuccessNow();
    lastIdlePulseTime = millis();

    Serial.println("Pairing successful.");
  }

  if (pkt.type == MSG_FIRE_ACK) {
    Serial.println("Fire acknowledged by dispenser.");
  }
}

// =====================================
// Button handling
// =====================================
void handleButton() {
  unsigned long now = millis();

  // INPUT_PULLUP means LOW = pressed
  rawButtonReading = digitalRead(BUTTON_PIN);

  // Track raw edges for debounce timing
  if (rawButtonReading != lastRawButtonReading) {
    rawLastChangeTime = now;
    lastRawButtonReading = rawButtonReading;
  }

  // Only accept a new stable state if the raw reading
  // has remained unchanged for the full debounce time
  if ((now - rawLastChangeTime) >= DEBOUNCE_MS) {
    if (debouncedButtonState != rawButtonReading) {
      debouncedButtonState = rawButtonReading;

      // Stable press detected
      if (debouncedButtonState == LOW) {
        pressActive = true;
        pressStartTime = now;
        longPressTriggered = false;

        Serial.print("BUTTON PRESSED at ms = ");
        Serial.println(now);
      }

      // Stable release detected
      else {
        if (pressActive) {
          unsigned long pressDuration = now - pressStartTime;

          Serial.print("BUTTON RELEASED at ms = ");
          Serial.println(now);
          Serial.print("Button was held for ms = ");
          Serial.println(pressDuration);

          if (paired) {
            if (!longPressTriggered && !pairingMode && pressDuration < LONG_PRESS_MS) {
              sendFireRequest();
            }
          } else {
            Serial.println("Button is not paired, so no fire request will be sent.");
          }
        } else {
          Serial.print("BUTTON RELEASED at ms = ");
          Serial.println(now);
          Serial.println("Release detected, but no active press was tracked.");
        }

        pressActive = false;
        longPressTriggered = false;
        lastReleaseTime = now;
      }
    }
  }

  // Long press detection while button remains stably pressed
  if (pressActive && debouncedButtonState == LOW && !longPressTriggered && !pairingMode) {
    bool hadCleanIdleBeforePress =
      (pressStartTime >= lastReleaseTime) &&
      ((pressStartTime - lastReleaseTime) >= REQUIRED_IDLE_BEFORE_PAIR);

    bool pairingLockoutActive =
      paired && ((now - lastFireRequestTime) < PAIR_LOCKOUT_AFTER_FIRE_MS);

    unsigned long heldTime = now - pressStartTime;

    if (!pairingLockoutActive && hadCleanIdleBeforePress && heldTime >= LONG_PRESS_MS) {
      longPressTriggered = true;
      pairingMode = true;
      ledMode = LED_PAIRING;

      Serial.println("Entered pairing mode.");
    }
  }
}

// =====================================
// Setup
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

  rawButtonReading = digitalRead(BUTTON_PIN);
  lastRawButtonReading = rawButtonReading;
  debouncedButtonState = rawButtonReading;
  rawLastChangeTime = millis();

  lastReleaseTime = millis();
  lastIdlePulseTime = millis();

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
  Serial.printf("Firmware: BUTTON_OFFICIAL_V1\n");
  Serial.printf("BUTTON_ID: %d\n", BUTTON_ID);
  Serial.printf("Paired: %s\n", paired ? "YES" : "NO");
  Serial.printf("Initial raw state: %d\n", rawButtonReading);
  Serial.printf("Initial debounced state: %d\n", debouncedButtonState);
}

// =====================================
// Main loop
// =====================================
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

  if (paired && !pairingMode) {
    if ((now - lastIdlePulseTime) >= PAIRED_IDLE_FLASH_PERIOD_MS) {
      lastIdlePulseTime = now;
    }
  }

  updateLED();
}