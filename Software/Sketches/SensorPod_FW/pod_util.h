/*==============================================================================
  Various utility constants and functions.

  This file is part of the LMN PODD distribution:
    https://github.com/lmnts/PODD

  CONTRIBUTORS:
    Chris Savage (2018)

  COPYRIGHT/LICENSE:
  Copyright (c) 2018 LMN Architects

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.

==============================================================================*/

#pragma once

// Standard libraries
// Contributed libraries
#include <Arduino.h>
// Local headers

// Define DEBUG, PODD_DEBUG, or PODD_UTIL_DEBUG to enable
// debug statements below
#if defined(DEBUG) && !defined(PODD_DEBUG)
#define PODD_DEBUG
#endif
#if defined(PODD_DEBUG) && !defined(PODD_UTIL_DEBUG)
#define PODD_UTIL_DEBUG
#endif


// Types =======================================================================

// To avoid string constants from using up space in dynamic memory,
// PROGMEM or the F() macro can be used to store them in flash.
// However, when using something like:
//   static const char LINE[] PROGMEM = "----";
// LINE must cast as (const __FlashStringHelper*) when printed:
//   Serial.print((const __FlashStringHelper*)LINE);
// Instead, LINE would print normally if we use:
//   const __FlashStringHelper *LINE = F("----");
//   Serial.print(LINE);
// We define a typedef FType to simplify this: 
//   FType LINE = F("----");
// ASIDE: In the first case, "static" in required to make PROGMEM
//        work as intended (unclear why).
typedef const __FlashStringHelper * FType;


// Constants/global variables ==================================================


// Functions ===================================================================

// Returns the amount of RAM available to the stack and/or heap.
size_t freeRAM();

// Writes to serial the status of the given pin, with optional
// label to include in output.
void pinCheck(const int pin, const String s="");
// Routine to output status of various PODD pins.  Used for
// debugging.
void poddPinChecks();

// Prints compilation info to serial output, with optional prefix
// for each output line and name of compilation file (defaults to
// main compilation input file).
void printCompilationInfo(const String prefix="", const String file=__BASE_FILE__);


//==============================================================================
