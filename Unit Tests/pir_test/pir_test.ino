void setup() {
  pinMode(A0, INPUT);
  pinMode(13, OUTPUT);
  // Start serial communication at a baud rate of 9600
  Serial.begin(9600);
}

void loop() {
  // Read the analog value from pin A0 (can be any analog pin)
  int sensorValue = analogRead(A0);

  if(sensorValue > 500) digitalWrite(13, 1);
  else digitalWrite(13, 0);
  // Print the value to the serial monitor
  Serial.println(sensorValue);

  // Add a small delay to make the output more readable
  delay(500);
}
