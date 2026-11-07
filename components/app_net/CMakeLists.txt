idf_component_register(SRCS "src/app_net.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_mosaico_app_recovery)
