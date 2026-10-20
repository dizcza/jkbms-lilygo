file (GLOB SOURCES *.c)
idf_component_register(SRCS ${SOURCES}
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "www/update.html"
                       REQUIRES jkbms nvs_flash esp_lcd esp_driver_spi esp_driver_gpio esp_timer
                                esp_wifi esp_netif esp_event esp_http_client json esp_driver_uart
                                esp_http_server app_update)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # Build-time secrets: KEY=VALUE lines of the secrets file, overridden by
    # environment variables of the same name. Run `idf.py reconfigure` after changing them.
    idf_build_get_property(project_dir PROJECT_DIR)
    set(JKBMS_SECRETS_FILE "${project_dir}/../secrets/jkbms/firmware.env")
    set(secret_vars ENV_JKBMS_UPLOAD_URL ENV_JKBMS_API_TOKEN
                    ENV_JKBMS_WIFI_SSID ENV_JKBMS_WIFI_PASS
                    ENV_JKBMS_WIFI_SSID1 ENV_JKBMS_WIFI_PASS1
                    ENV_WIFI_STA_SSID2 ENV_WIFI_STA_PASS2)
    if(EXISTS "${JKBMS_SECRETS_FILE}")
        file(STRINGS "${JKBMS_SECRETS_FILE}" secret_lines REGEX "^[A-Z0-9_]+=.+")
        foreach(line ${secret_lines})
            string(REGEX MATCH "^([A-Z0-9_]+)=(.*)$" _ "${line}")
            set(name "${CMAKE_MATCH_1}")
            set(value "${CMAKE_MATCH_2}")
            # VALUE, "VALUE" and 'VALUE' are the same; quotes keep spaces (e.g. in an SSID) readable
            # one MATCHES per if(): a failed second match would clear CMAKE_MATCH_1
            if(value MATCHES "^\"(.*)\"$")
                set(value "${CMAKE_MATCH_1}")
            elseif(value MATCHES "^'(.*)'$")
                set(value "${CMAKE_MATCH_1}")
            endif()
            set(secret_${name} "${value}")
        endforeach()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${JKBMS_SECRETS_FILE}")
    else()
        message(WARNING "${JKBMS_SECRETS_FILE} not found")
    endif()
    foreach(var ${secret_vars})
        if(DEFINED ENV{${var}} AND NOT "$ENV{${var}}" STREQUAL "")
            set(secret_${var} "$ENV{${var}}")
        endif()
        if(DEFINED secret_${var})
            target_compile_definitions(${COMPONENT_LIB} PRIVATE ${var}="${secret_${var}}")
        endif()
    endforeach()
    if(NOT DEFINED secret_ENV_JKBMS_UPLOAD_URL OR NOT DEFINED secret_ENV_JKBMS_API_TOKEN)
        message(WARNING "ENV_JKBMS_UPLOAD_URL or ENV_JKBMS_API_TOKEN is not set: status uploads are disabled")
    endif()
endif()
