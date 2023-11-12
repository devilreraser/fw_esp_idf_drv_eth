/* *****************************************************************************
 * File:   drv_eth.c
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
#include "drv_eth.h"
#include "cmd_eth.h"
#include "cmd_ethernet.h"

#include <sdkconfig.h>
#include "esp_idf_version.h"


#if CONFIG_ESP_ETH_ESP_IDF_VERSION_LESS_THAN_5
#define ESP_ETH_VERSION_BIGGER_OR_EQUAL_TO_5    0
#elif CONFIG_ESP_ETH_ESP_IDF_VERSION_GREATHER_EQUAL_5
#define ESP_ETH_VERSION_BIGGER_OR_EQUAL_TO_5    1
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#define ESP_ETH_VERSION_BIGGER_OR_EQUAL_TO_5    1
#else
#define ESP_ETH_VERSION_BIGGER_OR_EQUAL_TO_5    0
#endif


//#include "drv_wifi.h"???



#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

//#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
//#else
#include "esp_eth_netif_glue.h"
//#endif

#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#if CONFIG_DRV_ETH_USE_SPI_ETHERNET
#include "driver/spi_master.h"
#endif // CONFIG_DRV_ETH_USE_SPI_ETHERNET

//#include "esp_wifi.h"   //for mac address usage

#include "esp_rom_efuse.h"
#include "esp_mac.h"


#if CONFIG_APP_SOCKET_UDP_USE
#include "app_socket_udp.h"
#endif


//#include "drv_socket.h"


#if CONFIG_DRV_NVS_USE
#include "drv_nvs.h"
#endif

#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
//#include "lwip/err.h"
//#include "lwip/sys.h"
#include <netdb.h>


#define INDIRECT_HANDLE_GET_NETIF   1

#if INDIRECT_HANDLE_GET_NETIF
//#include "esp_netif_lwip_internal.h"
#else

typedef void (*dhcps_cb_t)(void* cb_arg, u8_t client_ip[4], u8_t client_mac[6]);

typedef u32_t dhcps_time_t;
typedef u8_t dhcps_offer_t;

/*   Defined in esp_misc.h */
typedef struct {
	bool enable;
	ip4_addr_t start_ip;
	ip4_addr_t end_ip;
} dhcps_lease_t;


typedef enum {
    DHCPS_HANDLE_CREATED = 0,
    DHCPS_HANDLE_STARTED,
    DHCPS_HANDLE_STOPPED,
    DHCPS_HANDLE_DELETE_PENDING,
} dhcps_handle_state;

typedef struct list_node {
	void *pnode;
	struct list_node *pnext;
} list_node;


struct dhcps_t {
    struct netif *dhcps_netif;
    ip4_addr_t broadcast_dhcps;
    ip4_addr_t server_address;
    ip4_addr_t dns_server;
    ip4_addr_t client_address;
    ip4_addr_t client_address_plus;
    ip4_addr_t dhcps_mask;
    list_node *plist;
    bool renew;
    dhcps_lease_t dhcps_poll;
    dhcps_time_t dhcps_lease_time;
    dhcps_offer_t dhcps_offer;
    dhcps_offer_t dhcps_dns;
    dhcps_cb_t dhcps_cb;
    void* dhcps_cb_arg;
    struct udp_pcb *dhcps_pcb;
    dhcps_handle_state state;
};

typedef struct dhcps_t dhcps_t;


/**
 * @brief Additional netif types when related data are needed
 */
enum netif_types {
    COMMON_LWIP_NETIF,
    PPP_LWIP_NETIF,
    SLIP_LWIP_NETIF
};
/**
 * @brief Related data to esp-netif (additional data for some special types of netif
 * (typically for point-point network types, such as PPP or SLIP)
 */
typedef struct netif_related_data {
    bool is_point2point;
    enum netif_types netif_type;
} netif_related_data_t;

/**
 * @brief Main esp-netif container with interface related information
 */
struct esp_netif_obj {
    // default interface addresses
    uint8_t mac[NETIF_MAX_HWADDR_LEN];
    esp_netif_ip_info_t* ip_info;
    esp_netif_ip_info_t* ip_info_old;

    // lwip netif related
    struct netif *lwip_netif;
    err_t (*lwip_init_fn)(struct netif*);
    void (*lwip_input_fn)(void *input_netif_handle, void *buffer, size_t len, void *eb);
    void * netif_handle;    // netif impl context (either vanilla lwip-netif or ppp_pcb)
    netif_related_data_t *related_data; // holds additional data for specific netifs
#if ESP_DHCPS
    dhcps_t *dhcps;
#endif
    // io driver related
    void* driver_handle;
    esp_err_t (*driver_transmit)(void *h, void *buffer, size_t len);
    esp_err_t (*driver_transmit_wrap)(void *h, void *buffer, size_t len, void *pbuf);
    void (*driver_free_rx_buffer)(void *h, void* buffer);

    // dhcp related
    esp_netif_dhcp_status_t dhcpc_status;
    esp_netif_dhcp_status_t dhcps_status;
    bool timer_running;

    // event translation
    ip_event_t get_ip_event;
    ip_event_t lost_ip_event;

    // misc flags, types, keys, priority
    esp_netif_flags_t flags;
    char * hostname;
    char * if_key;
    char * if_desc;
    int route_prio;
};

#endif

/* *****************************************************************************
 * Configuration Definitions
 **************************************************************************** */
#define TAG "drv_eth"

/* *****************************************************************************
 * Constants and Macros Definitions
 **************************************************************************** */
#if CONFIG_DRV_ETH_USE_SPI_ETHERNET
#define INIT_SPI_ETH_MODULE_CONFIG(eth_module_config, num)                                      \
    do {                                                                                        \
        eth_module_config[num].spi_cs_gpio = CONFIG_ETH_SPI_CS ##num## _GPIO;           \
        eth_module_config[num].int_gpio = CONFIG_ETH_SPI_INT ##num## _GPIO;             \
        eth_module_config[num].phy_reset_gpio = CONFIG_ETH_SPI_PHY_RST ##num## _GPIO;   \
        eth_module_config[num].phy_addr = CONFIG_ETH_SPI_PHY_ADDR ##num;                \
    } while(0)
#endif


/* *****************************************************************************
 * Enumeration Definitions
 **************************************************************************** */

/* *****************************************************************************
 * Type Definitions
 **************************************************************************** */
#if CONFIG_DRV_ETH_USE_SPI_ETHERNET
typedef struct {
    uint8_t spi_cs_gpio;
    uint8_t int_gpio;
    int8_t phy_reset_gpio;
    uint8_t phy_addr;
}spi_eth_module_config_t;
#endif


/* *****************************************************************************
 * Function-Like Macros
 **************************************************************************** */

/* *****************************************************************************
 * Variables Definitions
 **************************************************************************** */
SemaphoreHandle_t flag_eth_got_ip = NULL;

esp_eth_handle_t eth_phy_handle = NULL;
#if CONFIG_SPI_ETHERNETS_NUM
esp_eth_handle_t eth_spi_handle[CONFIG_SPI_ETHERNETS_NUM] = { NULL };
#else
esp_eth_handle_t eth_spi_handle[CONFIG_SPI_ETHERNETS_NUM] = {};
#endif
esp_eth_handle_t esp_handle_eth[CONFIG_SPI_ETHERNETS_NUM+1] = { NULL };

//bool eth_netif_init_passed = false;

bool eth_connected[CONFIG_SPI_ETHERNETS_NUM+1] = {false};

esp_netif_t* esp_netif_eth[CONFIG_SPI_ETHERNETS_NUM+1] = {NULL};        /* max 1 internal + all SPIs */
int esp_netif_eth_count = 0;


bool bSkipEthConfigSave = false;

char ip_eth_def[16] = "192.168.0.234";
char mask_eth_def[16] = "255.255.255.0";
char gw_eth_def[16] = "192.168.0.1";


uint32_t dhcp_eth = 1;
uint32_t ip_eth = 0;
uint32_t mask_eth = 0;
uint32_t gw_eth = 0;


/* *****************************************************************************
 * Prototype of functions definitions
 **************************************************************************** */

/* *****************************************************************************
 * Functions
 **************************************************************************** */

bool drv_eth_get_connected(int index)
{
    if (index < esp_netif_eth_count)
    {
        return eth_connected[index];
    }
    return false;
}

void drv_eth_set_connected(int index, bool connected)
{
    if (index < esp_netif_eth_count)
    {
        eth_connected[index] = connected;
    }
}


int drv_eth_get_netif_index_from_handle(esp_eth_handle_t eth_handle)
{
    int index = esp_netif_eth_count;

    if (eth_handle ==  NULL) return index;

    for (index = 0; index < esp_netif_eth_count; index++)
    {

        ESP_LOGE(TAG, "eth_handle : 0x%08X  esp_handle_eth[%d] : 0x%08X", (int)eth_handle, index, (int)esp_handle_eth[index]);
        #if INDIRECT_HANDLE_GET_NETIF
        if (eth_handle == esp_handle_eth[index])
        #else
        if (eth_handle == ((struct esp_netif_obj *)esp_netif_eth[index])->driver_handle)
        #endif
        {
            break;
        }
    }
    return index;
}


int drv_eth_get_netif_index(esp_netif_t* netif)
{
    int index = esp_netif_eth_count;

    for (index = 0; index < esp_netif_eth_count; index++)
    {
        if (netif == esp_netif_eth[index])
        {
            break;
        }
    }
    return index;
}

esp_eth_handle_t drv_eth_get_handle(int index)
{
    if (index < esp_netif_eth_count)
    {
        return esp_handle_eth[index];
    }
    return NULL;

}

esp_netif_t* drv_eth_get_netif(int index)
{
    if (index < esp_netif_eth_count)
    {
        return esp_netif_eth[index];
    }
    return NULL;
}

int drv_eth_get_netif_count(void)
{
    return esp_netif_eth_count;
}

#if CONFIG_DRV_NVS_USE
void drv_eth_save_config(void)
{
    if (bSkipEthConfigSave == false)
    {
        int err_eth = 0;    
        if (drv_nvs_write_u32("app_cfg","dhcp_eth", dhcp_eth)) err_eth++;
        if (drv_nvs_write_u32("app_cfg","ip_eth", ip_eth)) err_eth++;
        if (drv_nvs_write_u32("app_cfg","mask_eth", mask_eth)) err_eth++;
        if (drv_nvs_write_u32("app_cfg","gw_eth", gw_eth)) err_eth++;

        if (err_eth > 0)
        {
            ESP_LOGE(TAG, "Error Data Wrte to NVS ip info of ethernet phy");
        }

    }
}

void drv_eth_load_config(void)
{
    /* get ip_info eth */
    dhcp_eth = 1;
    ip_eth = inet_addr(ip_eth_def);
    mask_eth = inet_addr(mask_eth_def);
    gw_eth = inet_addr(gw_eth_def);

    int err_eth = 0;    
    if (drv_nvs_read_u32("app_cfg","dhcp_eth", &dhcp_eth)) err_eth++;
    if (drv_nvs_read_u32("app_cfg","ip_eth", &ip_eth)) err_eth++;
    if (drv_nvs_read_u32("app_cfg","mask_eth", &mask_eth)) err_eth++;
    if (drv_nvs_read_u32("app_cfg","gw_eth", &gw_eth)) err_eth++;

    if ((err_eth != 0) && (err_eth != 4))
    {
        ESP_LOGE(TAG, "Error Partial Data Read from NVS");
        dhcp_eth = 1;
        ip_eth = inet_addr(ip_eth_def);
        mask_eth = inet_addr(mask_eth_def);
        gw_eth = inet_addr(gw_eth_def);
    }
}

void drv_eth_cfg_check_save(void)
{
    uint32_t dhcp_eth_set;
    uint32_t   ip_eth_set;
    uint32_t mask_eth_set;
    uint32_t   gw_eth_set;

    drv_eth_load_config();
    dhcp_eth_set = dhcp_eth;
      ip_eth_set =   ip_eth;
    mask_eth_set = mask_eth;
      gw_eth_set =   gw_eth;

    if (dhcp_eth)
    {
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_eth[0];

        if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) 
        {
            ESP_LOGE(TAG, "Failed to get ip info");
        }
        else
        {
              ip_eth_set = ip_info.ip.addr;
            mask_eth_set = ip_info.netmask.addr;
              gw_eth_set = ip_info.gw.addr;
        }
        
    }
    
    bool skip_save = true;
    if (dhcp_eth != dhcp_eth_set)
    {
        dhcp_eth = dhcp_eth_set;
        skip_save = false;
    }
    if (  ip_eth !=   ip_eth_set)
    {
          ip_eth =   ip_eth_set;
        skip_save = false;
    }
    if (mask_eth != mask_eth_set)
    {
        mask_eth = mask_eth_set;
        skip_save = false;
    }
    if (  gw_eth !=   gw_eth_set)
    {
          gw_eth =   gw_eth_set;
        skip_save = false;
    }
    if (skip_save == false)
    {
        //bSkipEthConfigSave = false;
        drv_eth_save_config();
        //bSkipEthConfigSave = true;
    }
}

#endif  /* #if CONFIG_DRV_NVS_USE */


void drv_eth_set_config(void)
{
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_eth[0];
    esp_err_t esp_error;

    ESP_LOGE(TAG, "!!!!!!!!!!!!drv_eth_set_config!!!!!!!!!!!");
    esp_netif_dhcp_status_t status;


    if (esp_netif_dhcpc_get_status(netif, &status) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read dhcp client status");
    }
    else
    {
        if (dhcp_eth)
        {
            if (status == ESP_NETIF_DHCP_STOPPED)
            {    
                if (esp_netif_dhcpc_start(netif) != ESP_OK) 
                {
                    ESP_LOGE(TAG, "Failed to start dhcp client");
                }
                else
                {
                    ESP_LOGI(TAG, "Success started dhcp client");
                }
            }
            else
            {
                ESP_LOGI(TAG, "Already started dhcp client ETH0 | status:%d", status);
            }
        }
        else
        {
            if (status == ESP_NETIF_DHCP_STARTED)
            {    
                if (esp_netif_dhcpc_stop(netif) != ESP_OK) 
                {
                    ESP_LOGE(TAG, "Failed to stop dhcp client");
                }
                else
                {
                    ESP_LOGI(TAG, "Success stopped dhcp client");
                }
            }
            else
            {
                ESP_LOGI(TAG, "Already stopped dhcp client ETH0 | status:%d", status);
            }

        }

    }




    if (dhcp_eth)
    {
        if (esp_netif_dhcpc_get_status(netif, &status) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to read dhcp client status");
        }
        //used dhcp - do not change IP
        ESP_LOGI(TAG, "Connecting using dhcp client status:%d", status);
    }
    else
    if (esp_netif_dhcpc_get_status(netif, &status) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read dhcp client status");
    }
    else
    {

        esp_error = esp_netif_get_ip_info(netif, &ip_info);
        if (esp_error != ESP_OK) 
        {
            ESP_LOGE(TAG, "Failed to get ip info 0 error:%d", esp_error);
        }
        else
        {
            ESP_LOGI(TAG, "        IP ETH0: " IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI(TAG, "      MASK ETH0: " IPSTR, IP2STR(&ip_info.netmask));
            ESP_LOGI(TAG, "   GATEWAY ETH0: " IPSTR, IP2STR(&ip_info.gw));

            if ((ip_eth == 0) | (mask_eth == 0) | (mask_eth == 0))
            {
                ESP_LOGW(TAG, "Detected Zero IP Address Configuration - Defaulting ip info");
                ip_eth = inet_addr(ip_eth_def);
                mask_eth = inet_addr(mask_eth_def);
                gw_eth = inet_addr(gw_eth_def);
            }

            if ((ip_info.ip.addr        != ip_eth)
             || (ip_info.netmask.addr   != mask_eth)
             || (ip_info.gw.addr        != gw_eth))
            {

                if (status == ESP_NETIF_DHCP_STARTED)
                {    
                    if (esp_netif_dhcpc_stop(netif) != ESP_OK) 
                    {
                        ESP_LOGE(TAG, "Failed to stop dhcp client");
                    }
                    else
                    {
                        ESP_LOGI(TAG, "Success stopped dhcp client");
                    }
                }
                else
                {
                    ESP_LOGI(TAG, "Skip Stop DHCP | status:%d", status);
                }

                if (esp_netif_dhcpc_get_status(netif, &status) != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to read dhcp client status");
                }
                if (status == ESP_NETIF_DHCP_STOPPED)
                {    


                    ip_info.ip.addr         = ip_eth;
                    ip_info.netmask.addr    = mask_eth;
                    ip_info.gw.addr         = gw_eth;

                    esp_error = esp_netif_set_ip_info(netif, &ip_info);
                    if (esp_error != ESP_OK) 
                    {
                        ESP_LOGE(TAG, "Failed to set ip info 0 error:%d", esp_error);
                    }
                    else
                    {
                        //ESP_ERR_ESP_NETIF_INVALID_PARAMS
                        //ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "DHCP not Stopped to write default ip info | status:%d", status);
                }

            }

        }

    }






}

void drv_eth_set_dynamic_ip(esp_netif_t *netif)
{
    esp_netif_dhcp_status_t status;
    if (esp_netif_dhcpc_get_status(netif, &status) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read dhcp client status");
        return;
    }
    else if (status == ESP_NETIF_DHCP_STOPPED)
    {  
        #if CONFIG_APP_SOCKET_UDP_USE  
        app_socket_udp_eth_bootp_set_deny_connect(true);
        vTaskDelay(pdMS_TO_TICKS(10000));
        #endif

        if (esp_netif_dhcpc_start(netif) != ESP_OK) 
        {
            #if CONFIG_APP_SOCKET_UDP_USE
            app_socket_udp_eth_bootp_set_deny_connect(false);
            #endif
            ESP_LOGE(TAG, "Failed to start dhcp client");
            return;
        }
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) 
        {
            ESP_LOGE(TAG, "Failed to get ip info");

        }
        else
        {

        }
        dhcp_eth = 1;
        ip_eth = ip_info.ip.addr;
        mask_eth = ip_info.netmask.addr;
        gw_eth = ip_info.gw.addr;
        drv_eth_save_config();

        //esp_eth_stop(netif);
        //esp_eth_start(netif);
    }

}

uint32_t drv_eth_get_netmask_0(void)
{
    return mask_eth;
}

void drv_eth_set_dhcp_flag(uint32_t bDHCP)
{
    dhcp_eth = bDHCP;
}

void drv_eth_set_last_as_static_ip(esp_netif_t *netif, bool bSkipSave)
{
    if ((ip_eth == 0) || (mask_eth == 0) || (mask_eth == 0))
    {
        ESP_LOGI(TAG, "ETH Default IP Set Request as Static without save");
        drv_eth_set_static_ip(netif, ip_eth_def, mask_eth_def, gw_eth_def, bSkipSave);
    }
    else
    {
        ESP_LOGI(TAG, "ETH Last IP Set Request as Static without save");
        char ip_address_buf[16];
        char* ip_address = ip4addr_ntoa_r((ip4_addr_t*)&ip_eth, ip_address_buf, sizeof(ip_address_buf));
        char ip_netmask_buf[16];
        char* ip_netmask = ip4addr_ntoa_r((ip4_addr_t*)&mask_eth, ip_netmask_buf, sizeof(ip_netmask_buf));
        char ip_gateway_buf[16];
        char* ip_gateway = ip4addr_ntoa_r((ip4_addr_t*)&gw_eth, ip_gateway_buf, sizeof(ip_gateway_buf));
        drv_eth_set_static_ip(netif, ip_address, ip_netmask, ip_gateway, bSkipSave);
    }

}

void drv_eth_set_static_ip(esp_netif_t *netif, const char *ip_address, const char *ip_netmask, const char *gw_address, bool bSkipSave)
{
    esp_netif_dhcp_status_t status;
    if (esp_netif_dhcpc_get_status(netif, &status) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read dhcp client status");
        return;
    }
    else if (status == ESP_NETIF_DHCP_STARTED)
    {
        if (esp_netif_dhcpc_stop(netif) != ESP_OK) 
        {
            ESP_LOGE(TAG, "Failed to stop dhcp client");
            return;
        }
    }

    esp_netif_ip_info_t ip_info;
    //memset(&ip, 0 , sizeof(esp_netif_ip_info_t));

    if(netif == esp_netif_eth[0])
    {
        ESP_LOGI(TAG, "ETH PHY 0 IP Change Request");
    }
    else
    {
        ESP_LOGI(TAG, "IP Change Request unknown ETH interface");
        return;
    }


    //ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip_info));    /* this function returns first netif->lwip data which is 0 */
    ESP_ERROR_CHECK(esp_netif_get_old_ip_info(netif, &ip_info));

    ESP_LOGI(TAG, "From:");
    
    ESP_LOGI(TAG, "  IP: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "MASK: " IPSTR, IP2STR(&ip_info.netmask));
    ESP_LOGI(TAG, "  GW: " IPSTR, IP2STR(&ip_info.gw));

    ESP_LOGI(TAG, "To  :");

    if((ip_address != NULL) && (strlen(ip_address) > 0))ip_info.ip.addr = ipaddr_addr(ip_address);
    if((ip_netmask != NULL) && (strlen(ip_netmask) > 0))ip_info.netmask.addr = ipaddr_addr(ip_netmask);
    if((gw_address != NULL) && (strlen(gw_address) > 0))ip_info.gw.addr = ipaddr_addr(gw_address);
    ESP_LOGI(TAG, "  IP: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "MASK: " IPSTR, IP2STR(&ip_info.netmask));
    ESP_LOGI(TAG, "  GW: " IPSTR, IP2STR(&ip_info.gw));

    if (esp_netif_set_ip_info(netif, &ip_info) != ESP_OK) 
    {
        ESP_LOGE(TAG, "Failed to set ip info 1");
        return;
    }

    if (bSkipSave == false)
    {
        if(netif == esp_netif_eth[0])
        {
            dhcp_eth = 0;
            ip_eth = ip_info.ip.addr;
            mask_eth = ip_info.netmask.addr;
            gw_eth = ip_info.gw.addr;
            drv_eth_save_config();
        }
    }
    #if CONFIG_APP_SOCKET_UDP_USE
    app_socket_udp_eth_bootp_set_deny_connect(false);
    #endif
}

#if 0
static void drv_eth_set_saved_ip_info(esp_eth_handle_t eth_handle)
{

    if (eth_spi_handle[0] != eth_handle) 
    {
        ESP_LOGE(TAG, "Failed to set ip info saved in flash (not saved eth handle info) 0x%08X/0x%08X", eth_handle, eth_spi_handle[0]);
        return;
    }

    //#if CONFIG_SPI_ETHERNETS_NUM
    bSkipEthConfigSave = true;
    /* update load configuration */
    if (dhcp_eth)
    {
        drv_eth_set_dynamic_ip(drv_eth_get_netif(0));
    }
    else
    {
        char ip_address_buffer[16] = {0};
        char *ip_address = ip_address_buffer;
        ip4addr_ntoa_r((ip4_addr_t*)&ip_eth, ip_address, sizeof(ip_address_buffer));
        char ip_netmask_buffer[16] = {0};
        char *ip_netmask = ip_netmask_buffer;
        ip4addr_ntoa_r((ip4_addr_t*)&mask_eth, ip_netmask, sizeof(ip_netmask_buffer));
        char ip_gateway_buffer[16] = {0};
        char *ip_gateway = ip_gateway_buffer;
        ip4addr_ntoa_r((ip4_addr_t*)&gw_eth, ip_gateway, sizeof(ip_gateway_buffer));
        ESP_LOGI(TAG, "ETH Saved IP Set Request");
        drv_eth_set_static_ip(drv_eth_get_netif(0), ip_address, ip_netmask, ip_gateway, true);
    }
    bSkipEthConfigSave = false;
    //#endif

}
#endif


void drv_eth_print(void)
{
    esp_netif_dhcp_status_t status;
    if (esp_netif_dhcpc_get_status(esp_netif_eth[0], &status) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read dhcp client status");
        return;
    }

    esp_netif_ip_info_t ip_info;    
    ip_info.ip.addr         = ip_eth;
    ip_info.netmask.addr    = mask_eth;
    ip_info.gw.addr         = gw_eth;
    ESP_LOGI(TAG, "ETH INFO:CFG|IP:" IPSTR "|MSK:" IPSTR "|GW:" IPSTR, 
               IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
    esp_netif_get_old_ip_info(esp_netif_eth[0], &ip_info);
    ESP_LOGI(TAG, "ETH INFO:OLD|IP:" IPSTR "|MSK:" IPSTR "|GW:" IPSTR, 
               IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
    esp_netif_get_ip_info(esp_netif_eth[0], &ip_info);
    ESP_LOGI(TAG, "ETH DHCP:%" PRIu32 "/%d|IP:" IPSTR "|MSK:" IPSTR "|GW:" IPSTR, 
                dhcp_eth, (status == ESP_NETIF_DHCP_STARTED)?1:(status == ESP_NETIF_DHCP_STOPPED)?0:-1, 
               IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
   
}


void drv_eth_set_default_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info;
    //memset(&ip, 0 , sizeof(esp_netif_ip_info_t));

    

    if(netif == esp_netif_eth[0])
    {
        ESP_LOGI(TAG, "ETH PHY 0 IP Set On Link");
    }
    else
    {
        ESP_LOGI(TAG, "IP Set On Link unknown ETH interface");
        return;
    }


    //ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip_info));    /* this function returns first netif->lwip data which is 0 */
    ESP_ERROR_CHECK(esp_netif_get_old_ip_info(netif, &ip_info));

    ESP_LOGI(TAG, "Current:");
    
    ESP_LOGI(TAG, "  IP: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "MASK: " IPSTR, IP2STR(&ip_info.netmask));
    ESP_LOGI(TAG, "  GW: " IPSTR, IP2STR(&ip_info.gw));

    if ((ip_info.ip.addr == 0) || (ip_info.netmask.addr == 0) || (ip_info.gw.addr == 0))
    {
        ip_info.ip.addr = inet_addr(ip_eth_def);
        ip_info.netmask.addr = inet_addr(mask_eth_def);
        ip_info.gw.addr = inet_addr(gw_eth_def);

        ESP_LOGW(TAG, "Force Default  :");
        ESP_LOGW(TAG, "  IP: " IPSTR, IP2STR(&ip_info.ip));
        ESP_LOGW(TAG, "MASK: " IPSTR, IP2STR(&ip_info.netmask));
        ESP_LOGW(TAG, "  GW: " IPSTR, IP2STR(&ip_info.gw));

    }

    if (esp_netif_set_ip_info(netif, &ip_info) != ESP_OK) 
    {
        ESP_LOGE(TAG, "Failed to set ip info 2");
        return;
    }

}

#if CONFIG_USE_ETHERNET
/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
    int netif_index = drv_eth_get_netif_index_from_handle(eth_handle);

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

        int speed = ETH_SPEED_MAX;
        ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_G_SPEED, &speed));
        ESP_LOGI(TAG, "Ethernet Link %d MB", (speed == ETH_SPEED_100M)?100:10);
        
        
        //drv_eth_set_saved_ip_info(eth_handle);
        drv_eth_set_config();
        drv_eth_set_connected(netif_index, false);


        esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
        if (esp_netif_dhcpc_get_status(esp_netif_eth[netif_index], &status) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to read dhcp client status");
        }
        else
        {
            #if CONFIG_APP_SOCKET_UDP_USE
            if(status == ESP_NETIF_DHCP_STARTED)
            {
                app_socket_udp_eth_bootp_set_deny_connect(true);
            }
            else
            {
                app_socket_udp_eth_bootp_set_deny_connect(false);
            }
            #endif
        }


        if (netif_index < esp_netif_eth_count)
        {
            status = ESP_NETIF_DHCP_INIT;
            if (esp_netif_dhcpc_get_status(esp_netif_eth[netif_index], &status) != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to read dhcp client status");
            }
            if(status == ESP_NETIF_DHCP_STOPPED)
            {
                drv_eth_set_default_ip(esp_netif_eth[netif_index]);
            }
            else
            {
                ESP_LOGE(TAG, "drv_eth_set_default_ip skipped ETHERNET_EVENT_CONNECTED status: %d", status);
            }
            
        }
        

        //bETHLinkOnDetected = true;
        //nETHLinkOnTimeoutDetectIP = 0;
        
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        //esp_eth_stop(eth_handle); //may be not needed - only for test
        //esp_eth_start(eth_handle);
        #if CONFIG_APP_SOCKET_UDP_USE
        app_socket_udp_eth_bootp_set_deny_connect(true);
        #endif
        drv_eth_set_connected(netif_index, false);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        //esp_netif_dhcpc_stop(drv_eth_get_netif(0));
        //esp_netif_dhcpc_start(drv_eth_get_netif(0));
        drv_eth_set_connected(netif_index, false);
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        drv_eth_set_connected(netif_index, false);
        break;
    default:
        break;
    }
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");

    esp_netif_t* esp_netif = event->esp_netif;
    int eth_index = drv_eth_get_netif_index(esp_netif);
    drv_eth_set_connected(eth_index, true);
    
    esp_netif = drv_eth_get_netif(eth_index);
    ESP_LOGI(TAG, "eth_index:%d/%d", eth_index, esp_netif_eth_count);
    ESP_LOGI(TAG, "esp_netif:0x%08X", (int)esp_netif);
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    if (esp_netif != NULL)
    {
        esp_netif_dns_info_t dns_info_eth;
        esp_netif_get_dns_info(esp_netif,ESP_NETIF_DNS_MAIN, &dns_info_eth);
        ESP_LOGI(TAG, "DNS PRIMER: " IPSTR, IP2STR(&dns_info_eth.ip.u_addr.ip4));
        if (dns_info_eth.ip.u_addr.ip4.addr == 0)
        {
            dns_info_eth.ip.u_addr.ip4.addr = inet_addr("8.8.8.8");
            esp_netif_set_dns_info(esp_netif,ESP_NETIF_DNS_MAIN, &dns_info_eth);
            esp_netif_get_dns_info(esp_netif,ESP_NETIF_DNS_MAIN, &dns_info_eth);
            ESP_LOGI(TAG, "DNS PRIFIX: " IPSTR, IP2STR(&dns_info_eth.ip.u_addr.ip4));
        }
        esp_netif_get_dns_info(esp_netif,ESP_NETIF_DNS_BACKUP, &dns_info_eth);
        ESP_LOGI(TAG, "DNS SECOND: " IPSTR, IP2STR(&dns_info_eth.ip.u_addr.ip4));
        if (dns_info_eth.ip.u_addr.ip4.addr == 0)
        {
            dns_info_eth.ip.u_addr.ip4.addr = inet_addr("8.8.4.4");
            esp_netif_set_dns_info(esp_netif,ESP_NETIF_DNS_BACKUP, &dns_info_eth);
            esp_netif_get_dns_info(esp_netif,ESP_NETIF_DNS_BACKUP, &dns_info_eth);
            ESP_LOGI(TAG, "DNS SECFIX: " IPSTR, IP2STR(&dns_info_eth.ip.u_addr.ip4));
        }
        esp_netif_get_dns_info(esp_netif,ESP_NETIF_DNS_FALLBACK, &dns_info_eth);
        ESP_LOGI(TAG, "DNS FALLEN: " IPSTR, IP2STR(&dns_info_eth.ip.u_addr.ip4));
        if (dns_info_eth.ip.u_addr.ip4.addr == 0)
        {
            dns_info_eth.ip.u_addr.ip4.addr = ip_info->gw.addr;
            esp_netif_set_dns_info(esp_netif,ESP_NETIF_DNS_FALLBACK, &dns_info_eth);
            esp_netif_get_dns_info(esp_netif,ESP_NETIF_DNS_FALLBACK, &dns_info_eth);
            ESP_LOGI(TAG, "DNS FALFIX: " IPSTR, IP2STR(&dns_info_eth.ip.u_addr.ip4));
        }
        ESP_LOGI(TAG, "~~~~~~~~~~~");
    }

    //drv_eth_cfg_check_save();



    esp_netif_dhcp_status_t status;
    if (esp_netif_dhcpc_get_status(esp_netif_eth[0], &status) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read dhcp client status");
    }
    else
    {
        #if CONFIG_APP_SOCKET_UDP_USE
        if(status == ESP_NETIF_DHCP_STARTED)
        {
            app_socket_udp_eth_bootp_set_deny_connect(true);
        }
        else
        {
            app_socket_udp_eth_bootp_set_deny_connect(false);
        }
        #endif
    }

    //if (bETHLinkOnDetected)
    //{
    //    bETHLinkOnDetected = false;
    //}

    xSemaphoreGive(flag_eth_got_ip);
    // #if CONFIG_USE_WIFI
    // drv_wifi_or_eth_give_semaphore();
    // #endif
}


static void drv_eth_mac_fix(esp_eth_handle_t eth_handle, int eth_index, bool fix_forced)
{

    uint8_t mac_addr[6] = {0};

    esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
    ESP_LOGI(TAG, "Ethernet HW MAC ETH[%d] %02x:%02x:%02x:%02x:%02x:%02x",
                eth_index, mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

    if ((memcmp(mac_addr, (uint8_t[]) {0,0,0,0,0,0}, 6) == 0) || fix_forced)
    {
        esp_base_mac_addr_get(mac_addr);
        ESP_LOGI(TAG, "ESP Base MAC %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        esp_read_mac(mac_addr, ESP_MAC_IEEE802154);
        ESP_LOGI(TAG, "ESP IEEE MAC %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        esp_read_mac(mac_addr, ESP_MAC_WIFI_STA);
        ESP_LOGI(TAG, "ESP Wifi MAC %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        esp_read_mac(mac_addr, ESP_MAC_WIFI_SOFTAP);
        ESP_LOGI(TAG, "ESP AP   MAC %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        esp_read_mac(mac_addr, ESP_MAC_BT);
        ESP_LOGI(TAG, "ESP BT   MAC %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        esp_read_mac(mac_addr, ESP_MAC_ETH);
        ESP_LOGI(TAG, "ESP ETH  MAC %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

        if (eth_index == 0)
        {
            esp_read_mac(mac_addr, ESP_MAC_ETH);    //first interface uses ESP_MAC_ETH
            ESP_LOGI(TAG, "ESP ETH[%d] uses ETH  MAC %02x:%02x:%02x:%02x:%02x:%02x", eth_index, mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        }
        else
        if (eth_index == 1)
        {
            esp_read_mac(mac_addr, ESP_MAC_WIFI_SOFTAP);    //second interface uses ESP_MAC_WIFI_SOFTAP
            ESP_LOGI(TAG, "ESP ETH[%d] uses AP   MAC %02x:%02x:%02x:%02x:%02x:%02x", eth_index, mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        }
        else
        if (eth_index == 2)
        {
            esp_read_mac(mac_addr, ESP_MAC_BT);    //third interface uses ESP_MAC_BT
            ESP_LOGI(TAG, "ESP ETH[%d] uses BT   MAC %02x:%02x:%02x:%02x:%02x:%02x", eth_index, mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        }
        else
        if (eth_index == 3)
        {
            esp_read_mac(mac_addr, ESP_MAC_WIFI_STA);    //fourth interface uses ESP_MAC_WIFI_STA
            ESP_LOGI(TAG, "ESP ETH[%d] uses STA  MAC %02x:%02x:%02x:%02x:%02x:%02x", eth_index, mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        }
        else
        {
            esp_read_mac(mac_addr, ESP_MAC_ETH);    //third interface uses ESP_MAC_ETH
            /* The SPI Ethernet module might not have a burned factory MAC address, we cat to set it manually.
            02:00:00 is a Locally Administered OUI range so should not be used except when testing on a LAN under your control.
            */
            mac_addr[0] = 0x02;
            mac_addr[1] = 0x00;
            mac_addr[2] = 0x00;
            mac_addr[5] = eth_index;
            ESP_LOGI(TAG, "ESP ETH[%d] uses GEN%dMAC %02x:%02x:%02x:%02x:%02x:%02x", eth_index, eth_index - 4, mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        }

        ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));
    }
}

#endif  /* #if CONFIG_USE_ETHERNET */

void drv_eth_wait_get_ip_ms(int timeout)
{
    xSemaphoreTake(flag_eth_got_ip, pdMS_TO_TICKS(timeout));
}

void drv_eth_init(void)
{
    cmd_eth_register();
    cmd_ethernet_iperf_register();

    #if CONFIG_DRV_NVS_USE
    drv_eth_load_config();
    #endif

    flag_eth_got_ip = xSemaphoreCreateBinary(); 

#if CONFIG_USE_ETHERNET

    // #if CONFIG_USE_WIFI
    // drv_wifi_or_eth_create_semaphore();
    // #endif

    int eth_index = 0;

    ESP_LOGI(TAG, "drv_eth_init Started");
    //if (eth_netif_init_passed == false)
    //{
        //eth_netif_init_passed = true;
        //drv_wifi_netif_init_passed();

        // Initialize TCP/IP network interface (should be called only once in application)
        //ESP_ERROR_CHECK(esp_netif_init());

        // Create default event loop that running in background
        //ESP_ERROR_CHECK(esp_event_loop_create_default());
    //}
    
    esp_netif_eth_count = 0;    //must be zeroed also on de-init

#if CONFIG_DRV_ETH_USE_INTERNAL_ETHERNET
    ESP_LOGI(TAG, "Ethernet Internal Install Started");
    // Create new default instance of esp-netif for Ethernet
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);

#if ESP_ETH_VERSION_BIGGER_OR_EQUAL_TO_5
    // Init MAC and PHY configs to default
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    phy_config.phy_addr = CONFIG_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_ETH_PHY_RST_GPIO;
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_mdc_gpio_num = CONFIG_ETH_MDC_GPIO;
    esp32_emac_config.smi_mdio_gpio_num = CONFIG_ETH_MDIO_GPIO;
    mac_config.rx_task_stack_size += 1024;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
#else
    // Init MAC and PHY configs to default
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    phy_config.phy_addr = CONFIG_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_ETH_PHY_RST_GPIO;

    mac_config.smi_mdc_gpio_num = CONFIG_ETH_MDC_GPIO;
    mac_config.smi_mdio_gpio_num = CONFIG_ETH_MDIO_GPIO;
    mac_config.rx_task_stack_size += 1024;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&mac_config);
#endif

#if CONFIG_ETH_PHY_IP101
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
#elif CONFIG_ETH_PHY_RTL8201
    esp_eth_phy_t *phy = esp_eth_phy_new_rtl8201(&phy_config);
#elif CONFIG_ETH_PHY_LAN87XX
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
#elif CONFIG_ETH_PHY_DP83848
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
#elif CONFIG_ETH_PHY_KSZ80XX
    esp_eth_phy_t *phy = esp_eth_phy_new_ksz80xx(&phy_config);
#endif
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));

    bool fix_forced = true;
    drv_eth_mac_fix(eth_handle, eth_index, fix_forced);
    eth_index++;

    /* attach Ethernet driver to TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));
    eth_phy_handle = eth_handle;
    esp_handle_eth[esp_netif_eth_count] = eth_phy_handle;
    eth_connected[esp_netif_eth_count] = false;
    esp_netif_eth[esp_netif_eth_count++] = eth_netif;
    ESP_LOGI(TAG, "Ethernet Internal Install Success");
#endif //CONFIG_DRV_ETH_USE_INTERNAL_ETHERNET

#if CONFIG_DRV_ETH_USE_SPI_ETHERNET
    // Create instance(s) of esp-netif for SPI Ethernet(s)
    esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_ETH();
    esp_netif_config_t cfg_spi = {
        .base = &esp_netif_config,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH
    };
    esp_netif_t *eth_netif_spi[CONFIG_SPI_ETHERNETS_NUM] = { NULL };
    char if_key_str[10];
    char if_desc_str[10];
    char num_str[3];
    for (int i = 0; i < CONFIG_SPI_ETHERNETS_NUM; i++) {
        itoa(i, num_str, 10);
        strcat(strcpy(if_key_str, "ETH_SPI_"), num_str);
        strcat(strcpy(if_desc_str, "eth"), num_str);
        esp_netif_config.if_key = if_key_str;
        esp_netif_config.if_desc = if_desc_str;
        esp_netif_config.route_prio = 30 - i;
        eth_netif_spi[i] = esp_netif_new(&cfg_spi);
    }
    

    // Init MAC and PHY configs to default
    eth_mac_config_t mac_config_spi = ETH_MAC_DEFAULT_CONFIG();
    mac_config_spi.rx_task_stack_size += 2048;
    mac_config_spi.rx_task_prio = configMAX_PRIORITIES - 3;
    //mac_config_spi.flags |= ETH_MAC_FLAG_PIN_TO_CORE;   //force to pin to the core that calls this function (if in main function - core0)
    
    eth_phy_config_t phy_config_spi = ETH_PHY_DEFAULT_CONFIG();

    // Install GPIO ISR handler to be able to service SPI Eth modlues interrupts
    gpio_install_isr_service(0);

    // Init SPI bus
    spi_device_handle_t spi_handle[CONFIG_SPI_ETHERNETS_NUM] = { NULL };
    spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_ETH_SPI_MISO_GPIO,
        .mosi_io_num = CONFIG_ETH_SPI_MOSI_GPIO,
        .sclk_io_num = CONFIG_ETH_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        //.flags = SPICOMMON_BUSFLAG_IOMUX_PINS,
        #if CONFIG_SPI_MASTER_ISR_IN_IRAM
        .intr_flags = ESP_INTR_FLAG_LOWMED | ESP_INTR_FLAG_IRAM,
        #else
        .intr_flags = ESP_INTR_FAG_LOWMED,
        #endif

    };
    ESP_ERROR_CHECK(spi_bus_initialize(CONFIG_ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));



    // Init specific SPI Ethernet module configuration from Kconfig (CS GPIO, Interrupt GPIO, etc.)
    spi_eth_module_config_t spi_eth_module_config[CONFIG_SPI_ETHERNETS_NUM];
    INIT_SPI_ETH_MODULE_CONFIG(spi_eth_module_config, 0);
    ESP_LOGI(LOG_COLOR(LOG_COLOR_PURPLE) TAG, "Pinout ETH0 SPI | CS:%d SCLK:%d MOSI:%d MISO:%d INT:%d RST:%d", 
        spi_eth_module_config[0].spi_cs_gpio, buscfg.sclk_io_num, buscfg.mosi_io_num, buscfg.miso_io_num, spi_eth_module_config[0].int_gpio, spi_eth_module_config[0].phy_reset_gpio);
#if CONFIG_SPI_ETHERNETS_NUM > 1
    INIT_SPI_ETH_MODULE_CONFIG(spi_eth_module_config, 1);
    ESP_LOGI(LOG_COLOR(LOG_COLOR_PURPLE) TAG, "Pinout ETH1 SPI | CS:%d SCLK:%d MOSI:%d MISO:%d INT:%d RST:%d", 
        spi_eth_module_config[1].spi_cs_gpio, buscfg.sclk_io_num, buscfg.mosi_io_num, buscfg.miso_io_num, spi_eth_module_config[1].int_gpio, spi_eth_module_config[1].phy_reset_gpio);
#endif

    // Configure SPI interface and Ethernet driver for specific SPI module
    esp_eth_mac_t *mac_spi[CONFIG_SPI_ETHERNETS_NUM];
    esp_eth_phy_t *phy_spi[CONFIG_SPI_ETHERNETS_NUM];
    esp_eth_handle_t eth_handle_spi[CONFIG_SPI_ETHERNETS_NUM] = { NULL };
#if CONFIG_USE_KSZ8851SNL
    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = CONFIG_ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 20
    };

    for (int i = 0; i < CONFIG_SPI_ETHERNETS_NUM; i++) {
        // Set SPI module Chip Select GPIO
        devcfg.spics_io_num = spi_eth_module_config[i].spi_cs_gpio;

        ESP_ERROR_CHECK(spi_bus_add_device(CONFIG_ETH_SPI_HOST, &devcfg, &spi_handle[i]));
        // KSZ8851SNL ethernet driver is based on spi driver
        eth_ksz8851snl_config_t ksz8851snl_config = ETH_KSZ8851SNL_DEFAULT_CONFIG(spi_handle[i]);

        // Set remaining GPIO numbers and configuration used by the SPI module
        ksz8851snl_config.int_gpio_num = spi_eth_module_config[i].int_gpio;
        phy_config_spi.phy_addr = spi_eth_module_config[i].phy_addr;
        phy_config_spi.reset_gpio_num = spi_eth_module_config[i].phy_reset_gpio;

        mac_spi[i] = esp_eth_mac_new_ksz8851snl(&ksz8851snl_config, &mac_config_spi);
        phy_spi[i] = esp_eth_phy_new_ksz8851snl(&phy_config_spi);
    }
#elif CONFIG_USE_DM9051
    spi_device_interface_config_t devcfg = {
        .command_bits = 1,
        .address_bits = 7,
        .mode = 0,
        .clock_speed_hz = CONFIG_ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 20
    };

    for (int i = 0; i < CONFIG_SPI_ETHERNETS_NUM; i++) {
        // Set SPI module Chip Select GPIO
        devcfg.spics_io_num = spi_eth_module_config[i].spi_cs_gpio;

        ESP_ERROR_CHECK(spi_bus_add_device(CONFIG_ETH_SPI_HOST, &devcfg, &spi_handle[i]));
        // dm9051 ethernet driver is based on spi driver
        eth_dm9051_config_t dm9051_config = ETH_DM9051_DEFAULT_CONFIG(spi_handle[i]);

        // Set remaining GPIO numbers and configuration used by the SPI module
        dm9051_config.int_gpio_num = spi_eth_module_config[i].int_gpio;
        phy_config_spi.phy_addr = spi_eth_module_config[i].phy_addr;
        phy_config_spi.reset_gpio_num = spi_eth_module_config[i].phy_reset_gpio;

        mac_spi[i] = esp_eth_mac_new_dm9051(&dm9051_config, &mac_config_spi);
        phy_spi[i] = esp_eth_phy_new_dm9051(&phy_config_spi);
    }
#elif CONFIG_USE_W5500

    spi_device_interface_config_t devcfg = {
        .command_bits = 16, // Actually it's the address phase in W5500 SPI frame
        .address_bits = 8,  // Actually it's the control phase in W5500 SPI frame
        .mode = 3,
        .clock_speed_hz = CONFIG_ETH_SPI_CLOCK_MHZ * 1000 * 1000,   //max 40 MHz on non-dedicated to spi pins
        //.clock_speed_hz = 80 * 1000 * 1000,   //max 40 MHz on non-dedicated to spi pins
        .queue_size = 64    //was 20
    };

    for (int i = 0; i < CONFIG_SPI_ETHERNETS_NUM; i++) 
    {
        // Set SPI module Chip Select GPIO
        devcfg.spics_io_num = spi_eth_module_config[i].spi_cs_gpio;

        ESP_ERROR_CHECK(spi_bus_add_device(CONFIG_ETH_SPI_HOST, &devcfg, &spi_handle[i]));
        // w5500 ethernet driver is based on spi driver
        #if ESP_ETH_VERSION_BIGGER_OR_EQUAL_TO_5
            eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(CONFIG_ETH_SPI_HOST, &devcfg);
        #else
            eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_handle[i]);
        #endif

        // Set remaining GPIO numbers and configuration used by the SPI module
        w5500_config.int_gpio_num = spi_eth_module_config[i].int_gpio;
        phy_config_spi.phy_addr = spi_eth_module_config[i].phy_addr;
        phy_config_spi.reset_gpio_num = spi_eth_module_config[i].phy_reset_gpio;

        mac_spi[i] = esp_eth_mac_new_w5500(&w5500_config, &mac_config_spi);
        phy_spi[i] = esp_eth_phy_new_w5500(&phy_config_spi);
    }
#endif //CONFIG_USE_W5500

    for (int i = 0; i < CONFIG_SPI_ETHERNETS_NUM; i++) 
    {
        esp_eth_config_t eth_config_spi = ETH_DEFAULT_CONFIG(mac_spi[i], phy_spi[i]);
        ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config_spi, &eth_handle_spi[i]));
        eth_spi_handle[i] = eth_handle_spi[i];

        eth_connected[esp_netif_eth_count] = false;
        esp_netif_eth[esp_netif_eth_count] = eth_netif_spi[i];
        esp_handle_eth[esp_netif_eth_count++] = eth_handle_spi[i];

        bool fix_forced = true;
        drv_eth_mac_fix(eth_handle_spi[i], eth_index, fix_forced);
        eth_index++;

        // attach Ethernet driver to TCP/IP stack
        ESP_ERROR_CHECK(esp_netif_attach(eth_netif_spi[i], esp_eth_new_netif_glue(eth_handle_spi[i])));
    }
    eth_phy_handle = eth_handle_spi[0];
#endif // CONFIG_DRV_ETH_USE_SPI_ETHERNET

    
    //drv_eth_set_config();

    // Register user defined event handers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    /* start Ethernet driver state machine */
#if CONFIG_DRV_ETH_USE_INTERNAL_ETHERNET
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
#endif // CONFIG_DRV_ETH_USE_INTERNAL_ETHERNET
#if CONFIG_DRV_ETH_USE_SPI_ETHERNET
    for (int i = 0; i < CONFIG_SPI_ETHERNETS_NUM; i++) {
        //int speed = ETH_SPEED_100M;
        //ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle_spi[i], ETH_CMD_S_SPEED, &speed));
        ESP_ERROR_CHECK(esp_eth_start(eth_handle_spi[i]));
    }
#endif // CONFIG_DRV_ETH_USE_SPI_ETHERNET

#else  /* #if CONFIG_USE_ETHERNET */
    ESP_LOGE(TAG, "Ethernet intreface Not Installed. Select SPI or Internal.");
#endif  /* #if CONFIG_USE_ETHERNET */

}

esp_err_t drv_eth_get_mac(drv_eth_interface_t ifx, uint8_t mac[6])
{
    return esp_eth_ioctl(eth_phy_handle, ETH_CMD_G_MAC_ADDR, mac);
}
