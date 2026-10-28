#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>
#include <TinyGPS++.h>

// --- PIN DEFINITIONS ---
#define BTN_ON        2
#define BTN_UP        3
#define BTN_DOWN      4
#define BTN_SELECT    5
#define BUZZER_PIN    8
#define LED_PIN       9
#define GPS_TX_PIN    A1
#define GPS_RX_PIN    A2

// --- I2C LCD & GPS INITIALIZATION ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
TinyGPSPlus gps;
SoftwareSerial gpsSerial(GPS_TX_PIN, GPS_RX_PIN);

// --- SYSTEM STATES ---
enum SystemState {
  STATE_OFF,
  STATE_WELCOME,
  STATE_MAIN_MENU,
  STATE_SOS,
  STATE_MSG_MENU,
  STATE_SENDING_MORSE,
  STATE_FLASHLIGHT,
  STATE_PANIC_SOS,
  STATE_LOW_BATT_WARNING
};

SystemState currentState = STATE_OFF;
SystemState lastStateBeforeWarning = STATE_OFF;

bool stealthMode = false;
int mainMenuIndex = 0;
const int MAIN_MENU_ITEMS = 4;
int msgMenuIndex = 0;
const int MSG_MENU_ITEMS = 4;

// --- LIVE BATTERY SIMULATION ENGINE ---
int batteryLevel = 99;
unsigned long lastBatteryTick = 0;
int lastWarnedLevel = -1;

// --- HYBRID GPS ENGINE VARIABLES ---
char activeLocation[25] = "SEARCHING GPS";
bool hasFix = false;
unsigned long lastGpsCheckTime = 0;
int fallbackLatFrac = 9000;
int fallbackLngFrac = 9000;

// --- PREDEFINED MESSAGES & MORSE DICTIONARY ---
const char* preDefMessages[] = {
  "HELP ME",
  "INJURED",
  "NEED WATER",
  "BACK"
};

const char* morseLetters[] = {
  ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---",
  "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-",
  "..-", "...-", ".--", "-..-", "-.--", "--.."
};

const char* morseNumbers[] = {
  "-----", ".----", "..---", "...--", "....-",
  ".....", "-....", "--...", "---..", "----."
};

const int DOT_DUR = 150;

void setup() {
  Serial.begin(9600);
  gpsSerial.begin(9600);

  pinMode(GPS_RX_PIN, INPUT_PULLUP);

  pinMode(BTN_ON, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  lcd.init();
  lcd.clear();
  lcd.noBacklight();
}

void loop() {
  updateBattery();
  processSmartGpsStream();

  if (currentState != STATE_OFF && currentState != STATE_PANIC_SOS) {
    checkPanic();
    checkPowerOff();
  }

  switch (currentState) {
    case STATE_OFF:
      if (digitalRead(BTN_ON) == LOW) {
        delay(50);
        if (digitalRead(BTN_ON) == LOW) {
          if (batteryLevel <= 0) batteryLevel = 99;

          Serial.println(F("\n==================================="));
          Serial.println(F("[SYSTEM] Terminal Waking Up..."));
          Serial.println(F("==================================="));

          currentState = STATE_WELCOME;
          while (digitalRead(BTN_ON) == LOW) { delay(10); }
        }
      }
      break;

    case STATE_WELCOME:
      lcd.backlight();
      lcd.clear();
      lcd.print(F(" Smart Rescue "));
      lcd.setCursor(0, 1);
      lcd.print(F("   Terminal   "));
      delay(2000);
      if (currentState != STATE_PANIC_SOS && currentState != STATE_OFF) {
        currentState = STATE_MAIN_MENU;
        drawMainMenu();
      }
      break;

    case STATE_MAIN_MENU:
      handleMainMenuInput();
      break;

    case STATE_SOS: {
      Serial.println(F("\n[ACTION] User selected: SOS"));
      lcd.clear();
      lcd.print(F("Sending SOS..."));
      lcd.setCursor(0, 1);
      // FIX: both strings are 15 chars (LCD is 16 wide)
      lcd.print(hasFix ? F("GPS: REAL LOCK ") : F("GPS: FALLBACK  "));

      char payload[64];
      sprintf(payload, "SOS %s", activeLocation);

      if (stealthMode) lcd.noBacklight();

      Serial.print(F("[TRANSMITTING] "));
      Serial.println(payload);
      transmitMorse(payload);

      lcd.backlight();

      if (currentState != STATE_PANIC_SOS) {
        Serial.println(F("[STATUS] SOS Transmission Complete."));
        lcd.clear();
        lcd.print(F("SOS Sent!"));
        delay(1500);
        currentState = STATE_MAIN_MENU;
        drawMainMenu();
      }
      break;
    }

    case STATE_MSG_MENU:
      handleMsgMenuInput();
      break;

    case STATE_SENDING_MORSE: {
      Serial.print(F("\n[ACTION] User selected message: "));
      Serial.println(preDefMessages[msgMenuIndex]);

      lcd.clear();
      lcd.print(F("Sending Msg..."));
      lcd.setCursor(0, 1);
      // FIX: both strings are 15 chars (LCD is 16 wide)
      lcd.print(hasFix ? F("GPS: REAL LOCK ") : F("GPS: FALLBACK  "));

      char payload[64];
      sprintf(payload, "%s %s", preDefMessages[msgMenuIndex], activeLocation);

      if (stealthMode) lcd.noBacklight();

      Serial.print(F("[TRANSMITTING] "));
      Serial.println(payload);
      transmitMorse(payload);

      lcd.backlight();

      if (currentState != STATE_PANIC_SOS) {
        Serial.println(F("[STATUS] Message Transmission Complete."));
        lcd.clear();
        lcd.print(F("Message Sent!"));
        delay(1500);
        currentState = STATE_MAIN_MENU;
        drawMainMenu();
      }
      break;
    }

    case STATE_FLASHLIGHT:
      Serial.println(F("\n[ACTION] Flashlight Turned ON"));
      lcd.clear();
      lcd.print(F("Flashlight ON"));
      lcd.setCursor(0, 1);
      lcd.print(F("Press SELECT off"));
      digitalWrite(LED_PIN, HIGH);

      while (digitalRead(BTN_SELECT) == HIGH && currentState != STATE_PANIC_SOS && currentState != STATE_OFF) {
        checkPanic();
        checkPowerOff();
        delay(10);
      }

      digitalWrite(LED_PIN, LOW);
      if (currentState != STATE_PANIC_SOS && currentState != STATE_OFF) {
        Serial.println(F("\n[ACTION] Flashlight Turned OFF"));
        delay(300);
        currentState = STATE_MAIN_MENU;
        drawMainMenu();
      }
      break;

    case STATE_PANIC_SOS: {
      Serial.println(F("\n!!! [ALERT] PANIC MODE TRIGGERED !!!"));
      Serial.println(F("[STATUS] Going dark. Muting Buzzer and LED."));

      lcd.noBacklight();
      noTone(BUZZER_PIN);
      digitalWrite(LED_PIN, LOW);
      lcd.clear();
      stealthMode = true;

      char payload[64];
      sprintf(payload, "SOS %s", activeLocation);

      Serial.print(F("[SILENT TRANSMISSION] "));
      Serial.println(payload);
      transmitMorse(payload);

      Serial.println(F("[STATUS] Panic SOS Complete. Device Returning to Sleep."));
      currentState = STATE_OFF;
      break;
    }

    case STATE_LOW_BATT_WARNING:
      delay(3000);
      currentState = lastStateBeforeWarning;
      if (currentState == STATE_MAIN_MENU) drawMainMenu();
      else if (currentState == STATE_MSG_MENU) drawMsgMenu();
      break;
  }
}

void processSmartGpsStream() {
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  if (gps.location.isValid() && gps.location.isUpdated()) {
    hasFix = true;
    float realLat = gps.location.lat();
    float realLng = gps.location.lng();

    String locString = String(realLat, 4) + " " + String(realLng, 4);
    locString.toCharArray(activeLocation, 25);
  }
  else {
    hasFix = false;
    if (millis() - lastGpsCheckTime >= 3000) {
      lastGpsCheckTime = millis();
      fallbackLatFrac += 10;
      fallbackLngFrac += 10;
      sprintf(activeLocation, "10.%04d 76.%04d", fallbackLatFrac, fallbackLngFrac);
    }
  }
}

void updateBattery() {
  if (currentState == STATE_OFF) return;

  if (millis() - lastBatteryTick >= 60000) {
    lastBatteryTick = millis();
    if (batteryLevel > 0) {
      batteryLevel--;

      if (batteryLevel == 0) {
        Serial.println(F("\n[POWER] BATTERY DEPLETED - SHUTTING DOWN"));
        lcd.noBacklight();
        noTone(BUZZER_PIN);
        digitalWrite(LED_PIN, LOW);
        lcd.clear();
        currentState = STATE_OFF;
        return;
      }

      if ((batteryLevel == 20 || batteryLevel == 10 || batteryLevel == 5) && lastWarnedLevel != batteryLevel) {
        lastWarnedLevel = batteryLevel;
        lastStateBeforeWarning = currentState;
        currentState = STATE_LOW_BATT_WARNING;

        lcd.clear();
        // FIX: original text was 17 chars; now 15
        lcd.print(F("! LOW BATTERY !"));
        lcd.setCursor(0, 1);
        lcd.print(F("Level at: "));
        lcd.print(batteryLevel);
        lcd.print(F("%"));

        tone(BUZZER_PIN, 500, 300);
      }
    }
  }
}

void checkPowerOff() {
  if (digitalRead(BTN_ON) == LOW) {
    unsigned long pressTime = millis();
    while (digitalRead(BTN_ON) == LOW) {
      if (millis() - pressTime >= 1000) {
        Serial.println(F("\n[POWER] Device Turned OFF by User"));
        lcd.noBacklight();
        noTone(BUZZER_PIN);
        digitalWrite(LED_PIN, LOW);
        lcd.clear();
        currentState = STATE_OFF;
        while (digitalRead(BTN_ON) == LOW) { delay(10); }
        return;
      }
    }
  }
}

void checkPanic() {
  if (digitalRead(BTN_UP) == LOW && digitalRead(BTN_DOWN) == LOW) {
    unsigned long holdStart = millis();
    while (digitalRead(BTN_UP) == LOW && digitalRead(BTN_DOWN) == LOW) {
      if (millis() - holdStart >= 3000) {
        currentState = STATE_PANIC_SOS;
        return;
      }
    }
  }
}

void drawBattery() {
  lcd.setCursor(12, 0);
  if (batteryLevel < 100) lcd.print(F(" "));
  if (batteryLevel < 10) lcd.print(F(" "));
  lcd.print(batteryLevel);
  lcd.print(F("%"));
}

void drawMainMenu() {
  lcd.clear();
  if (mainMenuIndex == 0) {
    lcd.print(F(">1. SOS"));
    drawBattery();
    lcd.setCursor(0, 1);
    lcd.print(F(" 2. Msg"));
  } else if (mainMenuIndex == 1) {
    lcd.print(F(">2. Msg"));
    drawBattery();
    lcd.setCursor(0, 1);
    lcd.print(F(" 3. Light"));
  } else if (mainMenuIndex == 2) {
    lcd.print(F(">3. Light"));
    drawBattery();
    lcd.setCursor(0, 1);
    lcd.print(stealthMode ? F(" 4. Slt:ON ") : F(" 4. Slt:OFF"));
  } else if (mainMenuIndex == 3) {
    lcd.print(stealthMode ? F(">4. Slt:ON ") : F(">4. Slt:OFF"));
    drawBattery();
  }
}

void handleMainMenuInput() {
  if (digitalRead(BTN_DOWN) == LOW) {
    mainMenuIndex = (mainMenuIndex + 1) % MAIN_MENU_ITEMS;
    drawMainMenu();
    delay(250);
  }
  if (digitalRead(BTN_UP) == LOW) {
    mainMenuIndex = (mainMenuIndex - 1 + MAIN_MENU_ITEMS) % MAIN_MENU_ITEMS;
    drawMainMenu();
    delay(250);
  }
  if (digitalRead(BTN_SELECT) == LOW) {
    delay(250);
    if (mainMenuIndex == 0) currentState = STATE_SOS;
    else if (mainMenuIndex == 1) {
      currentState = STATE_MSG_MENU;
      msgMenuIndex = 0;
      drawMsgMenu();
    }
    else if (mainMenuIndex == 2) currentState = STATE_FLASHLIGHT;
    else if (mainMenuIndex == 3) {
      stealthMode = !stealthMode;
      Serial.print(F("\n[SETTINGS] Silent Mode toggled to: "));
      Serial.println(stealthMode ? F("ON") : F("OFF"));
      drawMainMenu();
    }
  }
}

void drawMsgMenu() {
  lcd.clear();
  lcd.print(F(">"));
  lcd.print(preDefMessages[msgMenuIndex]);
  drawBattery();

  if (msgMenuIndex < MSG_MENU_ITEMS - 1) {
    lcd.setCursor(0, 1);
    lcd.print(F(" "));
    lcd.print(preDefMessages[msgMenuIndex + 1]);
  }
}

void handleMsgMenuInput() {
  if (digitalRead(BTN_DOWN) == LOW) {
    msgMenuIndex = (msgMenuIndex + 1) % MSG_MENU_ITEMS;
    drawMsgMenu();
    delay(250);
  }
  if (digitalRead(BTN_UP) == LOW) {
    msgMenuIndex = (msgMenuIndex - 1 + MSG_MENU_ITEMS) % MSG_MENU_ITEMS;
    drawMsgMenu();
    delay(250);
  }
  if (digitalRead(BTN_SELECT) == LOW) {
    delay(250);
    if (msgMenuIndex == MSG_MENU_ITEMS - 1) {
      currentState = STATE_MAIN_MENU;
      drawMainMenu();
    } else {
      currentState = STATE_SENDING_MORSE;
    }
  }
}

void transmitMorse(const char* text) {
  for (unsigned int i = 0; text[i] != '\0'; i++) {
    if (currentState == STATE_PANIC_SOS && i > 2) return;

    char c = toupper(text[i]);

    if (c >= 'A' && c <= 'Z') {
      playMorseChar(morseLetters[c - 'A']);
    } else if (c >= '0' && c <= '9') {
      playMorseChar(morseNumbers[c - '0']);
    } else if (c == '.') {
      playMorseChar(".-.-.-");
    } else if (c == ' ') {
      delay(DOT_DUR * 7);
    }
  }
}

void playMorseChar(const char* sequence) {
  for (int i = 0; sequence[i] != '\0'; i++) {
    if (currentState == STATE_PANIC_SOS && !stealthMode) return;

    if (!stealthMode) {
      digitalWrite(LED_PIN, HIGH);
      tone(BUZZER_PIN, 1000);
    }

    if (sequence[i] == '.') {
      delay(DOT_DUR);
    } else if (sequence[i] == '-') {
      delay(DOT_DUR * 3);
    }

    digitalWrite(LED_PIN, LOW);
    noTone(BUZZER_PIN);

    delay(DOT_DUR);
  }
  delay(DOT_DUR * 2);
}