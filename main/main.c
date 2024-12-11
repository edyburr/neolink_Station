#include <string.h>
#include <inttypes.h>
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "nvs_flash.h"
#include <sys/socket.h>
#include <netdb.h>

#define MESH_ROUTER_SSID "urs" //router name here
#define MESH_ROUTER_PASSWD "aveganedo" //router password here
#define MESH_AP_PASSWD "neoMeshNetwork" //mesh password
#define MESH_TOPOLOGY MESH_TOPO_TREE //or MESH_TOPO_CHAIN

#define MESH_MAX_LAYER 6 //maximum mesh layer(s)
#define MESH_CHANNEL 0 //mesh channel 0-11
#define MESH_AP_CONNECTIONS 6
#define MESH_NON_MESH_AP_CONNECTIONS 0

#define MESH_ENABLE_PS 1
#define MESH_PS_DEV_DUTY 10 //device duty cycle
#define MESH_PS_NWK_DUTY 10 //network duty cycle
#define MESH_PS_DEV_DUTY_TYPE 1 //MESH_PS_DEV_DUTY_TYPE_DEMAND 0 and REQUEST 1
#define MESH_AP_AUTHMODE WIFI_AUTH_WPA2_PSK //AP authentication mode
//WIFI_AUTH_OPEN, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK

#define MESH_PS_NWK_DUTY_DURATION -1 //network duty cycle duration
#define MESH_PS_NWK_DUTY_RULE MESH_PS_NETWORK_DUTY_APPLIED_ENTIRE //network duty cycle rule MESH_PS_NETWORK_DUTY_APPLIED_UPLINK

// Variables -=-=-=-=-=-=-=-=-=- 

static const char *MESH_TAG = "neoMesh"; //TAG for logs, shows up in terminal
static const char *TAG = "neoMeshAPI"; //TAG for logs, shows up in terminal
static const char *SelfIdentity = "STU-001-NEO";

static const uint8_t MESH_ID[6] = { 0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
static bool is_running = true;
static bool is_mesh_connected = false;
static mesh_addr_t mesh_parent_addr;
static int mesh_layer = -1; //mesh layer
static esp_netif_t *netif_sta = NULL;

// neoLink Setup -=-=-=-=-=-=-=-=-=- 

typedef struct {
    char sensor_id[32];
    char patient_id[32];
    char sensor_data[128];
} mesh_message_t;

void neolink(void *arg) {
    mesh_data_t data;
    mesh_addr_t from;
    mesh_message_t msg = {0};
    int flag = 0;
    const char *host = "api.neobit.gg";
    const char *path = "/patients";

    data.data = (uint8_t *)&msg;
    data.size = sizeof(mesh_message_t);
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;

    is_running = true;
    while (is_running) {
        if (esp_mesh_is_root()) {
            // Root node receives and logs the messages
            while (true) {
                if (esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0) == ESP_OK) {
                    mesh_message_t *received = (mesh_message_t *)data.data;

                    ESP_LOGI("Root", "%s | measured %s for %s",
                             received->sensor_id,
                             received->sensor_data,
                             received->patient_id);

                    // HTTP Functionality
                    const char *port = "80"; // For HTTP
                    char request[512];
                    char post_data[256];
                    char patch_data[256];
                    struct addrinfo hints = {0};
                    struct addrinfo *res = NULL, *p = NULL;
                    int sock = -1;

                    // Build the JSON payload for POST
                    snprintf(post_data, sizeof(post_data),
                             "{\"patient_id\":\"%s\",\"sensor_id\":\"%s\",\"sensor_data\":\"%s\"}",
                             received->patient_id, received->sensor_id, received->sensor_data);

                    // Build the JSON payload for PATCH
                    snprintf(patch_data, sizeof(patch_data),
                             "{\"patient_id\":\"%s\",\"sensor_id\":\"%s\",\"sensor_data\":\"%s\"}",
                             received->patient_id, received->sensor_id, received->sensor_data);

                    // Resolve hostname to IP address
                    hints.ai_family = AF_INET;
                    hints.ai_socktype = SOCK_STREAM;

                    if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL) {
                        ESP_LOGE("HTTP", "DNS lookup failed for host %s", host);
                        continue;
                    }

                    // Try connecting to the resolved addresses
                    for (p = res; p != NULL; p = p->ai_next) {
                        sock = socket(p->ai_family, p->ai_socktype, 0);
                        if (sock < 0) continue;
                        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break; // Success
                        close(sock);
                        sock = -1;
                    }

                    if (sock < 0) {
                        ESP_LOGE("HTTP", "Unable to connect to %s", host);
                        freeaddrinfo(res);
                        continue;
                    }

                    // Build HTTP GET request to check if patient exists
                    snprintf(request, sizeof(request),
                             "GET %s/%s HTTP/1.1\r\n"
                             "Host: %s\r\n"
                             "Connection: close\r\n\r\n",
                             path, received->patient_id, host);

                    // Send the GET request
                    if (write(sock, request, strlen(request)) < 0) {
                        ESP_LOGE("HTTP", "Socket write failed");
                        close(sock);
                        freeaddrinfo(res);
                        continue;
                    }

                    // Read the GET response
                    char response[512];
                    int len = read(sock, response, sizeof(response) - 1);
                    if (len > 0) {
                        response[len] = '\0'; // Null-terminate the response
                        ESP_LOGI("HTTP", "GET Response:\n%s", response);
                    } else {
                        ESP_LOGE("HTTP", "Failed to read GET response");
                    }

                    // Determine if patient exists based on GET response
                    bool patient_exists = (strstr(response, "200 OK") != NULL);

                    // Clean up
                    close(sock);
                    freeaddrinfo(res);

                    // Reconnect for POST/PATCH request
                    if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL) {
                        ESP_LOGE("HTTP", "DNS lookup failed for host %s", host);
                        continue;
                    }

                    for (p = res; p != NULL; p = p->ai_next) {
                        sock = socket(p->ai_family, p->ai_socktype, 0);
                        if (sock < 0) continue;
                        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break; // Success
                        close(sock);
                        sock = -1;
                    }

                    if (sock < 0) {
                        ESP_LOGE("HTTP", "Unable to connect to %s", host);
                        freeaddrinfo(res);
                        continue;
                    }

                    // Build HTTP POST or PATCH request based on patient existence
                    if (patient_exists) {
                        snprintf(request, sizeof(request),
                                 "PATCH %s/%s HTTP/1.1\r\n"
                                 "Host: %s\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n\r\n"
                                 "%s",
                                 path, received->patient_id, host, strlen(patch_data), patch_data);
                    } else {
                        snprintf(request, sizeof(request),
                                 "POST %s HTTP/1.1\r\n"
                                 "Host: %s\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Content-Length: %d\r\n"
                                 "Connection: close\r\n\r\n"
                                 "%s",
                                 path, host, strlen(post_data), post_data);
                    }

                    // Send the POST/PATCH request
                    if (write(sock, request, strlen(request)) < 0) {
                        ESP_LOGE("HTTP", "Socket write failed");
                        close(sock);
                        freeaddrinfo(res);
                        continue;
                    }

                    // Read and log the response
                    len = read(sock, response, sizeof(response) - 1);
                    if (len > 0) {
                        response[len] = '\0'; // Null-terminate the response
                        ESP_LOGI("HTTP", "Response:\n%s", response);
                    } else {
                        ESP_LOGE("HTTP", "Failed to read response");
                    }

                    // Clean up
                    close(sock);
                    freeaddrinfo(res);

                    // Confirmation log
                    ESP_LOGI("Root", "Confirmed receipt from %s", received->sensor_id);
                }
            }
        } else {
            ESP_LOGE(SelfIdentity, "Currently NOT a ROOT node. Please reboot the Sensor Array!");
        }
        vTaskDelay(pdMS_TO_TICKS(5000)); // Send every 5 seconds
    }
    vTaskDelete(NULL);
}

esp_err_t esp_mesh_comm_p2p_start(void)
{
    static bool is_started = false;
    if (!is_started) {
        is_started = true;
        xTaskCreate(neolink, "neoLink 2-Way Communication Protocol", 4096, NULL, 5, NULL);
    }
    return ESP_OK;
}


// useless stuff

void mesh_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    mesh_addr_t id = {0,};
    static uint16_t last_layer = 0;

    switch (event_id) {
    case MESH_EVENT_STARTED: {
        esp_mesh_get_id(&id);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_MESH_STARTED>ID:"MACSTR"", MAC2STR(id.addr));
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOPPED>");
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *child_connected = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, "MACSTR"",
                 child_connected->aid,
                 MAC2STR(child_connected->mac));
    }
    break;
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *child_disconnected = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, "MACSTR"",
                 child_disconnected->aid,
                 MAC2STR(child_disconnected->mac));
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_ADD: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d, layer:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new, mesh_layer);
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d, layer:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new, mesh_layer);
    }
    break;
    case MESH_EVENT_NO_PARENT_FOUND: {
        mesh_event_no_parent_found_t *no_parent = (mesh_event_no_parent_found_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d",
                 no_parent->scan_times);
    }
    /* TODO handler for the failure */
    break;
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        esp_mesh_get_id(&id);
        mesh_layer = connected->self_layer;
        memcpy(&mesh_parent_addr.addr, connected->connected.bssid, 6);
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:"MACSTR"%s, ID:"MACSTR", duty:%d",
                 last_layer, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "", MAC2STR(id.addr), connected->duty);
        last_layer = mesh_layer;
        is_mesh_connected = true;
        if (esp_mesh_is_root()) {
            esp_netif_dhcpc_stop(netif_sta);
            esp_netif_dhcpc_start(netif_sta);
        }
        esp_mesh_comm_p2p_start();
    }
    break;
    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d",
                 disconnected->reason);
        is_mesh_connected = false;
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
        mesh_layer = layer_change->new_layer;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s",
                 last_layer, mesh_layer,
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "");
        last_layer = mesh_layer;
    }
    break;
    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_ADDRESS>root address:"MACSTR"",
                 MAC2STR(root_addr->addr));
    }
    break;
    case MESH_EVENT_VOTE_STARTED: {
        mesh_event_vote_started_t *vote_started = (mesh_event_vote_started_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, rc_addr:"MACSTR"",
                 vote_started->attempts,
                 vote_started->reason,
                 MAC2STR(vote_started->rc_addr.addr));
    }
    break;
    case MESH_EVENT_VOTE_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STOPPED>");
        break;
    }
    case MESH_EVENT_ROOT_SWITCH_REQ: {
        mesh_event_root_switch_req_t *switch_req = (mesh_event_root_switch_req_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:"MACSTR"",
                 switch_req->reason,
                 MAC2STR( switch_req->rc_addr.addr));
    }
    break;
    case MESH_EVENT_ROOT_SWITCH_ACK: {
        /* new root */
        mesh_layer = esp_mesh_get_layer();
        esp_mesh_get_parent_bssid(&mesh_parent_addr);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:"MACSTR"", mesh_layer, MAC2STR(mesh_parent_addr.addr));
    }
    break;
    case MESH_EVENT_TODS_STATE: {
        mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_TODS_REACHABLE>state:%d", *toDs_state);
    }
    break;
    case MESH_EVENT_ROOT_FIXED: {
        mesh_event_root_fixed_t *root_fixed = (mesh_event_root_fixed_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_FIXED>%s",
                 root_fixed->is_fixed ? "fixed" : "not fixed");
    }
    break;
    case MESH_EVENT_ROOT_ASKED_YIELD: {
        mesh_event_root_conflict_t *root_conflict = (mesh_event_root_conflict_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_ASKED_YIELD>"MACSTR", rssi:%d, capacity:%d",
                 MAC2STR(root_conflict->addr),
                 root_conflict->rssi,
                 root_conflict->capacity);
    }
    break;
    case MESH_EVENT_CHANNEL_SWITCH: {
        mesh_event_channel_switch_t *channel_switch = (mesh_event_channel_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d", channel_switch->channel);
    }
    break;
    case MESH_EVENT_SCAN_DONE: {
        mesh_event_scan_done_t *scan_done = (mesh_event_scan_done_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_SCAN_DONE>number:%d",
                 scan_done->number);
    }
    break;
    case MESH_EVENT_NETWORK_STATE: {
        mesh_event_network_state_t *network_state = (mesh_event_network_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d",
                 network_state->is_rootless);
    }
    break;
    case MESH_EVENT_STOP_RECONNECTION: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOP_RECONNECTION>");
    }
    break;
    case MESH_EVENT_FIND_NETWORK: {
        mesh_event_find_network_t *find_network = (mesh_event_find_network_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:"MACSTR"",
                 find_network->channel, MAC2STR(find_network->router_bssid));
    }
    break;
    case MESH_EVENT_ROUTER_SWITCH: {
        mesh_event_router_switch_t *router_switch = (mesh_event_router_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, "MACSTR"",
                 router_switch->ssid, router_switch->channel, MAC2STR(router_switch->bssid));
    }
    break;
    case MESH_EVENT_PS_PARENT_DUTY: {
        mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_PS_PARENT_DUTY>duty:%d", ps_duty->duty);
    }
    break;
    case MESH_EVENT_PS_CHILD_DUTY: {
        mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_PS_CHILD_DUTY>cidx:%d, "MACSTR", duty:%d", ps_duty->child_connected.aid-1,
                MAC2STR(ps_duty->child_connected.mac), ps_duty->duty);
    }
    break;
    default:
        ESP_LOGI(MESH_TAG, "unknown id:%" PRId32 "", event_id);
        break;
    }
}

void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(MESH_TAG, "<IP_EVENT_STA_GOT_IP>IP:" IPSTR, IP2STR(&event->ip_info.ip));

}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    /*  tcpip initialization */
    ESP_ERROR_CHECK(esp_netif_init());
    /*  event initialization */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /*  create network interfaces for mesh (only station instance saved for further manipulation, soft AP instance ignored */
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&netif_sta, NULL));
    /*  wifi initialization */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());
    /*  mesh initialization */
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    /*  set mesh topology */
    ESP_ERROR_CHECK(esp_mesh_set_topology(MESH_TOPOLOGY));
    /*  set mesh max layer according to the topology */
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));
    ESP_ERROR_CHECK(esp_mesh_set_xon_qsize(128));
#ifdef MESH_ENABLE_PS
    /* Enable mesh PS function */
    ESP_ERROR_CHECK(esp_mesh_enable_ps());
    /* better to increase the associate expired time, if a small duty cycle is set. */
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(60));
    /* better to increase the announce interval to avoid too much management traffic, if a small duty cycle is set. */
    ESP_ERROR_CHECK(esp_mesh_set_announce_interval(600, 3300));
#else
    /* Disable mesh PS function */
    ESP_ERROR_CHECK(esp_mesh_disable_ps());
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10));
#endif
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    /* mesh ID */
    memcpy((uint8_t *) &cfg.mesh_id, MESH_ID, 6);
    /* router */
    cfg.channel = MESH_CHANNEL;
    cfg.router.ssid_len = strlen(MESH_ROUTER_SSID);
    memcpy((uint8_t *) &cfg.router.ssid, MESH_ROUTER_SSID, cfg.router.ssid_len);
    memcpy((uint8_t *) &cfg.router.password, MESH_ROUTER_PASSWD,
           strlen(MESH_ROUTER_PASSWD));
    /* mesh softAP */
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(MESH_AP_AUTHMODE));
    cfg.mesh_ap.max_connection = MESH_AP_CONNECTIONS;
    cfg.mesh_ap.nonmesh_max_connection = MESH_NON_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *) &cfg.mesh_ap.password, MESH_AP_PASSWD,
           strlen(MESH_AP_PASSWD));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    /* mesh start */
    ESP_ERROR_CHECK(esp_mesh_start());
#ifdef CONFIG_MESH_ENABLE_PS
    /* set the device active duty cycle. (default:10, MESH_PS_DEVICE_DUTY_REQUEST) */
    ESP_ERROR_CHECK(esp_mesh_set_active_duty_cycle(MESH_PS_DEV_DUTY, MESH_PS_DEV_DUTY_TYPE));
    /* set the network active duty cycle. (default:10, -1, MESH_PS_NETWORK_DUTY_APPLIED_ENTIRE) */
    ESP_ERROR_CHECK(esp_mesh_set_network_duty_cycle(MESH_PS_NWK_DUTY, MESH_PS_NWK_DUTY_DURATION, MESH_PS_NWK_DUTY_RULE));
#endif
    ESP_LOGI(MESH_TAG, "neoMesh started successfully, heap:%" PRId32 ", %s<%d>%s, ps:%d",  esp_get_minimum_free_heap_size(),
             esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed",
             esp_mesh_get_topology(), esp_mesh_get_topology() ? "(chain)":"(tree)", esp_mesh_is_ps_enabled());

}

