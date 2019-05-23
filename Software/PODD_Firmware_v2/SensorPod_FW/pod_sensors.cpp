
/*
   pod_sensors.cpp
   2017 - Nick Turner and Morgan Redfield
   2018 - Chris Savage
   Licensed under the AGPLv3. For full license see LICENSE.md
   Copyright (c) 2017 LMN Architects, LLC

   Handle setup and reading from various sensors.
*/

#include "pod_sensors.h"
#include "pod_util.h"
#include "pod_serial.h"
#include "pod_config.h"

#include <limits.h>

#include <NeoSWSerial.h>
//#include <SoftwareSerial.h>
#include <Wire.h>
#include <ClosedCube_OPT3001.h>
#include <sps30.h>
//#include <TimerOne.h>
#include <TimerThree.h>


// General sensor-related variables
bool sensorsInitialized = false;

// Microphone pin (analog input)
#define MIC_PIN A0

// ADC / analog measurements
// Analog pin to continuously sample
#define FREE_RUNNING_PIN MIC_PIN
// ...converted to value used in data registers
#define FR_PIN (FREE_RUNNING_PIN - PIN_F0)
// ADC clock prescaling here specific to 8 MHz.
// See 'wiring_private.h' in teensy core files for other speeds.
#ifndef ADC_PRESCALER
#if defined(F_CPU) && (F_CPU != 8000000)
#error "CPU speed must be set to 8 MHz"
#endif
#define ADC_PRESCALER 0x06
#endif
// Various ADC-related data register values:
//   Set analog reference and pin to measure
#define FR_ADMUX (w_analog_reference | (FR_PIN & 0x1F))
//   Set trigger to free-running mode
//   Can also enabled higher speed (but higher power) conversion mode
#define FR_ADCSRB (0x00)
//#define FR_ADCSRB (0x00 | (1 << ADHSM))
//   Enable ADC and set clock prescaling,
//   but do not enable free-running mode
#define DEF_ADCSRA ((1 << ADEN) | ADC_PRESCALER)
//   Enable ADC, set clock prescaling,
//   set auto-trigger and start ADC conversions
#define FR_ADCSRA ((1 << ADEN) | (1 << ADSC) |(1 << ADATE) | ADC_PRESCALER)
// Flag to indicate if ADC is currently in free-running mode
volatile bool adcFreeRunning = false;

// Light [OPT3001]
// OPT3001 I2C address:
//   0x44  ADDR pin to GND
//   0x45  ADDR pin to VDD
//   0x46  ADDR pin to SDA
//   0x47  ADDR pin to SDL
#define OPT3001_ADDR 0x45
ClosedCube_OPT3001 opt3001;

// Sound [SparkFun 12758]
// Interval between sound samples in microseconds.
// Sampling occurs through ISR, but rate should be limited to
// only what is necessary as this will impact other ISRs.
#define SOUND_SAMPLE_INTERVAL_US 10000
// Flag to indicate if sound is currently being sampled
volatile bool soundSampling = false;

// Temperature/humidity [HIH8120]
// Address already hard-coded to this in HIH library
#define HIH_ADDR 0x27
// Library works for 8xxx line as well
//HIH61xx<TwoWire> hih(Wire);

// Radiant temperature [PR222J2]
#define RADIANT_TEMP_PIN A1

// CO2 [CozIR-A]
// Note Rx/Tx labeled for Teensy side of serial
// (reverse of Rx/Tx label on CO2 sensor)
#define CO2_PIN_RX PIN_B6
#define CO2_PIN_TX PIN_B5
// WARNING: Software serial implementations can interfere with
// other serial interfaces as processing routines prevent
// necessary interrupts from occurring in a timely manner.
// Alternative software implementations such as AltSoftSerial
// and NeoSWSerial should work better than the builtin
// SoftwareSerial, but they have their own issues: AltSoftSerial
// requires specific Rx/Tx pins and NeoSWSerial uses one of the
// hardware timers (hopefully nothing else is trying to use it...).
//SoftwareSerial CO2_serial(CO2_PIN_RX,CO2_PIN_TX);
NeoSWSerial CO2_serial(CO2_PIN_RX, CO2_PIN_TX);
// Note COZIR library modified to remove Serial.begin() call in
// constructor as we do _not_ want the serial interface running
// except when we actually want to communicate with the sensor.
//COZIR czr(CO2_serial);

// CO
//#define numCoRead 4
//int samples[numCoRead];
//#define CoSpecSensor A3
#define CO_PIN A3

// Particulate Matter (PM) Sensor: Sensirion SPS30
// PM pins and associated JST connector wire colors
// (may be specific to this batch of connector wires):
//   1 (blue):   5V
//   2 (green):  SDA/RX
//   3 (yellow): SCL/TX
//   4 (black):  SEL
//   5 (red):    GND
// The sensor draws ~ 20 mA in idle mode and ~ 60 mA in
// measurement mode.  New measurements are available every
// 1 second, without any built-in warmup period.  However,
// sensor readings seems to take 80-100 seconds to settle
// down once measurement mode is started, so the sensor
// should be run for that long before taking data for
// accuracy purposes.
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
// NOTE: The PODD board's voltage level translators are one-way
//   and cannot be used for either serial or I2C communication.
//   However, I2C relies on pull-up resistors to achieve the
//   high level and is LVTTL 3.3V compatible, so the SPS30
//   I2C lines be connected to the existing 3.3V I2C bus
//   (PODD PCB has 2.2 kOhm pull-up resistors already).
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
// Note Rx/Tx labeled for Teensy side of serial
// (reverse of Rx/Tx label on PM sensor).
// 5-pin connected oriented on PCB such that disconnected SEL line
// is on bottom, where orientations is such tat teensy-side of PCB
// is "top".
#define PM_PIN_RX PIN_C6
#define PM_PIN_TX PIN_C5
// Power enable pin
#define PM_ENABLE 42
// SPS30 object.
// Uses default I2C (Wire) interface.  Note the 32-byte buffer
// used by this class for teensy boards is insufficient to hold
// all the data returned by the sensor: the mass densities can
// be retrieved, but the number densities will not (set to zer0).
SPS30 sps30;
bool pmPowered = false;
bool pmRunning = false;
sps_values pmData;


//--------------------------------------------------------------------------------------------- [Sensor Reads]

void sensorSetup() {
  // Initialize/start I2C interface
  // Wire changes the Two-Wire Control Register (TWCR), so can use
  // its value to see if I2C interface already initialized.
  if (TWCR == 0) Wire.begin();
  
  // ADC initialization:
  // Using low-level ADC access rather than higher-level
  // Arduino analog routines for speed.  That means
  // readAnalog() must be used instead of analogRead()!
  //analogReference(EXTERNAL);
  initADC();
  
  // Light sensor
  initLightSensor();

  // Sound sensor
  initSoundSensor();
  
  // Temperature/humidity sensor
  initTemperatureSensor();

  // Globe/radiant temperature sensor
  initGlobeTemperatureSensor();
  
  // CO2 sensor
  initCO2Sensor();
  
  // CO sensor
  initCOSensor();
  
  // PM Sensor
  // Power to sensor initially turned off: must call
  // appropriate routines to power up and start PM sensor.
  initPMSensor();
}



// Sensor Initialization / Verification --------------------------------

/* Perform any sensor initialization.  Need only be called once.
   NOTE: This does not power on and/or start all the sensors.
   Sound sampling needs to be started and the particulate matter
   sensor needs to be powered on and started. */
void initSensors() {
  // Check if already initialized
  if (sensorsInitialized) return;

  // Initialize/start I2C interface
  // Wire changes the Two-Wire Control Register (TWCR), so can use
  // its value to see if I2C interface already initialized.
  if (TWCR == 0) Wire.begin();
  
  // ADC initialization:
  // Using low-level ADC access rather than higher-level
  // Arduino analog routines for speed.  That means
  // readAnalog() must be used instead of analogRead()!
  //analogReference(EXTERNAL);
  initADC();
  
  // Light sensor
  initLightSensor();

  // Sound sensor
  initSoundSensor();
  
  // Temperature/humidity sensor
  initTemperatureSensor();

  // Globe/radiant temperature sensor
  initGlobeTemperatureSensor();
  
  // CO2 sensor
  initCO2Sensor();
  
  // CO sensor
  initCOSensor();
  
  // PM Sensor
  // Power to sensor initially turned off: must call
  // appropriate routines to power up and start PM sensor.
  initPMSensor();

  // Flag to indicate routine has already been called
  sensorsInitialized = true;

  // Some sensors need a little time before further interactions
  delay(100);
}


/* Prints to serial the status of each sensor.
     Available: sensor is known to be present & working
     Unavailable: sensor is known not be present & working
     Unknown: sensor presence cannot be determined
     */
void printSensorCheck() {
  // Must cast as (const __FlashStringHelper*) when printing...
  //static const char AVAILABLE[] PROGMEM   = "         available         ";
  //static const char UNAVAILABLE[] PROGMEM = "        unavailable        ";
  //static const char UNPOWERED[] PROGMEM   = "    unknown (unpowered)    ";
  //static const char UNCHECKABLE[] PROGMEM = "unknown (cannot be checked)";
  // No cast required
  //FType AVAILABLE   = F("         available         ");
  //FType UNAVAILABLE = F("        unavailable        ");
  //FType UNPOWERED   = F("    unknown (unpowered)    ");
  //FType UNCHECKABLE = F("unknown (cannot be checked)");
  FType AVAILABLE   = F("    available         ");
  FType UNAVAILABLE = F("   unavailable        ");
  FType UNPOWERED   = F("     unknown     [unpowered]");
  FType UNCHECKABLE = F("     unknown     [cannot be checked]");
  Serial.print(F("Sensor availability:"));
  Serial.println();
  Serial.print(F("    Light:                   "));
  Serial.print(probeLightSensor() ? AVAILABLE : UNAVAILABLE);
  Serial.println();
  Serial.print(F("    Sound:                   "));
  Serial.print(UNCHECKABLE);
  Serial.println();
  Serial.print(F("    Temperature/humidity:    "));
  Serial.print(probeTemperatureSensor() ? AVAILABLE : UNAVAILABLE);
  Serial.println();
  Serial.print(F("    Radiant temperature:     "));
  Serial.print(UNCHECKABLE);
  Serial.println();
  Serial.print(F("    CO2:                     "));
  Serial.print(probeCO2Sensor() ? AVAILABLE : UNAVAILABLE);
  Serial.println();
  Serial.print(F("    CO:                      "));
  Serial.print(UNCHECKABLE);
  Serial.println();
  Serial.print(F("    Particulate matter:      "));
  if (isPMSensorPowered()) {
    Serial.print(probePMSensor() ? AVAILABLE : UNAVAILABLE);
  } else {
    Serial.print(UNPOWERED);
  }
  Serial.println();
}



// All-Sensor Testing --------------------------------------------------

/* Helper function: convert a sensor value to a fixed-length string. */
void sensorValueToString(int v, char *buff) {
  if (v != -1) {
    sprintf(buff," %6d ",v);
  } else {
    strcpy(buff,"    -- ");
  }
}


/* Helper function: convert a sensor value to a fixed-length string. */
void sensorValueToString(float v, char *buff) {
  if (!isnan(v)) {
    // Avoid overflowing buffer
    if (v > 99999.99) v = 99999.99;
    if (v < -9999.99) v = -9999.99;
    dtostrf(v,8,2,buff);
  } else {
    strcpy(buff,"   --  ");
  }
}

/* Testing routine to show all sensor values.
   Will sample the sensors for the given number of sample periods.
   Sample period in milliseconds. */
void testSensors(unsigned long cycles, unsigned long sampleInterval) {
  Serial.println();
  Serial.println(F("Sensor testing"));
  Serial.println(F("--------------"));
  Serial.println();
  
  Serial.println(F("Press any key to end sensor testing."));
  Serial.println();
  
  // Ensure sensors are initialized
  if (!sensorsInitialized) {
    Serial.println(F("Initializing sensors...."));
    initSensors();
    //delay(100);
  }
  
  // Ensure sound is being sampled
  bool wasSoundSampling = isSoundSampling();
  if (!wasSoundSampling) {
    //Serial.println(F("Initializing sound sensor...."));
    //initADC();
    //initSoundSensor();
    Serial.println(F("Starting sound sampling...."));
    startSoundSampling();
  } else {
    // Start a new set of samples
    //Serial.println(F("Starting sound sampling...."));
    resetSoundData();
  }

  // Ensure PM sensor is powered up and running
  bool wasPMPowered = isPMSensorPowered();
  bool wasPMRunning = isPMSensorRunning();
  if (!wasPMPowered) {
    //Serial.println(F("Initializing particulate matter sensor...."));
    //initPMSensor();
    //delay(100);
    Serial.println(F("Powering up particulate matter sensor...."));
    powerOnPMSensor();
    delay(1000);
  }
  if (!wasPMRunning && isPMSensorPowered()) {
    Serial.println(F("Starting particulate matter measurements...."));
    startPMSensor(false);
    Serial.println(F("  PM sensor takes 5-10 seconds to return the first measurement"));
    Serial.println(F("  and 60-120 seconds for measurements to settle down."));
    delay (1000);
  }
  
  // Arduino implementation of printf drops %f support to reduce
  // memory usage.  We use dsostrf instead.
  char hbuffer1[128],hbuffer2[128],hbuffer3[128];
  char sbuffer[128];
  char vbuffers[9][16];
  
  // Table header
  //Serial.println();
  sprintf(hbuffer1,"  %8s %8s %8s %8s %8s %8s %8s %8s %8s %8s",
          "  time  ","  light ","  sound ","humidity","  temp  ","rad temp",
          "   CO2  ","   CO   "," PM 2.5 ","  PM 10 ");
  //Serial.println(hbuffer1);
  // Arduino serial monitor allows for UTF-8 characters, like degree symbol.
  sprintf(hbuffer2,"  %8s %8s %8s %8s %8s %8s %8s %8s %8s %8s",
          "  [ms]  ","  [lux] ","  [???] ","   [%]  ","  [°F]  ","  [°F]  ",
          "  [ppm] ","  [??]  "," [ug/m3]"," [ug/m3]");
  //Serial.println(hbuffer2);
  sprintf(hbuffer3,"  %8s %8s %8s %8s %8s %8s %8s %8s %8s %8s",
          "--------","--------","--------","--------","--------","--------",
          "--------","--------","--------","--------");
  //Serial.println(hbuffer3);

  // Time at start of test (fixed)
  unsigned long t0 = millis();
  // Time at start of measurement (updated each loop)
  unsigned long t = millis();
  
  // Repeat measurements cycles number of times.
  // Logic here allows endless looping for cycles=-1 (maximum unsigned long).
  for (unsigned long k = 1; k <= cycles; k++) {
    // Try to adjust delay time to keep consistent interval.
    unsigned long dt = millis() - t;
    unsigned long tdelay = (dt < sampleInterval) ? sampleInterval - dt : 0;
    // Ensure some delay to allow keystrokes to be read
    if (tdelay < sampleInterval/2) tdelay = sampleInterval/2;
    //delay(tdelay);
    // Break out of the testing loop if the user sends anything
    // over the serial interface.
    if (getSerialChar(tdelay) != (char)(-1)) break;
    
    // Update loop time
    t = millis();

    // Repeat header every so often
    // Ensure write on first loop (k=1)
    if (k % 50 == 1) {
      Serial.println();
      Serial.println(hbuffer1);
      Serial.println(hbuffer2);
      Serial.println(hbuffer3);
    }

    // Extract sensor measurements and convert to strings.
    // Same order as will appear in output.
    sensorValueToString(getLight(),vbuffers[0]);
    sensorValueToString(getSound(),vbuffers[1]);
    retrieveTemperatureData();
    sensorValueToString(getRelHumidity(),vbuffers[2]);
    sensorValueToString(getTemperature(),vbuffers[3]);
    sensorValueToString(getGlobeTemperature(),vbuffers[4]);
    sensorValueToString(getCO2(),vbuffers[5]);
    sensorValueToString(getCO(),vbuffers[6]);
    retrievePMData();
    sensorValueToString(getPM2_5(),vbuffers[7]);
    sensorValueToString(getPM10(),vbuffers[8]);

    sprintf(sbuffer,"%10ld %8s %8s %8s %8s %8s %8s %8s %8s %8s",
            t-t0,vbuffers[0],vbuffers[1],vbuffers[2],vbuffers[3],vbuffers[4],
            vbuffers[5],vbuffers[6],vbuffers[7],vbuffers[8]);
    Serial.println(sbuffer);
  }
  
  Serial.println();
  
  // Turn off sound sampling and/or PM sensor if we started them
  if (!wasSoundSampling) {
    Serial.println(F("Stopping sound sampling...."));
    stopSoundSampling();
  }
  if (!wasPMRunning && isPMSensorRunning()) {
    Serial.println(F("Stopping particulate matter measurements...."));
    stopPMSensor();
  }
  if (!wasPMPowered && isPMSensorPowered()) {
    Serial.println(F("Powering down particulate matter sensor...."));
    powerOffPMSensor();
  }
  
  Serial.println(F("Sensor testing is complete."));
  Serial.println();
}



// ADC / Analog Measurements -------------------------------------------

/* To allow for rapid ADC measurements of the microphone without
   frequent CPU tie-ups (a standard ADC conversion takes ~ 0.1 ms,
   too long for an ISR) that might interfere with network/xbee
   communications, low-level ADC calls are used in place of the
   high-level Arduino routines.  This allows the ADC to be placed
   in free-running (continuously measuring) mode, though we must
   temporarily suspend that mode to take analog measurements on
   other (non-microphone) pins. */

// See pins_teensy.c for use of low-level ADC access on
// AT90USB1286 microcontroller.

/* Initializes ADC. */
void initADC() {
  // PODD has an external voltage reference (3.3V power line)
  analogReference(EXTERNAL);
  // Turn on ADC
  ADCSRA |= (1 << ADEN);
}


/* Places the ADC in free-running mode, taking continuous measurements
   of a specific analog pin (intended for microphone pin). */
void startADCFreeRunning() {
  if (adcFreeRunning) return;

  // Disable interrupts to prevent ISRs from changing values.
  // Store previous interrupt state so we can restore it afterwards.
  uint8_t oldSREG = SREG;  // Save interrupt status (among other things)
  cli();  // Disable interrupts

  // Disable digital input on pin
  DIDR0 |= (1 << FR_PIN);  // achieved with pinMode()?

  // Set ADC-related data registers:
  // * set analog reference and pin to measure
  //ADMUX = w_analog_reference | (FR_PIN & 0x1F);
  ADMUX = FR_ADMUX;
  // * set trigger to free-running mode
  //   can also enabled higher speed (but higher power) conversion mode
  //ADCSRB = 0x00;  // | (1 << ADHSM)
  ADCSRB = FR_ADCSRB;
  // * enable ADC and set clock prescaling
  //ADCSRA = (1 << ADEN) | ADC_PRESCALER;
  // * set auto-trigger and start ADC conversions
  //ADCSRA |= (1 << ADSC) | (1 << ADATE);
  // * enable ADC, set auto-trigger, set clock prescaling,
  //   and start ADC conversions
  ADCSRA = FR_ADCSRA;
  
  adcFreeRunning = true;
  
  // Restore interrupt status.
  // Re-enables interrupts if they were previously active.
  SREG = oldSREG;

  // Give chance for ADC to settle
  //delay(1);
}


/* Stops the ADC from continously taking measurements. */
void stopADCFreeRunning() {
  if (!adcFreeRunning) return;
  adcFreeRunning = false;
  
  // Disable interrupts to prevent ISRs from changing values.
  // Store previous interrupt state so we can restore it afterwards.
  uint8_t oldSREG = SREG;  // Save interrupt status (among other things)
  cli();  // Disable interrupts
  
  // Enable ADC and set clock prescaling,
  // but omit auto-trigger flag
  //ADCSRA = (1 << ADEN) | ADC_PRESCALER;
  ADCSRA = DEF_ADCSRA;

  // Restore interrupt status.
  // Re-enables interrupts if they were previously active.
  SREG = oldSREG;
}


/* Indicates if the ADC is currently in free-running (continuously
   sampling) mode. */
bool isADCFreeRunning() {
  return adcFreeRunning;
}


/* Takes an analog measurement of the given pin.  If the ADC is in
   free-running mode, it will stop, read the new pin, and then resume
   free-running on the original pin. */
int readAnalog(uint8_t pin) {
  bool wasrunning = adcFreeRunning;
  if (adcFreeRunning) stopADCFreeRunning();
  // Debugging
  //Serial.print("ADCSRA: ");
  //Serial.print(ADCSRA,HEX);
  //Serial.println();
  // Wait for any existing conversion to complete
  //while ((ADCSRA & (1 << ADSC)) != 0) continue;
  // Also ensure sensor is not in free-running mode (it _shouldn't_ be...)
  while (((ADCSRA & (1 << ADATE)) == 0) && ((ADCSRA & (1 << ADSC)) != 0)) continue;
  //delay(1);
  // analogRead will wait for ADC conversion
  int v = analogRead(pin);
  if (wasrunning) startADCFreeRunning();
  return v;
}


/* Returns the most recent ADC measurement in free-running mode.
   If not in free-running mode or a new measurement is not
   available since the last read, returns -1.  Measurements are
   restricted to the fixed free-running pin (intended to be the
   microphone pin).  This routine should be safe to call from
   an ISR. */
int readAnalogFast() {
  if (!adcFreeRunning) return -1;
  // Check if data registers are correct. If not, reset data registers.
  // This may occur if someone calls analogRead() instead of
  // readAnalog().
  const uint8_t ADCSRA_MASK = (1 << ADEN) | (1 << ADATE) | 0x08;
  if ((ADMUX != FR_ADMUX) || ((ADCSRA & ADCSRA_MASK) != (FR_ADCSRA  & ADCSRA_MASK))) {
    adcFreeRunning = false;  // otherwise next line will do nothing
    startADCFreeRunning();
    return -1;
  }
  // We assume there is at most one ISR calling this routine,
  // in which case we do not need to disable interrupts.
  // Check if in middle of conversion (always true for free-running?).
  //if (ADCSRA & (1 << ADSC)) return -1;
  // Check if interrupt (conversion complete) flag set.
  // If not set, there is no _new_ analog data to read.
  if (!(ADCSRA & (1 << ADIF))) return -1;
  // Read of low field locks results until high field read.
  // Read of high field will clear data available flag?
  uint8_t low = ADCL;
  int v = (ADCH << 8) | low;
  // Clear interrupt flag
  ADCSRA |= (1 << ADIF);
  return v;
}



// Light Sensor [OPT3001] ----------------------------------------------

/* Initializes the OPT3001 ambient light sensor. */
void initLightSensor() {
  opt3001.begin(OPT3001_ADDR);
  OPT3001_Config opt3001Config;
  // Use 0000b to 1011b to explicitly set range,
  // or use 1100b for automatic scaling of range.
  opt3001Config.RangeNumber = B1100;
  // Conversion time: 0 for 100ms, 1 for 800ms
  opt3001Config.ConvertionTime = B1;  // [sic]
  // Use 00b to shutdown the sensor, 01b for a single-shot read (returns
  // to 00b after the read completes), or 11b for continuous sensor reading.
  opt3001Config.ModeOfConversionOperation = B11;
  
  // The other configuration fields are irrelevant for our purposes and
  // not set here.  See documentation for other possibilities (notably if
  // intending to use the interrupt pin).
  
  // Note automatic scaling goes up or down by 1-2 scales (x2 or x0.5) with
  // each measurement cycle until it reaches a reasonable range.  With the
  // longer integration time, it may take several seconds (up to ~ 10s) for
  // the readings to stabilize; during that time, the readings may be too
  // low and/or lack precision.
  
  // Upload configuration to sensor
  OPT3001_ErrorCode opt3001Err = opt3001.writeConfig(opt3001Config);
  (void)opt3001Err;  // suppress unused variable warning
  //if (opt3001Err == NO_ERROR) {
  //  Serial.println(F("OPT3001 configured."));
  //} else {
  //  Serial.print(F("OPT3001 configuration error: "));
  //  Serial.println(opt3001Err);
  //}
}


/* Tests communication with the ambient light sensor. */
bool probeLightSensor() {
  // Check by trying to get a measurement value
  OPT3001 reading = opt3001.readResult();
  return (reading.error == NO_ERROR);
}


/* Gets the ambient light level in lux.  Returns NAN if there
   is a problem reading the sensor. */
float getLight() {
  // OPT3001: Range is 0.01 - 80,000 lux with resolution as
  // small as 0.01 lux.  Note with current configuration, it
  // may take several seconds for readings to stabilize if
  // the lighting condition changes drastically and rapidly.
  // That is, don't use this at a rave.
  OPT3001 reading = opt3001.readResult();
  // Return unphysical value if sensor read error
  if (reading.error != NO_ERROR) {
    //Serial.print(F("Error reading OPT3001 sensor ("));
    //Serial.print(reading.error);
    //Serial.println(F(")."));
    return NAN;
  }
  return reading.lux;
}



// Sound Sensor --------------------------------------------------------

// Routines below are for analog readings of a simple microphone.
// The one used in the PODDs is the SparkFun 12758: an electret
// microphone with a x60 amplifier.

// Sound levels are based upon the standard deviation of many samples
// of microphone output.  Conversion to dB(Z) (no frequency weighting)
// is for this particular microphone and setup.


/* Sound data structure. */
struct SoundData {
  unsigned long tstart;
  uint32_t N;
  int32_t sum;
  uint64_t sum2;  // 32-bit can overflow with many samples
  //int32_t sum2;  // 32-bit can overflow with many samples
  int min0,max0;
  void reset() {tstart=millis(); N=0; sum=0; sum2=0; min0=INT_MAX; max0=INT_MIN;}
  void add(int v) {
    // quick, integer-type operations only: may be called from ISR
    N++; sum+=v; int32_t v0=v; sum2+=(v0*v0);  // v^2 can overflow int
    if(v<min0) min0=v; if(v>max0) max0=v;
  }
  float ave() const {return (N > 0) ? ((float)sum)/N : 0;}
  float rms2() const {return (N > 0) ? ((float)sum2)/N : 0;}
  float rms() const {return (N > 0) ? sqrt(rms2()) : 0;}
  float sd() const {
    if (N == 0) return 0;
    float a=ave(); float a2=a*a; float r2 = rms2();
    return (r2 > a2) ? sqrt(r2-a2) : 0;
  }
  // min/max are macros (cannot use as member name...)
  float smin() const {return min0;}
  float smax() const {return max0;}
};
// Problems with functions in volatile struct...
//volatile SoundData soundData;
SoundData soundData;


/* Initializes the sound sensor (microphone) and associated data
   structures. */
void initSoundSensor() {
  pinMode(MIC_PIN,INPUT);
  //soundSampling = false;
  stopSoundSampling();
  //resetSoundData();
  soundData.reset();
  // Warn about uncalibrated results.
  Serial.println(F("Warning: Sound level values are uncalibrated (units are arbitrary)."));
}


/* Gets the average-fluctuation-based sound level since the last call 
   to this routine (or since sampling started).  Returns NAN if not
   currently sampling or no samples have been taken since last call. */
float getSound() {
  if (!soundSampling) return NAN;
  
  // Copy sound data into local variable to avoid ISR modifying
  // working copy.  Reset global structure to start a new
  // sampling period.  Temporarily disable interrupts to
  // prevent ISRs from modifying data structure while we make
  // the copy.
  uint8_t oldSREG = SREG;  // Save interrupt status (among other things)
  cli();  // Disable interrupts
  SoundData sd = soundData;
  soundData.reset();
  SREG = oldSREG;

  // Return invalid value if no samples were taken
  if (sd.N == 0) return NAN;

  // Standard deviation of ADC samples (no dB(Z) conversion).
  // Use of s.d. limits clipping-issues and provides for a better
  // time-average metric than max-min does.
  return sd.sd();

  // TODO: conversion to decibels (Z-weighted -> dBz)
}


/* Starts sampling sound in the background.
   Enables a timer-based ISR that continuously samples the microphone
   level and accumulates data until the sound level is read.
   Note this adds to the CPU workload and may interfere with other ISRs,
   though we strive for this ISR to be fast enough not to cause an issue. */
void startSoundSampling() {
  if (soundSampling) return;
  
  soundData.reset();
  
  // Put ADC in continuously-sampling mode
  startADCFreeRunning();
  
  // Begin timer and attach interrupt service routine (required order?)
  Timer3.initialize(SOUND_SAMPLE_INTERVAL_US);
  Timer3.attachInterrupt(sampleSoundISR);
  
  // Disable interrupts to prevent ISRs from changing values.
  // Store previous interrupt state so we can restore it afterwards.
  uint8_t oldSREG = SREG;  // Save interrupt status (among other things)
  cli();  // Disable interrupts
  soundSampling = true;
  SREG = oldSREG;
  
}


/* Stops sampling sound. */
void stopSoundSampling() {
  if (!soundSampling) return;

  // Stop timer, remove ISR
  Timer3.stop();
  Timer3.detachInterrupt();
  
  // Disable interrupts to prevent ISRs from changing values.
  // Store previous interrupt state so we can restore it afterwards.
  uint8_t oldSREG = SREG;  // Save interrupt status (among other things)
  cli();  // Disable interrupts
  soundSampling = false;
  SREG = oldSREG;
  
  // Halt ADC's continuously-sampling mode
  stopADCFreeRunning();
}


/* Indicates if sound sampling is currently occurring in the background. */
bool isSoundSampling() {
  return soundSampling;
}


/* Takes a sound sample.  Intended to be run as a timer-based ISR. */
void sampleSoundISR() {
  if (!soundSampling) return;
  // Take most recent measurement from continuously-sampling ADC.
  // Will return -1 if no new measurement available or if ADC not
  // in continuously-sampling mode.
  int v = readAnalogFast();
  if (v < 0) return;
  // Exclude extreme spikes
  //if ((v < 32) || (v >= 992)) return;
  //if ((v < 64) || (v >= 960)) return;
  // Add sample to statistics
  soundData.add(v);
}


/* Resets accumulated sound data for a new round of sound sampling.
   ISR-safe. */
void resetSoundData() {
  // Disable interrupts to prevent ISRs from changing values.
  // Store previous interrupt state so we can restore it afterwards.
  uint8_t oldSREG = SREG;  // Save interrupt status (among other things)
  cli();  // Disable interrupts
  soundData.reset();
  SREG = oldSREG;
}


/* Sound level testing routine.
   Will sample the sensor for the given number of sample periods.
   Sample period in milliseconds. */
// Sensor testing >>>>>>>>>>>>>>>>>>>>>>
#ifdef SENSOR_TESTING
void testSoundSensor(unsigned long cycles, unsigned long sampleInterval) {
  bool wasSampling = isSoundSampling();
  Serial.println();
  Serial.println(F("Sound level testing"));
  Serial.println(F("-------------------"));
  Serial.println();
  if (!wasSampling) {
    Serial.println(F("Initializing sound sensor...."));
    initADC();
    initSoundSensor();
    Serial.println(F("Starting sound sampling...."));
    startSoundSampling();
  } else {
    // Start a new set of samples
    Serial.println(F("Starting sound sampling...."));
    resetSoundData();
  }
  unsigned long t0 = millis();
  
  // Arduino implementation of printf drops %f support to reduce
  // memory usage.  We use dsostrf instead.
  char hbuffer1[128],hbuffer2[128];
  char sbuffer[128];
  char fbuffers[3][16];

  // Table header
  //Serial.println();
  sprintf(hbuffer1,"  %8s %8s %6s %8s %8s %8s %6s %6s %6s",
          "time[ms]","interval",
          "     N"," average","   rms  ","   sd   ","   min","   max","  diff");
  //Serial.println(hbuffer1);
  sprintf(hbuffer2,"  %8s %8s %6s %8s %8s %8s %6s %6s %6s",
          "--------","--------",
          " -----"," -------"," -------"," -------","   ---","   ---","  ----");
  //Serial.println(hbuffer2);

  // Repeat measurements cycles number of times.
  // Logic here allows endless looping for cycles=-1 (maximum unsigned long).
  for (unsigned long k = 1; k <= cycles; k++) {
    //delay(sampleInterval);
    // Break out of the testing loop if the user sends anything
    // over the serial interface.
    if (getSerialChar(sampleInterval) != (char)(-1)) break;

    // Repeat header every so often
    // Ensure write on first loop (k=1)
    if (k % 50 == 1) {
      Serial.println();
      Serial.println(hbuffer1);
      Serial.println(hbuffer2);
    }

    // Copy sound data into local variable to avoid ISR modifying
    // working copy.  Reset global structure to start a new
    // sampling period.  Temporarily disable interrupts to
    // prevent ISRs from modifying data structure while we make
    // the copy.
    uint8_t oldSREG = SREG;  // Save interrupt status (among other things)
    cli();  // Disable interrupts
    SoundData sd = soundData;
    soundData.reset();
    SREG = oldSREG;

    unsigned long t = millis() - t0;
    unsigned long dt = millis() - sd.tstart;
    dtostrf(sd.ave(),8,3,fbuffers[0]);
    dtostrf(sd.rms(),8,3,fbuffers[1]);
    dtostrf(sd.sd(),8,3,fbuffers[2]);
    sprintf(sbuffer,"%10ld %8ld %6ld %8s %8s %8s %6d %6d %6d",
            t,dt,sd.N,fbuffers[0],fbuffers[1],fbuffers[2],sd.min0,sd.max0,sd.max0-sd.min0);
    Serial.println(sbuffer);
  }
  
  Serial.println();
  if (!wasSampling) {
    Serial.println(F("Stopping sound sampling...."));
    stopSoundSampling();
  }
  Serial.println(F("Sound sensor testing is complete."));
  Serial.println();
}
#endif
//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<



// Temperature/Humidity Sensor [HIH8120] -------------------------------

/* Temperature/humidity data structure.
   Uses NAN when data is invalid. */
struct TempRHData {
  float _T;   // [C]
  float _RH;  // [%]
  void reset() {_T = NAN; _RH = NAN;}
  float Tc() {return _T;}
  float Tf() {return (isnan(_T)) ? NAN : 1.8*_T + 32;}
  float RH() {return _RH;}
};
TempRHData temperatureData;


/* Initializes the HIH8120 temperature/humidity sensor and associated
   data structures. */
void initTemperatureSensor() {
  //hih.initialise();
  temperatureData.reset();
}


/* Tests communication with the temperature/humidity sensor. */
bool probeTemperatureSensor() {
  // Test communication by requesting data from sensor.
  // Note writing to sensor triggers a conversion and would
  // tie up the sensor.
  const size_t BUFF_LEN = 2;
  //uint8_t buff[BUFF_LEN];
  size_t n = Wire.requestFrom(HIH_ADDR,BUFF_LEN);
  // Clear I2C buffer
  while (Wire.available()) Wire.read();
  // Communication successful if we received as many
  // bytes as requested
  return (n == BUFF_LEN);
}


/* Retrieves measurements from the temperature/humidity sensor.
   Returns true on success.  Actual data values can be accessed
   through below routines.  Takes ~40ms for sensor to perform
   conversion and return data. */
bool retrieveTemperatureData() {
  temperatureData.reset();
  // Sending an (empty) write command triggers a
  // sensor measurement
  Wire.beginTransmission(HIH_ADDR);
  if (Wire.endTransmission(HIH_ADDR) != 0) return false;

  // I2C data encoded in four bytes
  // See Honeywell's technical note on I2C communications with HumidIcon
  // sensors for a description.
  const size_t BUFF_LEN = 4;
  uint8_t buff[BUFF_LEN];
  
  // Typical measurement conversion time is ~ 37 ms.
  // Will repeatedly poll for data until we get new results
  // or we timeout (100 ms).
  unsigned long t0 = millis();
  delay(35);  // appears to be sufficient most of the time
  while (true) {
    size_t n = Wire.requestFrom(HIH_ADDR,BUFF_LEN);
    // Communication failed
    if (n != BUFF_LEN) return false;
    // Pull data from I2C buffer
    for (size_t k = 0; k < n; k++) buff[k] = Wire.read();
    // Check if returned data contains the new measurement
    // (two highest bits of first byte are zero)
    if ((buff[0] >> 6) == 0) {
      uint16_t rhraw = ((uint16_t)(buff[0] & 0x3F) << 8) | (uint16_t)buff[1];
      uint16_t traw = ((uint16_t)buff[2] << 6) | ((uint16_t)buff[3] >> 2);
      const float A = (1 / (float)16382);
      temperatureData._RH = 100 * A * rhraw;
      temperatureData._T  = 165 * A * traw - 40;
      return true;
    }
    // Timed-out
    if (millis() - t0 > 100) return false;
    delay(10);
  } 
  // Should not reach here...
  return false;
}


/* Returns the most recently retrieved temperature measurement in
   Farenheit (measurements can be retrieved using
   retrieveTemperatureData()).  Returns NAN if measurement
   failed/invalid. */
float getTemperature() {
  return temperatureData.Tf();
}


/* Returns the most recently retrieved relative humidity measurement 
   in percent (measurements can be retrieved using
   retrieveTemperatureData()).  Returns NAN if measurement
   failed/invalid. */
float getRelHumidity() {
  return temperatureData.RH();
}


/* Temperature/humidity testing routine.
   Will sample the sensor for the given number cycles.
   Sample period in milliseconds. */
// Sensor testing >>>>>>>>>>>>>>>>>>>>>>
#ifdef SENSOR_TESTING
void testTemperatureSensor(unsigned long cycles, unsigned long sampleInterval) {
  Serial.println();
  Serial.println(F("Temperature/humidity testing"));
  Serial.println(F("----------------------------"));
  Serial.println();
  Serial.println(F("Initializing temperature/humidity sensor...."));
  initTemperatureSensor();
  Serial.println(F("Starting temperature/humidity measurements...."));
  unsigned long t0 = millis();
  
  // Arduino implementation of printf drops %f support to reduce
  // memory usage.  We use dsostrf instead.
  char hbuffer1[128],hbuffer2[128];
  char sbuffer[128];
  char fbuffers[2][16];

  // Table header
  //Serial.println();
  sprintf(hbuffer1,"  %8s  %7s  %7s",
          "time[ms]","  T[F] ","  RH[%]");
  //Serial.println(hbuffer1);
  sprintf(hbuffer2,"  %8s  %7s  %7s",
          "--------"," ------"," ------");
  //Serial.println(hbuffer2);

  // Repeat measurements cycles number of times.
  // Logic here allows endless looping for cycles=-1 (maximum unsigned long).
  for (unsigned long k = 1; k <= cycles; k++) {
    //delay(sampleInterval);
    // Break out of the testing loop if the user sends anything
    // over the serial interface.
    if (getSerialChar(sampleInterval) != (char)(-1)) break;

    // Repeat header every so often
    // Ensure write on first loop (k=1)
    if (k % 50 == 1) {
      Serial.println();
      Serial.println(hbuffer1);
      Serial.println(hbuffer2);
    }

    unsigned long t = millis() - t0;
    if (retrieveTemperatureData()) {
      float T = getTemperature();
      dtostrf(T,7,2,fbuffers[0]);
      float RH = getRelHumidity();
      dtostrf(RH,7,2,fbuffers[1]);
    } else {
      strcpy(fbuffers[0],"   --- ");
      strcpy(fbuffers[1],"   --- ");
    }
    sprintf(sbuffer,"%10ld  %7s  %7s",
            t,fbuffers[0],fbuffers[1]);
    Serial.println(sbuffer);
  }
  
  Serial.println();
  Serial.println(F("Temperature/humidity sensor testing is complete."));
  Serial.println();
}
#endif
//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<



// Radiant Temperature Sensor [PR222J2] --------------------------------

// In order to measure radiant temperature, a temperature sensor is
// placed inside a dark sphere.  The intent is for the air within the
// sphere to equalize with the incident radiation on the sphere, but
// in practice, there will be some heat transfer with the outside air,
// more so if there is an air current.  The "globe" temperature is
// thus going to be somewhere between the radiant temperature and
// ambient air temperature, but should at least give an indication if
// the two differ significantly.

/* Initializes the temperature sensor placed inside a dark sphere;
   intended for radiant temperature measurements. */
void initGlobeTemperatureSensor() {
  // This will set the pin mode.
  //pinMode(RADIANT_TEMP_PIN,INPUT);
  // Analog read will set the pin mode.
  // Note use of readAnalog() instead of analogRead().
  readAnalog(RADIANT_TEMP_PIN);
}


/* Tests validity of the globe temperature sensor. */
bool probeGlobeTemperatureSensor() {
  // Make measurement of voltage across fixed resistor R in
  // GND-R-Rt-3.3V voltage divider.  If voltage is nearly
  // zero, the thermistor is likely not connected (effectively
  // infinite resistance) and any readings are invalid.
  // Note use of readAnalog() instead of analogRead().
  return (readAnalog(RADIANT_TEMP_PIN) >= 10);
}


/* Returns the current temperature within the sphere (globe) in 
   Farenheit.  Returns NAN if the sensor cannot be read or the
   data is invalid (e.g. missing sensor). */
float getGlobeTemperature() {
  // Voltage across R in GND-R-Rt-3.3V voltage divider.
  // Voltage in units of analog resolution (units will cancel).
  // Note use of readAnalog() instead of analogRead().
  float V = readAnalog(RADIANT_TEMP_PIN);
  // If V is very small, either the thermistor is not connected
  // or we are at the South Pole on a cold day
  if (V < 10) return NAN;
  // Fixed resistor value
  const int R = 10000; //actual resistor value
  // Invert voltage divider to get thermistor's resistance
  float Rt = R * (1024.0 / V - 1.0);

  // Find temperature corresponding to above resistance
  // Floats from U.S Sensor Corp. Curve J sheet
  const float A = 0.00147530413409933;
  const float B = 0.000236552076866679;
  const float C = 0.000000118857119853526;
  const float D = -0.000000000074635312369958;
  float logRt = log(Rt);
  float logRt2 = logRt * logRt;
  // Inverse of temperature in Kelvin
  float Tkinv = A + logRt * (B + logRt2 * (C + logRt2 * (D)));

  // Return in Kelvin
  //return 1/Tkinv;
  // Return in Celcius
  //return 1/Tkinv - 273.15;
  // Return in Farenheit
  return 1.8 * (1/Tkinv - 273.15) + 32;
}



// CO2 Sensor [CozIR-A] ------------------------------------------------

// Good reference for Arduino serial interface with CozIR CO2 sensor:
//   https://github.com/roder/cozir


/* Initializes the CO2 sensor. */
void initCO2Sensor() {
  // COZIR sensor communicates at 9600 baud
  CO2_serial.begin(9600);
  // Only enable serial interface while using it
  enableCO2Serial();
  // Set operating mode to polling
  cozirSendCommand('K',2);
  //czr.SetOperatingMode(CZR_POLLING);
  // Sensor needs ~ 8 - 10 ms before responding to further interaction.
  delay(12);
  // Set digital filter to 32: measurements are moving average of
  // previous NN measurements, which are taken at 2 Hz.
  cozirSendCommand('A',32);
  disableCO2Serial();
}


/* Tests communication with the CO2 sensor.
   Note communication tends to fail if less than 10 ms between
   interactions with sensor (init, probe, read).  May need to
   ensure at least 10 ms since last interaction. */
bool probeCO2Sensor() {
  // Communications sometimes fail if recently used
  //delay(10);
  // Only enable serial interface while using it
  enableCO2Serial();
  bool b = cozirSendCommand('a');  // arbitrary info command
  disableCO2Serial();
  return b;
}


/* Gets the current CO2 level, in ppm.  Returns -1 if something failed.
    Note with software serial communication, bit errors might occasionally
   occur: an occasional invalid value should be expected (and handled
   appropriately). */
int getCO2() {
  // Communications sometimes fail if recently used
  //delay(10);
  // Only enable serial interface while using it
  enableCO2Serial();
  //int v = czr.CO2();
  int v = cozirGetValue('Z');
  disableCO2Serial();
  return v;
}


/* Sets the current CO2 level, in ppm.  Use to calibrate sensor. */
void setCO2(int ppm) {
  // Ignore invalid values
  if (ppm <= 0) return;
  if (ppm > 10000) return;
  // Communications sometimes fail if recently used
  //delay(10);
  // Only enable serial interface while using it
  enableCO2Serial();
  //czr.CalibrateKnownGas((uint16_t)ppm);
  cozirSendCommand('X',ppm);
  disableCO2Serial();
}


/* Enable serial interface with CO2 sensor. */
void enableCO2Serial() {
  // SoftwareSerial must restart interface and listen,
  // NeoSWSerial need only listen.
  //CO2_serial.begin(9600);
  CO2_serial.listen();
}


/* Disable serial interface with CO2 sensor. */
void disableCO2Serial() {
  // NeoSWSerial can toggle serial interface with just listen/ignore:
  CO2_serial.ignore();
  // SoftwareSerial has no ignore and must be turned off...
  //CO2_serial.end();
}


/* Converts single character command and, optionally, an integer value
   to a valid CozIR CO2 sensor command string.  If integer is negative,
   it will be omitted. */
String cozirCommandString(char c, int v) {
  if (v < 0) {
    return String(c);
  }
  char buff[10];
  // Require non-negative integers of limited range
  uint16_t v0 = (uint16_t)(v > 65535 ? 65535 : v);
  sprintf(buff,"%c %u",c,v0);
  return String(buff);
}


/* Sends single character command and, optionally, an integer value to 
   the CozIR CO2 sensor over the serial interface.  If integer is
   negative, it will be omitted.  Returns true if communication was
   successful (sensor returned command character).  Note the serial
   buffer will be left with whatever the sensor sends after the
   command character. */
bool cozirSendCommand(char c, int v) {
  //cozirSendCommand(cozirCommandString(c,v));
  // Clear incoming serial buffer first
  while(CO2_serial.available()) CO2_serial.read();
  // Send command
  String s = cozirCommandString(c,v);
  CO2_serial.print(s);
  CO2_serial.print("\r\n");
  // Wait a limited time for response
  //const int TIMEOUT_MS = 20;
  const int TIMEOUT_MS = 15;
  for (int k = 0; k < TIMEOUT_MS; k++) {
    if (CO2_serial.available()) {
      // First non-space character should be same as command
      // character sent.
      char c0 = CO2_serial.read();
      if (c0 == ' ') continue;
      return (c0 == c);
      // Note post-command-character data received from sensor
      // remains in the serial buffer.
    }
    delay(1);
  }
  // Timed out
  return false;
}


/* Returns the (first) integer value provided by the CozIR CO2 sensor after
   sending the given command.  Returns -1 if something failed.
   Note with software serial communication, bit errors might occasionally
   occur: an occasional invalid value should be expected (and handled
   appropriately). */
int cozirGetValue(char c, int v) {
  // Send command
  if (!cozirSendCommand(c,v)) return -1;
  
  // Retrieve response
  // Note cozirSendCommand already stripped off command character
  // from serial response.
  // This buffer is not large enough for commands that return
  // multiple data fields.
  const size_t BUFF_LEN = 12;
  size_t n = 0;
  char buff[BUFF_LEN];
  // Continue reading until no new input available for 2ms
  // (in case serial data still arriving).
  // Double while loops here are not redundant...
  delay(2);
  while (CO2_serial.available()) {
    while (CO2_serial.available()) {
      if (n < BUFF_LEN-1) {
        buff[n] = CO2_serial.read();
        n++;
      }
    }
    delay(2);
  }
  buff[n] = '\0';

  // CozIR response string is command character, a space, and
  // one or more (space separated) non-negative integers, possibly
  // with leading zeros.  The response is terminated with \r\n.
  // Here, the command character is already stripped off and
  // the buffer is only sized to ensure the first returned number
  // is available: other numbers and the response termination may
  // be truncated.
  
  // Parse response
  // Should begin with a space...
  if (n < 2) return -1;
  // Check that next field is numeric and, specifically, a positive
  // integer.  Ensure only digits until we reach a space, newline, or
  // the end of the read buffer: if we see anything else, it is an
  // invalid character (possibly due to a serial bit error).
  size_t pos = 0;
  while (buff[pos] == ' ') pos++;
  //if (buff[pos] == '-') pos++;  // CozIR does not return negative values
  while ((buff[pos] >= '0') && (buff[pos] <= '9')) pos++;
  if ((pos < n) && !((buff[pos] == ' ') || (buff[pos] == '\r') || (buff[pos] == '\n'))) {
    //Serial.print(F("DEBUG: cozir buffer -> '"));
    //Serial.print(buff);
    //Serial.println(F("'"));
    return -1;
  }
  // Standard Arduino routines do not include means to check
  // for invalid input, so we use following instead.
  //const char *buff = s.c_str();
  char *end;
  long l = strtol(buff,&end,10);
  //if ((end == buff) || (l <= 0)) {
  //  Serial.print(F("DEBUG: cozir buffer -> '"));
  //  Serial.print(buff);
  //  Serial.println(F("'"));
  //}
  // No valid data to convert
  // Should not occur with check above...
  if (end == buff) return -1;
  // Check if characters were numeric
  return (int)l;
}



// CO Sensor [SPEC 3SP_CO_1000] ----------------------------------------

/* Initializes the CO sensor. */
void initCOSensor() {
  // This will set the pin mode.
  //pinMode(CO_PIN,INPUT);
  // Analog read will set the pin mode.
  // Note use of readAnalog() instead of analogRead().
  readAnalog(CO_PIN);
  // Warn about uncalibrated results.
  Serial.println(F("Warning: CO measurement values are uncalibrated (units are arbitrary)."));
}


/* Tests validity of the globe CO sensor. */
bool probeCOSensor() {
  // If the sensor is not connected, the voltage should be
  // nearly zero.
  // Note use of readAnalog() instead of analogRead().
  //return (readAnalog(CO_PIN) >= 10);
  // ...but those are valid (and typical!) low-CO measurement
  // values if the sensor is connected!  Does not seem to be
  // an easy way to determine if sensor is connected, so we
  // just return true.
  return true;
}


/* Gets the current CO level, in -ppm-.  Returns NAN if something failed.
   Note no temperature compensation is performed, though this sensor
   has a significant temperature dependence when temperatures are
   above around 80 F.
   NOTE: Currently uncalibrated -- this is NOT actually ppm! */
float getCO() {
  // Sensitivity Code is final n.nn number on sensor's circular label.
  // This is different for every individual sensor!
  // Vary from ~ 2 - 7, with most of the newer ones ~ 2.5 - 3.5.
  // TODO: Add ability to set sensitivity code through PODD menu
  //       system and save/read to EEPROM
  //float coSensitivityCode = 3.0;  // a typical value
  
  // Formulas from SPEC Sensors ULPSM-CO 968-01 document.
  // NOTE: This is for a companion board we are not using!
  /*
  float M = coSensitivityCode * 100 *1e-9 * 1e3;
  // Reference level is Vp/2, with no offset.
  // Converted to ADC units (0-1023).
  const int VGAS0 = 512 + 0;
  // Measured level
  int Vgas = readAnalog(CO_PIN);
  // CO concentration in ppm
  return (Vgas - VGAS0) / M;
  */

  // PODD board contains a potentiostat circuit as shown in the
  // SPEC Sensor Operation Overview document (Figure 3), but the
  // Counter and Reference lines seem to be reversed.
  // The output is presumably linear with CO concentration, but
  // it is unclear how to calibrate the results from this circuit.
  // It is also difficult to do a calibration by hand due to the
  // difficulty (and dangerousness) of creating a known, high-CO
  // environment.  For now simply return the the ADC output.
  // Note use of readAnalog() instead of analogRead().
  return readAnalog(CO_PIN);
}



// Particular Matter Sensor [SPS30] ------------------------------------

/* Initializes the particulate matter sensor, but does not power it up. */
void initPMSensor() {
  //pinMode(PM_PIN_P1,INPUT);
  //pinMode(PM_PIN_P2,INPUT);
  pinMode(PM_ENABLE,OUTPUT);
  // NOTE: PODD PCB only allows signals from PM to teensy and not
  //   in the reverse direction, so serial will not work through
  //   the board's PM lines.
  //sps30.SetSerialPin(PM_PIN_RX, PM_PIN_TX);
  digitalWrite(PM_ENABLE, LOW);
  resetPMData();
}


/* Power up the particulate matter sensor.
   Does not start sampling; should draw lower power in this idle
   state (documentation says < 8 mA, but PODD PCB + SPS30 system
   draws closer to 20 mA). */
void powerOnPMSensor() {
  if (pmPowered) return;
  digitalWrite(PM_ENABLE, HIGH);
  Serial.println(F("Powered on PM sensor."));
  delay(100);
  //sps30.begin(SOFTWARE_SERIAL);
  /*
  if (sps30.begin(SOFTWARE_SERIAL)) {
    Serial.println("Successfully started PM serial interface.");
  } else {
    Serial.println("Could not start PM serial interface.");
  }
  */
  //sps30.begin(I2C_COMMS);
  if (sps30.begin(I2C_COMMS)) {
    Serial.println(F("Successfully started PM sensor I2C interface."));
  } else {
    Serial.println(F("Could not start PM sensor I2C interface."));
  }
  pmPowered = true;
}


/* Power down the particulate matter sensor. */
void powerOffPMSensor() {
  if (!pmPowered) return;
  if (pmRunning) {
    stopPMSensor();
  }
  delay(100);
  digitalWrite(PM_ENABLE, LOW);
  Serial.println(F("Powered off PM sensor."));
  pmPowered = false;
}


/* Indicates if the particulate matter sensor is currently powered on. */
bool isPMSensorPowered() {
  return pmPowered;
}


/* Start taking measurements with the particulate matter sensor.
   Measurements are continually taken every second whether or not
   the sensor is polled.  The sensor takes ~ 5-8 seconds to warm up
   before the first valid data is available [cannot seem to find
   this number in documentation, but find ~ 5 seconds from
   experimentation].  Sensor draws ~ 60 mA while running.
   Note the measurements themselves do not stabilize until 80-120
   seconds after measurement mode is enabled.

   Argument indicates if routine should wait long enough for sensor
   to start up and begin taking measurements. */
void startPMSensor(bool wait) {
  if (pmRunning) return;
  if (!pmPowered) powerOnPMSensor();
  //if (!sps30.start()) return;
  if (sps30.start()) {
    Serial.println(F("Successfully started PM sensor."));
  } else {
    Serial.println(F("Could not start PM sensor."));
    return;
  }
  if (wait) delay(8000);
  pmRunning = true;
}


/* Stop the particulate matter sensor from taking measurements.
   Reduces power usage from ~ 60 mA to < 8 mA. */
void stopPMSensor() {
  if (!pmRunning) return;
  sps30.stop();
  Serial.println(F("Stopped PM sensor."));
  pmRunning = false;
}


/* Indicates if the particulate matter sensor is currently taking data. */
bool isPMSensorRunning() {
  return pmRunning;
}


/* Tests communication with the particulate matter sensor. */
bool probePMSensor() {
  if (!pmPowered) return false;
  if (!pmRunning) return false;
  return sps30.probe();
}


/* Cleans the particulate matter sensor by running the fan at high
   speed for 10 seconds.  Sensor must be powered and running.
   Returns boolean indicating if cleaning process occurred.

   Argument indicates if routine should wait long enough for sensor
   cleaning to complete. */
bool cleanPMSensor(bool wait) {
  if (!pmPowered) return false;
  if (!pmRunning) return false;
  if (!sps30.clean()) return false;
  Serial.println(F("Cleaning PM sensor.... (takes 12 seconds)"));
  if (wait) delay(12000);  // Need 10s, or 10s + spinup/down?
  return true;
}


/*  Sets the particulate matter sensor data to invalid values. */
void resetPMData() {
  pmData.MassPM1  = -1;
  pmData.MassPM2  = -1;
  pmData.MassPM4  = -1;
  pmData.MassPM10 = -1;
  pmData.NumPM0   = -1;
  pmData.NumPM1   = -1;
  pmData.NumPM2   = -1;
  pmData.NumPM4   = -1;
  pmData.NumPM10  = -1;
  pmData.PartSize = -1;
}


/* Retrieves measurements from the particulate matter sensor.
   Returns true on success.  Actual data values can be accessed
   through below routines.  Sensor must be powered on and
   running; new data is only available every 1 second
   (retrieval will fail if attempting to too soon after last
   retrieval). */
bool retrievePMData() {
  if (!pmPowered) return false;
  if (!pmRunning) return false;
  if (sps30.GetValues(&pmData) != ERR_OK) {
    resetPMData();
    return false;
  }
  return true;
}


/* Returns the most recently retrieved PM_2.5 measurement in ug/m^3
   (measurements can be retrieved using retrievePMData()).
   PM_2.5 is a measurement of particulate matter 2.5 um in diameter
   or smaller.  Sensor's particle size threshold is ~ 0.3 um.
   Returns -1 if measurement failed/invalid. */
float getPM2_5() {
  return pmData.MassPM2;
}


/* Returns the most recently retrieved PM_10 measurement in ug/m^3
   (measurements can be retrieved using retrievePMData()).
   PM_10 is a measurement of particulate matter 10 um in diameter
   or smaller.  Sensor's particle size threshold is ~ 0.3 um.
   Returns -1 if measurement failed/invalid. */
float getPM10() {
  return pmData.MassPM10;
}


/* Utility function to write dots to serial output over N consecutive
   pause intervals [ms]. */
// Sensor testing >>>>>>>>>>>>>>>>>>>>>>
#ifdef SENSOR_TESTING
void printPMPauseProgress(unsigned int N, unsigned long pause) {
  for (unsigned int k = 0; k < N; k++) {
    delay(pause);
    Serial.print('.');
    Serial.flush();
  }
}
#endif
//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<


/* Particulate matter sensor (Sensirion SPS30) testing routine.
   Will sample the sensor for the given number of cycles after an
   initial warmup period.  Sample and warmup periods in
   milliseconds. */
// Sensor testing >>>>>>>>>>>>>>>>>>>>>>
#ifdef SENSOR_TESTING
void testPMSensor(unsigned long cycles, unsigned long sampleInterval,
                  unsigned long offTime, unsigned long idleTime) {
  Serial.println();
  Serial.println(F("Particulate Matter Sensor (Sensirion SPS30) testing"));
  Serial.println(F("---------------------------------------------------"));
  Serial.println();
  Serial.println(F("Initializing particulate matter sensor...."));
  initPMSensor();
  Serial.print(F("Waiting in unpowered state ("));
  Serial.print(offTime/1000);
  Serial.print(F("s)"));
  printPMPauseProgress(offTime/1000,1000);
  Serial.println();
  Serial.println(F("Powering up particulate matter sensor...."));
  powerOnPMSensor();
  Serial.print(F("Waiting in idle mode ("));
  Serial.print(idleTime/1000);
  Serial.print(F("s)"));
  printPMPauseProgress(idleTime/1000,1000);
  Serial.println();
  
  // Debugging
  /*
  char buf[32];
  int sn_err = sps30.GetSerialNumber(buf,32);
  Serial.print("Serial number error code: ");
  Serial.println(sn_err);
  */
  
  Serial.println(F("Starting measurements...."));
  startPMSensor(false);
  unsigned long t0 = millis();
  
  // Arduino implementation of printf drops %f support to reduce
  // memory usage.  We use dsostrf instead.
  char hbuffer1[128],hbuffer2[128],hbuffer3[128];
  char sbuffer[128];
  char fbuffers[10][8];

  // Table header
  //Serial.println();
  sprintf(hbuffer1,"  %8s  %27s  %34s  %8s",
          "time[ms]"," mass concentration [ug/m3]",
          "   number concentration [#/cm3]   "," average");
  //Serial.println(hbuffer1);
  sprintf(hbuffer2,"  %8s  %6s %6s %6s %6s  %6s %6s %6s %6s %6s  %8s",
          "        "," PM1.0"," PM2.5"," PM4.0","PM10.0",
          "PM0.5"," PM1.0"," PM2.5"," PM4.0","PM10.0","size[um]");
  //Serial.println(hbuffer2);
  sprintf(hbuffer3,"  %8s  %27s  %34s  %8s",
          "--------","---------------------------",
          "----------------------------------","--------");
  //Serial.println(hbuffer3);

  // Repeat measurements cycles number of times.
  // Logic here allows endless looping for cycles=-1 (maximum unsigned long).
  for (unsigned long k = 1; k <= cycles; k++) {
    //delay(sampleInterval);
    // Break out of the testing loop if the user sends anything
    // over the serial interface.
    if (getSerialChar(sampleInterval) != (char)(-1)) break;
    
    // Repeat header every so often
    // Ensure write on first loop (k=1)
    if (k % 50 == 1) {
      Serial.println();
      Serial.println(hbuffer1);
      Serial.println(hbuffer2);
      Serial.println(hbuffer3);
    }

    unsigned long t = millis() - t0;
    if (!probePMSensor()) {
      sprintf(sbuffer," %8ld  %73s",
              t,"                       <failed to probe PM sensor>                       ");
      Serial.println(sbuffer);
      continue;
    }
    if (!retrievePMData()) {
      sprintf(sbuffer," %8ld  %73s",
              t,"                    <failed to retrieve sensor data>                     ");
      Serial.println(sbuffer);
      continue;
    }
    dtostrf(pmData.MassPM1,6,2,fbuffers[0]);
    dtostrf(pmData.MassPM2,6,2,fbuffers[1]);
    dtostrf(pmData.MassPM4,6,2,fbuffers[2]);
    dtostrf(pmData.MassPM10,6,2,fbuffers[3]);
    dtostrf(pmData.NumPM0,6,2,fbuffers[4]);
    dtostrf(pmData.NumPM1,6,2,fbuffers[5]);
    dtostrf(pmData.NumPM2,6,2,fbuffers[6]);
    dtostrf(pmData.NumPM4,6,2,fbuffers[7]);
    dtostrf(pmData.NumPM10,6,2,fbuffers[8]);
    dtostrf(pmData.PartSize,8,2,fbuffers[9]);
    sprintf(sbuffer,"%10ld  %6s %6s %6s %6s  %6s %6s %6s %6s %6s  %8s",
            t,fbuffers[0],fbuffers[1],fbuffers[2],fbuffers[3],
            fbuffers[4],fbuffers[5],fbuffers[6],fbuffers[7],fbuffers[8],
            fbuffers[9]);
    Serial.println(sbuffer);
  }

  Serial.println();
  Serial.println(F("Stopping measurements...."));
  stopPM();
  Serial.println(F("Powering down particulate matter sensor...."));
  powerOffPM();
  Serial.println(F("Particulate matter sensor testing is complete."));
  Serial.println();
}
#endif
//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
