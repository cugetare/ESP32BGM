idf_component_register(
    SRCS "main.cpp"
         "audio_decoder.cpp"
         "audio_output.cpp"
         "audio_task.cpp"
         "mode_manager.cpp"
         "source_radio.cpp"
         "source_sd.cpp"
         "source_ftp.cpp"
         "network_manager.cpp"
         "captive_dns.cpp"
         "settings_store.cpp"
         "web_server.cpp"
         "ota_update.cpp"
         "log_stream.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_audio_codec esp_codec_dev
    PRIV_REQUIRES esp_driver_i2c esp_driver_i2s esp_driver_sdmmc esp_driver_gpio sdmmc fatfs vfs
                  esp_wifi esp_netif esp_event esp_http_client esp_http_server esp-tls mbedtls
                  nvs_flash json esp_timer esp_psram lwip app_update esp_app_format
    EMBED_TXTFILES "web/root.html" "web/setup.html" "web/manage.html"
                   "web/sdinfo.html" "web/diagnostics.html"
                   "web/update.html" "web/logs.html"
)
