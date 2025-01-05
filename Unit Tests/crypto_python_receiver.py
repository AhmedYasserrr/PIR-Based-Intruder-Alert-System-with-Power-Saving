from Crypto.Cipher import AES
from Crypto.Util.Padding import unpad
import serial
import binascii

# AES key (matches the key in the Arduino code)
aes_key = bytes([
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
])

cipher = AES.new(aes_key, AES.MODE_ECB)

bluetooth_serial = serial.Serial('COM5', 9600, timeout=1)
print("Waiting for encrypted data from Arduino...")

while True:
    if bluetooth_serial.in_waiting > 0:
        encrypted_hex = bluetooth_serial.readline().decode().strip()
        print(f"Received encrypted data (hex): {encrypted_hex}")

        try:
            # Convert hex string to binary data
            encrypted_data = binascii.unhexlify(encrypted_hex)

            decrypted_data = cipher.decrypt(encrypted_data)

            # Unpad the decrypted data to retrieve the original plaintext
            plaintext = unpad(decrypted_data, AES.block_size).decode()

            cleaned_str = ''.join(char for char in plaintext if char.isalnum() or char.isspace() or char in '.:,;?!')

            print(cleaned_str)
        except Exception as e:
            print(f"Error during decryption: {e}")
