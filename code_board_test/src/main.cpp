#include <Arduino.h>

#include "Adc.h"
#include "Log.h"
#include "TestRunner.h"

namespace FrontEnd {
void Begin();
void Loop();
}

// The SWC adapter bring-up and hardware-test firmware.
//
// This is NOT the product firmware. It is the bench tool you run on a board that
// has just come back from assembly, to find out which of the hardware functions
// work and which do not. Read README.md for the test list and the wiring each one
// needs; read src/tests/*.cpp for what each test actually asserts.
//
// Everything is single-threaded: setup() brings up the console, the ADC, the DAC
// and (optionally) the web UI, then loop() polls both front ends in turn. There is
// no task and no RTOS beyond what the Arduino core and the WiFi stack bring with
// them, deliberately -- a bench tool that can deadlock is worse than no tool.

void setup()
{
    Log::Begin();

    Log::Rule('*');
    Log::Printf("  SWC ADAPTER -- BRING-UP AND HARDWARE TEST");
    Log::Printf("  console: ROM USB-Serial-JTAG  |  Serial = HWCDC");
    Log::Printf("  this is a TEST TOOL, not the product firmware");
    Log::Rule('*');

    FrontEnd::Begin();
}

void loop()
{
    FrontEnd::Loop();
}
