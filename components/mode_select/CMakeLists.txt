idf_component_register(SRCS "mode_select.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash esp_partition app_update spiffs esp_rcp_update
                                     driver esp_timer)
