# BMI270 accelerometer on the Tab5's internal I2C bus.
# The sensor needs Bosch's firmware blob, which comes with Bosch's open-source driver. Download it once
# with tools\get_bmi270.ps1 (puts bmi2.c, bmi270.c and headers in components/imu/bosch). Without those
# files this component still builds, but imu_init() reports "driver missing" and auto-rotate stays off.
set(srcs "imu.c")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/bosch/bmi2.c" AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/bosch/bmi270.c")
    list(APPEND srcs "bosch/bmi2.c" "bosch/bmi270.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "bosch"
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES esp_rom freertos log)

# The board package already links Espressif's own BMI270 component, which defines a symbol with the
# same name as Bosch's firmware blob. Give our copy a private name so the two can coexist.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/bosch/bmi270.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE bmi270_config_file=imu_bmi270_config_file)
endif()
