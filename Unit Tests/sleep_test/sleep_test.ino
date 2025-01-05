#include <EEPROM.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>

#define PIR_PIN 2
#define LED_PIN 13

volatile bool motionDetected = false;
int violationCount = 0;

unsigned long dayStartTime;
unsigned long previousTime;

const unsigned long dayDuration = 86400000;
const int eepromAddr = 0;

struct SystemState {
  int violationCount;
  unsigned long millisOffset;
} state;

void setup() {
  Serial.begin(9600);
  pinMode(PIR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);

  attachInterrupt(digitalPinToInterrupt(PIR_PIN), motionInterrupt, RISING);

  // Load system state from EEPROM
  EEPROM.get(eepromAddr, state);

  if (state.violationCount < 0 || state.violationCount > 1000) {
    state.violationCount = 0;
  }
  
  dayStartTime = millis();
  previousTime = millis();

  Serial.println("Setup complete.");
  Serial.print("Resuming with violations: ");
  Serial.println(state.violationCount);
  Serial.print("Millis offset: ");
  Serial.println(state.millisOffset);
}

void loop() {
  unsigned long currentTime = millis();
  unsigned long elapsedMillis = currentTime - previousTime;
  previousTime = currentTime;

  state.millisOffset += elapsedMillis;
  EEPROM.put(eepromAddr, state);

  if (state.millisOffset - dayStartTime >= dayDuration) {
    state.violationCount = 0;
    state.millisOffset = 0;
    EEPROM.put(eepromAddr, state);
    dayStartTime = millis();
    Serial.println("24-hour period reset.");
  }

  Serial.print("motionDetected b: ");
  Serial.println(motionDetected);
  if (motionDetected) {
    motionDetected = false;
    Serial.print("motionDetected A: ");
    Serial.println(motionDetected);

    state.violationCount++;
    EEPROM.put(eepromAddr, state);
    Serial.print("Violation detected. Count: ");
    Serial.println(state.violationCount);

    digitalWrite(LED_PIN, HIGH);
    delay(5000);
    digitalWrite(LED_PIN, LOW);
  }

  enterSleepMode();
}

void motionInterrupt() {
  motionDetected = true;
}

void enterSleepMode() {
  Serial.println("Entering sleep mode...");
  delay(100);
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();
  digitalWrite(LED_PIN, LOW);
  sleep_cpu();
  sleep_disable();
  Serial.println("Waking up from sleep...");
}
