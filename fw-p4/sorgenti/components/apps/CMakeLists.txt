set(APPS_DIR ./)
# I sorgenti sono raccolti a tappeto. Attenzione: aggiungendo un file nuovo
# CMake non se ne accorge da solo e il collegamento fallisce con "undefined
# reference"; serve un "idf.py reconfigure". (CONFIGURE_DEPENDS qui non si puo'
# usare: ESP-IDF rilegge questo file anche in modalita' script.)
file(GLOB_RECURSE APPS_C_SRCS ${APPS_DIR}/*.c)
file(GLOB_RECURSE APPS_CPP_SRCS ${APPS_DIR}/*.cpp)

idf_component_register(
    SRCS ${APPS_C_SRCS} ${APPS_CPP_SRCS}
    INCLUDE_DIRS ${APPS_DIR}
    # copia di riserva della pagina dell'editor (usata se la SD non ce l'ha):
    # la genera strumenti/costruisci_web.bat da V0.2/web/panel.html
    EMBED_FILES ${APPS_DIR}/home_dashboard/assets/panel.html.gz
    REQUIRES lvgl__lvgl esp_event esp_wifi nvs_flash esp_driver_jpeg esp_mm esp-brookesia bsp_extra esp32_p4_function_ev_board espressif__esp_lcd_touch_gt911 esp_websocket_client esp_http_server esp_https_server esp_http_client esp_netif esp_driver_ppa chmorgan__esp-audio-player driver mbedtls espressif__esp_hosted spiffs fatfs esp_app_format app_update espressif__mdns)

# -Wformat resta ACCESO, al contrario del resto del progetto: una stringa di
# formato sbagliata non e' una questione di stile. In batteria.cpp i
# segnaposti erano dodici per quattordici argomenti, e le due colonne che
# servivano a ricontrollare la taratura non sono mai state scritte per un
# ciclo intero di batteria. Il compilatore lo sapeva e non poteva dirlo.
#
# -Wformat-truncation resta spento, ed e' una distinzione non una pigrizia:
# un snprintf che taglia e' sicuro e definito, un argomento di troppo o del
# tipo sbagliato non lo e'. I punti che taglierebbero sono dieci, elencati
# in docs/formato-troncamento.md, e si guardano uno per uno.
target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wformat
        -Wno-format-truncation
        -Wno-incompatible-pointer-types
        -Wno-int-conversion
    >
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wformat
        -Wno-format-truncation
        -Wno-conversion-null
        -Wno-attributes
        -Wno-write-strings
    >
    -DLV_LVGL_H_INCLUDE_SIMPLE
)
