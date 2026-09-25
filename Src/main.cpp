/*
 * ============================================================
 * AUTOMATED COMMERCIAL MICRO-CLIMATE NURSERY
 * ============================================================
 *
 * The ESP32 monitors temperature, humidity and natural light
 * and controls a servo-operated ventilation window and
 * supplemental lighting.
 *
 * MAIN CONTROL PRIORITY:
 *     SAFETY > MANUAL > AUTO
 *
 * SAFETY:
 *     Invalid DHT22 data -> Safety Mode -> vent OPEN + warning.
 *
 * MANUAL:
 *     Button toggles Auto <-> Manual. Manual mode keeps the vent OPEN.
 *
 * AUTOMATIC:
 *     Temperature controls the vent.
 *     LDR light level controls supplemental lighting.
 *
 * The GPIO mapping and working control behaviour are unchanged.
 *
 * ============================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ESP32Servo.h>

// ============================================================
// PIN DEFINITIONS
// ============================================================

#define DHT_PIN 4
#define DHT_TYPE DHT22

#define LDR_PIN 34

#define SERVO_PIN 18

#define BUTTON_PIN 27

#define RED_LED 25
#define GREEN_LED 26
#define BLUE_LED 32
#define YELLOW_LED 14
#define ORANGE_LED 13

#define BUZZER_PIN 12

#define OLED_SDA 21
#define OLED_SCL 22

// ============================================================
// OLED SETTINGS
// ============================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  -1
);

// ============================================================
// SENSOR / SERVO OBJECTS
// ============================================================

DHT dht(DHT_PIN, DHT_TYPE);

Servo ventServo;

// ============================================================
// SYSTEM MODES
// ============================================================

enum SystemMode
{
  AUTO_MODE,
  MANUAL_MODE,
  SAFETY_MODE
};

SystemMode currentMode = AUTO_MODE;

// ============================================================
// TEMPERATURE THRESHOLDS
// ============================================================

const float TEMP_OPEN = 30.0;
const float TEMP_CLOSE = 28.0;

// ============================================================
// LIGHT THRESHOLD
// ============================================================

// IMPORTANT LDR BEHAVIOUR:
// In this Wokwi photoresistor module, a higher ADC value means less light.
// Therefore:
//     LDR > 700  -> dark / low light
//     LDR <= 700 -> bright / enough natural light
// The threshold is kept simple for this prototype.

const int LIGHT_THRESHOLD = 700;

// ============================================================
// SERVO ANGLES
// ============================================================

const int VENT_CLOSED_ANGLE = 0;
const int VENT_OPEN_ANGLE = 90;

// ============================================================
// VARIABLES
// ============================================================

float temperature = 0.0;
float humidity = 0.0;

int lightLevel = 0;

bool ventOpen = false;
bool supplementalLight = false;

bool sensorFault = false;

bool lastButtonState = HIGH;

// ============================================================
// TIMER VARIABLES
// ============================================================

unsigned long lastButtonTime = 0;
unsigned long lastSensorRead = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastSerialUpdate = 0;

// ============================================================
// TIMING SETTINGS
// ============================================================

const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long DISPLAY_INTERVAL = 500;
const unsigned long SERIAL_INTERVAL = 2000;
const unsigned long BUTTON_DEBOUNCE = 200;

// ============================================================
// FUNCTION: setVent
// ============================================================

/**
 * @brief Opens or closes the greenhouse ventilation vent.
 *
 * @param open true to open the vent, false to close the vent.
 * @return void
 */
void setVent(bool open)
{
  // This is the single function used to change the physical vent position.
  // Other parts of the program request OPEN/CLOSED by calling setVent().
  //     setVent(true)  -> open the vent
  //     setVent(false) -> close the vent
  ventOpen = open;

  if (open)
  {
    ventServo.write(VENT_OPEN_ANGLE);
  }
  else
  {
    ventServo.write(VENT_CLOSED_ANGLE);
  }
}

// ============================================================
// FUNCTION: updateBuzzer
// ============================================================

/**
 * @brief Controls the safety warning buzzer.
 *
 * The buzzer repeatedly beeps while the system
 * is in Safety Mode.
 *
 * @return void
 */
void updateBuzzer()
{
  // These static variables remember their values between calls.
  // This lets the buzzer beep without using delay(), so the rest of
  // the system can continue running at the same time.
  static unsigned long lastBeepTime = 0;
  static bool beepState = false;

  // Safety mode
  if (currentMode == SAFETY_MODE)
  {
    if (millis() - lastBeepTime >= 500)
    {
      lastBeepTime = millis();

      beepState = !beepState;

      if (beepState)
      {
        // 2 kHz warning tone
        tone(BUZZER_PIN, 2000);
      }
      else
      {
        noTone(BUZZER_PIN);
      }
    }
  }
  else
  {
    // Buzzer OFF in Auto and Manual
    beepState = false;
    noTone(BUZZER_PIN);
  }
}

// ============================================================
// FUNCTION: updateLEDs
// ============================================================

/**
 * @brief Updates LEDs according to the current system state.
 *
 * Green  = Auto mode
 * Blue   = Manual mode
 * Red    = Safety mode
 * Yellow = Supplemental lighting
 * Orange = Vent open
 *
 * @return void
 */
void updateLEDs()
{
  // Turn all LEDs OFF first
  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(BLUE_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(ORANGE_LED, LOW);

  // ==========================================================
  // MODE LED
  // ==========================================================

  if (currentMode == AUTO_MODE)
  {
    digitalWrite(GREEN_LED, HIGH);
  }
  else if (currentMode == MANUAL_MODE)
  {
    digitalWrite(BLUE_LED, HIGH);
  }
  else if (currentMode == SAFETY_MODE)
  {
    digitalWrite(RED_LED, HIGH);
  }

  // ==========================================================
  // SUPPLEMENTAL LIGHT
  // ==========================================================

  if (supplementalLight)
  {
    digitalWrite(YELLOW_LED, HIGH);
  }

  // ==========================================================
  // VENT STATUS
  // ==========================================================

  if (ventOpen)
  {
    digitalWrite(ORANGE_LED, HIGH);
  }
}

// ============================================================
// FUNCTION: readSensors
// ============================================================

/**
 * @brief Reads temperature, humidity and light level.
 *
 * Invalid DHT22 readings activate Safety Mode.
 *
 * @return void
 */
void readSensors()
{
  // Read into temporary variables first. The values are copied into the
  // main variables only after the DHT22 readings pass the validity checks.
  float newTemperature = dht.readTemperature();
  float newHumidity = dht.readHumidity();

  // ==========================================================
  // CHECK DHT22
  // ==========================================================

  // The DHT22 is the primary safety sensor. NaN means the sensor did not
  // return a valid number. Range checks reject values that are not valid
  // for this project. If the check fails, automatic control is stopped
  // and the system immediately enters Safety Mode.
  if (isnan(newTemperature) ||
      isnan(newHumidity) ||
      newHumidity < 0 ||
      newHumidity > 100 ||
      newTemperature < -40 ||
      newTemperature > 80)
  {
    sensorFault = true;

    temperature = NAN;
    humidity = NAN;

    // Safety Mode
    currentMode = SAFETY_MODE;

    // Safe posture = vent OPEN
    setVent(true);

    // Supplemental light OFF
    supplementalLight = false;

    Serial.println();
    Serial.println("!!! SENSOR FAILURE !!!");
    Serial.println("SAFETY MODE ACTIVATED");
    Serial.println("VENT OPEN");
    Serial.println("BUZZER ON");

    return;
  }

  // ==========================================================
  // VALID DHT22 DATA
  // ==========================================================

  temperature = newTemperature;
  humidity = newHumidity;

  sensorFault = false;

  // ==========================================================
  // READ LDR
  // ==========================================================

  // GPIO 34 is an ESP32 ADC input. analogRead() converts the LDR voltage
  // into a numeric ADC value used by the light-control logic.
  lightLevel = analogRead(LDR_PIN);

  // ==========================================================
  // SENSOR RECOVERY
  // ==========================================================

  if (currentMode == SAFETY_MODE)
  {
    currentMode = AUTO_MODE;

    Serial.println();
    Serial.println("DHT22 RECOVERED");
    Serial.println("MODE CHANGED: AUTO");
  }
}

// ============================================================
// FUNCTION: handleButton
// ============================================================

/**
 * @brief Handles the manual override push button.
 *
 * AUTO -> MANUAL
 * MANUAL -> AUTO
 *
 * Safety Mode has priority.
 *
 * @return void
 */
void handleButton()
{
  // The button uses INPUT_PULLUP:
  //     HIGH = released
  //     LOW  = pressed
  // The HIGH -> LOW transition is detected so holding the button does
  // not repeatedly switch the operating mode.
  bool buttonState = digitalRead(BUTTON_PIN);

  // Detect new button press
  if (buttonState == LOW && lastButtonState == HIGH)
  {
    if (millis() - lastButtonTime >= BUTTON_DEBOUNCE)
    {
      lastButtonTime = millis();

      // ======================================================
      // SAFETY MODE
      // ======================================================

      if (currentMode == SAFETY_MODE)
      {
        // Safety mode cannot be overridden
        setVent(true);

        Serial.println("BUTTON PRESSED");
        Serial.println("SAFETY MODE ACTIVE");
      }

      // ======================================================
      // AUTO -> MANUAL
      // ======================================================

      else if (currentMode == AUTO_MODE)
      {
        currentMode = MANUAL_MODE;

        // Manual override opens vent
        setVent(true);

        Serial.println("BUTTON PRESSED");
        Serial.println("MODE CHANGED: MANUAL");
      }

      // ======================================================
      // MANUAL -> AUTO
      // ======================================================

      else if (currentMode == MANUAL_MODE)
      {
        currentMode = AUTO_MODE;

        Serial.println("BUTTON PRESSED");
        Serial.println("MODE CHANGED: AUTO");
      }

      updateLEDs();
    }
  }

  lastButtonState = buttonState;
}

// ============================================================
// FUNCTION: runAutomaticControl
// ============================================================

/**
 * @brief Applies automatic temperature and light control.
 *
 * Temperature controls the ventilation servo.
 * LDR controls the supplemental lighting.
 *
 * @return void
 */
void runAutomaticControl()
{
  // Automatic rules are allowed to run only in AUTO_MODE. This prevents
  // automatic control from overriding Manual Override or Safety Mode.
  if (currentMode != AUTO_MODE)
  {
    return;
  }

  // ==========================================================
  // TEMPERATURE CONTROL
  // ==========================================================

  // Temperature controls the ventilation window.
  // At 30°C or above, the nursery is considered too hot -> OPEN the vent.
  // At 28°C or below, it has cooled enough -> CLOSE the vent.
  // Between 28°C and 30°C, keep the previous position.
  //
  // This gap is called HYSTERESIS. It prevents the servo from repeatedly
  // opening and closing when the temperature is close to a threshold.
  if (temperature >= TEMP_OPEN)
  {
    // Temperature too high
    setVent(true);
  }
  else if (temperature <= TEMP_CLOSE)
  {
    // Temperature sufficiently low
    setVent(false);
  }

  // Between 28°C and 30°C:
  // Keep previous vent position.

  // ==========================================================
  // SIMPLE LIGHT CONTROL
  // ==========================================================

  // The LDR controls only the supplemental lighting.
  // In this Wokwi setup, a higher ADC value means darker conditions.
  if (lightLevel > LIGHT_THRESHOLD)
  {
    // Low natural light -> turn supplemental light ON.
    supplementalLight = true;
  }
  else
  {
    // Enough natural light -> turn supplemental light OFF.
    supplementalLight = false;
  }
}

// ============================================================
// FUNCTION: runManualControl
// ============================================================

/**
 * @brief Applies manual override behaviour.
 *
 * Manual mode keeps the vent open.
 * Lighting continues to respond to the LDR.
 *
 * @return void
 */
void runManualControl()
{
  // Manual Override controls the ventilation, but the LDR lighting
  // function can still operate. The worker is overriding automatic
  // ventilation rather than disabling the whole system.
  if (currentMode == MANUAL_MODE)
  {
    // Manual mode keeps vent OPEN
    setVent(true);

    // Lighting still follows LDR
    if (lightLevel > LIGHT_THRESHOLD)
    {
      supplementalLight = true;
    }
    else
    {
      supplementalLight = false;
    }
  }
}

// ============================================================
// FUNCTION: runSafetyControl
// ============================================================

/**
 * @brief Applies the predefined safe posture.
 *
 * The vent is opened during a sensor failure.
 * Supplemental lighting is disabled.
 *
 * @return void
 */
void runSafetyControl()
{
  // Safety Mode has the highest priority. The predefined safe posture
  // is to keep the vent OPEN and supplemental lighting OFF when the
  // primary environmental sensor has failed.
  if (currentMode == SAFETY_MODE)
  {
    // Safety posture
    setVent(true);

    // Supplemental light OFF
    supplementalLight = false;
  }
}

// ============================================================
// FUNCTION: updateDisplay
// ============================================================

/**
 * @brief Displays live system information on the OLED.
 *
 * @return void
 */
void updateDisplay()
{
  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // ==========================================================
  // TITLE
  // ==========================================================

  display.setCursor(0, 0);
  display.println("MICRO-CLIMATE NURSERY");

  display.drawLine(
    0,
    10,
    127,
    10,
    SSD1306_WHITE
  );

  // ==========================================================
  // SAFETY MODE
  // ==========================================================

  if (currentMode == SAFETY_MODE)
  {
    display.setTextSize(2);

    display.setCursor(0, 16);
    display.println("SAFETY");

    display.setTextSize(1);

    display.setCursor(0, 38);
    display.println("SENSOR ERROR");

    display.setCursor(0, 50);
    display.println("VENT: OPEN");

    display.display();

    return;
  }

  // ==========================================================
  // TEMPERATURE
  // ==========================================================

  display.setCursor(0, 13);

  display.print("Temp: ");
  display.print(temperature, 1);
  display.println(" C");

  // ==========================================================
  // HUMIDITY
  // ==========================================================

  display.setCursor(0, 23);

  display.print("Humidity: ");
  display.print(humidity, 1);
  display.println(" %");

  // ==========================================================
  // LIGHT
  // ==========================================================

  display.setCursor(0, 33);

  display.print("Light: ");
  display.print(lightLevel);

  if (lightLevel > LIGHT_THRESHOLD)
  {
    display.println(" LOW");
  }
  else
  {
    display.println(" BRIGHT");
  }

  // ==========================================================
  // VENT
  // ==========================================================

  display.setCursor(0, 43);

  display.print("Vent: ");

  if (ventOpen)
  {
    display.print("OPEN");
  }
  else
  {
    display.print("CLOSED");
  }

  // ==========================================================
  // MODE
  // ==========================================================

  display.setCursor(70, 43);

  if (currentMode == AUTO_MODE)
  {
    display.print("AUTO");
  }
  else if (currentMode == MANUAL_MODE)
  {
    display.print("MANUAL");
  }

  // ==========================================================
  // SUPPLEMENTAL LIGHT
  // ==========================================================

  display.setCursor(0, 53);

  display.print("LED: ");

  if (supplementalLight)
  {
    display.print("ON");
  }
  else
  {
    display.print("OFF");
  }

  display.display();
}

// ============================================================
// FUNCTION: printSerialStatus
// ============================================================

/**
 * @brief Prints the current system status to UART.
 *
 * @return void
 */
void printSerialStatus()
{
  Serial.println();
  Serial.println("================================");
  Serial.println(" MICRO-CLIMATE NURSERY STATUS");
  Serial.println("================================");

  // ==========================================================
  // MODE
  // ==========================================================

  Serial.print("Mode: ");

  if (currentMode == AUTO_MODE)
  {
    Serial.println("AUTO");
  }
  else if (currentMode == MANUAL_MODE)
  {
    Serial.println("MANUAL");
  }
  else
  {
    Serial.println("SAFETY");
  }

  // ==========================================================
  // TEMPERATURE
  // ==========================================================

  Serial.print("Temperature: ");

  if (isnan(temperature))
  {
    Serial.println("ERROR");
  }
  else
  {
    Serial.print(temperature, 1);
    Serial.println(" C");
  }

  // ==========================================================
  // HUMIDITY
  // ==========================================================

  Serial.print("Humidity: ");

  if (isnan(humidity))
  {
    Serial.println("ERROR");
  }
  else
  {
    Serial.print(humidity, 1);
    Serial.println(" %");
  }

  // ==========================================================
  // LIGHT
  // ==========================================================

  Serial.print("LDR ADC: ");
  Serial.println(lightLevel);

  Serial.print("Light Condition: ");

  if (lightLevel > LIGHT_THRESHOLD)
  {
    Serial.println("LOW LIGHT");
  }
  else
  {
    Serial.println("BRIGHT");
  }

  // ==========================================================
  // VENT
  // ==========================================================

  Serial.print("Vent: ");

  if (ventOpen)
  {
    Serial.println("OPEN");
  }
  else
  {
    Serial.println("CLOSED");
  }

  // ==========================================================
  // SUPPLEMENTAL LIGHT
  // ==========================================================

  Serial.print("Supplemental Light: ");

  if (supplementalLight)
  {
    Serial.println("ON");
  }
  else
  {
    Serial.println("OFF");
  }

  // ==========================================================
  // BUZZER
  // ==========================================================

  Serial.print("Buzzer: ");

  if (currentMode == SAFETY_MODE)
  {
    Serial.println("ON");
  }
  else
  {
    Serial.println("OFF");
  }

  Serial.println("================================");
}

// ============================================================
// SETUP
// ============================================================

/**
 * @brief Initializes the greenhouse monitoring and control system.
 *
 * @return void
 */
void setup()
{
  // setup() runs once after the ESP32 starts. Hardware pins, sensors,
  // I2C, OLED and servo are initialized here.
  Serial.begin(115200);

  // ==========================================================
  // LEDs
  // ==========================================================

  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(ORANGE_LED, OUTPUT);

  // ==========================================================
  // BUZZER
  // ==========================================================

  pinMode(BUZZER_PIN, OUTPUT);

  // Start buzzer OFF
  noTone(BUZZER_PIN);

  // ==========================================================
  // BUTTON
  // ==========================================================

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // ==========================================================
  // LDR
  // ==========================================================

  pinMode(LDR_PIN, INPUT);

  // ==========================================================
  // DHT22
  // ==========================================================

  dht.begin();

  // ==========================================================
  // I2C
  // ==========================================================

  Wire.begin(OLED_SDA, OLED_SCL);

  // ==========================================================
  // OLED
  // ==========================================================

  if (!display.begin(
        SSD1306_SWITCHCAPVCC,
        0x3C))
  {
    Serial.println("OLED ERROR");
  }

  // ==========================================================
  // STARTUP SCREEN
  // ==========================================================

  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(15, 20);
  display.println("MICRO-CLIMATE");

  display.setCursor(30, 35);
  display.println("NURSERY");

  display.display();

  // ==========================================================
  // SERVO
  // ==========================================================

  ventServo.attach(SERVO_PIN);

  // Start with vent CLOSED
  setVent(false);

  // ==========================================================
  // INITIAL STATE
  // ==========================================================

  currentMode = AUTO_MODE;
  supplementalLight = false;

  updateLEDs();

  Serial.println();
  Serial.println("Micro-Climate Nursery Started");
  Serial.println("System Mode: AUTO");
  Serial.println("Buzzer: OFF");
}

// ============================================================
// MAIN LOOP
// ============================================================

/**
 * @brief Main non-blocking control loop.
 *
 * Handles button, sensors, modes, servo, LEDs,
 * buzzer, OLED and UART.
 *
 * @return void
 */
void loop()
{
  // millis() returns elapsed milliseconds since startup. It is used
  // instead of delay() so timed tasks do not block the rest of the system.
  unsigned long currentTime = millis();

  // ==========================================================
  // BUTTON
  // ==========================================================

  handleButton();

  // ==========================================================
  // SENSOR READING
  // ==========================================================

  if (currentTime - lastSensorRead >= SENSOR_INTERVAL)
  {
    lastSensorRead = currentTime;

    readSensors();
  }

  // ==========================================================
  // CONTROL PRIORITY
  //
  // SAFETY > MANUAL > AUTO
  //
  // 1. Safety Mode always overrides everything else.
  // 2. Manual Override is checked next.
  // 3. Automatic control runs only when neither higher-priority
  //    condition is active.
  // ==========================================================

  // Run exactly one control routine according to the priority above.
  // This prevents two modes from trying to control the vent at once.
  if (currentMode == SAFETY_MODE)
  {
    runSafetyControl();
  }
  else if (currentMode == MANUAL_MODE)
  {
    runManualControl();
  }
  else
  {
    runAutomaticControl();
  }

  // ==========================================================
  // UPDATE LEDs
  // ==========================================================

  updateLEDs();

  // ==========================================================
  // UPDATE BUZZER
  // ==========================================================

  updateBuzzer();

  // ==========================================================
  // OLED
  // ==========================================================

  if (currentTime - lastDisplayUpdate >= DISPLAY_INTERVAL)
  {
    lastDisplayUpdate = currentTime;

    updateDisplay();
  }

  // ==========================================================
  // UART
  // ==========================================================

  if (currentTime - lastSerialUpdate >= SERIAL_INTERVAL)
  {
    lastSerialUpdate = currentTime;

    printSerialStatus();
  }
}