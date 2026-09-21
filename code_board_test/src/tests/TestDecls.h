#pragma once

// Declarations for every test body, shared by the registry (TestList.cpp) and the
// definitions (TestsA/B/C.cpp).
//
// This exists because the two must agree EXACTLY. An anonymous namespace in the
// registry and `static` definitions in the test files look equivalent but are not:
// they are distinct internal-linkage namespaces in different translation units, so
// the registry's function pointers resolve to nothing and the link fails with
// "undefined reference" for every test. One header, included by both, is the fix --
// and it also means the registry cannot name a test that was never written.
//
// The names carry their printed NUMBER deliberately. The number is the operator's
// handle on a test (it is what the menu, the web page and the README all use), so
// a mismatch between the name and the registry's number column is visible at a
// glance rather than only at runtime.

#include "TestRunner.h"

namespace SwcTests {

TestRunner::Outcome Test01_BootIdentity();
TestRunner::Outcome Test02_Rails();
TestRunner::Outcome Test03_SpareGpio();
TestRunner::Outcome Test04_I2cScan();
TestRunner::Outcome Test05_DacWriteReadback();
TestRunner::Outcome Test06_DacPowerModes();
TestRunner::Outcome Test07_AdcCalibration();
TestRunner::Outcome Test08_AdcChannelsLive();
TestRunner::Outcome Test09_DacEeprom();
TestRunner::Outcome Test10_LdacNeverPulsed();

TestRunner::Outcome Test11_SensePath();
TestRunner::Outcome Test12_ServoChannel1();
TestRunner::Outcome Test13_ServoChannel2();
TestRunner::Outcome Test14_ServoLoopback1();
TestRunner::Outcome Test15_ServoLoopback2();
TestRunner::Outcome Test16_LadderInputs();
TestRunner::Outcome Test17_AuxInputs();
TestRunner::Outcome Test18_Temperature();
TestRunner::Outcome Test19_Buzzer();
TestRunner::Outcome Test20_Leds();

TestRunner::Outcome Test21_GainAutoSelect();
TestRunner::Outcome Test22_ServoTrimLoop();
TestRunner::Outcome Test23_IdleSafety();
TestRunner::Outcome Test24_UsbLink();
TestRunner::Outcome Test25_Wifi();
TestRunner::Outcome Test26_Nvs();
TestRunner::Outcome Test27_GesturePassthrough();
TestRunner::Outcome Test28_FullPassthrough();
TestRunner::Outcome Test29_ContinuityMap();
TestRunner::Outcome Test30_Endurance();

}  // namespace SwcTests
