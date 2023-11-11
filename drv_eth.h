/* *****************************************************************************
 * File:   drv_eth.h
 * Author: Dimitar Lilov
 *
 * Created on 2022 06 18
 * 
 * Description: ...
 * 
 **************************************************************************** */
#pragma once

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */


/* *****************************************************************************
 * Header Includes
 **************************************************************************** */
#include <stdint.h> 
#include "esp_err.h"  
#include "esp_netif.h"
#include "esp_eth.h"

/* *****************************************************************************
 * Configuration Definitions
 **************************************************************************** */

/* *****************************************************************************
 * Constants and Macros Definitions
 **************************************************************************** */

/* *****************************************************************************
 * Enumeration Definitions
 **************************************************************************** */
typedef enum
{
    DRV_ETH_IF_0,
    DRV_ETH_IF_1,
    DRV_ETH_IF_2,
    DRV_ETH_IF_3,
    DRV_ETH_IF_COUNT
} drv_eth_interface_t;


/* *****************************************************************************
 * Type Definitions
 **************************************************************************** */

/* *****************************************************************************
 * Function-Like Macro
 **************************************************************************** */

/* *****************************************************************************
 * Variables External Usage
 **************************************************************************** */ 

/* *****************************************************************************
 * Function Prototypes
 **************************************************************************** */
bool drv_eth_get_connected(int index);
esp_eth_handle_t drv_eth_get_handle(int index);
esp_netif_t* drv_eth_get_netif(int index);
int drv_eth_get_netif_count(void);
void drv_eth_load_config(void);
void drv_eth_set_dynamic_ip(esp_netif_t *netif);
void drv_eth_set_dhcp_flag(uint32_t bDHCP);
void drv_eth_set_last_as_static_ip(esp_netif_t *netif, bool bSkipSave);
void drv_eth_set_static_ip(esp_netif_t *netif, const char *ip_address, const char *ip_netmask, const char *gw_address, bool bSkipSave);
uint32_t drv_eth_get_netmask_0(void);
//void drv_eth_netif_init_passed(void);
void drv_eth_print(void);
void drv_eth_wait_get_ip_ms(int timeout);
void drv_eth_init(void);
esp_err_t drv_eth_get_mac(drv_eth_interface_t ifx, uint8_t mac[6]);

#ifdef __cplusplus
}
#endif /* __cplusplus */


