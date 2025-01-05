#include <EEPROM.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <Crypto.h>
#include <AES.h>
#include <SoftwareSerial.h>
#include <string.h>
#include <assert.h>
#include <avr/wdt.h>

#define PIR_PIN 2
#define LED_PIN 13
#define RX_PIN 10
#define TX_PIN 11

#define MAX_VIOLATIONS_PER_DAY  50
#define MAX_NO_MOTION_COUNT     150   // Approx. 20 minutes

bool motionDetected = false;
int violationCount = 0;
int timerCounter = 0;

unsigned long previousTime;
unsigned long lastMotionTime;

const unsigned long dayDuration = 86400000;
const int eepromAddr = 0;

SoftwareSerial bluetooth(RX_PIN, TX_PIN);

// Key (16 bytes for AES-128, must match the Python key)
byte key[16] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

AES128 aes128;

struct SystemState {
  int violationCount;
  unsigned long millisOffset;
} state;

void setup() {
  Serial.begin(9600);    
  bluetooth.begin(9600); 

  aes128.setKey(key, 16);

  pinMode(PIR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);

  // Attach an interrupt to the PIR_PIN on rising edge detection
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), motionInterrupt, RISING);

  // Load system state from EEPROM
  EEPROM.get(eepromAddr, state);

  // Validate the state values are within valid ranges
  if (state.violationCount < 0 || state.violationCount > MAX_VIOLATIONS_PER_DAY || state.millisOffset < 0) {
    state.violationCount = 0;
  }
  
  previousTime = millis();
  lastMotionTime = millis(); // Initialize last motion time

  delay(1500);
  processPlaintext("Resuming with violations: " + String(state.violationCount));
  delay(1500);
  processPlaintext("Millis offset: " + String(state.millisOffset));

  setupWatchdogTimer();
}

void loop() {
  unsigned long currentTime = millis();
  unsigned long elapsedMillis = currentTime - previousTime;
  previousTime = currentTime;

  state.millisOffset += elapsedMillis;
  EEPROM.put(eepromAddr, state);

  // Reset violation count after 24 hours
  if (state.millisOffset >= dayDuration) {
    state.violationCount = 0;
    state.millisOffset = 0;
    EEPROM.put(eepromAddr, state);
    processPlaintext("24-hour period reset.");
  }

  if (timerCounter >= MAX_NO_MOTION_COUNT) {  
    timerCounter = 0;  
    EEPROM.put(eepromAddr, state);
    processPlaintext("PIR malfunction: No motion");
    manualReset();
  }

  if (motionDetected) {
    motionDetected = false;
    state.violationCount++;
    EEPROM.put(eepromAddr, state);

    processPlaintext("Violations Count: " + String(state.violationCount));

    digitalWrite(LED_PIN, HIGH);
    delay(3000);
    digitalWrite(LED_PIN, LOW);

    lastMotionTime = millis(); // Update last motion time
  }

  enterSleepMode();
}

void motionInterrupt() {
  motionDetected = true;
  // Reset wdt counter and timerCounter
  wdt_reset();            
}

void enterSleepMode() {
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
  timerCounter++;  
}
void manualReset() {
  wdt_enable(WDTO_15MS);
  while (true);
}

void applyPadding(byte* data, size_t dataLength, size_t paddedLength) {
    assert(paddedLength >= dataLength && "Padded length must be greater or equal to data length.");
    byte paddingValue = paddedLength - dataLength;
    for (size_t i = dataLength; i < paddedLength; i++) {
        data[i] = paddingValue;
    }
}

void encryptData(const byte* plaintext, size_t paddedLength, byte* encryptedData) {
  assert(paddedLength % 16 == 0 && "Padded length must be a multiple of 16.");
  for (size_t i = 0; i < paddedLength / 16; i++) {
    aes128.encryptBlock(&encryptedData[i * 16], &plaintext[i * 16]);
  }
}

void sendEncryptedData(const byte* encryptedData, size_t length) {
  for (size_t i = 0; i < length; i++) {
    if (encryptedData[i] < 16) {
      bluetooth.print('0');
    }
    bluetooth.print(encryptedData[i], HEX);
  }
}

void processPlaintext(const String& plaintext) {
  Serial.println(plaintext);

  // Calculate padding
  size_t plaintextLength = plaintext.length();
  size_t paddedLength = ((plaintextLength + 15) / 16) * 16;

  // Create padded plaintext buffer
  byte paddedPlaintext[paddedLength];
  memset(paddedPlaintext, 0, paddedLength);
  memcpy(paddedPlaintext, plaintext.c_str(), plaintextLength);
  applyPadding(paddedPlaintext, plaintextLength, paddedLength);

  // Encrypt the padded plaintext
  byte encryptedData[paddedLength];
  encryptData(paddedPlaintext, paddedLength, encryptedData);

  // Send the encrypted data
  sendEncryptedData(encryptedData, paddedLength);
}
