// CardDealer FSM clean version
// Commands: SETUP, HOME, MOVE, SERVO, SPIN, ENABLE, DISABLE, SWEEP, SHUFFLE, START*, DEAL*, EJECT*, PUSH, SEAT, BUTTON, PAIR, ABORT.
// Web API: GET /status, POST /command with JSON {"command":"EJECT"}.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <Preferences.h>
const char* AP_SSID = "CardDealerESP32";
const char* AP_PASS = "dealcards123";
const char* STA_SSID = "";
const char* STA_PASS = "";
WebServer server(80);
#define STEP_PIN_1   13
#define DIR_PIN_1    12
#define ENABLE_PIN_1 19
#define STEP_PIN_2   27
#define DIR_PIN_2    33
#define ENABLE_PIN_2 21
#define STEP_PIN_3   15
#define DIR_PIN_3    32
#define ENABLE_PIN_3 8
#define LIMIT_PIN_1   14
#define LIMIT_PIN_2   20
#define LIMIT_PIN_3   36
#define SERVO_PIN          7
#define SERVO_LEDC_FREQ   50
#define SERVO_LEDC_RES    16
#define SERVO_MIN_US     500
#define SERVO_MAX_US    2460
#define SERVO_TRAVEL_MS  1000
#define POL1_A   25
#define POL1_B   26
#define POL2_A    4
#define POL2_B    5
#define POLOLU_FREQ  5000
#define POLOLU_RES      8
#define POT_PIN              34
#define POT_ADC_MIN           0
#define POT_ADC_MAX        4095
#define POL1_MIN_REVERSE   -150
#define POL1_MAX_REVERSE   -255
#define HOMING_STEP_US    1600
#define HOMING_SLOW_US    1600
#define MOVE_STEP_US      1200
#define HOPPER_SHUFFLE_STEP_US  2400
#define MOVE_STEP_FAST_US  800
#define HOMING_BACKOFF     50
#define HOMING_MAX_STEPS  3000
#define BASE_HOME_FAST_STEP_US       BASE_CRUISE_STEP_US
#define BASE_HOME_RELEASE_STEP_US    2200
#define BASE_HOME_FINAL_STEP_US      2200
#define BASE_HOME_ZERO_CHECK_STEPS 100
#define BASE_HOME_POSITION_MARGIN  300
#define BASE_HOME_FIRST_HIT_DELAY_MS 250
#define BASE_HOME_NEAR_SWITCH_EXTRA_STEPS 25
#define BASE_CIRCLE_STEPS          1230
#define BASE_HALF_CIRCLE_STEPS      615
#define BASE_START_STEP_US      1600
#define BASE_CRUISE_STEP_US      1000
#define BASE_MIN_RAMP_STEPS       20
#define BASE_MAX_RAMP_STEPS      220
#define BASE_RAMP_DIVISOR          4
#define DRIVER_ENABLE   LOW
#define DRIVER_DISABLE  HIGH
#define BASE_HOLD_DELAY_MS 1000
#define ROUND_ROBIN_BASE_HOME_DELAY_MS 750
#define SHELF_COUNT          6
#define SHELF_RETURN       150
#define EJECT_HEIGHT        25
#define MAX_TOTAL_CARDS    432
#define MAX_CARDS_PER_SLOT  13
const long SHELF_STEPS[SHELF_COUNT] = { 372, 407, 460, 505, 547, 590 };
const int HOMING_DIR[4] = { 0, LOW, HIGH, HIGH };
#define MAX_BUTTONS 8
#define DEFAULT_BUTTON_EJECT_PERCENT 50
#define ESPNOW_EVENT_QUEUE_SIZE 8
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
Preferences prefs;
uint8_t pairedButtonMACs[MAX_BUTTONS + 1][6];
bool buttonPaired[MAX_BUTTONS + 1];
long stepperPos[4] = { 0, 0, 0, 0 };
int  servoAngle    = 0;
bool ejectReady    = false;
String inputBuffer = "";
String abortBuffer = "";
int motionDepth    = 0;
String lastCommand  = "";
String lastResponse = "Booting";
String commandReply = "";
bool webAbortRequested = false;
unsigned long lastWebServiceUs = 0;
volatile bool baseHomeSwitchLatched = false;

void IRAM_ATTR onBaseHomeSwitchFalling() {
  baseHomeSwitchLatched = true;
}
int currentPlayers    = 4;
int currentDeckCount  = 1;
int currentCardsPerDeck = 52;
int currentTotalCards = 52;
long ejectPositions[MAX_BUTTONS + 1]   = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
bool ejectPositionSet[MAX_BUTTONS + 1] = { false, false, false, false, false, false, false, false, false };
int buttonPololu2Percents[MAX_BUTTONS + 1] = { 0, 50, 50, 50, 50, 50, 50, 50, 50 };
int activeEjectButtonId = 0;
bool cambioStartupInProgress = false;
bool texasHoldemActive = false;
bool texasHoldemSequenceInProgress = false;
int texasHoldemStage = 0;
bool texasHoldemBaseParkedAtTable = false;
long texasHoldemTableTargetCached = 0;
bool pokerActive = false;
bool pokerSequenceInProgress = false;
int pokerDrawCounts[MAX_BUTTONS + 1] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
const int POKER_MAX_DRAWS_PER_PLAYER = 3;
bool blackjackActive = false;
bool blackjackSequenceInProgress = false;
bool roundRobinDealInProgress = false;
enum CardDealerState {
  STATE_IDLING,
  STATE_RECEIVING_COMMAND,
  STATE_SETTING_UP,
  STATE_HOMING,
  STATE_MANUAL_MOVING,
  STATE_SHUFFLING,
  STATE_GAME_STARTING,
  STATE_DEALING,
  STATE_EJECTING,
  STATE_PUSHING,
  STATE_CALIBRATING,
  STATE_PAIRING_SERVICE,
  STATE_ABORTING,
  STATE_ERROR_STATE
};
const char* stateName(CardDealerState state);
void transitionTo(CardDealerState nextState, const String& reason);
CardDealerState classifyCommandState(String cmd);
CardDealerState currentState = STATE_IDLING;
String pendingSerialCommand = "";
bool serialCommandEventFlag = false;
enum EspNowQueuedEventType : uint8_t {
  ESPNOW_EVENT_NONE = 0,
  ESPNOW_EVENT_PAIR = 1,
  ESPNOW_EVENT_FIRE = 2
};
struct EspNowQueuedEvent {
  uint8_t type;
  uint8_t buttonId;
  uint8_t mac[6];
};
EspNowQueuedEvent espNowEventQueue[ESPNOW_EVENT_QUEUE_SIZE];
volatile uint8_t espNowEventHead = 0;
volatile uint8_t espNowEventTail = 0;
volatile unsigned long espNowQueueOverflowCount = 0;

const char* stateName(CardDealerState state) {
  switch (state) {
    case STATE_IDLING: return "IDLING";
    case STATE_RECEIVING_COMMAND: return "RECEIVING_COMMAND";
    case STATE_SETTING_UP: return "SETTING_UP";
    case STATE_HOMING: return "HOMING";
    case STATE_MANUAL_MOVING: return "MANUAL_MOVING";
    case STATE_SHUFFLING: return "SHUFFLING";
    case STATE_GAME_STARTING: return "GAME_STARTING";
    case STATE_DEALING: return "DEALING";
    case STATE_EJECTING: return "EJECTING";
    case STATE_PUSHING: return "PUSHING";
    case STATE_CALIBRATING: return "CALIBRATING";
    case STATE_PAIRING_SERVICE: return "PAIRING_SERVICE";
    case STATE_ABORTING: return "ABORTING";
    case STATE_ERROR_STATE: return "ERROR_STATE";
    default: return "UNKNOWN";
  }
}

void transitionTo(CardDealerState nextState, const String& reason) {
  if (currentState == nextState) return;
  Serial.print("FSM: ");
  Serial.print(stateName(currentState));
  Serial.print(" -> ");
  Serial.print(stateName(nextState));
  if (reason.length() > 0) {
    Serial.print("  event/service: ");
    Serial.print(reason);
  }
  Serial.println();
  currentState = nextState;
}

void fsmServiceState(CardDealerState nextState, const String& reason) {
  transitionTo(nextState, reason);
}

bool commandSucceeded() {
  return !lastResponse.startsWith("ERROR");
}

CardDealerState classifyCommandState(String cmd) {
  cmd.trim();
  if (cmd == "ABORT") return STATE_ABORTING;
  if (cmd.startsWith("SETUP ")) return STATE_SETTING_UP;
  if (cmd.startsWith("HOME ") || cmd == "HOMEBASE" || cmd == "ZEROBASE") return STATE_HOMING;
  if (cmd.startsWith("MOVE ") || cmd.startsWith("SERVO ") || cmd.startsWith("SPIN ") || cmd.startsWith("ENABLE ") || cmd.startsWith("DISABLE ") || cmd == "SWEEP" || cmd == "MANUALZEROBASE") return STATE_MANUAL_MOVING;
  if (cmd == "SHUFFLE") return STATE_SHUFFLING;
  if (cmd.startsWith("START") || cmd == "TEXASHOLDEMADVANCE" || cmd == "ENDTEXASHOLDEM" || cmd == "ENDGAME") return STATE_GAME_STARTING;
  if (cmd.startsWith("DEAL") || cmd.startsWith("BUTTONDEAL ") || cmd.startsWith("POKERDRAW ") || cmd.startsWith("GOTOSEAT ")) return STATE_DEALING;
  if (cmd == "EJECT" || cmd.startsWith("EJECT ") || cmd.startsWith("EJECTPERCENT ")) return STATE_EJECTING;
  if (cmd == "PUSH") return STATE_PUSHING;
  if (cmd.startsWith("SETEJECTPOS ") || cmd == "CLEARSEATS" || cmd.startsWith("SETBUTTONSPEED ") || cmd.startsWith("TESTBUTTONSPEED ")) return STATE_CALIBRATING;
  if (cmd == "LISTPAIRS" || cmd == "CLEARPAIRS") return STATE_PAIRING_SERVICE;
  return STATE_RECEIVING_COMMAND;
}
void stopPololu(int n);
void disableMotor(int n);
void stopAllEjectHardware();
void prepareBaseMotorMove();
void holdThenDisableBaseMotor();
void delayWithAbort(unsigned long durationMs);
bool cmdHomeBaseToIdle(const String& reason);
void processCommand(String cmd);
void executeCommandNow(String cmd);
void serviceCommandEvent(String cmd, const String& source);
bool checkSerialCommandEvent();
void serviceSerialCommandEvent();
void runCardDealerStateMachine();
void serviceEspNowEventQueue();
bool enqueueEspNowEvent(uint8_t eventType, uint8_t buttonId, const uint8_t *mac);
bool dequeueEspNowEvent(EspNowQueuedEvent &event);
void processEspNowEvent(const EspNowQueuedEvent &event);
bool commandSucceeded();
void fsmServiceState(CardDealerState nextState, const String& reason);
void checkImmediateAbort();
void handleWebServerDuringMotion();
void saveSeatPositions();
void loadSeatPositions();
void clearSeatPositions();
void cmdGotoSeat(int buttonId);
void cmdDealToSeat(int buttonId);
void cmdButtonDeal(int buttonId);
void cmdEjectRoundRobinNoHome(int buttonId);
void cmdShuffle();
bool cmdDealRoundRobin(int cardsEach);
void cmdStartCambio();
void cmdStartTexasHoldem();
void cmdEndTexasHoldem();
void cmdTexasHoldemButtonAdvance();
void cmdStartPoker();
void cmdPokerButtonDraw(int buttonId);
void cmdStartBlackjack();
void cmdEndGame();
bool allPlayerSeatsSaved();
bool cmdMoveToHoldemTableIfNeeded();
void cmdDealToTablePercent(int percent);
long holdemTablePosition();
long normalizeBasePosition(long pos);
void updateBasePositionOneStep(int direction);
long baseDeltaToHome(long current);
long baseDeltaToTarget(long current, long target);
void cmdZeroBase();
bool homeStepperToLimit(int n);
int readPololu1PotSpeed();
bool seekBaseHomeDirection(int direction, long maxSteps, const String& label);
bool finishBaseHomeFromSwitchHit(int approachDirection);
int baseRampDelayUs(long stepIndex, long totalSteps);
void moveStepperWithBaseRamp(int n, long steps);
void moveStepperWithBaseRampAtSpeed(int n, long steps, int nonBaseStepDelayUs);
void moveStepperRampedAtSpeed(int n, long steps, int cruiseStepDelayUs);
void moveStepperFixedSpeed(int n, long steps, int stepDelayUs);

void setReply(const String& msg) {
  commandReply = msg;
  lastResponse = msg;
}

void printAndSetReply(const String& msg) {
  Serial.println(msg);
  setReply(msg);
}

bool motionActive() {
  return motionDepth > 0;
}

void beginMotion() {
  motionDepth++;
}

void endMotion() {
  if (motionDepth > 0) {
    motionDepth--;
  }
}

void restartNow() {
  stopPololu(1);
  stopPololu(2);
  disableMotor(1);
  disableMotor(2);
  disableMotor(3);
  lastResponse = "ABORT: restarting ESP...";
  Serial.println("ABORT: restarting ESP...");
  Serial.flush();
  delay(20);
  ESP.restart();
}

void handleWebServerDuringMotion() {
  unsigned long now = micros();
  if ((unsigned long)(now - lastWebServiceUs) >= 10000) {
    lastWebServiceUs = now;
    server.handleClient();
    if (webAbortRequested) {
      restartNow();
    }
  }
}

void checkImmediateAbort() {
  if (!motionActive()) return;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      abortBuffer.trim();
      if (abortBuffer == "ABORT") {
        restartNow();
      }
      abortBuffer = "";
    } else {
      abortBuffer += c;
      if (abortBuffer.length() > 16) {
        abortBuffer.remove(0, abortBuffer.length() - 16);
      }
    }
  }
  handleWebServerDuringMotion();
}

String jsonEscape(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;      break;
    }
  }
  return out;
}

int countPairedButtons() {
  int count = 0;
  for (int i = 1; i <= MAX_BUTTONS; i++) {
    if (buttonPaired[i]) count++;
  }
  return count;
}

int countSetEjectPositions() {
  int count = 0;
  for (int i = 1; i <= MAX_BUTTONS; i++) {
    if (ejectPositionSet[i]) count++;
  }
  return count;
}

String macToString(const uint8_t *mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

void printMac(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 16) Serial.print("0");
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(":");
  }
}

bool macEquals(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, 6) == 0;
}

bool macIsZero(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) {
    if (mac[i] != 0) return false;
  }
  return true;
}

void clearAllPairedButtons() {
  for (int i = 0; i <= MAX_BUTTONS; i++) {
    buttonPaired[i] = false;
    memset(pairedButtonMACs[i], 0, 6);
  }
}

void savePairedButtons() {
  prefs.begin("btnpairs", false);
  prefs.putBytes("macs", pairedButtonMACs, sizeof(pairedButtonMACs));
  prefs.putBytes("flags", buttonPaired, sizeof(buttonPaired));
  prefs.end();
}

void loadPairedButtons() {
  clearAllPairedButtons();
  prefs.begin("btnpairs", true);
  size_t macLen  = prefs.getBytes("macs", pairedButtonMACs, sizeof(pairedButtonMACs));
  size_t flagLen = prefs.getBytes("flags", buttonPaired, sizeof(buttonPaired));
  prefs.end();
  if (macLen != sizeof(pairedButtonMACs) || flagLen != sizeof(buttonPaired)) {
    clearAllPairedButtons();
    return;
  }
  for (int i = 1; i <= MAX_BUTTONS; i++) {
    if (macIsZero(pairedButtonMACs[i])) {
      buttonPaired[i] = false;
    }
  }
}

void saveSeatPositions() {
  prefs.begin("seatpos", false);
  prefs.putBytes("pos", ejectPositions, sizeof(ejectPositions));
  prefs.putBytes("set", ejectPositionSet, sizeof(ejectPositionSet));
  prefs.putBytes("p2spd", buttonPololu2Percents, sizeof(buttonPololu2Percents));
  prefs.end();
}

void loadSeatPositions() {
  for (int i = 0; i <= MAX_BUTTONS; i++) {
    ejectPositions[i] = 0;
    ejectPositionSet[i] = false;
    buttonPololu2Percents[i] = DEFAULT_BUTTON_EJECT_PERCENT;
  }
  buttonPololu2Percents[0] = 0;
  prefs.begin("seatpos", true);
  size_t posLen = prefs.getBytes("pos", ejectPositions, sizeof(ejectPositions));
  size_t setLen = prefs.getBytes("set", ejectPositionSet, sizeof(ejectPositionSet));
  size_t spdLen = prefs.getBytes("p2spd", buttonPololu2Percents, sizeof(buttonPololu2Percents));
  prefs.end();
  if (posLen != sizeof(ejectPositions) || setLen != sizeof(ejectPositionSet)) {
    for (int i = 0; i <= MAX_BUTTONS; i++) {
      ejectPositions[i] = 0;
      ejectPositionSet[i] = false;
    }
  }
  if (spdLen != sizeof(buttonPololu2Percents)) {
    for (int i = 1; i <= MAX_BUTTONS; i++) {
      buttonPololu2Percents[i] = DEFAULT_BUTTON_EJECT_PERCENT;
    }
    buttonPololu2Percents[0] = 0;
  }
}

void clearSeatPositions() {
  for (int i = 0; i <= MAX_BUTTONS; i++) {
    ejectPositions[i] = 0;
    ejectPositionSet[i] = false;
    buttonPololu2Percents[i] = DEFAULT_BUTTON_EJECT_PERCENT;
  }
  buttonPololu2Percents[0] = 0;
  saveSeatPositions();
}

int pololu2PercentToSpeed(int percent) {
  percent = constrain(percent, 0, 100);
  return map(percent, 0, 100, -150, -255);
}

int readPololu1PotSpeed() {
  int raw = analogRead(POT_PIN);
  raw = constrain(raw, POT_ADC_MIN, POT_ADC_MAX);
  return map(raw, POT_ADC_MIN, POT_ADC_MAX, POL1_MIN_REVERSE, POL1_MAX_REVERSE);
}

uint32_t angleToDuty(int angle) {
  int pulseUs = map(angle, 0, 180, SERVO_MIN_US, SERVO_MAX_US);
  return (uint32_t)((float)pulseUs / 20000.0f * 65536.0f);
}
int stepPinFor(int n)   { return n == 1 ? STEP_PIN_1   : n == 2 ? STEP_PIN_2   : STEP_PIN_3; }
int dirPinFor(int n)    { return n == 1 ? DIR_PIN_1    : n == 2 ? DIR_PIN_2    : DIR_PIN_3; }
int limitPinFor(int n)  { return n == 1 ? LIMIT_PIN_1  : n == 2 ? LIMIT_PIN_2  : LIMIT_PIN_3; }
int enablePinFor(int n) { return n == 1 ? ENABLE_PIN_1 : n == 2 ? ENABLE_PIN_2 : ENABLE_PIN_3; }
int pololuA(int n) { return n == 1 ? POL1_A : POL2_A; }
int pololuB(int n) { return n == 1 ? POL1_B : POL2_B; }

void enableMotor(int n) {
  checkImmediateAbort();
  if (n == 3) {
    stopAllEjectHardware();
    if (!roundRobinDealInProgress) {
      disableMotor(1);
    }
    disableMotor(2);
  }
  digitalWrite(enablePinFor(n), DRIVER_ENABLE);
  delayMicroseconds(200);
  checkImmediateAbort();
}

void disableMotor(int n) {
  digitalWrite(enablePinFor(n), DRIVER_DISABLE);
  if (n == 1) {
    ejectReady = false;
  }
}

void stopAllEjectHardware() {
  stopPololu(1);
  stopPololu(2);
}

void prepareBaseMotorMove() {
  stopAllEjectHardware();
  if (!roundRobinDealInProgress) {
    disableMotor(1);
  }
  disableMotor(2);
  enableMotor(3);
}

void delayWithAbort(unsigned long durationMs) {
  unsigned long start = millis();
  while ((millis() - start) < durationMs) {
    checkImmediateAbort();
    delay(5);
  }
}

void holdThenDisableBaseMotor() {
  delayWithAbort(BASE_HOLD_DELAY_MS);
  disableMotor(3);
}

void stepOnce(int n, int stepDelayUs) {
  checkImmediateAbort();
  digitalWrite(stepPinFor(n), HIGH);
  delayMicroseconds(stepDelayUs);
  digitalWrite(stepPinFor(n), LOW);
  delayMicroseconds(100);
}

long baseAdaptiveRampSteps(long totalSteps) {
  if (totalSteps <= 1) return 0;
  long ramp = totalSteps / BASE_RAMP_DIVISOR;
  if (totalSteps >= (BASE_MIN_RAMP_STEPS * 2)) {
    ramp = max(ramp, (long)BASE_MIN_RAMP_STEPS);
  }
  ramp = min(ramp, (long)BASE_MAX_RAMP_STEPS);
  ramp = min(ramp, totalSteps / 2);
  return max(ramp, 1L);
}

int baseRampDelayUs(long stepIndex, long totalSteps) {
  if (totalSteps <= 1) return BASE_START_STEP_US;
  long rampSteps = baseAdaptiveRampSteps(totalSteps);
  if (rampSteps <= 0) return BASE_CRUISE_STEP_US;
  long distanceFromEnd = totalSteps - 1 - stepIndex;
  long rampZoneIndex = min(stepIndex, distanceFromEnd);
  if (rampZoneIndex >= rampSteps) {
    return BASE_CRUISE_STEP_US;
  }
  float progress = (float)(rampZoneIndex + 1) / (float)(rampSteps + 1);
  float smooth = progress * progress * (3.0f - 2.0f * progress);
  float delayUs = (float)BASE_START_STEP_US - smooth * (float)(BASE_START_STEP_US - BASE_CRUISE_STEP_US);
  return (int)delayUs;
}

int genericRampDelayUs(long stepIndex, long totalSteps, int cruiseStepDelayUs) {
  if (totalSteps <= 1) return cruiseStepDelayUs;
  long rampSteps = min(25L, totalSteps / 2);
  if (rampSteps < 1) rampSteps = 1;
  long distanceFromEnd = totalSteps - 1 - stepIndex;
  long rampZoneIndex = min(stepIndex, distanceFromEnd);
  if (rampZoneIndex >= rampSteps) {
    return cruiseStepDelayUs;
  }
  int startDelayUs = max(cruiseStepDelayUs + 800, cruiseStepDelayUs * 2);
  float progress = (float)(rampZoneIndex + 1) / (float)(rampSteps + 1);
  float smooth = progress * progress * (3.0f - 2.0f * progress);
  float delayUs = (float)startDelayUs - smooth * (float)(startDelayUs - cruiseStepDelayUs);
  return (int)delayUs;
}

void moveStepperWithBaseRamp(int n, long steps) {
  moveStepperWithBaseRampAtSpeed(n, steps, MOVE_STEP_US);
}

void moveStepperWithBaseRampAtSpeed(int n, long steps, int nonBaseStepDelayUs) {
  if (steps == 0) return;
  int direction = steps > 0 ? HIGH : LOW;
  digitalWrite(dirPinFor(n), direction);
  delayMicroseconds(10);
  long count = labs(steps);
  for (long i = 0; i < count; i++) {
    int delayUs = (n == 3) ? baseRampDelayUs(i, count) : nonBaseStepDelayUs;
    stepOnce(n, delayUs);
    if (n == 3) {
      updateBasePositionOneStep(direction);
    } else {
      stepperPos[n] += (steps > 0) ? 1 : -1;
    }
  }
}

void moveStepperRampedAtSpeed(int n, long steps, int cruiseStepDelayUs) {
  if (steps == 0) return;
  int direction = steps > 0 ? HIGH : LOW;
  digitalWrite(dirPinFor(n), direction);
  delayMicroseconds(10);
  long count = labs(steps);
  for (long i = 0; i < count; i++) {
    int delayUs = genericRampDelayUs(i, count, cruiseStepDelayUs);
    stepOnce(n, delayUs);
    if (n == 3) {
      updateBasePositionOneStep(direction);
    } else {
      stepperPos[n] += (steps > 0) ? 1 : -1;
    }
  }
}

void moveStepperFixedSpeed(int n, long steps, int stepDelayUs) {
  if (steps == 0) return;
  int direction = steps > 0 ? HIGH : LOW;
  digitalWrite(dirPinFor(n), direction);
  delayMicroseconds(10);
  long count = labs(steps);
  for (long i = 0; i < count; i++) {
    stepOnce(n, stepDelayUs);
    if (n == 3) {
      updateBasePositionOneStep(direction);
    } else {
      stepperPos[n] += (steps > 0) ? 1 : -1;
    }
  }
}

void setPololu(int n, int speed) {
  checkImmediateAbort();
  if (n == 1 && speed < 0) {
    int raw = analogRead(POT_PIN);
    raw = constrain(raw, POT_ADC_MIN, POT_ADC_MAX);
    speed = map(raw, POT_ADC_MIN, POT_ADC_MAX, POL1_MIN_REVERSE, POL1_MAX_REVERSE);
    Serial.print("Pololu 1 pot raw = ");
    Serial.print(raw);
    Serial.print(" mapped speed = ");
    Serial.println(speed);
  }
  if (speed >= 0) {
    ledcWrite(pololuA(n), (uint8_t)speed);
    ledcWrite(pololuB(n), 0);
  } else {
    ledcWrite(pololuA(n), 0);
    ledcWrite(pololuB(n), (uint8_t)(-speed));
  }
  checkImmediateAbort();
}

void stopPololu(int n) {
  ledcWrite(pololuA(n), 0);
  ledcWrite(pololuB(n), 0);
}

void moveServo(int angle, int currentAngle) {
  checkImmediateAbort();
  ledcWrite(SERVO_PIN, angleToDuty(angle));
  int distance = abs(angle - currentAngle);
  int waitMs = max(50, (int)(SERVO_TRAVEL_MS * distance / 180.0f));
  unsigned long start = millis();
  while ((millis() - start) < (unsigned long)waitMs) {
    checkImmediateAbort();
    delay(5);
  }
}

void sweepServo(int startAngle, int endAngle) {
  checkImmediateAbort();
  ledcWrite(SERVO_PIN, angleToDuty(endAngle));
  int distance = abs(endAngle - startAngle);
  int waitMs = max(50, (int)(SERVO_TRAVEL_MS * distance / 180.0f));
  unsigned long start = millis();
  while ((millis() - start) < (unsigned long)waitMs) {
    checkImmediateAbort();
    delay(5);
  }
}

void cmdSetup(int players, int decks, int cardsPerDeck) {
  if (players < 1 || players > 8) {
    printAndSetReply("ERROR: players must be 1..8");
    return;
  }
  if (decks < 1 || decks > 8) {
    printAndSetReply("ERROR: decks must be 1..8");
    return;
  }
  if (cardsPerDeck < 1 || cardsPerDeck > MAX_TOTAL_CARDS) {
    printAndSetReply("ERROR: cardsPerDeck must be between 1 and 432");
    return;
  }
  currentPlayers      = players;
  currentDeckCount    = decks;
  currentCardsPerDeck = cardsPerDeck;
  currentTotalCards   = currentDeckCount * currentCardsPerDeck;
  String msg = "OK: setup updated | players=" + String(currentPlayers) +
               " decks=" + String(currentDeckCount) +
               " cardsPerDeck=" + String(currentCardsPerDeck) +
               " totalCards=" + String(currentTotalCards);
  printAndSetReply(msg);
}

void cmdMove(int n, long steps) {
  fsmServiceState(STATE_MANUAL_MOVING, "manual move command");
  if (n < 1 || n > 3) {
    printAndSetReply("ERROR: motor index must be 1-3");
    return;
  }
  if (steps == 0) {
    printAndSetReply("OK");
    return;
  }
  beginMotion();
  moveStepperWithBaseRamp(n, steps);
  endMotion();
  printAndSetReply("OK");
}

bool seekBaseHomeDirection(int direction, long maxSteps, const String& label) {
  int pin = LIMIT_PIN_3;
  Serial.print("Base homing: ");
  Serial.print(label);
  Serial.print(" direction=");
  Serial.print(direction == HIGH ? "HIGH/positive" : "LOW/negative");
  Serial.print(" maxSteps=");
  Serial.println(maxSteps);
  baseHomeSwitchLatched = false;
  digitalWrite(DIR_PIN_3, direction);
  delayMicroseconds(10);
  long steps = 0;
  while (steps < maxSteps) {
    checkImmediateAbort();
    if (digitalRead(pin) == LOW || baseHomeSwitchLatched) {
      Serial.print("Base homing: switch event detected before step count=");
      Serial.println(steps);
      return true;
    }
    stepOnce(3, BASE_HOME_FAST_STEP_US);
    updateBasePositionOneStep(direction);
    steps++;
    if (digitalRead(pin) == LOW || baseHomeSwitchLatched) {
      Serial.print("Base homing: switch event detected after steps=");
      Serial.println(steps);
      return true;
    }
  }
  Serial.print("Base homing: no switch event found during ");
  Serial.println(label);
  return false;
}

bool finishBaseHomeFromSwitchHit(int approachDirection) {
  int pin = LIMIT_PIN_3;
  int releaseDirection = approachDirection;
  int finalApproachDirection = (approachDirection == HIGH) ? LOW : HIGH;
  Serial.print("Base homing: first switch hit, pausing ");
  Serial.print(BASE_HOME_FIRST_HIT_DELAY_MS);
  Serial.println(" ms before looking for the second edge...");
  delay(BASE_HOME_FIRST_HIT_DELAY_MS);
  Serial.println("Base homing: switch found, moving through until released...");
  digitalWrite(DIR_PIN_3, releaseDirection);
  delayMicroseconds(10);
  long releaseSteps = 0;
  while (digitalRead(pin) == LOW && releaseSteps < HOMING_MAX_STEPS) {
    stepOnce(3, BASE_HOME_RELEASE_STEP_US);
    updateBasePositionOneStep(releaseDirection);
    releaseSteps++;
  }
  if (digitalRead(pin) == LOW) {
    printAndSetReply("ERROR: base home switch did not release after being found");
    return false;
  }
  Serial.println("Base homing: final slow approach to home edge...");
  digitalWrite(DIR_PIN_3, finalApproachDirection);
  delayMicroseconds(10);
  long finalSteps = 0;
  while (digitalRead(pin) == HIGH && finalSteps < HOMING_MAX_STEPS) {
    stepOnce(3, BASE_HOME_FINAL_STEP_US);
    updateBasePositionOneStep(finalApproachDirection);
    finalSteps++;
  }
  if (digitalRead(pin) == HIGH) {
    printAndSetReply("ERROR: base home switch not found during final slow approach");
    return false;
  }
  stepperPos[3] = 0;
  Serial.println("Base homing: Motor 3 position set to 0.");
  return true;
}

bool homeStepperToLimit(int n) {
  int towardLimit   = HOMING_DIR[n];
  int awayFromLimit = (towardLimit == HIGH) ? LOW : HIGH;
  Serial.print("Homing motor ");
  Serial.print(n);
  Serial.println(": using active-LOW limit switch...");
  if (n == 3) {
    int pin = LIMIT_PIN_3;
    if (digitalRead(pin) == LOW) {
      Serial.println("Base homing: switch already pressed at start. Moving through switch zone...");
      baseHomeSwitchLatched = false;
      digitalWrite(DIR_PIN_3, HIGH);
      delayMicroseconds(10);
      long releaseSteps = 0;
      while (digitalRead(pin) == LOW && releaseSteps < HOMING_MAX_STEPS) {
        checkImmediateAbort();
        stepOnce(3, BASE_HOME_RELEASE_STEP_US);
        updateBasePositionOneStep(HIGH);
        releaseSteps++;
      }
      if (digitalRead(pin) == LOW) {
        printAndSetReply("ERROR: base home switch did not release at start");
        return false;
      }
      for (long i = 0; i < BASE_HOME_NEAR_SWITCH_EXTRA_STEPS; i++) {
        checkImmediateAbort();
        stepOnce(3, BASE_HOME_RELEASE_STEP_US);
        updateBasePositionOneStep(HIGH);
      }
      Serial.print("Base homing: switch cleared after release steps=");
      Serial.println(releaseSteps);
      return finishBaseHomeFromSwitchHit(HIGH);
    }
    long currentBasePosition = normalizeBasePosition(stepperPos[3]);
    long shortestHomeDelta = baseDeltaToHome(currentBasePosition);
    int shortestHomeDirection = (shortestHomeDelta >= 0) ? HIGH : LOW;
    long expectedStepsToHome = labs(shortestHomeDelta);
    long shortSearchSteps = expectedStepsToHome + BASE_HOME_POSITION_MARGIN;
    if (expectedStepsToHome == 0) {
      shortSearchSteps = BASE_HOME_ZERO_CHECK_STEPS;
    }
    shortSearchSteps = constrain(shortSearchSteps, 1L, (long)HOMING_MAX_STEPS);
    Serial.print("Base homing: current position estimate=");
    Serial.print(currentBasePosition);
    Serial.print(" shortest delta to home=");
    Serial.print(shortestHomeDelta);
    Serial.print(" search steps=");
    Serial.println(shortSearchSteps);
    if (seekBaseHomeDirection(shortestHomeDirection, shortSearchSteps, "shortest-path home search")) {
      return finishBaseHomeFromSwitchHit(shortestHomeDirection);
    }
    int recoveryDirection = (shortestHomeDirection == HIGH) ? LOW : HIGH;
    long fullRecoverySteps = BASE_CIRCLE_STEPS + BASE_HOME_POSITION_MARGIN + 150;
    fullRecoverySteps = constrain(fullRecoverySteps, 1L, (long)HOMING_MAX_STEPS);
    Serial.println("Base homing: shortest-path search missed switch, trying full recovery opposite direction...");
    if (seekBaseHomeDirection(recoveryDirection, fullRecoverySteps, "opposite-direction recovery search")) {
      return finishBaseHomeFromSwitchHit(recoveryDirection);
    }
    Serial.println("Base homing: opposite recovery missed switch, trying original direction full recovery...");
    if (seekBaseHomeDirection(shortestHomeDirection, fullRecoverySteps, "original-direction full recovery search")) {
      return finishBaseHomeFromSwitchHit(shortestHomeDirection);
    }
    printAndSetReply("ERROR: base home switch not found during shortest-path or recovery homing search");
    return false;
  }
  if (digitalRead(limitPinFor(n)) == LOW) {
    Serial.println("Homing: switch already pressed, backing off until released...");
    digitalWrite(dirPinFor(n), awayFromLimit);
    delayMicroseconds(10);
    long releaseSteps = 0;
    while (digitalRead(limitPinFor(n)) == LOW && releaseSteps < HOMING_MAX_STEPS) {
      stepOnce(n, HOMING_STEP_US);
      releaseSteps++;
    }
    if (digitalRead(limitPinFor(n)) == LOW) {
      printAndSetReply("ERROR: home switch did not release for motor " + String(n));
      return false;
    }
  }
  Serial.println("Homing: moving to limit...");
  digitalWrite(dirPinFor(n), towardLimit);
  delayMicroseconds(10);
  long seekSteps = 0;
  while (digitalRead(limitPinFor(n)) == HIGH && seekSteps < HOMING_MAX_STEPS) {
    stepOnce(n, HOMING_STEP_US);
    seekSteps++;
  }
  if (digitalRead(limitPinFor(n)) == HIGH) {
    printAndSetReply("ERROR: home switch not found for motor " + String(n));
    return false;
  }
  Serial.println("Homing: backing off...");
  digitalWrite(dirPinFor(n), awayFromLimit);
  delayMicroseconds(10);
  for (int i = 0; i < HOMING_BACKOFF; i++) {
    stepOnce(n, HOMING_STEP_US);
  }
  Serial.println("Homing: re-zeroing slowly...");
  digitalWrite(dirPinFor(n), towardLimit);
  delayMicroseconds(10);
  long rezeroSteps = 0;
  while (digitalRead(limitPinFor(n)) == HIGH && rezeroSteps < HOMING_MAX_STEPS) {
    stepOnce(n, HOMING_SLOW_US);
    rezeroSteps++;
  }
  if (digitalRead(limitPinFor(n)) == HIGH) {
    printAndSetReply("ERROR: home switch not found during slow re-zero for motor " + String(n));
    return false;
  }
  stepperPos[n] = 0;
  if (n == 1) ejectReady = false;
  Serial.print("Homing motor ");
  Serial.print(n);
  Serial.println(": position set to 0.");
  return true;
}

void cmdHome(int n) {
  fsmServiceState(STATE_HOMING, "home command");
  if (n < 1 || n > 3) {
    printAndSetReply("ERROR: motor index must be 1-3");
    return;
  }
  beginMotion();
  if (n == 3) {
    prepareBaseMotorMove();
  } else {
    enableMotor(n);
  }
  bool ok = homeStepperToLimit(n);
  if (n == 3) {
    holdThenDisableBaseMotor();
  }
  endMotion();
  if (ok) printAndSetReply("OK");
}

bool cmdHomeBaseToIdle(const String& reason) {
  Serial.print("Base idle home: ");
  Serial.println(reason);
  beginMotion();
  prepareBaseMotorMove();
  bool ok = homeStepperToLimit(3);
  holdThenDisableBaseMotor();
  endMotion();
  if (ok) {
    activeEjectButtonId = 0;
    texasHoldemBaseParkedAtTable = false;
    printAndSetReply("OK: base re-homed to idle");
  }
  return ok;
}

void cmdServo(int angle) {
  if (angle < 0 || angle > 180) {
    printAndSetReply("ERROR: angle must be 0..180");
    return;
  }
  beginMotion();
  moveServo(angle, servoAngle);
  servoAngle = angle;
  endMotion();
  printAndSetReply("OK");
}

void cmdSpin(int n, int speed, unsigned long durationMs) {
  if (n < 1 || n > 2) {
    printAndSetReply("ERROR: pololu index must be 1 or 2");
    return;
  }
  if (speed > 255 || speed < -255) {
    printAndSetReply("ERROR: speed must be -255..255");
    return;
  }
  beginMotion();
  setPololu(n, speed);
  unsigned long start = millis();
  while ((millis() - start) < durationMs) {
    checkImmediateAbort();
    delay(5);
  }
  stopPololu(n);
  endMotion();
  printAndSetReply("OK");
}

void cmdEnable(int n) {
  if (n < 1 || n > 3) {
    printAndSetReply("ERROR: motor index must be 1-3");
    return;
  }
  enableMotor(n);
  printAndSetReply("OK");
}

void cmdDisable(int n) {
  if (n < 1 || n > 3) {
    printAndSetReply("ERROR: motor index must be 1-3");
    return;
  }
  disableMotor(n);
  printAndSetReply("OK");
}

void cmdSweep() {
  fsmServiceState(STATE_MANUAL_MOVING, "servo sweep service");
  beginMotion();
  Serial.println("Sweep: starting pololu 1...");
  setPololu(1, -200);
  Serial.println("Sweep: servo 0 -> 180...");
  sweepServo(0, 180);
  Serial.println("Sweep: stopping pololu 1...");
  stopPololu(1);
  Serial.println("Sweep: servo 180 -> 0...");
  sweepServo(180, 0);
  servoAngle = 0;
  endMotion();
  printAndSetReply("OK");
}

void cmdEjectWithPololu2Percent(int pololu2Percent) {
  pololu2Percent = constrain(pololu2Percent, 0, 100);
  beginMotion();
  fsmServiceState(STATE_HOMING, "eject sequence: home Motor 1");
  Serial.println("Eject: enabling motor 1...");
  enableMotor(1);
  Serial.println("Eject: homing motor 1...");
  cmdHome(1);
  if (!commandSucceeded()) {
    stopAllEjectHardware();
    disableMotor(1);
    endMotion();
    return;
  }
  fsmServiceState(STATE_MANUAL_MOVING, "eject sequence: move Motor 1 to eject height");
  Serial.println("Eject: moving motor 1 to eject height...");
  cmdMove(1, EJECT_HEIGHT);
  if (!commandSucceeded()) {
    stopAllEjectHardware();
    disableMotor(1);
    endMotion();
    return;
  }
  ejectReady = true;
  fsmServiceState(STATE_EJECTING, "eject sequence: fire card");
  Serial.println("Eject: motor 1 is at eject height, ejecting card...");
  int pololu2Speed = pololu2PercentToSpeed(pololu2Percent);
  Serial.print("Eject: Pololu 2 percent = ");
  Serial.print(pololu2Percent);
  Serial.print(" speed = ");
  Serial.println(pololu2Speed);
  setPololu(1, -200);
  setPololu(2, pololu2Speed);
  sweepServo(0, 180);
  stopPololu(1);
  stopPololu(2);
  sweepServo(180, 0);
  servoAngle = 0;
  endMotion();
  printAndSetReply("OK");
}

void cmdEject(int buttonId = 0) {
  if ((buttonId < 1 || buttonId > MAX_BUTTONS) && activeEjectButtonId >= 1 && activeEjectButtonId <= MAX_BUTTONS) {
    buttonId = activeEjectButtonId;
  }
  int pololu2Percent = DEFAULT_BUTTON_EJECT_PERCENT;
  if (buttonId >= 1 && buttonId <= MAX_BUTTONS) {
    pololu2Percent = constrain(buttonPololu2Percents[buttonId], 0, 100);
  }
  cmdEjectWithPololu2Percent(pololu2Percent);
}

void cmdEjectRoundRobinNoHome(int buttonId) {
  if (buttonId < 1 || buttonId > MAX_BUTTONS) {
    printAndSetReply("ERROR: button must be 1..8");
    return;
  }
  int pololu2Percent = constrain(buttonPololu2Percents[buttonId], 0, 100);
  int pololu2Speed = pololu2PercentToSpeed(pololu2Percent);
  beginMotion();
  fsmServiceState(STATE_EJECTING, "round-robin eject service");
  enableMotor(1);
  ejectReady = true;
  Serial.print("Round-robin eject: Player ");
  Serial.print(buttonId);
  Serial.print(" | Pololu 2 percent = ");
  Serial.print(pololu2Percent);
  Serial.print(" speed = ");
  Serial.println(pololu2Speed);
  setPololu(1, -200);
  setPololu(2, pololu2Speed);
  sweepServo(0, 180);
  stopPololu(1);
  stopPololu(2);
  sweepServo(180, 0);
  servoAngle = 0;
  endMotion();
  printAndSetReply("OK");
}

void cmdPush() {
  beginMotion();
  fsmServiceState(STATE_PUSHING, "push sequence started");
  Serial.println("Push: enabling motor 2...");
  enableMotor(2);
  fsmServiceState(STATE_HOMING, "push sequence: home Motor 2");
  cmdHome(2);
  if (!commandSucceeded()) {
    disableMotor(2);
    endMotion();
    return;
  }
  fsmServiceState(STATE_PUSHING, "push sequence: extend pusher with ramp");
  Serial.print("Push: ramped MOVE 2 -125 at cruise ");
  Serial.print(MOVE_STEP_FAST_US);
  Serial.println(" us/step...");
  moveStepperRampedAtSpeed(2, -125, MOVE_STEP_FAST_US);
  delayWithAbort(1000);
  Serial.print("Push: ramped MOVE 2 125 at cruise ");
  Serial.print(MOVE_STEP_FAST_US);
  Serial.println(" us/step...");
  moveStepperRampedAtSpeed(2, 125, MOVE_STEP_FAST_US);
  Serial.println("Push: disabling motor 2...");
  disableMotor(2);
  endMotion();
  printAndSetReply("OK");
}

void cmdSetEjectPos(int buttonId) {
  if (buttonId < 1 || buttonId > MAX_BUTTONS) {
    printAndSetReply("ERROR: button must be 1..8");
    return;
  }
  long savedPosition = normalizeBasePosition(stepperPos[3]);
  ejectPositions[buttonId] = savedPosition;
  ejectPositionSet[buttonId] = true;
  saveSeatPositions();
  Serial.print("Calibration: saved player ");
  Serial.print(buttonId);
  Serial.print(" at home-referenced position ");
  Serial.println(savedPosition);
  beginMotion();
  prepareBaseMotorMove();
  Serial.println("Calibration: re-homing Motor 3 after saving seat...");
  bool homeOk = homeStepperToLimit(3);
  holdThenDisableBaseMotor();
  endMotion();
  if (!homeOk) {
    return;
  }
  printAndSetReply(
    "OK: button " + String(buttonId) +
    " seat position set to " + String(savedPosition) +
    " | Motor 3 re-homed for next calibration"
  );
}

void cmdGotoSeat(int buttonId) {
  if (buttonId < 1 || buttonId > MAX_BUTTONS) {
    printAndSetReply("ERROR: button must be 1..8");
    return;
  }
  if (!ejectPositionSet[buttonId]) {
    printAndSetReply("ERROR: no saved seat position for button " + String(buttonId));
    return;
  }
  long target = ejectPositions[buttonId];
  Serial.print("Base goto seat: button ");
  Serial.print(buttonId);
  Serial.print(" saved target from home=");
  Serial.println(target);
  beginMotion();
  fsmServiceState(STATE_HOMING, "goto seat: home Motor 3 first");
  prepareBaseMotorMove();
  Serial.println("Base goto seat: homing motor 3 first...");
  if (!homeStepperToLimit(3)) {
    holdThenDisableBaseMotor();
    endMotion();
    return;
  }
  if (roundRobinDealInProgress && ROUND_ROBIN_BASE_HOME_DELAY_MS > 0) {
    Serial.print("Round-robin: base homed, pausing before rotating to player for ms=");
    Serial.println(ROUND_ROBIN_BASE_HOME_DELAY_MS);
    delayWithAbort(ROUND_ROBIN_BASE_HOME_DELAY_MS);
  }
  long delta = baseDeltaToTarget(stepperPos[3], target);
  Serial.print("Base goto seat: moving from home to target, delta=");
  Serial.println(delta);
  if (delta != 0) {
    fsmServiceState(STATE_DEALING, "goto seat: rotate base to saved player position");
    moveStepperWithBaseRamp(3, delta);
  }
  stepperPos[3] = normalizeBasePosition(target);
  holdThenDisableBaseMotor();
  activeEjectButtonId = buttonId;
  texasHoldemBaseParkedAtTable = false;
  endMotion();
  printAndSetReply("OK: at seat " + String(buttonId));
}

void cmdDealToSeat(int buttonId) {
  if (buttonId < 1 || buttonId > MAX_BUTTONS) {
    printAndSetReply("ERROR: button must be 1..8");
    return;
  }
  if (!ejectPositionSet[buttonId]) {
    printAndSetReply("ERROR: no saved seat position for button " + String(buttonId));
    return;
  }
  fsmServiceState(STATE_DEALING, "deal to seat: move base to player");
  cmdGotoSeat(buttonId);
  if (!commandSucceeded()) return;
  fsmServiceState(STATE_EJECTING, "deal to seat: eject card");
  cmdEject(buttonId);
}

void cmdButtonDeal(int buttonId) {
  if (buttonId < 1 || buttonId > MAX_BUTTONS) {
    printAndSetReply("ERROR: button must be 1..8");
    return;
  }
  if (!ejectPositionSet[buttonId]) {
    printAndSetReply("ERROR: no saved seat position for button " + String(buttonId));
    return;
  }
  fsmServiceState(STATE_DEALING, "button deal: move to requested player");
  cmdGotoSeat(buttonId);
  if (!commandSucceeded()) return;
  fsmServiceState(STATE_EJECTING, "button deal: eject requested card");
  cmdEject(buttonId);
  if (!commandSucceeded()) return;
  fsmServiceState(STATE_HOMING, "button deal: return base to idle home");
  cmdHomeBaseToIdle("after normal button deal");
}

void cmdGotoBaseTarget(long target, const String &label) {
  Serial.print("Base move to ");
  Serial.print(label);
  Serial.print(" saved target from home=");
  Serial.println(target);
  beginMotion();
  fsmServiceState(STATE_HOMING, "base target move: home Motor 3 first");
  prepareBaseMotorMove();
  Serial.println("Base target move: homing motor 3 first...");
  if (!homeStepperToLimit(3)) {
    holdThenDisableBaseMotor();
    endMotion();
    return;
  }
  long delta = baseDeltaToTarget(stepperPos[3], target);
  Serial.print("Base target move: moving from home to target, delta=");
  Serial.println(delta);
  if (delta != 0) {
    fsmServiceState(STATE_DEALING, "base target move: rotate base to target");
    moveStepperWithBaseRamp(3, delta);
  }
  stepperPos[3] = normalizeBasePosition(target);
  holdThenDisableBaseMotor();
  activeEjectButtonId = 0;
  endMotion();
  printAndSetReply("OK: at " + label);
}

long holdemTablePosition() {
  if (currentPlayers < 2 || currentPlayers > MAX_BUTTONS) {
    printAndSetReply("ERROR: players must be 2..8 for Texas Hold'em");
    return stepperPos[3];
  }
  if (!ejectPositionSet[1] || !ejectPositionSet[currentPlayers]) {
    printAndSetReply("ERROR: first and last player positions must be saved");
    return stepperPos[3];
  }
  int middlePlayer = (currentPlayers + 1) / 2;
  if (!ejectPositionSet[middlePlayer]) middlePlayer = 1;
  long first = ejectPositions[1];
  long last  = ejectPositions[currentPlayers];
  long target = normalizeBasePosition(first + (baseDeltaToTarget(first, last) / 2));
  Serial.print("Texas Hold'em table target = ");
  Serial.println(target);
  return target;
}

bool cmdMoveToHoldemTableIfNeeded() {
  lastResponse = "";
  long target = holdemTablePosition();
  if (lastResponse.startsWith("ERROR")) return false;
  target = normalizeBasePosition(target);
  if (texasHoldemBaseParkedAtTable && normalizeBasePosition(stepperPos[3]) == target) {
    Serial.println("Texas Hold'em table cards: base already parked at community-card target. Skipping base home/move.");
    activeEjectButtonId = 0;
    return true;
  }
  cmdGotoBaseTarget(target, "Texas Hold'em table cards");
  if (lastResponse.startsWith("ERROR")) {
    texasHoldemBaseParkedAtTable = false;
    return false;
  }
  texasHoldemTableTargetCached = target;
  texasHoldemBaseParkedAtTable = true;
  return true;
}

void cmdDealToTablePercent(int percent) {
  if (!cmdMoveToHoldemTableIfNeeded()) return;
  cmdEjectWithPololu2Percent(percent);
}

bool allPlayerSeatsSaved() {
  if (currentPlayers < 1 || currentPlayers > MAX_BUTTONS) {
    printAndSetReply("ERROR: players must be 1..8 before starting game");
    return false;
  }
  for (int player = 1; player <= currentPlayers; player++) {
    if (!ejectPositionSet[player]) {
      printAndSetReply("ERROR: no saved seat position for player " + String(player));
      return false;
    }
  }
  return true;
}

void resetPokerDrawCounts() {
  for (int i = 0; i <= MAX_BUTTONS; i++) {
    pokerDrawCounts[i] = 0;
  }
}

void cmdEndGame() {
  cambioStartupInProgress = false;
  texasHoldemActive = false;
  texasHoldemSequenceInProgress = false;
  texasHoldemStage = 0;
  texasHoldemBaseParkedAtTable = false;
  texasHoldemTableTargetCached = 0;
  pokerActive = false;
  pokerSequenceInProgress = false;
  resetPokerDrawCounts();
  blackjackActive = false;
  blackjackSequenceInProgress = false;
  printAndSetReply("OK: game ended. Normal button dealing restored.");
}

void cmdEndTexasHoldem() {
  texasHoldemActive = false;
  texasHoldemSequenceInProgress = false;
  texasHoldemStage = 0;
  texasHoldemBaseParkedAtTable = false;
  texasHoldemTableTargetCached = 0;
  printAndSetReply("OK: Texas Hold'em ended. Normal button dealing restored.");
}

void cmdStartTexasHoldem() {
  if (texasHoldemSequenceInProgress) {
    printAndSetReply("ERROR: Texas Hold'em sequence already in progress");
    return;
  }
  if (!allPlayerSeatsSaved()) return;
  cmdEndGame();
  texasHoldemActive = true;
  texasHoldemSequenceInProgress = true;
  texasHoldemStage = 0;
  texasHoldemBaseParkedAtTable = false;
  texasHoldemTableTargetCached = 0;
  Serial.println("Texas Hold'em startup: shuffle first, then deal 2 cards to each player.");
  cmdShuffle();
  if (lastResponse.startsWith("ERROR")) {
    texasHoldemSequenceInProgress = false;
    return;
  }
  bool dealtOk = cmdDealRoundRobin(2);
  texasHoldemSequenceInProgress = false;
  if (dealtOk) {
    texasHoldemStage = 0;
    printAndSetReply("OK: Texas Hold'em ready. Shuffled and dealt 2 cards to each player. Next button press deals flop.");
  }
}

void cmdTexasHoldemButtonAdvance() {
  if (!texasHoldemActive) {
    printAndSetReply("ERROR: Texas Hold'em is not active");
    return;
  }
  if (texasHoldemSequenceInProgress) {
    printAndSetReply("BUSY: Texas Hold'em sequence already in progress");
    return;
  }
  texasHoldemSequenceInProgress = true;
  if (texasHoldemStage == 0) {
    Serial.println("Texas Hold'em button: flop sequence. Burn at 0%, then 100%, 95%, 90% community cards.");
    cmdDealToTablePercent(0);
    if (!lastResponse.startsWith("ERROR")) cmdDealToTablePercent(100);
    if (!lastResponse.startsWith("ERROR")) cmdDealToTablePercent(95);
    if (!lastResponse.startsWith("ERROR")) cmdDealToTablePercent(90);
    if (!lastResponse.startsWith("ERROR")) {
      texasHoldemStage = 1;
      printAndSetReply("OK: flop complete. Next button press deals turn.");
    }
  } else if (texasHoldemStage == 1) {
    Serial.println("Texas Hold'em button: turn sequence. Burn at 0%, then 85% community card.");
    cmdDealToTablePercent(0);
    if (!lastResponse.startsWith("ERROR")) cmdDealToTablePercent(85);
    if (!lastResponse.startsWith("ERROR")) {
      texasHoldemStage = 2;
      printAndSetReply("OK: turn complete. Next button press deals river.");
    }
  } else if (texasHoldemStage == 2) {
    Serial.println("Texas Hold'em button: river sequence. Burn at 0%, then 80% community card.");
    cmdDealToTablePercent(0);
    if (!lastResponse.startsWith("ERROR")) cmdDealToTablePercent(80);
    if (!lastResponse.startsWith("ERROR")) {
      texasHoldemStage = 3;
      printAndSetReply("OK: river complete. Next button press starts a new Texas Hold'em hand.");
    }
  } else {
    Serial.println("Texas Hold'em button: new hand sequence. Shuffle and redeal hole cards.");
    texasHoldemSequenceInProgress = false;
    cmdStartTexasHoldem();
    return;
  }
  texasHoldemSequenceInProgress = false;
}

bool cmdDealRoundRobin(int cardsEach) {
  if (cardsEach < 1 || cardsEach > 20) {
    printAndSetReply("ERROR: cardsEach out of range");
    return false;
  }
  if (currentPlayers < 1 || currentPlayers > MAX_BUTTONS) {
    printAndSetReply("ERROR: players must be 1..8 before dealing");
    return false;
  }
  for (int player = 1; player <= currentPlayers; player++) {
    if (!ejectPositionSet[player]) {
      printAndSetReply("ERROR: no saved seat position for player " + String(player));
      return false;
    }
  }
  Serial.print("Round-robin deal: ");
  Serial.print(cardsEach);
  Serial.print(" cards each to ");
  Serial.print(currentPlayers);
  Serial.println(" players.");
  roundRobinDealInProgress = true;
  Serial.println("Round-robin: preparing Motor 1 at eject height once...");
  enableMotor(1);
  cmdHome(1);
  if (lastResponse.startsWith("ERROR")) {
    roundRobinDealInProgress = false;
    return false;
  }
  cmdMove(1, EJECT_HEIGHT);
  if (lastResponse.startsWith("ERROR")) {
    roundRobinDealInProgress = false;
    return false;
  }
  ejectReady = true;
  for (int card = 0; card < cardsEach; card++) {
    for (int player = 1; player <= currentPlayers; player++) {
      checkImmediateAbort();
      Serial.print("Round-robin: card ");
      Serial.print(card + 1);
      Serial.print(" to player ");
      Serial.println(player);
      cmdGotoSeat(player);
      if (lastResponse.startsWith("ERROR")) {
        roundRobinDealInProgress = false;
        return false;
      }
      cmdEjectRoundRobinNoHome(player);
      if (lastResponse.startsWith("ERROR")) {
        roundRobinDealInProgress = false;
        return false;
      }
    }
  }
  roundRobinDealInProgress = false;
  printAndSetReply("OK: round-robin deal complete");
  return true;
}

void cmdStartCambio() {
  if (cambioStartupInProgress) {
    printAndSetReply("ERROR: Cambio startup already in progress");
    return;
  }
  if (!allPlayerSeatsSaved()) return;
  cmdEndGame();
  cambioStartupInProgress = true;
  Serial.println("Cambio startup: shuffle first, then deal 4 cards to each player.");
  cmdShuffle();
  if (lastResponse.startsWith("ERROR")) {
    cambioStartupInProgress = false;
    return;
  }
  bool dealtOk = cmdDealRoundRobin(4);
  cambioStartupInProgress = false;
  if (dealtOk) {
    printAndSetReply("OK: Cambio startup complete. Shuffled and dealt 4 cards to each player.");
  }
}

void cmdStartPoker() {
  if (pokerSequenceInProgress) {
    printAndSetReply("ERROR: Poker sequence already in progress");
    return;
  }
  if (!allPlayerSeatsSaved()) return;
  cmdEndGame();
  pokerActive = true;
  pokerSequenceInProgress = true;
  resetPokerDrawCounts();
  Serial.println("Poker startup: shuffle first, then deal 5 cards to each player.");
  cmdShuffle();
  if (lastResponse.startsWith("ERROR")) {
    pokerSequenceInProgress = false;
    return;
  }
  bool dealtOk = cmdDealRoundRobin(5);
  pokerSequenceInProgress = false;
  if (dealtOk) {
    printAndSetReply("OK: Poker ready. Shuffled and dealt 5 cards to each player. Each player may draw up to 3 cards with their button.");
  }
}

void cmdPokerButtonDraw(int buttonId) {
  if (!pokerActive) {
    printAndSetReply("ERROR: Poker is not active");
    return;
  }
  if (buttonId < 1 || buttonId > currentPlayers) {
    printAndSetReply("ERROR: button is not assigned to an active poker player");
    return;
  }
  if (pokerSequenceInProgress) {
    printAndSetReply("BUSY: Poker sequence already in progress");
    return;
  }
  if (pokerDrawCounts[buttonId] >= POKER_MAX_DRAWS_PER_PLAYER) {
    printAndSetReply("OK: Player " + String(buttonId) + " has already drawn the max of 3 cards. No card dealt.");
    return;
  }
  pokerSequenceInProgress = true;
  cmdDealToSeat(buttonId);
  if (!lastResponse.startsWith("ERROR")) {
    cmdHomeBaseToIdle("after poker button draw");
  }
  pokerSequenceInProgress = false;
  if (!lastResponse.startsWith("ERROR")) {
    pokerDrawCounts[buttonId]++;
    printAndSetReply("OK: Poker draw dealt to Player " + String(buttonId) + ". Draws used: " + String(pokerDrawCounts[buttonId]) + "/3. Base returned to home idle.");
  }
}

void cmdStartBlackjack() {
  if (blackjackSequenceInProgress) {
    printAndSetReply("ERROR: Blackjack sequence already in progress");
    return;
  }
  if (!allPlayerSeatsSaved()) return;
  cmdEndGame();
  blackjackActive = true;
  blackjackSequenceInProgress = true;
  Serial.println("Blackjack startup: shuffle first, then deal 2 cards to each player.");
  cmdShuffle();
  if (lastResponse.startsWith("ERROR")) {
    blackjackSequenceInProgress = false;
    return;
  }
  bool dealtOk = cmdDealRoundRobin(2);
  blackjackSequenceInProgress = false;
  if (dealtOk) {
    printAndSetReply("OK: Blackjack ready. Shuffled and dealt 2 cards to each player. Button presses now hit players.");
  }
}

void cmdSetButtonSpeed(int buttonId, int percent) {
  if (buttonId < 1 || buttonId > MAX_BUTTONS) {
    printAndSetReply("ERROR: button must be 1..8");
    return;
  }
  if (percent < 0 || percent > 100) {
    printAndSetReply("ERROR: speed percent must be 0..100");
    return;
  }
  buttonPololu2Percents[buttonId] = percent;
  saveSeatPositions();
  printAndSetReply(
    "OK: button " + String(buttonId) +
    " Pololu 2 speed set to " + String(percent) + "%"
  );
}

void cmdTestButtonSpeed(int buttonId, unsigned long durationMs = 700) {
  if (buttonId < 1 || buttonId > MAX_BUTTONS) {
    printAndSetReply("ERROR: button must be 1..8");
    return;
  }
  int speed = pololu2PercentToSpeed(buttonPololu2Percents[buttonId]);
  beginMotion();
  setPololu(2, speed);
  unsigned long start = millis();
  while ((millis() - start) < durationMs) {
    checkImmediateAbort();
    delay(5);
  }
  stopPololu(2);
  endMotion();
  printAndSetReply("OK");
}

void cmdZeroBase() {
  stepperPos[3] = 0;
  printAndSetReply("OK: base position zeroed");
}

void cmdShuffle() {
  beginMotion();
  fsmServiceState(STATE_SHUFFLING, "shuffle sequence started");
  const int totalCards = currentTotalCards;
  const int maxShuffleableCards = SHELF_COUNT * MAX_CARDS_PER_SLOT;
  int slotMins[SHELF_COUNT] = {0, 0, 0, 0, 0, 0};
  if (totalCards == 52) {
    slotMins[0] = 5;
    slotMins[1] = 9;
    slotMins[2] = 10;
    slotMins[3] = 10;
    slotMins[4] = 9;
    slotMins[5] = 9;
  }
  bool slotEnabled[SHELF_COUNT] = {true, true, true, true, true, true};
  if (totalCards < 1 || totalCards > MAX_TOTAL_CARDS) {
    endMotion();
    printAndSetReply("ERROR: totalCards out of range");
    return;
  }
  if (totalCards > maxShuffleableCards) {
    endMotion();
    printAndSetReply(
      "ERROR: totalCards exceeds register capacity (" + String(maxShuffleableCards) + ")"
    );
    return;
  }
  int requiredCards = 0;
  for (int s = 0; s < SHELF_COUNT; s++) {
    requiredCards += slotMins[s];
  }
  if (requiredCards > totalCards) {
    endMotion();
    printAndSetReply("ERROR: slot minimums exceed total cards");
    return;
  }
  Serial.print("Shuffle: using totalCards = ");
  Serial.println(totalCards);
  enableMotor(1);
  enableMotor(2);
  fsmServiceState(STATE_HOMING, "shuffle setup: home Motor 2");
  Serial.println("Shuffle: homing motor 2...");
  cmdHome(2);
  if (!commandSucceeded()) {
    disableMotor(1);
    disableMotor(2);
    endMotion();
    return;
  }
  disableMotor(2);
  fsmServiceState(STATE_HOMING, "shuffle setup: home Motor 1");
  Serial.println("Shuffle: homing motor 1...");
  cmdHome(1);
  if (!commandSucceeded()) {
    disableMotor(1);
    endMotion();
    return;
  }
  int sequence[MAX_TOTAL_CARDS];
  int shelfCounts[SHELF_COUNT] = {0};
  int seqLen = 0;
  randomSeed(micros());
  fsmServiceState(STATE_SHUFFLING, "shuffle planning: apply slot minimums");
  for (int s = 0; s < SHELF_COUNT; s++) {
    checkImmediateAbort();
    if (!slotEnabled[s] && slotMins[s] > 0) {
      endMotion();
      printAndSetReply("ERROR: disabled slot has required minimum");
      return;
    }
    if (slotMins[s] > MAX_CARDS_PER_SLOT) {
      endMotion();
      printAndSetReply("ERROR: slot minimum exceeds slot capacity");
      return;
    }
    for (int i = 0; i < slotMins[s]; i++) {
      sequence[seqLen++] = s;
      shelfCounts[s]++;
    }
  }
  while (seqLen < totalCards) {
    checkImmediateAbort();
    int availableShelves[SHELF_COUNT];
    int availableCount = 0;
    for (int s = 0; s < SHELF_COUNT; s++) {
      if (slotEnabled[s] && shelfCounts[s] < MAX_CARDS_PER_SLOT) {
        availableShelves[availableCount++] = s;
      }
    }
    if (availableCount == 0) {
      endMotion();
      printAndSetReply("ERROR: no shelf space remaining");
      return;
    }
    int chosenShelf = availableShelves[random(0, availableCount)];
    sequence[seqLen++] = chosenShelf;
    shelfCounts[chosenShelf]++;
  }
  for (int i = totalCards - 1; i > 0; i--) {
    int j = random(0, i + 1);
    int temp = sequence[i];
    sequence[i] = sequence[j];
    sequence[j] = temp;
  }
  fsmServiceState(STATE_SHUFFLING, "shuffle execution: feed cards into shelves");
  for (int i = 0; i < totalCards; i++) {
    checkImmediateAbort();
    long targetSteps = SHELF_STEPS[sequence[i]];
    long delta = targetSteps - stepperPos[1];
    Serial.print("Shuffle: card ");
    Serial.print(i + 1);
    Serial.print(" of ");
    Serial.print(totalCards);
    Serial.print(" -> shelf ");
    Serial.println(sequence[i] + 1);
    moveStepperFixedSpeed(1, delta, HOPPER_SHUFFLE_STEP_US);
    cmdSweep();
    if (!commandSucceeded()) {
      disableMotor(1);
      endMotion();
      return;
    }
    fsmServiceState(STATE_SHUFFLING, "shuffle execution: continue feeding cards");
  }
  Serial.println("Shuffle: dealing complete, returning...");
  fsmServiceState(STATE_HOMING, "shuffle cleanup: home Motor 1");
  cmdHome(1);
  if (!commandSucceeded()) {
    disableMotor(1);
    endMotion();
    return;
  }
  delayWithAbort(100);
  fsmServiceState(STATE_MANUAL_MOVING, "shuffle cleanup: return hopper to shelf return position");
  cmdMove(1, SHELF_RETURN - stepperPos[1]);
  if (!commandSucceeded()) {
    disableMotor(1);
    endMotion();
    return;
  }
  delayWithAbort(500);
  disableMotor(1);
  disableMotor(3);
  sweepServo(0, 180);
  fsmServiceState(STATE_PUSHING, "shuffle cleanup: clear register with pusher");
  cmdPush();
  if (!commandSucceeded()) {
    disableMotor(2);
    endMotion();
    return;
  }
  delayWithAbort(500);
  sweepServo(180, 0);
  disableMotor(2);
  delayWithAbort(500);
  enableMotor(1);
  fsmServiceState(STATE_HOMING, "shuffle cleanup: repeated hopper settle cycle");
  for (int i = 0; i < 3; i++) {
    cmdHome(1);
    if (!commandSucceeded()) {
      disableMotor(1);
      endMotion();
      return;
    }
  }
  for (int cycle = 0; cycle < 6; cycle++) {
    fsmServiceState(STATE_MANUAL_MOVING, "shuffle cleanup: hopper settle lift");
    cmdMove(1, 150);
    if (!commandSucceeded()) {
      disableMotor(1);
      endMotion();
      return;
    }
    fsmServiceState(STATE_HOMING, "shuffle cleanup: hopper settle re-home");
    for (int i = 0; i < 2; i++) {
      cmdHome(1);
      if (!commandSucceeded()) {
        disableMotor(1);
        endMotion();
        return;
      }
    }
  }
  disableMotor(1);
  endMotion();
  printAndSetReply("OK");
}

bool addPeer(const uint8_t *mac) {
  if (mac == nullptr) return false;
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_err_t err = esp_now_add_peer(&peerInfo);
  return (err == ESP_OK);
}

bool sendPacket(const uint8_t *destMac, MessageType type, uint8_t buttonId) {
  if (destMac == nullptr) return false;
  Packet pkt;
  pkt.type     = (uint8_t)type;
  pkt.buttonId = buttonId;
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

void pairButton(uint8_t buttonId, const uint8_t *mac) {
  if (buttonId < 1 || buttonId > MAX_BUTTONS) return;
  memcpy(pairedButtonMACs[buttonId], mac, 6);
  buttonPaired[buttonId] = true;
  savePairedButtons();
  Serial.print("Paired button ");
  Serial.print(buttonId);
  Serial.print(" to MAC ");
  printMac(mac);
  Serial.println();
}

bool isAuthorizedButton(uint8_t buttonId, const uint8_t *srcMac) {
  if (buttonId < 1 || buttonId > MAX_BUTTONS) return false;
  if (!buttonPaired[buttonId]) return false;
  return macEquals(pairedButtonMACs[buttonId], srcMac);
}

bool enqueueEspNowEvent(uint8_t eventType, uint8_t buttonId, const uint8_t *mac) {
  if (mac == nullptr) return false;
  uint8_t nextHead = (uint8_t)((espNowEventHead + 1) % ESPNOW_EVENT_QUEUE_SIZE);
  if (nextHead == espNowEventTail) {
    espNowQueueOverflowCount++;
    return false;
  }
  espNowEventQueue[espNowEventHead].type = eventType;
  espNowEventQueue[espNowEventHead].buttonId = buttonId;
  memcpy(espNowEventQueue[espNowEventHead].mac, mac, 6);
  espNowEventHead = nextHead;
  return true;
}

bool dequeueEspNowEvent(EspNowQueuedEvent &event) {
  if (espNowEventTail == espNowEventHead) return false;
  noInterrupts();
  event = espNowEventQueue[espNowEventTail];
  espNowEventTail = (uint8_t)((espNowEventTail + 1) % ESPNOW_EVENT_QUEUE_SIZE);
  interrupts();
  return true;
}

void processEspNowEvent(const EspNowQueuedEvent &event) {
  if (event.buttonId < 1 || event.buttonId > MAX_BUTTONS) {
    return;
  }
  if (event.type == ESPNOW_EVENT_PAIR) {
    lastCommand = "BUTTON_PAIR_" + String(event.buttonId);
    fsmServiceState(STATE_PAIRING_SERVICE, "ESP-NOW pair request from button " + String(event.buttonId));
    if (!addPeer(event.mac)) {
      printAndSetReply("ERROR: failed to add button peer during pairing");
      transitionTo(STATE_ERROR_STATE, "pairing failed");
      return;
    }
    pairButton(event.buttonId, event.mac);
    if (sendPacket(event.mac, MSG_PAIR_ACK, event.buttonId)) {
      printAndSetReply("OK: paired button " + String(event.buttonId));
    } else {
      printAndSetReply("ERROR: failed to send PAIR_ACK");
      transitionTo(STATE_ERROR_STATE, "pair ACK failed");
      return;
    }
    transitionTo(STATE_IDLING, "pairing service complete");
    return;
  }
  if (event.type == ESPNOW_EVENT_FIRE) {
    if (!isAuthorizedButton(event.buttonId, event.mac)) {
      lastResponse = "ERROR: unauthorized FIRE_REQUEST ignored for button " + String(event.buttonId);
      Serial.println(lastResponse);
      transitionTo(STATE_ERROR_STATE, "unauthorized button fire");
      return;
    }
    if (!addPeer(event.mac)) {
      Serial.println("Failed to add peer for FIRE_ACK.");
    } else {
      sendPacket(event.mac, MSG_FIRE_ACK, event.buttonId);
    }
    if (motionActive() || cambioStartupInProgress || texasHoldemSequenceInProgress || pokerSequenceInProgress || blackjackSequenceInProgress) {
      lastResponse = "BUSY: ignoring button fire during active machine sequence";
      Serial.println(lastResponse);
      return;
    }
    lastCommand = "BUTTON_FIRE_" + String(event.buttonId);
    if (texasHoldemActive) {
      serviceCommandEvent("TEXASHOLDEMADVANCE", "Button " + String(event.buttonId));
      return;
    }
    if (pokerActive) {
      serviceCommandEvent("POKERDRAW " + String(event.buttonId), "Button " + String(event.buttonId));
      return;
    }
    serviceCommandEvent("BUTTONDEAL " + String(event.buttonId), "Button " + String(event.buttonId));
    return;
  }
}

void serviceEspNowEventQueue() {
  EspNowQueuedEvent event;
  while (dequeueEspNowEvent(event)) {
    processEspNowEvent(event);
  }
}

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.print("ESP-NOW send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}

void onDataRecv(const esp_now_recv_info_t *recvInfo, const uint8_t *incomingData, int len) {
  if (recvInfo == nullptr || incomingData == nullptr) return;
  if (len != sizeof(Packet)) return;
  Packet pkt;
  memcpy(&pkt, incomingData, sizeof(pkt));
  const uint8_t *srcMac = recvInfo->src_addr;
  if (pkt.buttonId < 1 || pkt.buttonId > MAX_BUTTONS) {
    return;
  }
  if (pkt.type == MSG_PAIR_REQUEST) {
    enqueueEspNowEvent(ESPNOW_EVENT_PAIR, pkt.buttonId, srcMac);
    return;
  }
  if (pkt.type == MSG_FIRE_REQUEST) {
    enqueueEspNowEvent(ESPNOW_EVENT_FIRE, pkt.buttonId, srcMac);
    return;
  }
}

void executeCommandNow(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  lastCommand  = cmd;
  commandReply = "";
  if (cmd == "ABORT") {
    restartNow();
    return;
  } else if (cmd.startsWith("SETUP ")) {
    int sp1 = cmd.indexOf(' ', 6);
    int sp2 = sp1 != -1 ? cmd.indexOf(' ', sp1 + 1) : -1;
    if (sp1 == -1 || sp2 == -1) {
      printAndSetReply("ERROR: usage: SETUP <players> <decks> <cardsPerDeck>");
      return;
    }
    int players      = cmd.substring(6, sp1).toInt();
    int decks        = cmd.substring(sp1 + 1, sp2).toInt();
    int cardsPerDeck = cmd.substring(sp2 + 1).toInt();
    cmdSetup(players, decks, cardsPerDeck);
  } else if (cmd.startsWith("SETBUTTONSPEED ")) {
    int sp1 = cmd.indexOf(' ', 15);
    if (sp1 == -1) {
      printAndSetReply("ERROR: usage: SETBUTTONSPEED <buttonId> <percent>");
      return;
    }
    int buttonId = cmd.substring(15, sp1).toInt();
    int percent = cmd.substring(sp1 + 1).toInt();
    cmdSetButtonSpeed(buttonId, percent);
  } else if (cmd.startsWith("TESTBUTTONSPEED ")) {
    int buttonId = cmd.substring(16).toInt();
    cmdTestButtonSpeed(buttonId);
  } else if (cmd.startsWith("HOME ")) {
    int n = cmd.substring(5).toInt();
    cmdHome(n);
  } else if (cmd.startsWith("MOVE ")) {
    int sp = cmd.indexOf(' ', 5);
    if (sp == -1) {
      printAndSetReply("ERROR: usage: MOVE <n> <steps>");
      return;
    }
    int  n     = cmd.substring(5, sp).toInt();
    long steps = cmd.substring(sp + 1).toInt();
    cmdMove(n, steps);
  } else if (cmd.startsWith("SERVO ")) {
    int angle = cmd.substring(6).toInt();
    cmdServo(angle);
  } else if (cmd.startsWith("SPIN ")) {
    int sp1 = cmd.indexOf(' ', 5);
    int sp2 = sp1 != -1 ? cmd.indexOf(' ', sp1 + 1) : -1;
    if (sp1 == -1 || sp2 == -1) {
      printAndSetReply("ERROR: usage: SPIN <n> <speed> <ms>");
      return;
    }
    int n           = cmd.substring(5, sp1).toInt();
    int speed       = cmd.substring(sp1 + 1, sp2).toInt();
    unsigned long durMs = (unsigned long)cmd.substring(sp2 + 1).toInt();
    cmdSpin(n, speed, durMs);
  } else if (cmd.startsWith("ENABLE ")) {
    int n = cmd.substring(7).toInt();
    cmdEnable(n);
  } else if (cmd.startsWith("DISABLE ")) {
    int n = cmd.substring(8).toInt();
    cmdDisable(n);
  } else if (cmd == "SWEEP") {
    cmdSweep();
  } else if (cmd == "SHUFFLE") {
    cmdShuffle();
  } else if (cmd == "STARTCAMBIO") {
    cmdStartCambio();
  } else if (cmd == "STARTTEXASHOLDEM") {
    cmdStartTexasHoldem();
  } else if (cmd == "STARTPOKER") {
    cmdStartPoker();
  } else if (cmd == "STARTBLACKJACK") {
    cmdStartBlackjack();
  } else if (cmd == "TEXASHOLDEMADVANCE") {
    cmdTexasHoldemButtonAdvance();
  } else if (cmd == "ENDTEXASHOLDEM") {
    cmdEndTexasHoldem();
  } else if (cmd == "ENDGAME") {
    cmdEndGame();
  } else if (cmd.startsWith("DEALROUNDROBIN ")) {
    int cardsEach = cmd.substring(15).toInt();
    cmdDealRoundRobin(cardsEach);
  } else if (cmd == "EJECT") {
    cmdEject();
  } else if (cmd.startsWith("EJECT ")) {
    int buttonId = cmd.substring(6).toInt();
    cmdEject(buttonId);
  } else if (cmd.startsWith("EJECTPERCENT ")) {
    int percent = cmd.substring(13).toInt();
    cmdEjectWithPololu2Percent(percent);
  } else if (cmd.startsWith("DEALTOTABLE ")) {
    int percent = cmd.substring(12).toInt();
    cmdDealToTablePercent(percent);
  } else if (cmd.startsWith("DEALTOSEAT ")) {
    int buttonId = cmd.substring(11).toInt();
    cmdDealToSeat(buttonId);
  } else if (cmd.startsWith("POKERDRAW ")) {
    int buttonId = cmd.substring(10).toInt();
    cmdPokerButtonDraw(buttonId);
  } else if (cmd.startsWith("BUTTONDEAL ")) {
    int buttonId = cmd.substring(11).toInt();
    cmdButtonDeal(buttonId);
  } else if (cmd == "PUSH") {
    cmdPush();
  } else if (cmd.startsWith("SETEJECTPOS ")) {
    int buttonId = cmd.substring(12).toInt();
    cmdSetEjectPos(buttonId);
  } else if (cmd.startsWith("GOTOSEAT ")) {
    int buttonId = cmd.substring(9).toInt();
    cmdGotoSeat(buttonId);
  } else if (cmd == "HOMEBASE" || cmd == "ZEROBASE") {
    cmdHome(3);
  } else if (cmd == "MANUALZEROBASE") {
    cmdZeroBase();
  } else if (cmd == "CLEARSEATS") {
    clearSeatPositions();
    printAndSetReply("OK: cleared all saved seat positions");
  } else if (cmd == "CLEARPAIRS") {
    clearAllPairedButtons();
    savePairedButtons();
    printAndSetReply("OK: cleared all paired buttons");
  } else if (cmd == "LISTPAIRS") {
    String report = "Paired buttons:";
    Serial.println(report);
    for (int i = 1; i <= MAX_BUTTONS; i++) {
      String line = "  Button " + String(i) + ": ";
      if (buttonPaired[i]) {
        line += macToString(pairedButtonMACs[i]);
      } else {
        line += "not paired";
      }
      Serial.println(line);
      report += "\n" + line;
    }
    Serial.println("OK");
    report += "\nOK";
    setReply(report);
  } else {
    Serial.println("ERROR: unknown command");
    Serial.println("  SETUP <players> <decks> <cardsPerDeck>  store setup values");
    Serial.println("  HOME <n>               home stepper 1-3");
    Serial.println("  MOVE <n> <steps>       move stepper 1-3 by steps (+/-)");
    Serial.println("  SERVO <angle>          move servo to angle 0..180 at full speed");
    Serial.println("  SPIN <n> <speed> <ms>  spin pololu 1-2 at -255..255 for ms");
    Serial.println("  ENABLE <n>             enable stepper driver 1-3");
    Serial.println("  DISABLE <n>            disable stepper driver 1-3");
    Serial.println("  SWEEP                  pololu 1 on, servo 0->180->0, pololu 1 off");
    Serial.println("  SHUFFLE                home, shuffle with per-slot capacity limit");
    Serial.println("  EJECT                  eject a card");
    Serial.println("  PUSH                   enable motor 2, move -125, move +125, disable motor 2");
    Serial.println("  BUTTONDEAL <button>    button-style deal to seat, then return base home");
    Serial.println("  SETEJECTPOS <button>   store current motor 3 position for button 1..8");
    Serial.println("  GOTOSEAT <button>      move motor 3 to the saved seat for button 1..8");
    Serial.println("  HOMEBASE / ZEROBASE    home motor 3 using the pin 36 base limit switch");
    Serial.println("  MANUALZEROBASE         manually set current motor 3 position to 0 without moving");
    Serial.println("  CLEARSEATS             clear all stored seat positions");
    Serial.println("  ABORT                  restart the ESP immediately");
    Serial.println("  LISTPAIRS              list stored paired button MACs");
    Serial.println("  CLEARPAIRS             clear all stored button pairings");
    setReply("ERROR: unknown command");
  }
}

void serviceCommandEvent(String cmd, const String& source) {
  cmd.trim();
  if (cmd.length() == 0) return;
  CardDealerState commandState = classifyCommandState(cmd);
  transitionTo(commandState, source + " command received: " + cmd);
  executeCommandNow(cmd);
  if (cmd != "ABORT") {
    if (lastResponse.startsWith("ERROR")) {
      transitionTo(STATE_ERROR_STATE, "command returned an error");
    } else {
      transitionTo(STATE_IDLING, "command service complete");
    }
  }
}

void processCommand(String cmd) {
  serviceCommandEvent(cmd, "external");
}

bool checkSerialCommandEvent() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputBuffer.length() > 0) {
        pendingSerialCommand = inputBuffer;
        inputBuffer = "";
        serialCommandEventFlag = true;
        return true;
      }
    } else {
      inputBuffer += c;
    }
  }
  return serialCommandEventFlag;
}

void serviceSerialCommandEvent() {
  if (!serialCommandEventFlag) return;
  serialCommandEventFlag = false;
  serviceCommandEvent(pendingSerialCommand, "Serial");
  pendingSerialCommand = "";
}

void runCardDealerStateMachine() {
  serviceEspNowEventQueue();
  if (checkSerialCommandEvent()) {
    serviceSerialCommandEvent();
  }
  switch (currentState) {
    case STATE_IDLING:
      break;
    case STATE_RECEIVING_COMMAND:
    case STATE_SETTING_UP:
    case STATE_HOMING:
    case STATE_MANUAL_MOVING:
    case STATE_SHUFFLING:
    case STATE_GAME_STARTING:
    case STATE_DEALING:
    case STATE_EJECTING:
    case STATE_PUSHING:
    case STATE_CALIBRATING:
    case STATE_PAIRING_SERVICE:
      break;
    case STATE_ABORTING:
      restartNow();
      break;
    case STATE_ERROR_STATE:
      break;
  }
}

void loop() {
  server.handleClient();
  runCardDealerStateMachine();
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

String extractCommandFromRequestBody(const String& body) {
  String trimmed = body;
  trimmed.trim();
  if (trimmed.length() == 0) return "";
  if (!trimmed.startsWith("{")) {
    return trimmed;
  }
  int key   = trimmed.indexOf("\"command\"");
  if (key == -1) return "";
  int colon = trimmed.indexOf(':', key);
  if (colon == -1) return "";
  int q1 = trimmed.indexOf('\"', colon + 1);
  if (q1 == -1) return "";
  int q2 = trimmed.indexOf('\"', q1 + 1);
  if (q2 == -1) return "";
  return trimmed.substring(q1 + 1, q2);
}

void handleOptions() {
  addCorsHeaders();
  server.send(204);
}

void handleRoot() {
  addCorsHeaders();
  String html;
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Card Dealer ESP32</title></head><body style='font-family:Arial;padding:24px;'>";
  html += "<h2>Card Dealer ESP32</h2>";
  html += "<p>Web API is live.</p>";
  html += "<ul>";
  html += "<li>GET /status</li>";
  html += "<li>POST /command with JSON: {\"command\":\"EJECT\"}</li>";
  html += "</ul>";
  html += "<p>AP IP: " + WiFi.softAPIP().toString() + "</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleStatus() {
  addCorsHeaders();
  String json = "{";
  json += "\"connected\":true,";
  json += "\"busy\":" + String((motionActive() || cambioStartupInProgress || texasHoldemSequenceInProgress || pokerSequenceInProgress || blackjackSequenceInProgress) ? "true" : "false") + ",";
  json += "\"ap_ssid\":\"" + jsonEscape(String(AP_SSID)) + "\",";
  json += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"sta_ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"lastCommand\":\"" + jsonEscape(lastCommand) + "\",";
  json += "\"lastResponse\":\"" + jsonEscape(lastResponse) + "\",";
  json += "\"fsmState\":\"" + String(stateName(currentState)) + "\",";
  json += "\"espNowQueueOverflowCount\":" + String(espNowQueueOverflowCount) + ",";
  json += "\"pairedButtons\":" + String(countPairedButtons()) + ",";
  json += "\"ejectReady\":" + String(ejectReady ? "true" : "false") + ",";
  json += "\"cambioStartupInProgress\":" + String(cambioStartupInProgress ? "true" : "false") + ",";
  json += "\"texasHoldemActive\":" + String(texasHoldemActive ? "true" : "false") + ",";
  json += "\"texasHoldemSequenceInProgress\":" + String(texasHoldemSequenceInProgress ? "true" : "false") + ",";
  json += "\"texasHoldemStage\":" + String(texasHoldemStage) + ",";
  json += "\"pokerActive\":" + String(pokerActive ? "true" : "false") + ",";
  json += "\"pokerSequenceInProgress\":" + String(pokerSequenceInProgress ? "true" : "false") + ",";
  json += "\"blackjackActive\":" + String(blackjackActive ? "true" : "false") + ",";
  json += "\"blackjackSequenceInProgress\":" + String(blackjackSequenceInProgress ? "true" : "false") + ",";
  json += "\"activeEjectButtonId\":" + String(activeEjectButtonId) + ",";
  json += "\"players\":" + String(currentPlayers) + ",";
  json += "\"deckCount\":" + String(currentDeckCount) + ",";
  json += "\"cardsPerDeck\":" + String(currentCardsPerDeck) + ",";
  json += "\"totalCards\":" + String(currentTotalCards) + ",";
  json += "\"motorPositions\":[" + String(stepperPos[1]) + "," + String(stepperPos[2]) + "," + String(stepperPos[3]) + "],";
  json += "\"ejectPositionsSet\":" + String(countSetEjectPositions()) + ",";
  json += "\"ejectPositions\":[";
  for (int i = 1; i <= MAX_BUTTONS; i++) {
    json += ejectPositionSet[i] ? String(ejectPositions[i]) : "null";
    if (i < MAX_BUTTONS) json += ",";
  }
  json += "],";
  json += "\"buttonPololu2Percents\":[";
  for (int i = 1; i <= MAX_BUTTONS; i++) {
    json += String(buttonPololu2Percents[i]);
  if (i < MAX_BUTTONS) json += ",";
  }
  json += "]";
  json += "}";
  server.send(200, "application/json", json);
}

void handleCommand() {
  addCorsHeaders();
  String cmd = "";
  if (server.hasArg("plain")) {
    cmd = extractCommandFromRequestBody(server.arg("plain"));
  }
  if (cmd.length() == 0 && server.hasArg("command")) {
    cmd = server.arg("command");
    cmd.trim();
  }
  if (cmd.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"message\":\"Missing command\"}");
    return;
  }
  if (motionActive() && cmd != "ABORT") {
    server.send(409, "application/json", "{\"ok\":false,\"message\":\"Busy running another command\"}");
    return;
  }
  if (motionActive() && cmd == "ABORT") {
    webAbortRequested = true;
    server.send(202, "application/json", "{\"ok\":true,\"message\":\"ABORT queued\"}");
    return;
  }
  processCommand(cmd);
  String json = "{";
  json += "\"ok\":";
  json += (lastResponse.startsWith("ERROR") ? "false" : "true");
  json += ",";
  json += "\"command\":\"" + jsonEscape(cmd) + "\",";
  json += "\"message\":\"" + jsonEscape(lastResponse) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/command", HTTP_POST, handleCommand);
  server.on("/", HTTP_OPTIONS, handleOptions);
  server.on("/status", HTTP_OPTIONS, handleOptions);
  server.on("/command", HTTP_OPTIONS, handleOptions);
  server.begin();
  Serial.println("HTTP server started.");
  Serial.println("  GET  /status");
  Serial.println("  POST /command");
}

void setupWiFi() {
  WiFi.mode(WIFI_AP_STA);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASS, 1);
  if (apOk) {
    Serial.print("SoftAP started. SSID: ");
    Serial.println(AP_SSID);
    Serial.print("SoftAP IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("SoftAP start failed.");
  }
  if (strlen(STA_SSID) > 0) {
    Serial.print("Connecting to STA Wi-Fi: ");
    Serial.println(STA_SSID);
    WiFi.begin(STA_SSID, STA_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("STA connected. IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("STA connect timed out. Continuing with SoftAP only.");
    }
  }
}

void setup() {
  Serial.begin(115200);
  const int stepPins[]   = { STEP_PIN_1, STEP_PIN_2, STEP_PIN_3 };
  const int dirPins[]    = { DIR_PIN_1,  DIR_PIN_2,  DIR_PIN_3  };
  const int enablePins[] = { ENABLE_PIN_1, ENABLE_PIN_2, ENABLE_PIN_3 };
  for (int i = 0; i < 3; i++) {
    pinMode(stepPins[i], OUTPUT);
    pinMode(dirPins[i], OUTPUT);
    pinMode(enablePins[i], OUTPUT);
    digitalWrite(dirPins[i], LOW);
    digitalWrite(enablePins[i], DRIVER_DISABLE);
  }
  pinMode(LIMIT_PIN_1, INPUT_PULLUP);
  pinMode(LIMIT_PIN_2, INPUT_PULLUP);
  pinMode(LIMIT_PIN_3, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(LIMIT_PIN_3), onBaseHomeSwitchFalling, FALLING);
  pinMode(POT_PIN, INPUT);
  analogReadResolution(12);
  ledcAttach(SERVO_PIN, SERVO_LEDC_FREQ, SERVO_LEDC_RES);
  ledcWrite(SERVO_PIN, angleToDuty(0));
  servoAngle = 0;
  ledcAttach(POL1_A, POLOLU_FREQ, POLOLU_RES);
  ledcWrite(POL1_A, 0);
  ledcAttach(POL1_B, POLOLU_FREQ, POLOLU_RES);
  ledcWrite(POL1_B, 0);
  ledcAttach(POL2_A, POLOLU_FREQ, POLOLU_RES);
  ledcWrite(POL2_A, 0);
  ledcAttach(POL2_B, POLOLU_FREQ, POLOLU_RES);
  ledcWrite(POL2_B, 0);
  loadPairedButtons();
  loadSeatPositions();
  stepperPos[3] = 0;
  disableMotor(3);
  setupWiFi();
  setupWebServer();
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed.");
    while (true) { delay(1000); }
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);
  for (int i = 1; i <= MAX_BUTTONS; i++) {
    if (buttonPaired[i]) {
      addPeer(pairedButtonMACs[i]);
    }
  }
  lastResponse = "Ready";
  Serial.println("Ready.");
  Serial.println("Base reference: HOME 3 uses the pin 36 limit switch as zero.");
  Serial.println("Commands: SETUP | HOME | MOVE | SERVO | SPIN | ENABLE | DISABLE | SWEEP | SHUFFLE | STARTCAMBIO | STARTTEXASHOLDEM | ENDTEXASHOLDEM | ENDGAME | EJECT | EJECTPERCENT | DEALTOTABLE | DEALTOSEAT | BUTTONDEAL | PUSH | SETEJECTPOS | GOTOSEAT | HOMEBASE | ZEROBASE | MANUALZEROBASE | CLEARSEATS | ABORT | LISTPAIRS | CLEARPAIRS");
}

long normalizeBasePosition(long pos) {
  long wrapped = pos % BASE_CIRCLE_STEPS;
  if (wrapped < 0) wrapped += BASE_CIRCLE_STEPS;
  return wrapped;
}

void updateBasePositionOneStep(int direction) {
  if (direction == HIGH) {
    stepperPos[3]++;
    if (stepperPos[3] >= BASE_CIRCLE_STEPS) stepperPos[3] = 0;
  } else {
    stepperPos[3]--;
    if (stepperPos[3] < 0) stepperPos[3] = BASE_CIRCLE_STEPS - 1;
  }
  if (digitalRead(LIMIT_PIN_3) == LOW) {
    stepperPos[3] = 0;
  }
}

long baseDeltaToHome(long current) {
  current = normalizeBasePosition(current);
  if (current == 0) return 0;
  if (current <= BASE_HALF_CIRCLE_STEPS) {
    return -current;
  }
  return BASE_CIRCLE_STEPS - current;
}

long baseDeltaToTarget(long current, long target) {
  current = normalizeBasePosition(current);
  target = normalizeBasePosition(target);
  long forward = target - current;
  if (forward < 0) forward += BASE_CIRCLE_STEPS;
  long reverse = forward - BASE_CIRCLE_STEPS;
  if (forward <= labs(reverse)) return forward;
  return reverse;
}
