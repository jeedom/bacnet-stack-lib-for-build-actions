/* ============================================================================
 * FONCTIONS CLIENT - Discovery, Read, Write, COV
 * Ces fonctions permettent au serveur d'agir également comme client BACnet
 * ============================================================================ */

/* Utility functions */
static bool parse_object_id(const char *str, BACNET_OBJECT_TYPE *type, uint32_t *instance)
{
    char type_str[64];
    unsigned int inst;
    int i;
    
    if (sscanf(str, "%63[^:]:%u", type_str, &inst) == 2) {
        *instance = inst;
        
        /* Try to parse object type */
        for (i = 0; i < MAX_BACNET_OBJECT_TYPE; i++) {
            if (strcasecmp(type_str, bactext_object_type_name(i)) == 0) {
                *type = i;
                return true;
            }
        }
    }
    
    return false;
}

static bool ip_to_bacnet_address(const char *ip_str, BACNET_ADDRESS *addr)
{
    unsigned int ip_parts[4];
    
    if (sscanf(ip_str, "%u.%u.%u.%u", &ip_parts[0], &ip_parts[1], 
               &ip_parts[2], &ip_parts[3]) != 4) {
        return false;
    }
    
    /* Validate IP parts */
    if (ip_parts[0] > 255 || ip_parts[1] > 255 || 
        ip_parts[2] > 255 || ip_parts[3] > 255) {
        return false;
    }
    
    /* BACnet/IP MAC address = 4 bytes IP + 2 bytes port (0xBAC0 = 47808) */
    addr->mac[0] = (uint8_t)ip_parts[0];
    addr->mac[1] = (uint8_t)ip_parts[1];
    addr->mac[2] = (uint8_t)ip_parts[2];
    addr->mac[3] = (uint8_t)ip_parts[3];
    addr->mac[4] = 0xBA;  /* Port high byte */
    addr->mac[5] = 0xC0;  /* Port low byte */
    addr->mac_len = 6;
    addr->net = 0;  /* Local network */
    addr->len = 0;  /* No SADR */
    
    return true;
}

static char *client_create_error_response(const char *error_msg)
{
    json_t *response = json_object();
    json_object_set_new(response, "status", json_string("error"));
    json_object_set_new(response, "error", json_string(error_msg));
    
    char *json_str = json_dumps(response, JSON_COMPACT);
    json_decref(response);
    return json_str;
}

static char *client_create_success_response(const char *message)
{
    json_t *response = json_object();
    json_object_set_new(response, "status", json_string("success"));
    json_object_set_new(response, "message", json_string(message));
    
    char *json_str = json_dumps(response, JSON_COMPACT);
    json_decref(response);
    return json_str;
}

/* Device list management */
static DISCOVERED_DEVICE *get_device_by_id(uint32_t device_id)
{
    size_t i;
    for (i = 0; i < device_count; i++) {
        if (device_list[i].device_id == device_id) {
            return &device_list[i];
        }
    }
    return NULL;
}

static bool add_discovered_device(uint32_t device_id, BACNET_ADDRESS *addr,
                                  unsigned max_apdu, unsigned segmentation,
                                  uint16_t vendor_id)
{
    DISCOVERED_DEVICE *dev;
    
    pthread_mutex_lock(&device_list_mutex);
    
    /* Check if device already exists */
    dev = get_device_by_id(device_id);
    if (dev) {
        /* Update existing device */
        memcpy(&dev->address, addr, sizeof(BACNET_ADDRESS));
        dev->max_apdu = max_apdu;
        dev->segmentation = segmentation;
        dev->vendor_id = vendor_id;
        pthread_mutex_unlock(&device_list_mutex);
        return true;
    }
    
    /* Add new device */
    if (device_count < MAX_DISCOVERED_DEVICES) {
        dev = &device_list[device_count];
        dev->device_id = device_id;
        memcpy(&dev->address, addr, sizeof(BACNET_ADDRESS));
        dev->max_apdu = max_apdu;
        dev->segmentation = segmentation;
        dev->vendor_id = vendor_id;
        device_count++;
        pthread_mutex_unlock(&device_list_mutex);
        return true;
    }
    
    pthread_mutex_unlock(&device_list_mutex);
    return false;
}

/* Request tracking */
static void cleanup_old_requests(void)
{
    time_t now = time(NULL);
    size_t i;
    
    pthread_mutex_lock(&pending_mutex);
    for (i = 0; i < MAX_PENDING_REQUESTS; i++) {
        if (pending_requests[i].invoke_id != 0 &&
            (now - pending_requests[i].timestamp) > 30) {
            if (pending_requests[i].response_json) {
                free(pending_requests[i].response_json);
            }
            memset(&pending_requests[i], 0, sizeof(PENDING_REQUEST));
        }
    }
    pthread_mutex_unlock(&pending_mutex);
}

static void complete_request(uint8_t invoke_id, const char *json_response, bool is_error)
{
    size_t i;
    
    pthread_mutex_lock(&pending_mutex);
    for (i = 0; i < MAX_PENDING_REQUESTS; i++) {
        if (pending_requests[i].invoke_id == invoke_id) {
            pending_requests[i].completed = true;
            pending_requests[i].error = is_error;
            if (pending_requests[i].response_json) {
                free(pending_requests[i].response_json);
            }
            pending_requests[i].response_json = strdup(json_response);
            pthread_mutex_unlock(&pending_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&pending_mutex);
}

/* Client BACnet Handlers */
static void client_i_am_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src)
{
    uint32_t device_id = 0;
    unsigned max_apdu = 0;
    int segmentation = 0;
    uint16_t vendor_id = 0;
    int len = 0;
    
    len = bacnet_iam_request_decode(service_request, service_len, 
                                     &device_id, &max_apdu, &segmentation, &vendor_id);
    
    if (len > 0) {
        printf("[CLIENT] I-Am from device %u (max_apdu=%u, vendor=%u)\n",
               device_id, max_apdu, vendor_id);
        add_discovered_device(device_id, src, max_apdu, segmentation, vendor_id);
    }
}

static void client_read_property_ack_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data)
{
    BACNET_READ_PROPERTY_DATA data;
    BACNET_APPLICATION_DATA_VALUE value;
    int len = 0;
    (void)src;
    
    len = rp_ack_decode_service_request(service_request, service_len, &data);
    if (len > 0) {
        json_t *response = json_object();
        json_object_set_new(response, "status", json_string("success"));
        json_object_set_new(response, "service", json_string("ReadProperty"));
        
        json_t *result = json_object();
        json_object_set_new(result, "objectType", 
                          json_string(bactext_object_type_name(data.object_type)));
        json_object_set_new(result, "objectInstance", json_integer(data.object_instance));
        json_object_set_new(result, "property",
                          json_string(bactext_property_name(data.object_property)));
        
        /* Decode values */
        if (data.array_index == 0) {
            /* Array count */
            len = bacapp_decode_application_data(data.application_data,
                                                data.application_data_len, &value);
            if (len > 0 && value.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT) {
                json_object_set_new(result, "arraySize", json_integer(value.type.Unsigned_Int));
            }
        } else {
            /* Array values or single value */
            json_t *values_array = json_array();
            uint8_t *app_data = data.application_data;
            uint32_t app_data_len = data.application_data_len;
            
            while (app_data_len > 0) {
                len = bacapp_decode_application_data(app_data, app_data_len, &value);
                if (len <= 0) break;
                
                char value_str[256];
                BACNET_OBJECT_PROPERTY_VALUE obj_value;
                obj_value.object_type = data.object_type;
                obj_value.object_instance = data.object_instance;
                obj_value.object_property = data.object_property;
                obj_value.array_index = BACNET_ARRAY_ALL;
                obj_value.value = &value;
                
                bacapp_snprintf_value(value_str, sizeof(value_str), &obj_value);
                json_array_append_new(values_array, json_string(value_str));
                
                app_data += len;
                app_data_len -= len;
            }
            
            json_object_set_new(result, "values", values_array);
        }
        
        json_object_set_new(response, "result", result);
        
        char *json_str = json_dumps(response, JSON_COMPACT);
        complete_request(service_data->invoke_id, json_str, false);
        free(json_str);
        json_decref(response);
    }
}

static void client_read_property_multiple_ack_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data)
{
    /* Simplified RPM handler */
    (void)service_request;
    (void)service_len;
    (void)src;
    
    char *json = client_create_success_response("RPM-ACK received");
    complete_request(service_data->invoke_id, json, false);
    free(json);
}

static void client_read_range_ack_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data)
{
    /* Simplified ReadRange handler */
    (void)service_request;
    (void)service_len;
    (void)src;
    
    char *json = client_create_success_response("ReadRange-ACK received");
    complete_request(service_data->invoke_id, json, false);
    free(json);
}

static void client_write_property_ack_handler(BACNET_ADDRESS *src, uint8_t invoke_id)
{
    (void)src;
    
    char *json = client_create_success_response("WriteProperty successful");
    complete_request(invoke_id, json, false);
    free(json);
}

static void client_cov_notification_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src)
{
    (void)service_request;
    (void)service_len;
    (void)src;
    
    printf("[CLIENT] COV notification received\n");
}

static void client_error_handler(
    BACNET_ADDRESS *src,
    uint8_t invoke_id,
    BACNET_ERROR_CLASS error_class,
    BACNET_ERROR_CODE error_code)
{
    (void)src;
    
    json_t *response = json_object();
    json_object_set_new(response, "status", json_string("error"));
    json_object_set_new(response, "errorClass", json_string(bactext_error_class_name(error_class)));
    json_object_set_new(response, "errorCode", json_string(bactext_error_code_name(error_code)));
    
    char *json_str = json_dumps(response, JSON_COMPACT);
    complete_request(invoke_id, json_str, true);
    free(json_str);
    json_decref(response);
}

static void client_abort_handler(
    BACNET_ADDRESS *src,
    uint8_t invoke_id,
    uint8_t abort_reason,
    bool server)
{
    (void)src;
    (void)server;
    
    json_t *response = json_object();
    json_object_set_new(response, "status", json_string("aborted"));
    json_object_set_new(response, "reason", json_string(bactext_abort_reason_name(abort_reason)));
    
    char *json_str = json_dumps(response, JSON_COMPACT);
    complete_request(invoke_id, json_str, true);
    free(json_str);
    json_decref(response);
}

static void client_reject_handler(
    BACNET_ADDRESS *src,
    uint8_t invoke_id,
    uint8_t reject_reason)
{
    (void)src;
    
    json_t *response = json_object();
    json_object_set_new(response, "status", json_string("rejected"));
    json_object_set_new(response, "reason", json_string(bactext_reject_reason_name(reject_reason)));
    
    char *json_str = json_dumps(response, JSON_COMPACT);
    complete_request(invoke_id, json_str, true);
    free(json_str);
    json_decref(response);
}

/* Client command handlers */
static int handle_client_whois(json_t *root)
{
    json_t *cmd_obj = json_object_get(root, "cmd");
    int32_t device_min = -1;
    int32_t device_max = -1;
    json_t *min_obj, *max_obj;
    char *response;
    
    if (!cmd_obj || strcmp(json_string_value(cmd_obj), "whois") != 0) {
        return -1;  /* Not a whois command */
    }
    
    min_obj = json_object_get(root, "deviceMin");
    max_obj = json_object_get(root, "deviceMax");
    
    if (min_obj && json_is_integer(min_obj)) {
        device_min = json_integer_value(min_obj);
    }
    if (max_obj && json_is_integer(max_obj)) {
        device_max = json_integer_value(max_obj);
    }
    
    /* Convert to valid range */
    if (device_min < 0) device_min = 0;
    if (device_max < 0) device_max = 4194303;
    if (device_min > device_max) {
        int32_t tmp = device_min;
        device_min = device_max;
        device_max = tmp;
    }
    
    printf("[CLIENT] Sending Who-Is (min=%d, max=%d)\n", device_min, device_max);
    Send_WhoIs_Global(device_min, device_max);
    
    sleep(4);  /* Wait for I-Am responses */
    
    response = client_create_success_response("Who-Is sent");
    write(g_client_fd, response, strlen(response));
    write(g_client_fd, "\n", 1);
    free(response);
    
    return 0;
}

static int handle_client_devicelist(json_t *root)
{
    json_t *cmd_obj = json_object_get(root, "cmd");
    json_t *response, *devices;
    char *json_str;
    size_t i;
    
    if (!cmd_obj || strcmp(json_string_value(cmd_obj), "devicelist") != 0) {
        return -1;
    }
    
    response = json_object();
    json_object_set_new(response, "status", json_string("success"));
    
    devices = json_array();
    pthread_mutex_lock(&device_list_mutex);
    for (i = 0; i < device_count; i++) {
        json_t *dev = json_object();
        json_object_set_new(dev, "deviceId", json_integer(device_list[i].device_id));
        json_object_set_new(dev, "maxApdu", json_integer(device_list[i].max_apdu));
        json_object_set_new(dev, "vendorId", json_integer(device_list[i].vendor_id));
        
        /* Format MAC address */
        char mac_str[64] = "";
        int j;
        for (j = 0; j < device_list[i].address.mac_len && j < MAX_MAC_LEN; j++) {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%s%02X", j > 0 ? ":" : "", 
                    device_list[i].address.mac[j]);
            strcat(mac_str, tmp);
        }
        json_object_set_new(dev, "mac", json_string(mac_str));
        
        /* Extract IP if it's BACnet/IP */
        if (device_list[i].address.mac_len == 6) {
            char ip_str[32];
            snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                    device_list[i].address.mac[0],
                    device_list[i].address.mac[1],
                    device_list[i].address.mac[2],
                    device_list[i].address.mac[3]);
            json_object_set_new(dev, "ip", json_string(ip_str));
        }
        
        json_array_append_new(devices, dev);
    }
    pthread_mutex_unlock(&device_list_mutex);
    
    json_object_set_new(response, "devices", devices);
    json_object_set_new(response, "count", json_integer(device_count));
    
    json_str = json_dumps(response, JSON_COMPACT);
    write(g_client_fd, json_str, strlen(json_str));
    write(g_client_fd, "\n", 1);
    free(json_str);
    json_decref(response);
    
    return 0;
}

static int handle_client_objectlist(json_t *root)
{
    json_t *cmd_obj = json_object_get(root, "cmd");
    json_t *device_obj, *ip_obj;
    uint32_t target_device_id;
    BACNET_ADDRESS target_addr;
    DISCOVERED_DEVICE *dev;
    uint8_t invoke_id;
    char *response;
    int timeout;
    size_t i;
    PENDING_REQUEST *req = NULL;
    
    if (!cmd_obj || strcmp(json_string_value(cmd_obj), "objectlist") != 0) {
        return -1;
    }
    
    device_obj = json_object_get(root, "device");
    ip_obj = json_object_get(root, "ip");
    
    if (!device_obj || !json_is_integer(device_obj)) {
        response = client_create_error_response("Missing device parameter");
        write(g_client_fd, response, strlen(response));
        write(g_client_fd, "\n", 1);
        free(response);
        return 0;
    }
    
    target_device_id = json_integer_value(device_obj);
    
    /* Get device address */
    if (ip_obj && json_is_string(ip_obj)) {
        /* Use provided IP */
        if (!ip_to_bacnet_address(json_string_value(ip_obj), &target_addr)) {
            response = client_create_error_response("Invalid IP address");
            write(g_client_fd, response, strlen(response));
            write(g_client_fd, "\n", 1);
            free(response);
            return 0;
        }
    } else {
        /* Look up in discovered devices */
        dev = get_device_by_id(target_device_id);
        if (!dev) {
            response = client_create_error_response("Device not found");
            write(g_client_fd, response, strlen(response));
            write(g_client_fd, "\n", 1);
            free(response);
            return 0;
        }
        memcpy(&target_addr, &dev->address, sizeof(BACNET_ADDRESS));
    }
    
    /* Allocate invoke ID and track request */
    invoke_id = tsm_next_free_invokeID();
    if (invoke_id == 0) {
        response = client_create_error_response("No free invoke ID");
        write(g_client_fd, response, strlen(response));
        write(g_client_fd, "\n", 1);
        free(response);
        return 0;
    }
    
    /* Track the request */
    pthread_mutex_lock(&pending_mutex);
    for (i = 0; i < MAX_PENDING_REQUESTS; i++) {
        if (pending_requests[i].invoke_id == 0) {
            pending_requests[i].invoke_id = invoke_id;
            pending_requests[i].completed = false;
            pending_requests[i].error = false;
            pending_requests[i].response_json = NULL;
            pending_requests[i].timestamp = time(NULL);
            req = &pending_requests[i];
            break;
        }
    }
    pthread_mutex_unlock(&pending_mutex);
    
    if (!req) {
        response = client_create_error_response("No free request slot");
        write(g_client_fd, response, strlen(response));
        write(g_client_fd, "\n", 1);
        free(response);
        return 0;
    }
    
    /* Send ReadProperty for object-list */
    if (!Send_Read_Property_Request_Address(
            &target_addr,
            1476,  /* max APDU */
            OBJECT_DEVICE,
            target_device_id,
            PROP_OBJECT_LIST,
            BACNET_ARRAY_ALL)) {
        
        pthread_mutex_lock(&pending_mutex);
        memset(req, 0, sizeof(PENDING_REQUEST));
        pthread_mutex_unlock(&pending_mutex);
        
        response = client_create_error_response("Failed to send request");
        write(g_client_fd, response, strlen(response));
        write(g_client_fd, "\n", 1);
        free(response);
        return 0;
    }
    
    /* Wait for response */
    for (timeout = 0; timeout < 100; timeout++) {  /* 10 seconds */
        usleep(100000);  /* 100ms */
        
        pthread_mutex_lock(&pending_mutex);
        if (req->completed) {
            if (req->response_json) {
                write(g_client_fd, req->response_json, strlen(req->response_json));
                write(g_client_fd, "\n", 1);
                free(req->response_json);
            }
            memset(req, 0, sizeof(PENDING_REQUEST));
            pthread_mutex_unlock(&pending_mutex);
            return 0;
        }
        pthread_mutex_unlock(&pending_mutex);
    }
    
    /* Timeout */
    pthread_mutex_lock(&pending_mutex);
    memset(req, 0, sizeof(PENDING_REQUEST));
    pthread_mutex_unlock(&pending_mutex);
    
    response = client_create_error_response("Timeout waiting for response");
    write(g_client_fd, response, strlen(response));
    write(g_client_fd, "\n", 1);
    free(response);
    
    return 0;
}

/* FIN FONCTIONS CLIENT */
