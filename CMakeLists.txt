idf_component_register(SRCS "cmd_eth.c" "drv_eth.c" "cmd_ethernet.c"
                    INCLUDE_DIRS "." 
                    REQUIRES    "driver" 
                                "lwip" 
                                "console" 
                                "esp_eth"
                                "drv_wifi" 
                                "drv_socket" 
                                "app_socket" 
                                      )
                 

