# Locate ESP-DL's helper for embedding a model file in flash
idf_build_get_property(component_targets __COMPONENT_TARGETS)
if ("___idf_espressif__esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
elseif ("___idf_esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
endif()
set(cmake_dir ${espdl_dir}/fbs_loader/cmake)   # utilities.cmake needs this variable
include(${cmake_dir}/utilities.cmake)

set(model_file ${CMAKE_CURRENT_LIST_DIR}/models/fire_det.espdl)
if (NOT EXISTS ${model_file})
    message(FATAL_ERROR "\nModel missing: copy export/fire_det_s8_esp32s3.espdl from the training "
                        "project to main/models/fire_det.espdl\n")
endif()

idf_component_register(
    SRCS geolocate.cpp app_main.cpp board_i2c.cpp camera.cpp detector.cpp buzzer.cpp storage.cpp wifi_sync.cpp
         ads1115.cpp sensors.cpp bme280.cpp vision_service.cpp settings.cpp alerts.cpp sms.cpp cloud_link.cpp device_status.cpp
    INCLUDE_DIRS . compat
    REQUIRES esp-dl esp32-camera esp_driver_i2c esp_driver_gpio esp_driver_ledc esp_driver_sdmmc
             esp_jpeg esp_timer fatfs sdmmc vfs wear_levelling esp_wifi esp_netif esp_event esp_http_server nvs_flash esp_http_client mbedtls cjson lwip)

# Put the model in flash (read in place / copied to PSRAM at boot)
target_add_aligned_binary_data(${COMPONENT_LIB} ${model_file} BINARY)

# esp32-camera declares this public header dependency in its manifest. Also link
# it explicitly so local/offline component builds work with the manager disabled.
idf_component_get_property(camera_lib esp32-camera COMPONENT_LIB)
idf_component_get_property(jpeg_lib esp_jpeg COMPONENT_LIB)
target_link_libraries(${camera_lib} PUBLIC ${jpeg_lib})
