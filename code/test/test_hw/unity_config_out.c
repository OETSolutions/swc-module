// The Unity output plumbing, matching PlatformIO's espidf block exactly.
//
// Providing a custom test/unity_config.h makes PlatformIO SKIP generating both
// unity_config.h and unity_config.c, so these four functions must exist here.
// Their names and signatures are fixed by what PlatformIO's generated header
// declares -- they are C, not C++, and must not be mangled.
//
// putchar goes to the console. N-16 (fixed 2026-09-25): the console PRIMARY is
// UART0 (TP7/TP8 on this board, `CONFIG_ESP_CONSOLE_UART_DEFAULT`), with
// USB-Serial-JTAG as the SECONDARY (`ESP_ROM_CONSOLE_OUTPUT_SECONDARY` duplicates
// output there), which is where `pio test -e esp32s3` reads results from. UART0 is
// OFF the USB PHY, so this stays readable even after TinyUSB takes the PHY; a
// Serial-JTAG primary would not (spec 4.1).

#include <stdio.h>

void unityOutputStart(unsigned long baudrate)
{
    // The console is already up by the time app_main runs. Accepted and ignored
    // to match the signature PlatformIO's unity_config.h declares.
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
