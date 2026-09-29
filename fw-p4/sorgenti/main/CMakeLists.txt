idf_component_register(
    SRCS main.cpp
    INCLUDE_DIRS .)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE 
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)

if(CONFIG_ESP_BROOKESIA_MEMORY_USE_CUSTOM)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "esp-brookesia" OR COMPONENT MATCHES "espressif__esp-brookesia")
            set(TARGET_COMPONENT ${COMPONENT})
            break()
        endif()
    endforeach()

    if(TARGET_COMPONENT STREQUAL "")
        message(FATAL_ERROR "Component 'esp-brookesia' not found.")
    else()
        idf_component_get_property(ESP_BROOKESIA_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
    endif()

    target_compile_options(
        ${ESP_BROOKESIA_LIB}
        PUBLIC
            "-DESP_BROOKESIA_MEMORY_CUSTOM_MALLOC(x)=heap_caps_aligned_alloc(1, x, MALLOC_CAP_SPIRAM)"  "-DESP_BROOKESIA_MEMORY_CUSTOM_FREE=free"
    )
endif()

if(CONFIG_LV_MEM_CUSTOM)
    # Allocatore di LVGL: alloc, realloc e free passano tutti da lvgl_mem.c,
    # che sceglie RAM interna o PSRAM (Impostazioni > Debug) e conta i byte.
    # Prima solo l'alloc era deviato in PSRAM e i blocchi ridimensionati
    # potevano finire in RAM interna.
    target_sources(${LVGL_LIB} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lvgl_mem.c)
    target_compile_options(
        ${LVGL_LIB}
        PRIVATE
            "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/lvgl_mem.h"
            "-DLV_MEM_CUSTOM_ALLOC=lvgl_mem_alloc"
            "-DLV_MEM_CUSTOM_REALLOC=lvgl_mem_realloc"
            "-DLV_MEM_CUSTOM_FREE=lvgl_mem_free"
    )
endif()

# Senza FLASH_IN_PROJECT "idf.py flash" non riscrive la partizione dati:
# aggiornare il firmware non cancella piu' i ritocchi della dashboard ne' la
# configurazione dello slideshow. Un pannello nuovo la formatta da solo al
# primo avvio (CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL). Per azzerarla di
# proposito c'e' "Cancella tutto e riparti" nelle impostazioni, oppure
# "idf.py storage-flash" da qui.
spiffs_create_partition_image(storage ../spiffs)