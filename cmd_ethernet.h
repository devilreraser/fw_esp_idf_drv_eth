/* Console example — declarations of command registration functions.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <sdkconfig.h>
#if CONFIG_USE_ETHERNET

// Register iperf Ethernet functions
//void register_ethernet(void);
void cmd_ethernet_iperf_register(void);

#endif //#if CONFIG_USE_ETHERNET

#ifdef __cplusplus
}
#endif
