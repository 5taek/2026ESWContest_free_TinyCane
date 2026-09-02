set(FALL_DETECTION_EDGE_IMPULSE_DATA_FORWARDER OFF CACHE BOOL
    "Build firmware that streams raw LSM9DS1 accelerometer and gyroscope samples for Edge Impulse")

set(FALL_DETECTION_TEST_MODE OFF CACHE BOOL
    "Enable human-readable fall-detection and keyword-spotting logs on the shared USB port")

set(FALL_DETECTION_SRCS
    "accelerometer_handler.cpp")

if(FALL_DETECTION_EDGE_IMPULSE_DATA_FORWARDER)
    list(APPEND FALL_DETECTION_SRCS
        "edge_impulse_data_forwarder_main.cc")
    set(FALL_DETECTION_REQUIRES driver esp-tflite-micro)
else()
    file(GLOB_RECURSE KWS_EDGE_IMPULSE_SRCS
        "edge-impulse-sdk/*.cpp"
        "edge-impulse-sdk/*.c"
        "edge-impulse-sdk/*.cc"
        "tflite-model/*.cpp"
        "tflite-model/*.c")
    list(APPEND FALL_DETECTION_SRCS
        "Fall_detection.cc"
        "ai_inference_lock.cc"
        "fall_alert_sender.cc"
        "keyword_spotting.cc"
        "../models/fall_model.cc"
        "model_input_provider.cc"
        ${KWS_EDGE_IMPULSE_SRCS})
    set(FALL_DETECTION_REQUIRES
        driver esp-dsp esp_event esp_http_client esp_netif esp-tflite-micro
        esp_timer esp_wifi nvs_flash mbedtls esp_driver_usb_serial_jtag)
endif()

idf_component_register(
    SRCS ${FALL_DETECTION_SRCS}
    INCLUDE_DIRS "."
                 "edge-impulse-sdk"
                 "model-parameters"
                 "tflite-model"
                 "edge-impulse-sdk/porting/espressif/esp-dsp/modules/common/include"
    REQUIRES ${FALL_DETECTION_REQUIRES}
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    "-Wno-error"
    "-Wno-format"
    "-Wno-unused-but-set-parameter"
    "-Wno-missing-field-initializers"
    "-Wno-error=maybe-uninitialized"
    "-Wno-error=uninitialized"
    "-Wa,-I${CMAKE_CURRENT_SOURCE_DIR}")

if(FALL_DETECTION_TEST_MODE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FALL_DETECTION_TEST_MODE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FALL_DETECTION_TEST_MODE=0)
endif()

if(NOT FALL_DETECTION_EDGE_IMPULSE_DATA_FORWARDER)
    # The KWS classifier's tflite_micro.h otherwise ei_aligned_calloc()s its
    # 12006-byte tensor arena fresh on every single inference call and frees
    # it right after. On this board (no PSRAM, ~30KB free internal heap once
    # Wi-Fi/TLS/the KWS ring buffer are up) that repeated alloc/free is
    # exactly what was intermittently failing with "Failed to allocate
    # TFLite arena" -- reserving it once at link time removes the class of
    # failure entirely, the same way Fall_detection.cc's own tensor_arena
    # already is a static array.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EI_CLASSIFIER_ALLOCATION_STATIC=1)
endif()
