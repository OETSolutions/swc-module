// The Unity output plumbing, matching PlatformIO's espidf block exactly.
//
// Providing a custom test/unity_config.h makes PlatformIO SKIP generating both
// unity_config.h and unity_config.c, so these four functions must exist here.
// Their names and signatures are fixed by what PlatformIO's generated header
// declares -- they are C, not C++, and must not be mangled.
//
// putchar goes to the ROM USB-Serial-JTAG console (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG),
// which is where `pio test -e esp32s3` reads results from.

#include <stdio.h>

void unityOutputStart(unsigned long baudrate)
{
    // The console is already up by the time app_main runs, and the baud rate is
    // meaningless for USB-Serial-JTAG. Accepted and ignored to match the
    // signature PlatformIO's unity_config.h declares.
    (void)baudrate;
}

void unityOutputChar(unsigned int c)
{
    putchar(c);
}

void unityOutputFlush(void)
{
    fflush(stdout);
}

void unityOutputComplete(void)
{
}
