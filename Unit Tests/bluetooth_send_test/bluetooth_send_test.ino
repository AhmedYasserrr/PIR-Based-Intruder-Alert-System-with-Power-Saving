#include <Crypto.h>
#include <AES.h>
#include <SoftwareSerial.h>
#include <string.h>

// Bluetooth SoftwareSerial pins
#define RX_PIN 10
#define TX_PIN 11

// Create a SoftwareSerial object for Bluetooth communication
SoftwareSerial bluetooth(RX_PIN, TX_PIN);

// Key (16 bytes for AES-128, must match the Python key)
byte key[16] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

// AES object
AES128 aes128;

// Function to add PKCS7 padding
void applyPadding(byte* data, size_t dataLength, size_t paddedLength) {
  byte paddingValue = paddedLength - dataLength; // Padding byte value
  for (size_t i = dataLength; i < paddedLength; i++) {
    data[i] = paddingValue;
  }
}

void setup() {
  Serial.begin(9600); // Initialize serial communication for debugging
  bluetooth.begin(9600); // Initialize Bluetooth communication

  while (!Serial) {
    ; // Wait for serial port to connect (useful for native USB boards)
  }

  aes128.setKey(key, 16); // Set the AES key

  Serial.println("Bluetooth AES encryption ready.");
  Serial.println("Enter plaintext to encrypt (max 16 characters):");
}

void loop() {
  if (Serial.available()) {
    // Read the plaintext input from Bluetooth
    String plaintext = Serial.readStringUntil('\n');
    plaintext.trim(); // Remove any unwanted whitespace
    Serial.print("Received plaintext: ");
    Serial.println(plaintext);

    // Calculate padding
    size_t plaintextLength = plaintext.length();
    size_t paddedLength = ((plaintextLength + 15) / 16) * 16; // Round up to nearest multiple of 16

    // Create padded plaintext buffer
    byte paddedPlaintext[paddedLength];
    memset(paddedPlaintext, 0, paddedLength); // Initialize with zeros
    memcpy(paddedPlaintext, plaintext.c_str(), plaintextLength);
    applyPadding(paddedPlaintext, plaintextLength, paddedLength);

    // Encrypt the padded plaintext
    byte encryptedData[paddedLength];
    for (size_t i = 0; i < paddedLength / 16; i++) {
      aes128.encryptBlock(
        &encryptedData[i * 16],
        &paddedPlaintext[i * 16]
      );
    }

    // Convert encrypted data to hex and send over Bluetooth
    Serial.print("Encrypted data (hex): ");
    for (size_t i = 0; i < paddedLength; i++) {
      if (encryptedData[i] < 16) {
        Serial.print('0');
        bluetooth.print('0');
      }
      Serial.print(encryptedData[i], HEX);
      bluetooth.print(encryptedData[i], HEX);
    }
    Serial.println();
    // bluetooth.println();

    Serial.println("Waiting for next plaintext...");
  }
}
