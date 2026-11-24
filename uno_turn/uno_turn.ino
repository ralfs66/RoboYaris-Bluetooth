// Arduino Uno R4 Minima → CL86Y
// D2 = STEP → PU+
// D3 = DIR  → DR+
// GND       → PU- and DR-

#define STEP_PIN 2
#define DIR_PIN 3

// One shaft turn. Match this to the CL86Y pulse/rev switches.
#define STEPS_PER_REV 3200

// 10 us pulse, 800 us between edges → about 2.6 seconds per turn.
#define PULSE_US 10
#define STEP_INTERVAL_US 800

void rotateMotor(bool clockwise) {
  digitalWrite(DIR_PIN, clockwise ? HIGH : LOW);
  delay(5);

  for (int i = 0; i < STEPS_PER_REV; i++) {
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(PULSE_US);
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(STEP_INTERVAL_US);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN, LOW);

  delay(2000);
  Serial.println("Uno ready");
}

void loop() {
  Serial.println("Clockwise");
  rotateMotor(true);
  delay(2000);

  Serial.println("Counterclockwise");
  rotateMotor(false);
  delay(2000);
}
