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
