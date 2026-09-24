# The pure-logic half of the firmware, built as ONE IDF component.
#
# This file is load-bearing and was missing: without it, `code/lib/` is not a
# component, so the IDF build compiles only `src/` and NEVER LINKS A SINGLE LINE
# of the modules under lib/. The build succeeds and the binary looks healthy --
# it was 215 KB, exactly Task 1's bare-main.c baseline -- which is why the gap
# survived eleven tasks. `nm firmware.elf | grep GainPolicy` returning nothing is
# what exposed it.
#
# ONE component rather than one per subdirectory (Analog/, Config/, System/, ...)
# because the modules include each other by a path rooted at lib/ (`#include
# "Config/ConfigModel.h"`), so lib/ must be a single include root. Splitting it
# would mean nine CMakeLists.txt files each re-declaring that root and the
# REQUIRES chain between them.
file(GLOB_RECURSE swc_lib_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

idf_component_register(
    SRCS ${swc_lib_sources}
    INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    # Each entry here is the component that OWNS a header this library includes.
    # IDF does not put driver headers on the include path by default, so a
    # missing entry is a `fatal error: driver/gpio.h: No such file or directory`
    # rather than anything about REQUIRES.
    #
    #   json          cJSON, for ConfigCodec. IDF 5.5.5 bundles 1.7.19 at
    #                 framework-espidf/components/json. It is NOT in the common
    #                 requires, so it must be named or ConfigCodec.cpp fails to
    #                 find cJSON.h.
    #   esp_driver_gpio  driver/gpio.h        -- EspHal's LEDs, buzzer, ~LDAC
    #   esp_driver_i2c   driver/i2c_master.h  -- EspHal's MCP4728 link
    #   esp_adc          esp_adc/*.h          -- EspHal's ADC + eFuse calibration
    #   nvs_flash        nvs.h                -- EspHal's NVS blob access
    #   esp_timer        esp_timer.h          -- EspHal's now_ms/now_us
    #   app_update       esp_ota_ops.h        -- OtaUsb's slot read + boot switch
    #   esp_http_client  esp_http_client.h    -- OtaWifi's manifest + image fetch
    #   esp_https_ota    esp_https_ota.h      -- the esp_https_ota path (spec 9.5)
    #   mbedtls          -- OtaWifi's trust ANCHOR is a PINNED CA PEM
    #                    (`Update/ReleaseCa.h`, N-62 fixed), so `esp_crt_bundle`
    #                    is no longer used. mbedtls is still listed because the
    #                    verified-TLS path (`esp-tls`/`esp_http_client`) needs it;
    #                    without it the temptation is to drop verification and
    #                    reach for `setInsecure`, which spec 9.5 forbids outright.
    #   esp_tinyusb      tinyusb.h, tusb_cdc_acm.h -- the CDC transport. This is a
    #                    MANAGED component, not an IDF one: IDF 5.5.5 has no
    #                    components/tinyusb, and src/idf_component.yml is what
    #                    fetches it. Declaring it here is also what makes the
    #                    include path exist; without it UsbCdc.cpp fails with
    #                    "tinyusb.h: No such file or directory".
    #
    # The maintenance radio's stack (spec 8.1-8.4, MaintenanceRadio.cpp):
    #   esp_http_server  esp_http_server.h  -- the page and its /api/* endpoints
    #   esp_wifi         esp_wifi.h         -- the driver, in AP+STA for the page
    #   esp_netif        esp_netif.h        -- the AP's address, and the netif the
    #                    page is reachable on before the device joins a network
    #   esp_event        esp_event.h        -- the default loop the WiFi driver and
    #                    the provisioning manager both post to
    #   wifi_provisioning wifi_provisioning/*.h -- the manager + BLE scheme. Its
    #                    own CMakeLists REQUIRES protocomm and PRIV_REQUIRES bt, so
    #                    those are pulled transitively; `bt` is named here anyway
    #                    because ESP_MAC_WIFI_* and the controller's memory release
    #                    are reached from this library's own file.
    #   esp_hw_support   esp_mac.h          -- esp_read_mac, the source of BOTH the
    #                    BLE PoP and the web token (spec 8.3/8.4)
    REQUIRES json esp_driver_gpio esp_driver_i2c esp_adc nvs_flash esp_timer
             app_update esp_http_client esp_https_ota mbedtls esp_tinyusb
             esp_http_server esp_wifi esp_netif esp_event wifi_provisioning bt
             esp_hw_support
)
