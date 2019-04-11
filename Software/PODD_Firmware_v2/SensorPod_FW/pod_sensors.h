
/*
 * pod_sensors.h
 * 2017 - Nick Turner and Morgan Redfield
 * Licensed under the AGPLv3. For full license see LICENSE.md 
 * Copyright (c) 2017 LMN Architects, LLC
 */

#ifndef POD_SENSORS_H
#define POD_SENSORS_H

#include "Arduino.h"

#define MIC_PIN A0
#define PM_ENABLE 42

#define MIN_RH_TEMP -40 // Operating range, degrees C
#define MAX_RX_TEMP 125 // Operating range, degress C
#define MIN_RH_HUM 0
#define MAX_RX_HUM 100
#define MIN_LIGHT 0     // Sensor range, lux
#define MAX_LIGHT 80000 // Sensor range, lux
#define MIN_GLOBE_TEMP -67 // Operating range, degrees F
#define MAX_GLOBE_TEMP 176 // Operating range, degrees F
#define MIN_SOUND 200
#define MAX_SOUND 1500
#define MIN_CO2 0 // Operating range, ppm
#define MAX_CO2 2000 // Operating range, ppm 
#define MIN_CO 0 // Operating range, ppm
#define MAX_CO 1000 // Operatiing range, ppm
#define MIN_PM2_5 -1
#define MAX_PM2_5 10000
#define MIN_PM10 -1
#define MAX_PM10 10000

// Define this to enable particulate matter sensor testing
// output and routines.
#define PM_TESTING


//--------------------------------------------------------------------------------------------- [Sensor Reads]

void sensorSetup();
bool verifySensors();

// get readings
float getRHTemp();
float getRHHum();
float getLight();
double getGlobeTemp();
double getSound();
int getCO2();
float getCO();

// Uncomment to enable code for corresponding particulate matter sensor.
// If none are defined, dummy code will be provided.
//#define USE_OLD_PM
#define USE_SPS30_PM
//#define USE_SMPWM01C_PM

// Original particulate matter sensor routines
#if defined(USE_OLD_PM)
void updatePM();
float getPM2_5();
float getPM10();
// Sensirion SPS30 Particulate Matter Sensor routines
#elif defined(USE_SPS30_PM)
void initPM();
void powerOnPM();
void powerOffPM();
bool isPMPowered();
void startPM(bool wait=false);
void stopPM();
bool isPMRunning();
bool probePM();
bool cleanPM(bool wait=false);
void resetPMData();
bool retrievePMData();
inline void updatePM(){retrievePMData();}  // for compatibility
float getPM2_5();
float getPM10();
// Below only used for testing
#ifdef PM_TESTING
void printPMPauseProgress(unsigned int N, unsigned long pause = 1000);
void testPMSensor(unsigned int cycles, unsigned long sampleTime = 3000,
                  unsigned long offTime = 10000, unsigned long idleTime = 10000);
#endif
// SM-PWM-01C Particulate Matter Sensor routines
#elif defined(USE_SMPWM01C_PM)
void initPM();
void startPM();
void stopPM();
void startPMSampling();
void stopPMSampling(unsigned long updateInterval = -1);
bool isPMSampling();
void resetPMSampling();
void processPMPulseISR();
void processPM();
float getPM2();
float getPM10();
// Below only used for testing
#ifdef PM_TESTING
void printPMPauseProgress(unsigned int N, unsigned long pause = 1000);
void testPMSensor(unsigned int cycles, unsigned long sampleTime = 30000,
                  unsigned long warmupTime = 90000);
#endif
// Dummy particulate matter sensor routines
#else
inline void initPM(){}
inline void updatePM(){}
inline void powerUpPM() {}
inline void powerDownPM() {}
inline bool isPMPowered(){return false;}
inline void startPM(bool wait=false){}
inline void stopPM(){}
inline bool isPMRunning(){return false;}
inline bool probePM(){return false;}
inline bool cleanPM(bool wait=false);
inline void resetPMData(){}
inline bool retrievePMData(){return false;}
inline float getPM2_5(){return -1;}
inline float getPM10(){return -1;}
#endif

#endif
