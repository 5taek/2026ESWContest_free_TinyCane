idf_component_register(
    SRCS "src/vl53l5cx_api.c" "src/platform.c"  
    INCLUDE_DIRS "inc"         # 헤더 파일(.h)이 있는 폴더 이름
    REQUIRES esp_driver_i2c soc hal
)