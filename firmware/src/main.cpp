#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  Serial.println("hello world");
}

void loop() {
  Serial.println("I'm alive");
  delay(1000);
}