file (GLOB SOURCES *.c)
idf_component_register(SRCS ${SOURCES}
                       INCLUDE_DIRS "."
                       REQUIRES bt esp_timer nvs_flash)
