# Le schermate di avvio di Elecrow (elecrow_ui) non le chiama nessuno in questo
# progetto: erano due bitmap RGB888 non compresse da 1,8 MB l'una, 3,7 MB di
# firmware e 218 MB di array C nel repository. Rimosse.
idf_component_register(
    SRCS "esp32_p4_function_ev_board.c" "my_codec.c" "bsp_stc8h1kxx.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES driver
    PRIV_REQUIRES esp_lcd usb spiffs fatfs
)
