#include <ESP32Servo.h>

Servo servo;

const int servoPin = 25;
// 500 us = 0 deg, 2500 us = 180 deg. Sweep 120 deg around center.
const int sweepMin = 30;
const int sweepMax = 150;

void setup() {
  servo.setPeriodHertz(50);
  servo.attach(servoPin, 500, 2500);
  servo.write(sweepMin);
}

void loop() {
  servo.write(sweepMax);
  delay(2000);
  servo.write(sweepMin);
  delay(2000);
}
