set(SOURCES wifi_provisioning.cpp)
            
idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS .  ./include
                    REQUIRES nvs_flash esp_https_server
                    EMBED_TXTFILES web/index.html
)
