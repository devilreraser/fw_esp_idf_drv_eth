/* *****************************************************************************
 * File:   cmd_eth.c
 * Author: Dimitar Lilov
 *
 * Created on 2022 06 18
 * 
 * Description: ...
 * 
 **************************************************************************** */

/* *****************************************************************************
 * Header Includes
 **************************************************************************** */
#include "cmd_eth.h"
#include "drv_eth.h"

#include <string.h>

#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"

#include "argtable3/argtable3.h"

/* *****************************************************************************
 * Configuration Definitions
 **************************************************************************** */
#define TAG "cmd_eth"

/* *****************************************************************************
 * Constants and Macros Definitions
 **************************************************************************** */

/* *****************************************************************************
 * Enumeration Definitions
 **************************************************************************** */

/* *****************************************************************************
 * Type Definitions
 **************************************************************************** */

/* *****************************************************************************
 * Function-Like Macros
 **************************************************************************** */

/* *****************************************************************************
 * Variables Definitions
 **************************************************************************** */

static struct {
    struct arg_str *device;
    struct arg_str *ip;
    struct arg_str *mask;
    struct arg_str *gw;
    struct arg_str *dhcp;
    struct arg_end *end;
} eth_args;

char null_string_eth[] = "";

/* *****************************************************************************
 * Prototype of functions definitions
 **************************************************************************** */

/* *****************************************************************************
 * Functions
 **************************************************************************** */
static int update_eth(int argc, char **argv)
{
    ESP_LOGI(__func__, "argc=%d", argc);
    for (int i = 0; i < argc; i++)
    {
        ESP_LOGI(__func__, "argv[%d]=%s", i, argv[i]);
    }

    int nerrors = arg_parse(argc, argv, (void **)&eth_args);
    if (nerrors != ESP_OK)
    {
        arg_print_errors(stderr, eth_args.end, argv[0]);
        return ESP_FAIL;
    }

    const char* device_interface = eth_args.device->sval[0];
    if (strlen(device_interface) > 0)
    {
        esp_netif_t *netif = NULL;
        if (strcmp(device_interface,"eth") == 0)
        {
            ESP_LOGI(TAG, "Eth 0 Interface Selected");
            netif = drv_eth_get_netif(0);
        }
        else if (strcmp(device_interface,"eth0") == 0)
        {
            ESP_LOGI(TAG, "Eth 0 Interface Selected");
            netif = drv_eth_get_netif(0);
        }

        const char* ip_address = NULL;
        const char* ip_netmask = NULL;
        const char* gw_address = NULL;

        if ((eth_args.ip->sval[0] != NULL)
         || (eth_args.mask->sval[0] != NULL)
         || (eth_args.gw->sval[0] != NULL))
        {
            ESP_LOGI(TAG, "0 ip_netmask 0x%08X", (int)ip_netmask);
            if (ip_netmask != NULL)
            {
                ESP_LOGI(TAG, "0 *ip_netmask %s", ip_netmask);
            }

            //if (netif != NULL)
            {
                ip_address = eth_args.ip->sval[0];
                ip_netmask = eth_args.mask->sval[0];

                ESP_LOGI(TAG, "1 ip_netmask 0x%08X", (int)ip_netmask);
                if (ip_netmask != NULL)
                {
                    ESP_LOGI(TAG, "1 *ip_netmask %s", ip_netmask);
                }

                gw_address = eth_args.gw->sval[0];  
            }
            drv_eth_set_static_ip(netif, ip_address, ip_netmask, gw_address, false);
        }

        if (eth_args.dhcp->sval[0] != NULL)
        {
            if (strcmp(eth_args.dhcp->sval[0],"1") == 0)
            {
                drv_eth_set_dynamic_ip(netif);
            }
        }
    }


    return 0;
}

static void register_eth(void)
{
    eth_args.device = arg_strn("i", "interface","<adapter interface>",  0, 1, "Command can be : eth [-i {eth|eth0}]");
    eth_args.ip     = arg_strn("a", "address",  "<ip address>",         0, 1, "Command can be : eth [-a 192.168.0.10]");
    eth_args.mask   = arg_strn("m", "netmask",  "<netmask>",            0, 1, "Command can be : eth [-m 255.255.255.0]");
    eth_args.gw     = arg_strn("g", "gateway",  "<gateway>",            0, 1, "Command can be : eth [-g 192.168.0.1]");
    eth_args.dhcp   = arg_strn("a", "dhcp",     "<dhcp use>",           0, 1, "Command can be : eth [-d 1]");
    eth_args.end    = arg_end(5);

    const esp_console_cmd_t cmd_eth = {
        .command = "eth",
        .help = "Eth Settings Update Request",
        .hint = NULL,
        .func = &update_eth,
        .argtable = &eth_args,
    };

    //eth_args.device->sval[0] = null_string_eth;
    //eth_args.ip->sval[0] = null_string_eth;
    //eth_args.mask->sval[0] = null_string_eth;
    //eth_args.gw->sval[0] = null_string_eth;

    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_eth));
}


void cmd_eth_register(void)
{
    register_eth();
}
