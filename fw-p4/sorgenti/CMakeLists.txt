# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# The configuration is to prevent compilation errors on the Windows system.
set(CMAKE_OBJECT_PATH_MAX 512)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# --- Ponte per ESP-IDF 6 -----------------------------------------------------
#
# Fino alla 5.x il vecchio componente "driver" tirava dentro tutti i driver dei
# singoli periferici: bastava dipendere da lui e si avevano driver/gpio.h,
# driver/i2s_std.h, driver/ledc.h e compagnia. Dalla 6.0 non lo fa piu', e
# ogni componente deve chiedersi i suoi.
#
# Il nostro codice lo si sistema (e lo si e' fatto), ma i componenti di terzi
# no: esp-audio-player e esp_codec_dev danno per scontato il vecchio
# comportamento e non si possono correggere, perche' il gestore dei componenti
# li riscarica e ogni modifica sparisce.
#
# Questa riga rimette quei driver fra i requisiti comuni a tutti i componenti,
# che e' esattamente quello che faceva la 5.x. E' lo stesso meccanismo che usa
# ESP-IDF al proprio interno per l'architettura. Il giorno in cui i componenti
# di terzi si aggiorneranno, queste righe si possono togliere una per una.
idf_build_set_property(__COMPONENT_REQUIRES_COMMON esp_driver_gpio  APPEND)
idf_build_set_property(__COMPONENT_REQUIRES_COMMON esp_driver_i2c   APPEND)
idf_build_set_property(__COMPONENT_REQUIRES_COMMON esp_driver_i2s   APPEND)
idf_build_set_property(__COMPONENT_REQUIRES_COMMON esp_driver_spi   APPEND)
idf_build_set_property(__COMPONENT_REQUIRES_COMMON esp_driver_ledc  APPEND)
idf_build_set_property(__COMPONENT_REQUIRES_COMMON esp_driver_uart  APPEND)
# -----------------------------------------------------------------------------

# set(EXTRA_COMPONENT_DIRS
#     ../common_components
#     )

set(CXX_COMPILE_OPTIONS
    -Wno-deprecated-enum-enum-conversion
    -Wno-write-strings
)

add_compile_options(
    -Wno-format
    -Wno-unused-variable
    -Wno-missing-field-initializers
    -Wno-unused-but-set-variable
    -Wno-cast-function-type
    -Wno-ignored-qualifiers
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-enum-enum-conversion")

project(HA_Display VERSION 0.3.3)
