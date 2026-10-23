idf_component_register(SRCS "main.c" "jbuf.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES usb_host_uac usb esp_wifi esp_event nvs_flash esp_timer esp_adc esp_driver_pcnt esp_driver_gpio linkproto)
if(DEFINED LINK_DIAG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${LINK_DIAG})
endif()
