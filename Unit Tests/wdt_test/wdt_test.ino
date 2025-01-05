#include <EEPROM.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>

#define PIR_PIN 2
#define LED_PIN 13

volatile bool motionDetected = false;
int violationCount = 0;
unsigned long lastTriggerTime = 0;
unsigned long dayStartTime;

const unsigned long dayDuration = 86400000;
const int eepromAddr = 0;

volatile int watchdogCounter = 0;
const int watchdogMaxCounter = 12;

void setup() {
  Serial.begin(9600);
  pinMode(PIR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);

  attachInterrupt(digitalPinToInterrupt(PIR_PIN), motionInterrupt, RISING);

  violationCount = EEPROM.read(eepromAddr);
  if (violationCount < 0 || violationCount > 1000) {
    violationCount = 0;
    EEPROM.put(eepromAddr, violationCount);
  }
  dayStartTime = millis();
  setupWatchdogTimer();
  Serial.println("Setup complete.");
}

void loop() {
  wdt_reset();

  if (watchdogCounter >= watchdogMaxCounter) {
    watchdogCounter = 0;
    manualReset();
  }

  if (millis() - dayStartTime >= dayDuration) {
    violationCount = 0;
    EEPROM.put(eepromAddr, violationCount);
    dayStartTime = millis();
    Serial.println("24-hour period reset.");
  }

  Serial.print("motionDetected b: ");
  Serial.println(motionDetected);
  if (motionDetected) {
    motionDetected = false;
    Serial.print("motionDetected A: ");
    Serial.println(motionDetected);

    violationCount++;
    EEPROM.put(eepromAddr, violationCount);
    Serial.print("Violation detected. Count: ");
    Serial.println(violationCount);

    digitalWrite(LED_PIN, HIGH);
    delay(5000);
    digitalWrite(LED_PIN, LOW);

    lastTriggerTime = millis();
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

void setupWatchdogTimer() {
  wdt_enable(WDTO_8S);
  cli();
  WDTCSR |= (1 << WDIE);
  sei();
}

ISR(WDT_vect) {
  watchdogCounter++;
}

void manualReset() {
  cli();
  wdt_enable(WDTO_15MS);
  while (1);
}
