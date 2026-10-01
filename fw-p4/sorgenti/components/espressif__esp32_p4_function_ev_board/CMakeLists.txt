# Le schermate di avvio di Elecrow (elecrow_ui) non le chiama nessuno in questo
# progetto: erano due bitmap RGB888 non compresse da 1,8 MB l'una, 3,7 MB di
# firmware e 218 MB di array C nel repository. Rimosse.
idf_component_register(
    SRCS "esp32_p4_function_ev_board.c" "my_codec.c" "bsp_stc8h1kxx.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES driver
    # Dalla 6.0 il componente "driver" non porta piu' dentro i driver dei
    # singoli periferici: questo BSP usa ledc (retroilluminazione), i2s e
    # i2s_pdm (audio e microfono), uart e sdmmc, e se li deve chiedere.
    PRIV_REQUIRES esp_lcd usb spiffs fatfs
                  esp_driver_ledc esp_driver_i2s esp_driver_uart
                  esp_driver_sdmmc esp_driver_gpio esp_driver_i2c
)
