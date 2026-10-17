idf_component_register(SRCS "main.c" "usb_descriptors.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES tinyusb usb esp_hw_support esp_wifi esp_event nvs_flash esp_timer linkproto
                       WHOLE_ARCHIVE)
if(DEFINED LINK_DIAG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${LINK_DIAG})
endif()
