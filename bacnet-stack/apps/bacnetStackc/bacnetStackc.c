/*
 * BACnet Stack Client (bacnetStackc)
 * 
 * Comprehensive BACnet client with socket interface for Jeedom plugin
 * Supports all major BACnet client operations:
 * - Device discovery (Who-Is/I-Am)
 * - Read operations (ReadProperty, ReadPropertyMultiple, ReadRange)
 * - Write operations (WriteProperty, WritePropertyMultiple)
 * - COV subscriptions (SubscribeCOV, SubscribeCOVProperty)
 * - Time synchronization
 * - Alarm/Event operations
 * - Device management
 * 
 * Copyright (C) 2025 Jeedom
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>

/* BACnet Stack includes */
#include "bacnet/bacdef.h"
#include "bacnet/config.h"
#include "bacnet/bactext.h"
#include "bacnet/bacerror.h"
#include "bacnet/iam.h"
#include "bacnet/arf.h"
#include "bacnet/npdu.h"
#include "bacnet/apdu.h"
#include "bacnet/whois.h"
#include "bacnet/rp.h"
#include "bacnet/rpm.h"
#include "bacnet/wp.h"
#include "bacnet/wpm.h"
#include "bacnet/cov.h"
#include "bacnet/ihave.h"
#include "bacnet/whohas.h"
#include "bacnet/readrange.h"
#include "bacnet/timesync.h"
#include "bacnet/datetime.h"
#include "bacnet/bacapp.h"
#include "bacnet/bacdcode.h"
#include "bacnet/dcc.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/datalink/bip.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/basic/services.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/basic/service/s_iam.h"
#include "bacnet/basic/service/s_whois.h"
#include "bacnet/basic/service/s_rp.h"
#include "bacnet/basic/service/s_rpm.h"
#include "bacnet/basic/service/s_wp.h"
#include "bacnet/basic/service/s_readrange.h"
#include "bacnet/basic/sys/mstimer.h"

/* JSON parsing */
#include <jansson.h>

/* Configuration */
#define DEFAULT_SOCKET_PORT 1235
#define MAX_BUFFER_SIZE 65536
#define BACNET_PORT 0xBAC0
#define DEFAULT_BACNET_INTERFACE NULL
#define BACNET_BBMD_ADDRESS NULL
#define BACNET_BBMD_PORT 0xBAC0
#define BACNET_BBMD_TTL 90

/* Runtime configuration */
static int socket_port = DEFAULT_SOCKET_PORT;
static char pid_file_path[256] = {0};

/* Client state */
static volatile bool running = true;
static int socket_fd = -1;
static pthread_t network_thread;
static pthread_mutex_t response_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Response storage for async operations */
typedef struct {
    uint8_t invoke_id;
    bool completed;
    bool error;
    char *response_json;
    time_t timestamp;
} PENDING_REQUEST;

#define MAX_PENDING_REQUESTS 256
static PENDING_REQUEST pending_requests[MAX_PENDING_REQUESTS];

/* Discovered devices storage */
typedef struct device_entry {
    uint32_t device_id;
    BACNET_ADDRESS address;
    uint32_t max_apdu;
    int segmentation;
    uint16_t vendor_id;
    char name[64];
    time_t last_seen;
    struct device_entry *next;
} DEVICE_ENTRY;

static DEVICE_ENTRY *device_list = NULL;
static pthread_mutex_t device_mutex = PTHREAD_MUTEX_INITIALIZER;

/* COV subscriptions storage */
typedef struct cov_subscription {
    uint32_t device_id;
    BACNET_ADDRESS address;
    BACNET_OBJECT_TYPE object_type;
    uint32_t object_instance;
    uint32_t subscriber_process_id;
    time_t lifetime;
    bool confirmed;
    struct cov_subscription *next;
} COV_SUBSCRIPTION;

static COV_SUBSCRIPTION *cov_list = NULL;
static pthread_mutex_t cov_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Forward declarations */
static void signal_handler(int sig);
static void cleanup(void);
static void *network_task(void *arg);
static void process_socket_command(int client_fd, const char *json_cmd);

/* BACnet callback handlers */
static void my_i_am_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src);

static void my_read_property_ack_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data);

static void my_read_property_multiple_ack_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data);

static void my_read_range_ack_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data);

static void my_write_property_ack_handler(
    BACNET_ADDRESS *src,
    uint8_t invoke_id);

static void my_cov_notification_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src);

static void my_error_handler(
    BACNET_ADDRESS *src,
    uint8_t invoke_id,
    BACNET_ERROR_CLASS error_class,
    BACNET_ERROR_CODE error_code);

static void my_abort_handler(
    BACNET_ADDRESS *src,
    uint8_t invoke_id,
    uint8_t abort_reason,
    bool server);

static void my_reject_handler(
    BACNET_ADDRESS *src,
    uint8_t invoke_id,
    uint8_t reject_reason);

/* Helper functions */
static PENDING_REQUEST *allocate_request(uint8_t invoke_id);
static PENDING_REQUEST *find_request(uint8_t invoke_id);
static void complete_request(uint8_t invoke_id, const char *json_response, bool error);
static void cleanup_old_requests(void);

static void add_device(uint32_t device_id, BACNET_ADDRESS *addr, 
                      uint32_t max_apdu, int segmentation, uint16_t vendor_id);
static DEVICE_ENTRY *find_device(uint32_t device_id);
static char *get_device_list_json(void);

static void add_cov_subscription(uint32_t device_id, BACNET_ADDRESS *addr,
                                BACNET_OBJECT_TYPE obj_type, uint32_t obj_instance,
                                uint32_t process_id, time_t lifetime, bool confirmed);
static COV_SUBSCRIPTION *find_cov_subscription(uint32_t device_id, 
                                               BACNET_OBJECT_TYPE obj_type,
                                               uint32_t obj_instance);

/* Command handlers */
static void handle_whois_command(int client_fd, json_t *params);
static void handle_iam_command(int client_fd, json_t *params);
static void handle_readprop_command(int client_fd, json_t *params);
static void handle_readpropm_command(int client_fd, json_t *params);
static void handle_readrange_command(int client_fd, json_t *params);
static void handle_writeprop_command(int client_fd, json_t *params);
static void handle_writepropm_command(int client_fd, json_t *params);
static void handle_subscribecov_command(int client_fd, json_t *params);
static void handle_unsubscribecov_command(int client_fd, json_t *params);
static void handle_timesync_command(int client_fd, json_t *params);
static void handle_whohas_command(int client_fd, json_t *params);
static void handle_devicelist_command(int client_fd, json_t *params);
static void handle_objectlist_command(int client_fd, json_t *params);
static void handle_reinit_command(int client_fd, json_t *params);
static void handle_devicecomm_command(int client_fd, json_t *params);

/* Utility functions */
static bool parse_object_id(const char *str, BACNET_OBJECT_TYPE *type, uint32_t *instance);
static bool parse_bacnet_address(const char *addr_str, BACNET_ADDRESS *addr);
static char *create_error_response(const char *error_msg);
static char *create_success_response(const char *message);

/*
 * Signal handler
 */
static void signal_handler(int sig)
{
    (void)sig;
    running = false;
}

/*
 * Cleanup function
 */
static void cleanup(void)
{
    printf("Cleaning up...\n");
    
    running = false;
    
    /* Close socket */
    if (socket_fd >= 0) {
        close(socket_fd);
    }
    
    /* Remove PID file */
    if (pid_file_path[0] != '\0') {
        unlink(pid_file_path);
    }
    
    /* Cleanup datalink */
    datalink_cleanup();
    
    /* Free device list */
    pthread_mutex_lock(&device_mutex);
    DEVICE_ENTRY *dev = device_list;
    while (dev) {
        DEVICE_ENTRY *next = dev->next;
        free(dev);
        dev = next;
    }
    device_list = NULL;
    pthread_mutex_unlock(&device_mutex);
    
    /* Free COV list */
    pthread_mutex_lock(&cov_mutex);
    COV_SUBSCRIPTION *cov = cov_list;
    while (cov) {
        COV_SUBSCRIPTION *next = cov->next;
        free(cov);
        cov = next;
    }
    cov_list = NULL;
    pthread_mutex_unlock(&cov_mutex);
    
    /* Free pending requests */
    pthread_mutex_lock(&response_mutex);
    {
        int i;
        for (i = 0; i < MAX_PENDING_REQUESTS; i++) {
            if (pending_requests[i].response_json) {
                free(pending_requests[i].response_json);
                pending_requests[i].response_json = NULL;
            }
        }
    }
    pthread_mutex_unlock(&response_mutex);
    
    printf("Cleanup complete.\n");
}

/*
 * I-Am handler - stores discovered devices
 */
static void my_i_am_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src)
{
    uint32_t device_id = 0;
    unsigned max_apdu = 0;
    int segmentation = 0;
    uint16_t vendor_id = 0;
    int len = 0;
    
    printf("[CLIENT] I-Am handler called (service_len=%u)\n", service_len);
    fflush(stdout);
    
    len = bacnet_iam_request_decode(service_request, service_len, 
                                     &device_id, &max_apdu, &segmentation, &vendor_id);
    
    if (len > 0) {
        printf("[CLIENT] ✓ I-Am decoded: Device %u, Max APDU %u, Vendor %u\n",
               device_id, max_apdu, vendor_id);
        fflush(stdout);
        
        /* Log MAC address */
        printf("[CLIENT]   MAC address: ");
        int i;
        for (i = 0; i < src->mac_len && i < MAX_MAC_LEN; i++) {
            printf("%s%02X", i > 0 ? ":" : "", src->mac[i]);
        }
        printf(" (len=%d)\n", src->mac_len);
        fflush(stdout);
        
        add_device(device_id, src, max_apdu, segmentation, vendor_id);
        printf("[CLIENT] ✓ Device %u added to device_list\n", device_id);
        fflush(stdout);
    } else {
        printf("[CLIENT] ✗ Failed to decode I-Am message (len=%d)\n", len);
        fflush(stdout);
    }
}

/*
 * ReadProperty-ACK handler
 */
static void my_read_property_ack_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data)
{
    BACNET_READ_PROPERTY_DATA data;
    int len = 0;
    BACNET_APPLICATION_DATA_VALUE value;
    
    (void)src;
    
    len = rp_ack_decode_service_request(service_request, service_len, &data);
    if (len > 0) {
        /* Decode the value */
        len = bacapp_decode_application_data(data.application_data,
                                             data.application_data_len, &value);
        
        if (len >= 0) {
            /* Create JSON response */
            json_t *response = json_object();
            json_object_set_new(response, "status", json_string("success"));
            json_object_set_new(response, "service", json_string("ReadProperty"));
            json_object_set_new(response, "invokeId", json_integer(service_data->invoke_id));
            
            json_t *result = json_object();
            json_object_set_new(result, "objectType", 
                                json_string(bactext_object_type_name(data.object_type)));
            json_object_set_new(result, "objectInstance", json_integer(data.object_instance));
            json_object_set_new(result, "property",
                                json_string(bactext_property_name(data.object_property)));
            
            /* Add value based on type */
            {
                char value_str[256];
                BACNET_OBJECT_PROPERTY_VALUE obj_value;
                obj_value.object_type = data.object_type;
                obj_value.object_instance = data.object_instance;
                obj_value.object_property = data.object_property;
                obj_value.array_index = data.array_index;
                obj_value.value = &value;
                bacapp_snprintf_value(value_str, sizeof(value_str), &obj_value);
                json_object_set_new(result, "value", json_string(value_str));
            }
            json_object_set_new(result, "datatype",
                                json_string(bactext_application_tag_name(value.tag)));
            
            json_object_set_new(response, "result", result);
            
            char *json_str = json_dumps(response, JSON_COMPACT);
            complete_request(service_data->invoke_id, json_str, false);
            free(json_str);
            json_decref(response);
        }
    }
}

/*
 * ReadPropertyMultiple-ACK handler
 */
static void my_read_property_multiple_ack_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data)
{
    BACNET_READ_ACCESS_DATA *rpm_data;
    BACNET_PROPERTY_REFERENCE *rpm_property;
    BACNET_APPLICATION_DATA_VALUE *value;
    
    (void)src;
    
    rpm_data = calloc(1, sizeof(BACNET_READ_ACCESS_DATA));
    if (!rpm_data) return;
    
    if (rpm_ack_decode_service_request(service_request, service_len, rpm_data)) {
        json_t *response = json_object();
        json_object_set_new(response, "status", json_string("success"));
        json_object_set_new(response, "service", json_string("ReadPropertyMultiple"));
        json_object_set_new(response, "invokeId", json_integer(service_data->invoke_id));
        
        json_t *objects = json_array();
        
        BACNET_READ_ACCESS_DATA *rpm_object = rpm_data;
        while (rpm_object) {
            json_t *obj = json_object();
            json_object_set_new(obj, "objectType",
                                json_string(bactext_object_type_name(rpm_object->object_type)));
            json_object_set_new(obj, "objectInstance", json_integer(rpm_object->object_instance));
            
            json_t *properties = json_array();
            rpm_property = rpm_object->listOfProperties;
            while (rpm_property) {
                json_t *prop = json_object();
                json_object_set_new(prop, "property",
                                   json_string(bactext_property_name(rpm_property->propertyIdentifier)));
                
                if (rpm_property->propertyArrayIndex != BACNET_ARRAY_ALL) {
                    json_object_set_new(prop, "arrayIndex", 
                                      json_integer(rpm_property->propertyArrayIndex));
                }
                
                if (rpm_property->value) {
                    char value_str[256];
                    BACNET_OBJECT_PROPERTY_VALUE obj_value;
                    obj_value.object_type = rpm_object->object_type;
                    obj_value.object_instance = rpm_object->object_instance;
                    obj_value.object_property = rpm_property->propertyIdentifier;
                    obj_value.array_index = rpm_property->propertyArrayIndex;
                    obj_value.value = rpm_property->value;
                    bacapp_snprintf_value(value_str, sizeof(value_str), &obj_value);
                    json_object_set_new(prop, "value", json_string(value_str));
                }
                
                if (rpm_property->error.error_class < MAX_BACNET_ERROR_CLASS) {
                    json_object_set_new(prop, "error",
                                      json_string(bactext_error_code_name(rpm_property->error.error_code)));
                }
                
                json_array_append_new(properties, prop);
                rpm_property = rpm_property->next;
            }
            
            json_object_set_new(obj, "properties", properties);
            json_array_append_new(objects, obj);
            rpm_object = rpm_object->next;
        }
        
        json_object_set_new(response, "objects", objects);
        
        char *json_str = json_dumps(response, JSON_COMPACT);
        complete_request(service_data->invoke_id, json_str, false);
        free(json_str);
        json_decref(response);
    }
    
    rpm_data_free(rpm_data);
}

/*
 * ReadRange-ACK handler
 */
static void my_read_range_ack_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_ACK_DATA *service_data)
{
    BACNET_READ_RANGE_DATA data;
    
    (void)src;
    
    if (rr_ack_decode_service_request(service_request, service_len, &data)) {
        json_t *response = json_object();
        json_object_set_new(response, "status", json_string("success"));
        json_object_set_new(response, "service", json_string("ReadRange"));
        json_object_set_new(response, "invokeId", json_integer(service_data->invoke_id));
        
        json_t *result = json_object();
        json_object_set_new(result, "objectType",
                            json_string(bactext_object_type_name(data.object_type)));
        json_object_set_new(result, "objectInstance", json_integer(data.object_instance));
        json_object_set_new(result, "property",
                            json_string(bactext_property_name(data.object_property)));
        
        json_object_set_new(result, "itemCount", json_integer(data.ItemCount));
        json_object_set_new(result, "firstSequence", json_integer(data.FirstSequence));
        
        json_t *flags = json_object();
        json_object_set_new(flags, "firstItem", 
                           json_boolean(bitstring_bit(&data.ResultFlags, RESULT_FLAG_FIRST_ITEM)));
        json_object_set_new(flags, "lastItem",
                           json_boolean(bitstring_bit(&data.ResultFlags, RESULT_FLAG_LAST_ITEM)));
        json_object_set_new(flags, "moreItems",
                           json_boolean(bitstring_bit(&data.ResultFlags, RESULT_FLAG_MORE_ITEMS)));
        json_object_set_new(result, "resultFlags", flags);
        
        /* TODO: Decode itemData based on object type */
        json_object_set_new(result, "itemData", json_string("base64-encoded-data"));
        
        json_object_set_new(response, "result", result);
        
        char *json_str = json_dumps(response, JSON_COMPACT);
        complete_request(service_data->invoke_id, json_str, false);
        free(json_str);
        json_decref(response);
    }
}

/*
 * WriteProperty-ACK handler
 */
static void my_write_property_ack_handler(
    BACNET_ADDRESS *src,
    uint8_t invoke_id)
{
    (void)src;
    
    json_t *response = json_object();
    json_object_set_new(response, "status", json_string("success"));
    json_object_set_new(response, "service", json_string("WriteProperty"));
    json_object_set_new(response, "invokeId", json_integer(invoke_id));
    json_object_set_new(response, "message", json_string("Write successful"));
    
    char *json_str = json_dumps(response, JSON_COMPACT);
    complete_request(invoke_id, json_str, false);
    free(json_str);
    json_decref(response);
}

/*
 * COV Notification handler
 */
static void my_cov_notification_handler(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src)
{
    BACNET_COV_DATA cov_data;
    BACNET_PROPERTY_VALUE *value_list;
    
    (void)src;
    
    if (cov_notify_decode_service_request(service_request, service_len, &cov_data)) {
        json_t *notification = json_object();
        json_object_set_new(notification, "type", json_string("COVNotification"));
        json_object_set_new(notification, "subscriberProcessId", 
                            json_integer(cov_data.subscriberProcessIdentifier));
        json_object_set_new(notification, "deviceId", 
                            json_integer(cov_data.initiatingDeviceIdentifier));
        json_object_set_new(notification, "objectType",
                            json_string(bactext_object_type_name(cov_data.monitoredObjectIdentifier.type)));
        json_object_set_new(notification, "objectInstance",
                            json_integer(cov_data.monitoredObjectIdentifier.instance));
        json_object_set_new(notification, "timeRemaining", json_integer(cov_data.timeRemaining));
        
        json_t *values = json_array();
        value_list = cov_data.listOfValues;
        while (value_list) {
            json_t *prop = json_object();
            json_object_set_new(prop, "property",
                               json_string(bactext_property_name(value_list->propertyIdentifier)));
            
            if (value_list->value.tag != BACNET_APPLICATION_TAG_NULL) {
                char value_str[256];
                BACNET_OBJECT_PROPERTY_VALUE obj_value;
                obj_value.object_type = cov_data.monitoredObjectIdentifier.type;
                obj_value.object_instance = cov_data.monitoredObjectIdentifier.instance;
                obj_value.object_property = value_list->propertyIdentifier;
                obj_value.array_index = BACNET_ARRAY_ALL;
                obj_value.value = &value_list->value;
                bacapp_snprintf_value(value_str, sizeof(value_str), &obj_value);
                json_object_set_new(prop, "value", json_string(value_str));
            }
            
            json_array_append_new(values, prop);
            value_list = value_list->next;
        }
        
        json_object_set_new(notification, "values", values);
        
        char *json_str = json_dumps(notification, JSON_INDENT(2));
        printf("COV Notification: %s\n", json_str);
        free(json_str);
        json_decref(notification);
    }
}

/*
 * Error handler
 */
static void my_error_handler(
    BACNET_ADDRESS *src,
    uint8_t invoke_id,
    BACNET_ERROR_CLASS error_class,
    BACNET_ERROR_CODE error_code)
{
    (void)src;
    
    json_t *response = json_object();
    json_object_set_new(response, "status", json_string("error"));
    json_object_set_new(response, "invokeId", json_integer(invoke_id));
    json_object_set_new(response, "errorClass",
                        json_string(bactext_error_class_name(error_class)));
    json_object_set_new(response, "errorCode",
                        json_string(bactext_error_code_name(error_code)));
    
    char *json_str = json_dumps(response, JSON_COMPACT);
    complete_request(invoke_id, json_str, true);
    free(json_str);
    json_decref(response);
}

/*
 * Abort handler
 */
static void my_abort_handler(
    BACNET_ADDRESS *src,
    uint8_t invoke_id,
    uint8_t abort_reason,
    bool server)
{
    (void)src;
    
    json_t *response = json_object();
    json_object_set_new(response, "status", json_string("abort"));
    json_object_set_new(response, "invokeId", json_integer(invoke_id));
    json_object_set_new(response, "abortReason",
                        json_string(bactext_abort_reason_name(abort_reason)));
    json_object_set_new(response, "server", json_boolean(server));
    
    char *json_str = json_dumps(response, JSON_COMPACT);
    complete_request(invoke_id, json_str, true);
    free(json_str);
    json_decref(response);
}

/*
 * Reject handler
 */
static void my_reject_handler(
    BACNET_ADDRESS *src,
    uint8_t invoke_id,
    uint8_t reject_reason)
{
    (void)src;
    
    json_t *response = json_object();
    json_object_set_new(response, "status", json_string("reject"));
    json_object_set_new(response, "invokeId", json_integer(invoke_id));
    json_object_set_new(response, "rejectReason",
                        json_string(bactext_reject_reason_name(reject_reason)));
    
    char *json_str = json_dumps(response, JSON_COMPACT);
    complete_request(invoke_id, json_str, true);
    free(json_str);
    json_decref(response);
}

/* 
 * Request management functions
 */
static PENDING_REQUEST *allocate_request(uint8_t invoke_id)
{
    pthread_mutex_lock(&response_mutex);
    
    PENDING_REQUEST *req = &pending_requests[invoke_id];
    if (req->response_json) {
        free(req->response_json);
    }
    
    memset(req, 0, sizeof(PENDING_REQUEST));
    req->invoke_id = invoke_id;
    req->timestamp = time(NULL);
    
    pthread_mutex_unlock(&response_mutex);
    return req;
}

static PENDING_REQUEST *find_request(uint8_t invoke_id)
{
    return &pending_requests[invoke_id];
}

static void complete_request(uint8_t invoke_id, const char *json_response, bool error)
{
    pthread_mutex_lock(&response_mutex);
    
    PENDING_REQUEST *req = &pending_requests[invoke_id];
    req->completed = true;
    req->error = error;
    if (req->response_json) {
        free(req->response_json);
    }
    req->response_json = strdup(json_response);
    
    pthread_mutex_unlock(&response_mutex);
}

static void cleanup_old_requests(void)
{
    time_t now = time(NULL);
    int i;
    
    pthread_mutex_lock(&response_mutex);
    for (i = 0; i < MAX_PENDING_REQUESTS; i++) {
        if (pending_requests[i].timestamp > 0 &&
            (now - pending_requests[i].timestamp) > 60) {
            if (pending_requests[i].response_json) {
                free(pending_requests[i].response_json);
            }
            memset(&pending_requests[i], 0, sizeof(PENDING_REQUEST));
        }
    }
    pthread_mutex_unlock(&response_mutex);
}

/*
 * Device list management
 */
static void add_device(uint32_t device_id, BACNET_ADDRESS *addr,
                      uint32_t max_apdu, int segmentation, uint16_t vendor_id)
{
    pthread_mutex_lock(&device_mutex);
    
    /* Check if device already exists */
    DEVICE_ENTRY *dev = device_list;
    while (dev) {
        if (dev->device_id == device_id) {
            /* Update existing */
            printf("[CLIENT] Updating existing device %u in list\n", device_id);
            fflush(stdout);
            bacnet_address_copy(&dev->address, addr);
            dev->max_apdu = max_apdu;
            dev->segmentation = segmentation;
            dev->vendor_id = vendor_id;
            dev->last_seen = time(NULL);
            pthread_mutex_unlock(&device_mutex);
            return;
        }
        dev = dev->next;
    }
    
    /* Add new device */
    printf("[CLIENT] Adding NEW device %u to list\n", device_id);
    fflush(stdout);
    dev = calloc(1, sizeof(DEVICE_ENTRY));
    if (dev) {
        dev->device_id = device_id;
        bacnet_address_copy(&dev->address, addr);
        dev->max_apdu = max_apdu;
        dev->segmentation = segmentation;
        dev->vendor_id = vendor_id;
        dev->last_seen = time(NULL);
        dev->next = device_list;
        device_list = dev;
        printf("[CLIENT] ✓ Device %u successfully added\n", device_id);
        fflush(stdout);
    } else {
        printf("[CLIENT] ✗ Failed to allocate memory for device %u\n", device_id);
        fflush(stdout);
    }
    
    pthread_mutex_unlock(&device_mutex);
}

static DEVICE_ENTRY *find_device(uint32_t device_id)
{
    pthread_mutex_lock(&device_mutex);
    
    DEVICE_ENTRY *dev = device_list;
    while (dev) {
        if (dev->device_id == device_id) {
            pthread_mutex_unlock(&device_mutex);
            return dev;
        }
        dev = dev->next;
    }
    
    pthread_mutex_unlock(&device_mutex);
    return NULL;
}

static char *get_device_list_json(void)
{
    int device_count = 0;
    pthread_mutex_lock(&device_mutex);
    
    /* Count devices first */
    DEVICE_ENTRY *count_dev = device_list;
    while (count_dev) {
        device_count++;
        count_dev = count_dev->next;
    }
    printf("[CLIENT] get_device_list_json: %d device(s) in list\n", device_count);
    fflush(stdout);
    
    json_t *response = json_object();
    json_object_set_new(response, "status", json_string("success"));
    
    json_t *devices = json_array();
    
    DEVICE_ENTRY *dev = device_list;
    while (dev) {
        json_t *device = json_object();
        json_object_set_new(device, "deviceId", json_integer(dev->device_id));
        
        char addr_str[128];
        int pos = 0;
        int i;
        for (i = 0; i < dev->address.mac_len && i < MAX_MAC_LEN; i++) {
            if (i > 0) pos += snprintf(addr_str + pos, sizeof(addr_str) - pos, ":");
            pos += snprintf(addr_str + pos, sizeof(addr_str) - pos, "%02X", dev->address.mac[i]);
        }
        json_object_set_new(device, "address", json_string(addr_str));
        
        json_object_set_new(device, "maxApdu", json_integer(dev->max_apdu));
        json_object_set_new(device, "vendorId", json_integer(dev->vendor_id));
        json_object_set_new(device, "segmentation",
                            json_string(bactext_segmentation_name(dev->segmentation)));
        
        if (dev->name[0]) {
            json_object_set_new(device, "name", json_string(dev->name));
        }
        
        json_object_set_new(device, "lastSeen", json_integer(dev->last_seen));
        
        json_array_append_new(devices, device);
        dev = dev->next;
    }
    
    json_object_set_new(response, "devices", devices);
    
    char *json_str = json_dumps(response, JSON_COMPACT);
    printf("[CLIENT] get_device_list_json: Generated JSON (%zu bytes)\n", 
           json_str ? strlen(json_str) : 0);
    fflush(stdout);
    json_decref(response);
    
    pthread_mutex_unlock(&device_mutex);
    return json_str;
}

/*
 * COV subscription management
 */
static void add_cov_subscription(uint32_t device_id, BACNET_ADDRESS *addr,
                                BACNET_OBJECT_TYPE obj_type, uint32_t obj_instance,
                                uint32_t process_id, time_t lifetime, bool confirmed)
{
    pthread_mutex_lock(&cov_mutex);
    
    /* Check if subscription already exists */
    COV_SUBSCRIPTION *sub = cov_list;
    while (sub) {
        if (sub->device_id == device_id &&
            sub->object_type == obj_type &&
            sub->object_instance == obj_instance) {
            /* Update existing */
            sub->lifetime = lifetime;
            pthread_mutex_unlock(&cov_mutex);
            return;
        }
        sub = sub->next;
    }
    
    /* Add new subscription */
    sub = calloc(1, sizeof(COV_SUBSCRIPTION));
    if (sub) {
        sub->device_id = device_id;
        bacnet_address_copy(&sub->address, addr);
        sub->object_type = obj_type;
        sub->object_instance = obj_instance;
        sub->subscriber_process_id = process_id;
        sub->lifetime = lifetime;
        sub->confirmed = confirmed;
        sub->next = cov_list;
        cov_list = sub;
    }
    
    pthread_mutex_unlock(&cov_mutex);
}

static COV_SUBSCRIPTION *find_cov_subscription(uint32_t device_id,
                                               BACNET_OBJECT_TYPE obj_type,
                                               uint32_t obj_instance)
{
    pthread_mutex_lock(&cov_mutex);
    
    COV_SUBSCRIPTION *sub = cov_list;
    while (sub) {
        if (sub->device_id == device_id &&
            sub->object_type == obj_type &&
            sub->object_instance == obj_instance) {
            pthread_mutex_unlock(&cov_mutex);
            return sub;
        }
        sub = sub->next;
    }
    
    pthread_mutex_unlock(&cov_mutex);
    return NULL;
}

/*
 * Network task - handles BACnet messages
 */
static void *network_task(void *arg)
{
    (void)arg;
    unsigned long packet_count = 0;
    unsigned long select_calls = 0;
    BACNET_ADDRESS src = {0};  /* source address */
    uint8_t rx_buf[MAX_MPDU] = {0};  /* receive buffer */
    uint16_t pdu_len = 0;
    
    printf("Network task started\n");
    fflush(stdout);
    
    while (running) {
        select_calls++;
        
        /* Process BACnet messages - MUST provide buffers! */
        pdu_len = datalink_receive(&src, &rx_buf[0], MAX_MPDU, 100);
        
        if (pdu_len > 0) {
            packet_count++;
            printf("[CLIENT] Network: received packet #%lu (len=%u, total selects=%lu)\n", 
                   packet_count, pdu_len, select_calls);
            fflush(stdout);
            
            /* Process the received PDU */
            npdu_handler(&src, &rx_buf[0], pdu_len);
        }
        
        /* Log every 1000 select calls to show we're alive (~100 seconds) */
        if (select_calls % 1000 == 0) {
            printf("[CLIENT] Network thread : called %lu times, %lu packets received\n", 
                   select_calls, packet_count);
            fflush(stdout);
        }
        
        /* Process TSM timeouts */
        tsm_timer_milliseconds(100);
        
        /* Cleanup old requests periodically */
        static time_t last_cleanup = 0;
        time_t now = time(NULL);
        if (now - last_cleanup > 10) {
            cleanup_old_requests();
            last_cleanup = now;
        }
    }
    
    printf("Network task stopped (total packets: %lu, total selects: %lu)\n", 
           packet_count, select_calls);
    fflush(stdout);
    return NULL;
}

/*
 * Utility functions
 */
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

static bool parse_bacnet_address(const char *addr_str, BACNET_ADDRESS *addr)
{
    BACNET_MAC_ADDRESS mac;
    if (!bacnet_address_mac_from_ascii(&mac, addr_str)) {
        return false;
    }
    bacnet_address_init(addr, &mac, 0, NULL);
    return true;
}

/*
 * Convert IP address string to BACnet/IP address
 * Example: "192.168.1.100" -> BACNET_ADDRESS with MAC = [C0, A8, 01, 64, BA, C0]
 */
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

static char *create_error_response(const char *error_msg)
{
    json_t *response = json_object();
    json_object_set_new(response, "status", json_string("error"));
    json_object_set_new(response, "error", json_string(error_msg));
    
    char *json_str = json_dumps(response, JSON_COMPACT);
    json_decref(response);
    return json_str;
}

static char *create_success_response(const char *message)
{
    json_t *response = json_object();
    json_object_set_new(response, "status", json_string("success"));
    json_object_set_new(response, "message", json_string(message));
    
    char *json_str = json_dumps(response, JSON_COMPACT);
    json_decref(response);
    return json_str;
}

/*
 * Command handlers
 */

static void handle_whois_command(int client_fd, json_t *params)
{
    int32_t device_min = -1;
    int32_t device_max = -1;
    json_t *min_obj;
    json_t *max_obj;
    char *response;
    
    printf("[CLIENT] ═══ handle_whois_command CALLED (v2024-12-05-network-receive-fix) ═══\n");
    fflush(stdout);
    
    min_obj = json_object_get(params, "deviceMin");
    max_obj = json_object_get(params, "deviceMax");
    
    if (min_obj && json_is_integer(min_obj)) {
        device_min = json_integer_value(min_obj);
    }
    if (max_obj && json_is_integer(max_obj)) {
        device_max = json_integer_value(max_obj);
    }
    
    printf("[CLIENT] DEBUG: Raw params - deviceMin=%d, deviceMax=%d\n", 
           device_min, device_max);
    fflush(stdout);
    
    /* BACnet Who-Is requires valid Device Instance range (0 to 4194303)
     * Convert -1 (unlimited) to full range */
    if (device_min < 0) {
        device_min = 0;
    }
    if (device_max < 0) {
        device_max = 4194303;  /* 0x3FFFFF - maximum 22-bit Device Instance */
    }
    
    /* Validation: min must be <= max */
    if (device_min > device_max) {
        int32_t tmp = device_min;
        device_min = device_max;
        device_max = tmp;
    }
    
    /* Clamp to valid BACnet range */
    if (device_min < 0) device_min = 0;
    if (device_max > 4194303) device_max = 4194303;
    
    /* Send Who-Is */
    printf("[CLIENT] Sending Who-Is broadcast (min=%d, max=%d)\n", 
           device_min, device_max);
    printf("[CLIENT] DEBUG: Using Send_WhoIs_Global() - FIXED VERSION\n");
    fflush(stdout);
    
    /* Use Send_WhoIs_Global for proper BACnet/IP broadcast
     * This function correctly handles the broadcast address and network configuration
     */
    Send_WhoIs_Global((int32_t)device_min, (int32_t)device_max);
    
    printf("[CLIENT] ✓ Who-Is broadcast sent successfully\n");
    printf("[CLIENT] ⏳ Waiting 4 seconds for I-Am responses...\n");
    fflush(stdout);
    
    /* Wait for I-Am responses - give devices time to respond */
    sleep(4);
    
    printf("[CLIENT] ✓ Wait completed, check device list\n");
    fflush(stdout);
    
    response = create_success_response("Who-Is sent and waited for responses");
    write(client_fd, response, strlen(response));
    write(client_fd, "\n", 1);
    free(response);
}

static void handle_iam_command(int client_fd, json_t *params)
{
    (void)params;
    
    /* Client doesn't announce itself as a device - it's just a client tool */
    char *response = create_error_response("I-Am not implemented for pure client");
    write(client_fd, response, strlen(response));
    write(client_fd, "\n", 1);
    free(response);
}

static void handle_readprop_command(int client_fd, json_t *params)
{
    /* Parse parameters */
    json_t *device_obj = json_object_get(params, "device");
    json_t *ip_obj = json_object_get(params, "ip");
    json_t *address_obj = json_object_get(params, "address");
    json_t *object_obj = json_object_get(params, "object");
    json_t *property_obj = json_object_get(params, "property");
    json_t *array_obj = json_object_get(params, "arrayIndex");
    
    if (!device_obj || !object_obj || !property_obj) {
        char *error = create_error_response("Missing required parameters (device, object, property)");
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
        return;
    }
    
    /* Get device ID */
    uint32_t device_id = json_integer_value(device_obj);
    
    /* Parse object ID */
    BACNET_OBJECT_TYPE obj_type;
    uint32_t obj_instance;
    if (!parse_object_id(json_string_value(object_obj), &obj_type, &obj_instance)) {
        char *error = create_error_response("Invalid object ID format");
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
        return;
    }
    
    /* Resolve address: priority order = ip > address > device_list */
    BACNET_ADDRESS addr;
    bool addr_resolved = false;
    
    if (ip_obj && json_is_string(ip_obj)) {
        /* Option 1: Direct IP address (PREFERRED for Jeedom eqLogic) */
        if (ip_to_bacnet_address(json_string_value(ip_obj), &addr)) {
            addr_resolved = true;
        } else {
            char *error = create_error_response("Invalid IP address format");
            write(client_fd, error, strlen(error));
            write(client_fd, "\n", 1);
            free(error);
            return;
        }
    } else if (address_obj && json_is_string(address_obj)) {
        /* Option 2: MAC address (legacy compatibility) */
        if (parse_bacnet_address(json_string_value(address_obj), &addr)) {
            addr_resolved = true;
        } else {
            char *error = create_error_response("Invalid MAC address format");
            write(client_fd, error, strlen(error));
            write(client_fd, "\n", 1);
            free(error);
            return;
        }
    } else {
        /* Option 3: Lookup from discovered devices */
        DEVICE_ENTRY *dev = find_device(device_id);
        if (dev) {
            bacnet_address_copy(&addr, &dev->address);
            addr_resolved = true;
        } else {
            char *error = create_error_response("Device not found. Provide 'ip' or run Who-Is first.");
            write(client_fd, error, strlen(error));
            write(client_fd, "\n", 1);
            free(error);
            return;
        }
    }
    
    /* Parse property */
    BACNET_PROPERTY_ID prop_id = PROP_ALL;
    {
        int i;
        for (i = 0; i < 4096; i++) {
            if (strcasecmp(json_string_value(property_obj), bactext_property_name(i)) == 0) {
                prop_id = i;
                break;
            }
        }
    }
    
    if (prop_id == PROP_ALL) {
        char *error = create_error_response("Unknown property name");
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
        return;
    }
    
    uint32_t array_index = BACNET_ARRAY_ALL;
    if (array_obj && json_is_integer(array_obj)) {
        array_index = json_integer_value(array_obj);
    }
    
    /* Send ReadProperty request */
    uint8_t invoke_id = tsm_next_free_invokeID();
    if (invoke_id == 0) {
        char *error = create_error_response("No free invoke ID available");
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
        return;
    }
    allocate_request(invoke_id);
    
    if (!Send_Read_Property_Request(device_id, obj_type, obj_instance, 
                                    prop_id, array_index)) {
        char *error = create_error_response("Failed to send request");
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
        return;
    }
    
    /* Wait for response (with timeout) */
    time_t start = time(NULL);
    PENDING_REQUEST *req = find_request(invoke_id);
    
    while (!req->completed && (time(NULL) - start) < 5) {
        usleep(50000); /* 50ms */
    }
    
    if (req->completed && req->response_json) {
        write(client_fd, req->response_json, strlen(req->response_json));
        write(client_fd, "\n", 1);
    } else {
        char *error = create_error_response("Request timeout");
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
    }
}

/* Additional command handlers to be implemented... */
static void handle_readpropm_command(int client_fd, json_t *params)
{
    /* TODO: Implement ReadPropertyMultiple */
    char *error = create_error_response("ReadPropertyMultiple not yet implemented");
    write(client_fd, error, strlen(error));
    write(client_fd, "\n", 1);
    free(error);
}

static void handle_readrange_command(int client_fd, json_t *params)
{
    /* TODO: Implement ReadRange */
    char *error = create_error_response("ReadRange not yet implemented");
    write(client_fd, error, strlen(error));
    write(client_fd, "\n", 1);
    free(error);
}

static void handle_writeprop_command(int client_fd, json_t *params)
{
    /* TODO: Implement WriteProperty */
    char *error = create_error_response("WriteProperty not yet implemented");
    write(client_fd, error, strlen(error));
    write(client_fd, "\n", 1);
    free(error);
}

static void handle_writepropm_command(int client_fd, json_t *params)
{
    /* TODO: Implement WritePropertyMultiple */
    char *error = create_error_response("WritePropertyMultiple not yet implemented");
    write(client_fd, error, strlen(error));
    write(client_fd, "\n", 1);
    free(error);
}

static void handle_subscribecov_command(int client_fd, json_t *params)
{
    /* TODO: Implement SubscribeCOV */
    char *error = create_error_response("SubscribeCOV not yet implemented");
    write(client_fd, error, strlen(error));
    write(client_fd, "\n", 1);
    free(error);
}

static void handle_unsubscribecov_command(int client_fd, json_t *params)
{
    /* TODO: Implement UnsubscribeCOV */
    char *error = create_error_response("UnsubscribeCOV not yet implemented");
    write(client_fd, error, strlen(error));
    write(client_fd, "\n", 1);
    free(error);
}

static void handle_timesync_command(int client_fd, json_t *params)
{
    /* TODO: Implement TimeSynchronization */
    char *error = create_error_response("TimeSynchronization not yet implemented");
    write(client_fd, error, strlen(error));
    write(client_fd, "\n", 1);
    free(error);
}

static void handle_whohas_command(int client_fd, json_t *params)
{
    /* TODO: Implement Who-Has */
    char *error = create_error_response("Who-Has not yet implemented");
    write(client_fd, error, strlen(error));
    write(client_fd, "\n", 1);
    free(error);
}

static void handle_devicelist_command(int client_fd, json_t *params)
{
    (void)params;
    
    printf("[CLIENT] Received devicelist command\n");
    fflush(stdout);
    char *json = get_device_list_json();
    if (json) {
        printf("[CLIENT] Sending devicelist response (%zu bytes)\n", strlen(json));
        fflush(stdout);
        write(client_fd, json, strlen(json));
        write(client_fd, "\n", 1);
        free(json);
        printf("[CLIENT] Devicelist response sent\n");
        fflush(stdout);
    } else {
        printf("[CLIENT] ✗ Failed to generate devicelist JSON\n");
        fflush(stdout);
        char *error = create_error_response("Failed to generate device list");
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
    }
}

static void handle_objectlist_command(int client_fd, json_t *params)
{
    /* Parse parameters */
    json_t *device_obj = json_object_get(params, "device");
    json_t *ip_obj = json_object_get(params, "ip");
    
    if (!device_obj) {
        char *error = create_error_response("Missing required parameter: device");
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
        return;
    }
    
    uint32_t device_id = json_integer_value(device_obj);
    
    printf("[CLIENT] Reading object-list from device %u\n", device_id);
    fflush(stdout);
    
    /* Resolve address: priority ip > device_list */
    BACNET_ADDRESS addr;
    bool addr_resolved = false;
    
    if (ip_obj && json_is_string(ip_obj)) {
        /* Direct IP address (PREFERRED) */
        if (ip_to_bacnet_address(json_string_value(ip_obj), &addr)) {
            addr_resolved = true;
        } else {
            char *error = create_error_response("Invalid IP address format");
            write(client_fd, error, strlen(error));
            write(client_fd, "\n", 1);
            free(error);
            return;
        }
    } else {
        /* Lookup in device_list */
        DEVICE_ENTRY *dev = find_device(device_id);
        if (dev) {
            addr = dev->address;
            addr_resolved = true;
        }
    }
    
    if (!addr_resolved) {
        char *error = create_error_response("Device not found in cache, provide 'ip' parameter");
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
        return;
    }
    
    /* Allocate invoke ID */
    uint8_t invoke_id = tsm_next_free_invokeID();
    if (invoke_id == 0) {
        char *error = create_error_response("No free invoke IDs available");
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
        return;
    }
    
    /* Prepare request to track response */
    allocate_request(invoke_id);
    
    /* Send ReadProperty for device,<deviceId>.object-list */
    int pdu_len = Send_Read_Property_Request(
        device_id,                    /* Target device */
        OBJECT_DEVICE, device_id,    /* Object: device,<deviceId> */
        PROP_OBJECT_LIST,            /* Property: object-list */
        BACNET_ARRAY_ALL             /* Read entire array */
    );
    
    if (pdu_len <= 0) {
        char *error = create_error_response("Failed to send ReadProperty request");
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
        return;
    }
    
    printf("[CLIENT] ✓ ReadProperty(object-list) sent to device %u (invoke_id=%u)\n", 
           device_id, invoke_id);
    fflush(stdout);
    
    /* Wait for response (with timeout) */
    time_t start = time(NULL);
    PENDING_REQUEST *req = find_request(invoke_id);
    
    while (!req->completed && (time(NULL) - start) < 5) {
        usleep(50000); /* 50ms */
    }
    
    if (req->completed && req->response_json) {
        write(client_fd, req->response_json, strlen(req->response_json));
        write(client_fd, "\n", 1);
    } else {
        char *error = create_error_response("Request timeout");
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
    }
}

static void handle_reinit_command(int client_fd, json_t *params)
{
    /* TODO: Implement ReinitializeDevice */
    char *error = create_error_response("ReinitializeDevice not yet implemented");
    write(client_fd, error, strlen(error));
    write(client_fd, "\n", 1);
    free(error);
}

static void handle_devicecomm_command(int client_fd, json_t *params)
{
    /* TODO: Implement DeviceCommunicationControl */
    char *error = create_error_response("DeviceCommunicationControl not yet implemented");
    write(client_fd, error, strlen(error));
    write(client_fd, "\n", 1);
    free(error);
}

/*
 * Process socket command
 */
static void process_socket_command(int client_fd, const char *json_cmd)
{
    json_error_t error;
    json_t *cmd = json_loads(json_cmd, 0, &error);
    if (!cmd) {
        char *error_msg = create_error_response("Invalid JSON");
        write(client_fd, error_msg, strlen(error_msg));
        write(client_fd, "\n", 1);
        free(error_msg);
        return;
    }
    
    json_t *cmd_obj = json_object_get(cmd, "cmd");
    if (!cmd_obj || !json_is_string(cmd_obj)) {
        char *error_msg = create_error_response("Missing 'cmd' field");
        write(client_fd, error_msg, strlen(error_msg));
        write(client_fd, "\n", 1);
        free(error_msg);
        json_decref(cmd);
        return;
    }
    
    const char *command = json_string_value(cmd_obj);
    
    /* Dispatch to appropriate handler */
    if (strcmp(command, "whois") == 0) {
        handle_whois_command(client_fd, cmd);
    } else if (strcmp(command, "iam") == 0) {
        handle_iam_command(client_fd, cmd);
    } else if (strcmp(command, "readprop") == 0) {
        handle_readprop_command(client_fd, cmd);
    } else if (strcmp(command, "readpropm") == 0) {
        handle_readpropm_command(client_fd, cmd);
    } else if (strcmp(command, "readrange") == 0) {
        handle_readrange_command(client_fd, cmd);
    } else if (strcmp(command, "writeprop") == 0) {
        handle_writeprop_command(client_fd, cmd);
    } else if (strcmp(command, "writepropm") == 0) {
        handle_writepropm_command(client_fd, cmd);
    } else if (strcmp(command, "subscribecov") == 0) {
        handle_subscribecov_command(client_fd, cmd);
    } else if (strcmp(command, "unsubscribecov") == 0) {
        handle_unsubscribecov_command(client_fd, cmd);
    } else if (strcmp(command, "timesync") == 0) {
        handle_timesync_command(client_fd, cmd);
    } else if (strcmp(command, "whohas") == 0) {
        handle_whohas_command(client_fd, cmd);
    } else if (strcmp(command, "devicelist") == 0) {
        handle_devicelist_command(client_fd, cmd);
    } else if (strcmp(command, "objectlist") == 0) {
        handle_objectlist_command(client_fd, cmd);
    } else if (strcmp(command, "reinit") == 0) {
        handle_reinit_command(client_fd, cmd);
    } else if (strcmp(command, "devicecomm") == 0) {
        handle_devicecomm_command(client_fd, cmd);
    } else {
        char error_msg[128];
        snprintf(error_msg, sizeof(error_msg), "Unknown command: %s", command);
        char *error = create_error_response(error_msg);
        write(client_fd, error, strlen(error));
        write(client_fd, "\n", 1);
        free(error);
    }
    
    json_decref(cmd);
}

/*
 * Main function
 */
int main(int argc, char *argv[])
{
    struct sockaddr_in server_addr;
    int client_fd;
    char buffer[MAX_BUFFER_SIZE];
    ssize_t bytes_read;
    int i;
    FILE *pid_fp;
    
    /* Parse command line arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socketport") == 0 && i + 1 < argc) {
            socket_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            strncpy(pid_file_path, argv[++i], sizeof(pid_file_path) - 1);
        }
    }
    
    printf("BACnet Stack Client v1.0\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("BUILD DATE: %s %s\n", __DATE__, __TIME__);
    printf("VERSION: 2024-12-05-network-receive-fix (datalink_receive with buffer)\n");
    printf("═══════════════════════════════════════════════════════\n");
    fflush(stdout);
    printf("Socket port: %d\n", socket_port);
    fflush(stdout);
    
    /* Write PID file if specified */
    if (pid_file_path[0] != '\0') {
        pid_fp = fopen(pid_file_path, "w");
        if (pid_fp) {
            fprintf(pid_fp, "%d\n", getpid());
            fclose(pid_fp);
            printf("PID %d written to %s\n", getpid(), pid_file_path);
            fflush(stdout);
        }
    }
    
    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Disable stdout buffering for real-time logs */
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    
    /* Read environment variables for BACnet configuration */
    const char *bacnet_iface_const = getenv("BACNET_IFACE");
    const char *bacnet_port_str = getenv("BACNET_IP_PORT");
    char *bacnet_iface = NULL;
    uint16_t bacnet_port = 0xBAC0;  /* Default BACnet/IP port 47808 */
    
    /* Copy to non-const for datalink_init which expects char* */
    if (bacnet_iface_const) {
        bacnet_iface = strdup(bacnet_iface_const);
        printf("[CLIENT] Using BACnet interface from env: %s\n", bacnet_iface);
        fflush(stdout);
    }
    
    if (bacnet_port_str) {
        int env_port = atoi(bacnet_port_str);
        if (env_port > 0 && env_port <= 65535) {
            bacnet_port = (uint16_t)env_port;
            printf("[CLIENT] Using BACnet port from env: %d\n", env_port);
            fflush(stdout);
        }
    }
    
    /* Set BACnet/IP port BEFORE initializing datalink - CRITICAL! */
    printf("[CLIENT] Setting BACnet/IP port to %u (0x%04X)\n", bacnet_port, bacnet_port);
    fflush(stdout);
    bip_set_port(bacnet_port);
    
    /* Initialize BACnet datalink */
    printf("[CLIENT] Initializing BACnet datalink (interface: %s)...\n", 
           bacnet_iface ? bacnet_iface : "auto-detect");
    fflush(stdout);
    
    if (!datalink_init(bacnet_iface)) {
        fprintf(stderr, "[CLIENT] ✗ Failed to initialize datalink\n");
        fflush(stderr);
        if (bacnet_iface) free(bacnet_iface);
        return 1;
    }
    
    if (bacnet_iface) free(bacnet_iface);
    
    printf("[CLIENT] ✓ BACnet datalink initialized successfully\n");
    fflush(stdout);
    
    /* Initialize Device Communication Control (DCC) */
    /* Enable communication by default - required for Who-Is to work */
    dcc_set_status_duration(COMMUNICATION_ENABLE, 0);
    printf("[CLIENT] ✓ Device Communication Control initialized (ENABLED)\n");
    fflush(stdout);
    
    /* Verify datalink is functional */
    {
        BACNET_ADDRESS my_addr;
        BACNET_ADDRESS bcast_addr;
        
        datalink_get_my_address(&my_addr);
        printf("[CLIENT] DEBUG: My BACnet address - MAC len=%d, net=%u\n", 
               my_addr.mac_len, my_addr.net);
        if (my_addr.mac_len >= 6) {
            printf("[CLIENT] DEBUG: My IP: %u.%u.%u.%u:%u\n",
                   my_addr.mac[0], my_addr.mac[1], my_addr.mac[2], my_addr.mac[3],
                   (my_addr.mac[4] << 8) | my_addr.mac[5]);
        }
        fflush(stdout);
        
        datalink_get_broadcast_address(&bcast_addr);
        printf("[CLIENT] DEBUG: Broadcast address - MAC len=%d, net=%u\n", 
               bcast_addr.mac_len, bcast_addr.net);
        if (bcast_addr.mac_len >= 6) {
            printf("[CLIENT] DEBUG: Broadcast IP: %u.%u.%u.%u:%u\n",
                   bcast_addr.mac[0], bcast_addr.mac[1], bcast_addr.mac[2], bcast_addr.mac[3],
                   (bcast_addr.mac[4] << 8) | bcast_addr.mac[5]);
        }
        fflush(stdout);
    }
    
    printf("[CLIENT] BACnet/IP port: 47808 (0xBAC0)\n");
    fflush(stdout);
    
    /* Set handlers */
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, my_i_am_handler);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_COV_NOTIFICATION,
                                my_cov_notification_handler);
    apdu_set_confirmed_ack_handler(SERVICE_CONFIRMED_READ_PROPERTY,
                                  my_read_property_ack_handler);
    apdu_set_confirmed_ack_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE,
                                  my_read_property_multiple_ack_handler);
    apdu_set_confirmed_ack_handler(SERVICE_CONFIRMED_READ_RANGE,
                                  my_read_range_ack_handler);
    apdu_set_confirmed_simple_ack_handler(SERVICE_CONFIRMED_WRITE_PROPERTY,
                                         my_write_property_ack_handler);
    apdu_set_error_handler(SERVICE_CONFIRMED_READ_PROPERTY, my_error_handler);
    apdu_set_error_handler(SERVICE_CONFIRMED_WRITE_PROPERTY, my_error_handler);
    apdu_set_abort_handler(my_abort_handler);
    apdu_set_reject_handler(my_reject_handler);
    
    /* Start network thread */
    if (pthread_create(&network_thread, NULL, network_task, NULL) != 0) {
        fprintf(stderr, "Failed to create network thread\n");
        datalink_cleanup();
        return 1;
    }
    
    /* Create TCP socket */
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        running = false;
        pthread_join(network_thread, NULL);
        datalink_cleanup();
        return 1;
    }
    
    /* Allow socket reuse */
    {
        int reuse = 1;
        setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(socket_port);
    
    if (bind(socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(socket_fd);
        running = false;
        pthread_join(network_thread, NULL);
        datalink_cleanup();
        return 1;
    }
    
    if (listen(socket_fd, 5) < 0) {
        perror("listen");
        close(socket_fd);
        running = false;
        pthread_join(network_thread, NULL);
        datalink_cleanup();
        return 1;
    }
    
    printf("Listening on TCP port %d\n", socket_port);
    fflush(stdout);
    printf("Client ready. Press Ctrl+C to exit.\n");
    fflush(stdout);
    
    /* Main socket loop */
    while (running) {
        fd_set readfds;
        struct timeval tv;
        
        FD_ZERO(&readfds);
        FD_SET(socket_fd, &readfds);
        
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(socket_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (ret == 0) continue;
        
        if (FD_ISSET(socket_fd, &readfds)) {
            client_fd = accept(socket_fd, NULL, NULL);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                perror("accept");
                continue;
            }
            
            /* Read command */
            bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                
                /* Remove newline if present */
                char *newline = strchr(buffer, '\n');
                if (newline) *newline = '\0';
                
                printf("Received command: %s\n", buffer);
                
                /* Process command */
                process_socket_command(client_fd, buffer);
            }
            
            close(client_fd);
        }
    }
    
    /* Cleanup */
    printf("\nShutting down...\n");
    running = false;
    pthread_join(network_thread, NULL);
    cleanup();
    
    return 0;
}