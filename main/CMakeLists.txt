set(SOURCES
    "app_main.cc"
    "agent_mqtt.cc"
    "battery_monitor.cc"
    "button_manager.cc"
    "button_wrapper.cc"
    "note_store.cc"
    "red_block_pet.cc"
    "rlcd_display.cc"
    "serial_config.cc"
    "shtc3_sensor.cc"
    "ui.cc"
    "wechat_bot.cc"
    "wifi_manager.cc"
    "../third_party/qrcodegen/qrcodegen.c"
)

idf_build_get_property(build_components BUILD_COMPONENTS)

function(find_component_by_pattern PATTERN COMPONENT_VAR)
    set(found "")
    foreach(component ${build_components})
        if(component MATCHES "${PATTERN}")
            set(found "${component}")
            break()
        endif()
    endforeach()
    set(${COMPONENT_VAR} "${found}" PARENT_SCOPE)
endfunction()

find_component_by_pattern("xiaozhi-fonts" WEC_FONTS_COMPONENT)
find_component_by_pattern("esp_lvgl_port" WEC_LVGL_PORT_COMPONENT)
if(CONFIG_WEC_PRODUCT_DISK)
    list(APPEND SOURCES "usb_product_disk.cc")
    find_component_by_pattern("esp_tinyusb" WEC_TINYUSB_COMPONENT)
    set(WEC_PRODUCT_DISK_REQUIRES
        fatfs
        wear_levelling
        ${WEC_TINYUSB_COMPONENT}
    )
endif()

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "." "../third_party/qrcodegen"
    REQUIRES
        nvs_flash
        spiffs
        esp_event
        esp_netif
        esp_wifi
        esp_http_client
        mqtt
        esp_timer
        esp_psram
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_adc
        mbedtls
        json
        button
        lvgl
        ${WEC_LVGL_PORT_COMPONENT}
        ${WEC_FONTS_COMPONENT}
        ${WEC_PRODUCT_DISK_REQUIRES}
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
