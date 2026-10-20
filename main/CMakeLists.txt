file (GLOB SOURCES *.c)
idf_component_register(SRCS ${SOURCES}
                       INCLUDE_DIRS "."
                       REQUIRES jkbms nvs_flash esp_lcd esp_driver_spi esp_driver_gpio esp_timer)
