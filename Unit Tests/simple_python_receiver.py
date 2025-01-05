import serial

try:
    bluetooth_serial = serial.Serial('COM5', 9600, timeout=1)  
    print("Waiting for encrypted data from Arduino...")
except serial.SerialException as e:
    print(f"Error: Could not open serial port: {e}")
    exit()

try:
    while True:
        if bluetooth_serial.in_waiting > 0:
            text = bluetooth_serial.readline().decode().strip()
            if text:
                print(f"Received: {text}")
except KeyboardInterrupt:
    print("\nExiting...")
finally:
    bluetooth_serial.close()
    print("Serial port closed.")
