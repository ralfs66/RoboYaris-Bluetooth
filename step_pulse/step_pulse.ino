#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define STEP_PIN 25
#define DIR_PIN 26
#define STEP_LEDC_CH 0

#define GAS_PIN 18
#define BRAKE_PIN 2
#define CAM_PIN 19
#define GEAR_PIN 32
#define GAS_LEDC_CH 8
#define BRAKE_LEDC_CH 9
#define CAM_LEDC_CH 10
#define GEAR_LEDC_CH 11

#define SERVO_HZ 50
#define SERVO_BITS 14
#define SERVO_PERIOD_US 20000
#define SERVO_MIN_US 500
#define SERVO_MAX_US 2500
#define GAS_REST_DEG 90
#define BRAKE_REST_DEG 45
#define BRAKE_MAX_DEG 180
#define GAS_STROKE_DEG 65
#define BRAKE_STROKE_DEG 45
#define CAM_CENTER_DEG 90
#define CAM_MAX_DEG 90

#define STEPS_PER_REV 3200

// Junior hockey puck driving a Yaris wheel, about 1:7.
#define WHEEL_RATIO 7.0f
// Full stick: one wheel turn in about 10 s. Center of the stick stays slow.
#define MAX_WHEEL_DPS (140.0f / 4.0f)
#define MIN_WHEEL_DPS 4.0f

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define JOY_TIMEOUT_MS 200
// Brake stays down this long before the gearbox servo is allowed to move.
#define GEAR_SHIFT_HOLD_MS 2000UL
#define GEAR_SHIFT_BRAKE 100

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

bool oledOk = false;
int joyX = 0;
int gasCmd = 0;
int brakeCmd = 0;
int camCmd = 0;
int gearCmd = 0;
int gearServo = 0;
int gearHoldTarget = -1;
unsigned long gearHoldStartMs = 0;
int brakeOut = 0;
unsigned long lastJoyMs = 0;
int shownJoy = 999;
int shownGas = -1;
int shownBrake = -1;
int shownCam = -999;
int shownGear = -1;
bool shownLink = false;
bool shownAlarm = false;
bool pulsing = false;
bool dirCw = true;
uint32_t currentFreq = 0;

char lineBuf[48];
int lineLen = 0;

bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool startOled() {
  Wire.begin(21, 22);
  Wire.setClock(400000);
  if (!i2cPresent(0x3C) || !display.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false)) {
    return false;
  }
  oledOk = true;
  return true;
}

void showStatus(bool force) {
  bool link = (millis() - lastJoyMs) <= JOY_TIMEOUT_MS;
  bool alarm = !link;
  if (!oledOk || (!force && joyX == shownJoy && link == shownLink && alarm == shownAlarm &&
                  gasCmd == shownGas && brakeOut == shownBrake && camCmd == shownCam &&
                  gearServo == shownGear)) return;
  shownJoy = joyX;
  shownLink = link;
  shownAlarm = alarm;
  shownGas = gasCmd;
  shownBrake = brakeOut;
  shownCam = camCmd;
  shownGear = gearServo;

  int speed = abs(joyX);
  bool moving = speed >= 6 && !alarm;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 4);
  if (alarm) display.print("NO LINK");
  else if (!moving) display.print("STOP");
  else display.print(joyX > 0 ? "RIGHT" : "LEFT");

  display.setTextSize(2);
  display.setCursor(0, 22);
  if (alarm || !moving) display.print("0%");
  else {
    display.print(speed);
    display.print("%");
  }

  display.setTextSize(1);
  display.setCursor(0, 46);
  display.print("USB");
  display.setCursor(0, 56);
  display.print("G");
  display.print(link ? gasCmd : 0);
  display.print(" B");
  display.print(link ? brakeOut : 0);
  display.print(" C");
  display.print(link ? camCmd : 0);
  display.print(" ");
  display.print("PRND"[constrain(link ? gearServo : 0, 0, 3)]);
  display.display();
}

int degToUs(int deg) {
  return map(constrain(deg, 0, 180), 0, 180, SERVO_MIN_US, SERVO_MAX_US);
}

// Stick centered stays straight ahead. Full stick is 90° left or 90° right.
int camUs(int command) {
  int cmd = constrain(command, -100, 100);
  int deg = CAM_CENTER_DEG - (cmd * CAM_MAX_DEG) / 100;
  return degToUs(deg);
}

// Park to Drive is 150° of the servo's 180° range. Reverse and Neutral sit between.
// Park stays at the high end. The unused 30° is left on the Drive side.
int gearUs(int gear) {
  int deg = constrain(gear, 0, 3) * 50;
  int driveUs = SERVO_MIN_US + (SERVO_MAX_US - SERVO_MIN_US) * 30 / 180;
  return map(deg, 0, 150, SERVO_MAX_US, driveUs);
}

int pedalUs(int command, int restDeg, int strokeDeg, int maxDeg) {
  int deg = restDeg + (constrain(command, 0, 100) * strokeDeg) / 100;
  return map(constrain(deg, 0, maxDeg), 0, maxDeg, SERVO_MIN_US, SERVO_MAX_US);
}

void servoWriteRaw(int pin, int us) {
  uint32_t duty = ((uint32_t)us * ((1UL << SERVO_BITS) - 1UL)) / SERVO_PERIOD_US;
  ledcWrite(pin, duty);
}

void servoWriteUs(int pin, int us) {
  servoWriteRaw(pin, constrain(us, SERVO_MIN_US, SERVO_MAX_US));
}

void attachServo(int pin, int channel, int us) {
  ledcAttachChannel(pin, SERVO_HZ, SERVO_BITS, channel);
  servoWriteUs(pin, us);
}

void driveServos(bool linkOk) {
  int g = linkOk ? gasCmd : 0;
  int b = linkOk ? brakeCmd : 0;
  int c = linkOk ? camCmd : 0;
  int gear = linkOk ? gearCmd : 0;

  // Press the brake, hold 2 s, then move the gearbox. The brake stays on for
  // that move and follows the driver again on the next pass.
  if (!linkOk) {
    gearHoldTarget = -1;
    gearServo = 0;
  } else if (gear != gearServo) {
    if (gearHoldTarget != gear) {
      gearHoldTarget = gear;
      gearHoldStartMs = millis();
    }
    b = GEAR_SHIFT_BRAKE;
    if (millis() - gearHoldStartMs >= GEAR_SHIFT_HOLD_MS) {
      gearServo = gear;
      gearHoldTarget = -1;
    }
  } else {
    gearHoldTarget = -1;
  }

  brakeOut = b;
  servoWriteUs(GAS_PIN, pedalUs(g, GAS_REST_DEG, -GAS_STROKE_DEG, 180));
  servoWriteUs(BRAKE_PIN, pedalUs(b, BRAKE_REST_DEG, -BRAKE_STROKE_DEG, BRAKE_MAX_DEG));
  servoWriteUs(CAM_PIN, camUs(c));
  servoWriteUs(GEAR_PIN, gearUs(gearServo));
}

void stopPulses() {
  if (pulsing) {
    ledcWrite(STEP_PIN, 0);
    ledcDetach(STEP_PIN);
    pulsing = false;
    currentFreq = 0;
  }
  pinMode(STEP_PIN, OUTPUT);
  digitalWrite(STEP_PIN, LOW);
}

void applyPulses(float stepsPerSec) {
  if (fabsf(stepsPerSec) < 20.0f) {
    stopPulses();
    return;
  }

  bool cw = stepsPerSec > 0;
  if (!pulsing || cw != dirCw) {
    stopPulses();
    dirCw = cw;
    digitalWrite(DIR_PIN, cw ? HIGH : LOW);
    delayMicroseconds(200);
  }

  uint32_t freq = (uint32_t)fabsf(stepsPerSec);
  if (freq < 20) freq = 20;
  if (!pulsing) {
    ledcAttachChannel(STEP_PIN, freq, 8, STEP_LEDC_CH);
    ledcWrite(STEP_PIN, 128);
    pulsing = true;
    currentFreq = freq;
  } else if (freq != currentFreq) {
    ledcChangeFrequency(STEP_PIN, freq, 8);
    ledcWrite(STEP_PIN, 128);
    currentFreq = freq;
  }
}

void applyCommand(int x, int g, int b, int c, bool hasCam, int gear, bool hasGear) {
  joyX = constrain(x, -100, 100);
  gasCmd = constrain(g, 0, 100);
  brakeCmd = constrain(b, 0, 100);
  if (hasCam) camCmd = constrain(c, -100, 100);
  if (hasGear) gearCmd = constrain(gear, 0, 3);
  lastJoyMs = millis();
  if (abs(joyX) < 6) stopPulses();
}

void parseLine(char *line) {
  int x = 0, g = 0, b = 0, c = 0, gear = 0;
  int n = sscanf(line, "%d %d %d %d %d", &x, &g, &b, &c, &gear);
  if (n >= 3) applyCommand(x, g, b, c, n >= 4, gear, n >= 5);
}

void readUsb() {
  while (Serial.available() > 0) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = 0;
        parseLine(lineBuf);
        lineLen = 0;
      }
    } else if (lineLen < (int)sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = ch;
    } else {
      lineLen = 0;
    }
  }
}

void serviceMotor(bool linkOk) {
  if (!linkOk) {
    stopPulses();
    return;
  }

  float target = 0;
  int mag = abs(joyX);
  if (mag >= 6) {
    float u = (mag - 6) / 94.0f;
    u = powf(u, 2.8f);
    float wheelDps = MIN_WHEEL_DPS + (MAX_WHEEL_DPS - MIN_WHEEL_DPS) * u;
    float rate = wheelDps / 360.0f * STEPS_PER_REV * WHEEL_RATIO;
    target = joyX > 0 ? -rate : rate;
  }
  applyPulses(target);
}

void setup() {
  Serial.begin(115200);
  Serial.setRxBufferSize(1024);
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN, LOW);
  attachServo(GAS_PIN, GAS_LEDC_CH, pedalUs(0, GAS_REST_DEG, -GAS_STROKE_DEG, 180));
  attachServo(BRAKE_PIN, BRAKE_LEDC_CH, pedalUs(0, BRAKE_REST_DEG, -BRAKE_STROKE_DEG, BRAKE_MAX_DEG));
  attachServo(CAM_PIN, CAM_LEDC_CH, camUs(0));
  attachServo(GEAR_PIN, GEAR_LEDC_CH, gearUs(0));
  driveServos(false);
  startOled();
  lastJoyMs = 0;
  showStatus(true);
  Serial.println("USB READY");
}

void loop() {
  readUsb();
  bool linkOk = (millis() - lastJoyMs) <= JOY_TIMEOUT_MS;
  if (!linkOk) {
    joyX = 0;
    gasCmd = 0;
    brakeCmd = 0;
    camCmd = 0;
    gearCmd = 0;
  }
  serviceMotor(linkOk);
  driveServos(linkOk);
  showStatus(false);
}
