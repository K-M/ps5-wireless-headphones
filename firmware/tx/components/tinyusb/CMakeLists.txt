# Minimal IDF wrapper around upstream TinyUSB (hathach/tinyusb), device + audio only.
idf_component_register(
    SRCS
        "src/tusb.c"
        "src/common/tusb_fifo.c"
        "src/device/usbd.c"
        "src/class/audio/audio_device.c"
        "src/portable/synopsys/dwc2/dcd_dwc2.c"
        "src/portable/synopsys/dwc2/dwc2_common.c"
    INCLUDE_DIRS "src" "config"
    REQUIRES freertos
    PRIV_REQUIRES esp_hw_support soc)

target_compile_options(${COMPONENT_LIB} PUBLIC "-DCFG_TUSB_MCU=OPT_MCU_ESP32S3")
set_source_files_properties("src/device/usbd.c" PROPERTIES COMPILE_FLAGS "-Wno-type-limits")
