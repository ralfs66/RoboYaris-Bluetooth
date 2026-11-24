#define GAS_PIN 18
#define BRAKE_PIN 2
#define GAS_LEDC_CH 8
#define BRAKE_LEDC_CH 9

#define SERVO_HZ 50
#define SERVO_BITS 14
#define SERVO_PERIOD_US 20000
#define SERVO_MIN_US 500
#define SERVO_MAX_US 2500

void writeDeg(int pin, int deg) {
  int us = map(constrain(deg, 0, 180), 0, 180, SERVO_MIN_US, SERVO_MAX_US);
  uint32_t duty = ((uint32_t)us * ((1UL << SERVO_BITS) - 1UL)) / SERVO_PERIOD_US;
  ledcWrite(pin, duty);
}

void setup() {
  Serial.begin(115200);
  ledcAttachChannel(GAS_PIN, SERVO_HZ, SERVO_BITS, GAS_LEDC_CH);
  ledcAttachChannel(BRAKE_PIN, SERVO_HZ, SERVO_BITS, BRAKE_LEDC_CH);
  writeDeg(GAS_PIN, 0);
  writeDeg(BRAKE_PIN, 0);
  Serial.println("GAS 0  BRAKE 0");
}

void loop() {
  writeDeg(GAS_PIN, 0);
  writeDeg(BRAKE_PIN, 0);
  delay(20);
}
