idf_component_register(
    SRCS "bridge_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        espressif__esp_tinyusb
        sahlberg__libsmb2
        led_strip
        esp_timer
        esp_driver_uart
        esp_driver_sdmmc
        sdmmc
        esp_blockdev
        fatfs
        nvs_flash
        esp_netif
        esp_event
        esp_wifi
        lwip
)

# Per-build defaults the firmware uses when NVS holds no answer yet (timezone offset, archive
# depth). `-DBRIDGE_TZO_DEFAULT=60` on the idf.py command line only creates a CMake CACHE
# variable - it is never turned into a preprocessor define, so a build that believed it carried
# 60 shipped the hardcoded 0 instead (proven: the identifier occurred zero times in
# build/compile_commands.json, while the log line reported "+0 min vs UTC"). Forward them.
foreach(_bridge_var BRIDGE_TZO_DEFAULT BRIDGE_KEEP_DAYS_DEFAULT BRIDGE_RO_DEFAULT)
    if(DEFINED ${_bridge_var})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE "${_bridge_var}=${${_bridge_var}}")
    endif()
endforeach()
