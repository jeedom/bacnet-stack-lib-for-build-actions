/**
 * @file
 * @brief BACnet server (Jeedom) + socket JSON runtime config
 * Basé sur server-mini de BACnet Stack, démarre vide (0 objets)
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

#include <jansson.h>

/* BACnet Stack includes */
#include "bacnet/apdu.h"
#include "bacnet/bacdcode.h"
#include "bacnet/bacdef.h"
#include "bacnet/bactext.h"
#include "bacnet/bacapp.h"
#include "bacnet/bacstr.h"
#include "bacnet/wp.h"
#include "bacnet/rp.h"
#include "bacnet/datetime.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/basic/object/ai.h"
#include "bacnet/basic/object/ao.h"
#include "bacnet/basic/object/av.h"
#include "bacnet/basic/object/bi.h"
#include "bacnet/basic/object/bo.h"
#include "bacnet/basic/object/bv.h"
#include "bacnet/basic/object/ms-input.h"
#include "bacnet/basic/object/mso.h"
#include "bacnet/basic/object/msv.h"
#include "bacnet/basic/object/schedule.h"
#include "bacnet/basic/object/trendlog.h"
#include "schedule_override.h"
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/services.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/datalink/dlenv.h"
#include "bacnet/dcc.h"
#include "bacnet/getevent.h"
#include "bacnet/iam.h"
#include "bacnet/npdu.h"
#include "bacnet/version.h"
#include "bacnet/basic/sys/mstimer.h"
#include "bacnet/basic/tsm/tsm.h"

extern SCHEDULE_DESCR Schedule_Descr[MAX_SCHEDULES];

static void Init_Schedules(void);

static int save_current_config(void);

static void Schedule_Init_Empty(void)
{
    printf("Schedule_Init_Empty: Schedules will be created from JSON only\n");
}

static void Trend_Log_Init_Empty(void)
{
    unsigned int i;
    BACNET_WRITE_PROPERTY_DATA wp_data;
    BACNET_APPLICATION_DATA_VALUE value;
    int len;
    
    printf("Trend_Log_Init_Empty: Disabling all %d Trendlogs...\n", MAX_TREND_LOGS);
    
    /* Désactiver TOUS les Trendlogs */
    for (i = 0; i < MAX_TREND_LOGS; i++) {
        if (!Trend_Log_Valid_Instance(i)) {
            continue;
        }
        
        /* ENABLE = FALSE */
        memset(&wp_data, 0, sizeof(wp_data));
        memset(&value, 0, sizeof(value));
        
        value.tag = BACNET_APPLICATION_TAG_BOOLEAN;
        value.type.Boolean = false;
        
        len = bacapp_encode_application_data(wp_data.application_data, &value);
        
        wp_data.object_type = OBJECT_TRENDLOG;
        wp_data.object_instance = i;
        wp_data.object_property = PROP_ENABLE;
        wp_data.array_index = BACNET_ARRAY_ALL;
        wp_data.application_data_len = len;
        
        Trend_Log_Write_Property(&wp_data);
        
        /* RECORD_COUNT = 0 (effacer buffer) */
        memset(&value, 0, sizeof(value));
        value.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
        value.type.Unsigned_Int = 0;
        
        len = bacapp_encode_application_data(wp_data.application_data, &value);
        wp_data.object_property = PROP_RECORD_COUNT;
        wp_data.application_data_len = len;
        
        Trend_Log_Write_Property(&wp_data);
    }
    
    printf("Trend_Log_Init_Empty: All Trendlogs disabled and cleared.\n");
}

static void print_timestamp_log(const char *message)
{
    time_t now;
    struct tm *local_time;
    char timestamp[64];
    
    time(&now);
    local_time = localtime(&now);
    
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", local_time);
    printf("[%s] %s\n", timestamp, message);
    fflush(stdout);
}

/* Handler personnalisé pour WriteProperty : sauvegarde après chaque écriture */
void my_handler_write_property(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_DATA *service_data)
{
    handler_write_property(service_request, service_len, src, service_data);
    save_current_config();
}

/* Buffers */
static uint8_t Rx_Buf[MAX_MPDU] = { 0 };
static struct mstimer BACNET_Task_Timer;
static struct mstimer Schedule_PV_Timer;
static struct mstimer BACNET_TSM_Timer;
static struct mstimer BACNET_Address_Timer;
static struct mstimer Config_Save_Timer;

/* Helper function to set object name */
static bool set_object_name(BACNET_OBJECT_TYPE obj_type, uint32_t instance, const char *name)
{
    bool status = false;

    if (!name) return false;

    printf("Setting name for object type=%d instance=%u to '%s'\n", obj_type, instance, name);

    switch (obj_type) {
        case OBJECT_ANALOG_INPUT:
            status = Analog_Input_Name_Set(instance, name);
            break;
        case OBJECT_ANALOG_OUTPUT:
            status = Analog_Output_Name_Set(instance, name);
            break;
        case OBJECT_ANALOG_VALUE:
            status = Analog_Value_Name_Set(instance, name);
            break;
        case OBJECT_BINARY_INPUT:
            status = Binary_Input_Name_Set(instance, name);
            break;
        case OBJECT_BINARY_OUTPUT:
            status = Binary_Output_Name_Set(instance, name);
            break;
        case OBJECT_BINARY_VALUE:
            status = Binary_Value_Name_Set(instance, name);
            break;
        case OBJECT_MULTI_STATE_INPUT:
            status = Multistate_Input_Name_Set(instance, name);
            break;
        case OBJECT_MULTI_STATE_OUTPUT:
            status = Multistate_Output_Name_Set(instance, name);
            break;
        case OBJECT_MULTI_STATE_VALUE:
            status = Multistate_Value_Name_Set(instance, name);
            break;
        default:
            break;
    }

    printf("Name set result: %s\n", status ? "SUCCESS" : "FAILED");
    return status;
}


static bool create_trendlog(uint32_t instance, const char *name, 
                           BACNET_OBJECT_TYPE source_type, 
                           uint32_t source_instance,
                           uint32_t log_interval,
                           uint32_t buffer_size,
                           bool enable)
{
    BACNET_WRITE_PROPERTY_DATA wp_data;
    BACNET_APPLICATION_DATA_VALUE value;
    BACNET_DEVICE_OBJECT_PROPERTY_REFERENCE source_ref;
    int len;
    bool success;
    
    (void)buffer_size;  /* BUFFER_SIZE est read-only */
    
    success = true;
    
    /* Vérifier l'instance */
    if (!Trend_Log_Valid_Instance(instance)) {
        fprintf(stderr, "ERROR: Trendlog instance %u not valid\n", instance);
        return false;
    }
    
    printf("═══════════════════════════════════════════════════════\n");
    printf("Configuring Trendlog %u: %s\n", instance, name ? name : "(no name)");
    printf("═══════════════════════════════════════════════════════\n");
    
    /* Initialiser les structures */
    memset(&wp_data, 0, sizeof(wp_data));
    memset(&value, 0, sizeof(value));
    memset(&source_ref, 0, sizeof(source_ref));
    
    /* Configuration de base du Write Property */
    wp_data.object_type = OBJECT_TRENDLOG;
    wp_data.object_instance = instance;
    wp_data.array_index = BACNET_ARRAY_ALL;
    
    /* ========================================
     * 0. DÉSACTIVER D'ABORD (CRITIQUE)
     * ======================================== */
    memset(&value, 0, sizeof(value));
    value.tag = BACNET_APPLICATION_TAG_BOOLEAN;
    value.type.Boolean = false;
    
    len = bacapp_encode_application_data(wp_data.application_data, &value);
    wp_data.object_property = PROP_ENABLE;
    wp_data.application_data_len = len;
    
    if (!Trend_Log_Write_Property(&wp_data)) {
        fprintf(stderr, "  ⚠️  Warning: Failed to disable first (continuing anyway)\n");
    } else {
        printf("  ✓ Disabled first\n");
    }
    
    /* ========================================
     * 1. EFFACER BUFFER D'ABORD
     * ======================================== */
    memset(&value, 0, sizeof(value));
    value.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
    value.type.Unsigned_Int = 0;
    
    len = bacapp_encode_application_data(wp_data.application_data, &value);
    wp_data.object_property = PROP_RECORD_COUNT;
    wp_data.application_data_len = len;
    
    if (!Trend_Log_Write_Property(&wp_data)) {
        fprintf(stderr, "  ✗ Failed to clear RECORD_COUNT\n");
    } else {
        printf("  ✓ Buffer cleared (RECORD_COUNT = 0)\n");
    }
    
    /* ========================================
     * 2. LOG_DEVICE_OBJECT_PROPERTY
     * ======================================== */
    source_ref.objectIdentifier.type = source_type;
    source_ref.objectIdentifier.instance = source_instance;
    source_ref.propertyIdentifier = PROP_PRESENT_VALUE;
    source_ref.arrayIndex = BACNET_ARRAY_ALL;
    source_ref.deviceIdentifier.type = OBJECT_DEVICE;
    source_ref.deviceIdentifier.instance = Device_Object_Instance_Number();
    
    /* Encoder directement dans wp_data.application_data */
    len = bacapp_encode_device_obj_property_ref(wp_data.application_data, &source_ref);
    if (len <= 0) {
        fprintf(stderr, "  ✗ Failed to encode LOG_DEVICE_OBJECT_PROPERTY\n");
        return false;
    }
    
    wp_data.object_property = PROP_LOG_DEVICE_OBJECT_PROPERTY;
    wp_data.application_data_len = len;
    
    if (!Trend_Log_Write_Property(&wp_data)) {
        fprintf(stderr, "  ✗ Failed to set LOG_DEVICE_OBJECT_PROPERTY\n");
        fprintf(stderr, "    Error: class=%d code=%d\n", 
                wp_data.error_class, wp_data.error_code);
        success = false;
    } else {
        printf("  ✓ Linked to: %s[%u].PRESENT_VALUE\n", 
               bactext_object_type_name(source_type), source_instance);
    }
    
    /* ========================================
     * 3. LOGGING_TYPE = POLLED
     * ======================================== */
    memset(&value, 0, sizeof(value));
    value.tag = BACNET_APPLICATION_TAG_ENUMERATED;
    value.type.Enumerated = LOGGING_TYPE_POLLED;
    
    len = bacapp_encode_application_data(wp_data.application_data, &value);
    wp_data.object_property = PROP_LOGGING_TYPE;
    wp_data.application_data_len = len;
    
    if (!Trend_Log_Write_Property(&wp_data)) {
        fprintf(stderr, "  ✗ Failed to set LOGGING_TYPE\n");
        success = false;
    } else {
        printf("  ✓ Logging Type: POLLED\n");
    }
    
    /* ========================================
     * 4. LOG_INTERVAL (en centisecondes)
     * ======================================== */
    if (log_interval > 0) {
        memset(&value, 0, sizeof(value));
        value.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
        value.type.Unsigned_Int = log_interval * 100;  /* Secondes -> centisecondes */
        
        len = bacapp_encode_application_data(wp_data.application_data, &value);
        wp_data.object_property = PROP_LOG_INTERVAL;
        wp_data.application_data_len = len;
        
        if (!Trend_Log_Write_Property(&wp_data)) {
            fprintf(stderr, "  ✗ Failed to set LOG_INTERVAL\n");
            success = false;
        } else {
            printf("  ✓ Log Interval: %u seconds (%u cs)\n", 
                   log_interval, log_interval * 100);
        }
    }
    
    /* ========================================
     * 5. ALIGN_INTERVALS = TRUE
     * ======================================== */
    memset(&value, 0, sizeof(value));
    value.tag = BACNET_APPLICATION_TAG_BOOLEAN;
    value.type.Boolean = true;
    
    len = bacapp_encode_application_data(wp_data.application_data, &value);
    wp_data.object_property = PROP_ALIGN_INTERVALS;
    wp_data.application_data_len = len;
    
    if (!Trend_Log_Write_Property(&wp_data)) {
        fprintf(stderr, "  ✗ Failed to set ALIGN_INTERVALS\n");
        success = false;
    } else {
        printf("  ✓ Align Intervals: YES\n");
    }
    
    /* ========================================
     * 6. STOP_WHEN_FULL = FALSE (circulaire)
     * ======================================== */
    memset(&value, 0, sizeof(value));
    value.tag = BACNET_APPLICATION_TAG_BOOLEAN;
    value.type.Boolean = false;
    
    len = bacapp_encode_application_data(wp_data.application_data, &value);
    wp_data.object_property = PROP_STOP_WHEN_FULL;
    wp_data.application_data_len = len;
    
    if (!Trend_Log_Write_Property(&wp_data)) {
        fprintf(stderr, "  ✗ Failed to set STOP_WHEN_FULL\n");
        success = false;
    } else {
        printf("  ✓ Stop When Full: NO (circular)\n");
    }
    
    /* ========================================
     * 7. ENABLE (EN DERNIER)
     * ======================================== */
    memset(&value, 0, sizeof(value));
    value.tag = BACNET_APPLICATION_TAG_BOOLEAN;
    value.type.Boolean = enable;
    
    len = bacapp_encode_application_data(wp_data.application_data, &value);
    wp_data.object_property = PROP_ENABLE;
    wp_data.application_data_len = len;
    
    if (!Trend_Log_Write_Property(&wp_data)) {
        fprintf(stderr, "  ✗ Failed to set ENABLE\n");
        success = false;
    } else {
        printf("  ✓ Enabled: %s\n", enable ? "YES ✓" : "NO");
    }
    
    /* ========================================
     * Résumé
     * ======================================== */
    if (success) {
        printf("═══════════════════════════════════════════════════════\n");
        printf("✓ Trendlog %u configured successfully\n", instance);
        printf("  Ready to log data from %s[%u]\n", 
               bactext_object_type_name(source_type), source_instance);
        printf("═══════════════════════════════════════════════════════\n\n");
    } else {
        printf("═══════════════════════════════════════════════════════\n");
        printf("✗ Trendlog %u configuration FAILED\n", instance);
        printf("═══════════════════════════════════════════════════════\n\n");
    }
    
    return success;
}


/* Socket control */
static int g_socket_port = 55031;
static int g_listen_fd = -1;
static int g_client_fd = -1;
static char g_cmd_buf[8192];
static size_t g_cmd_len = 0;
static char g_pidfile[256] = {0};
static char g_config_file[512] = {0};

/* Signal handling */
static volatile sig_atomic_t g_shutdown = 0;
static void sig_handler(int sig) {
    printf("Signal %d received, shutting down...\n", sig);
    fflush(stdout);
    g_shutdown = 1;
}

/* Custom Object Table */
static object_functions_t My_Object_Table[] = {
    /* Device object (required) */
    { OBJECT_DEVICE,
      NULL,
      Device_Count,
      Device_Index_To_Instance,
      Device_Valid_Object_Instance_Number,
      Device_Object_Name,
      Device_Read_Property_Local,
      Device_Write_Property_Local,
      Device_Property_Lists,
      DeviceGetRRInfo,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },

    /* Analog Input */
    { OBJECT_ANALOG_INPUT,
      Analog_Input_Init,
      Analog_Input_Count,
      Analog_Input_Index_To_Instance,
      Analog_Input_Valid_Instance,
      Analog_Input_Object_Name,
      Analog_Input_Read_Property,
      Analog_Input_Write_Property,
      Analog_Input_Property_Lists,
      NULL, NULL,
      Analog_Input_Encode_Value_List,
      NULL, NULL, NULL, NULL, NULL,
      Analog_Input_Create,
      Analog_Input_Delete,
      NULL },

    /* Analog Output */
    { OBJECT_ANALOG_OUTPUT,
      Analog_Output_Init,
      Analog_Output_Count,
      Analog_Output_Index_To_Instance,
      Analog_Output_Valid_Instance,
      Analog_Output_Object_Name,
      Analog_Output_Read_Property,
      Analog_Output_Write_Property,
      Analog_Output_Property_Lists,
      NULL, NULL,
      Analog_Output_Encode_Value_List,
      NULL, NULL, NULL, NULL, NULL,
      Analog_Output_Create,
      Analog_Output_Delete,
      NULL },

    /* Analog Value */
    { OBJECT_ANALOG_VALUE,
      Analog_Value_Init,
      Analog_Value_Count,
      Analog_Value_Index_To_Instance,
      Analog_Value_Valid_Instance,
      Analog_Value_Object_Name,
      Analog_Value_Read_Property,
      Analog_Value_Write_Property,
      Analog_Value_Property_Lists,
      NULL, NULL,
      Analog_Value_Encode_Value_List,
      NULL, NULL, NULL, NULL, NULL,
      Analog_Value_Create,
      Analog_Value_Delete,
      NULL },

    /* Binary Input */
    { OBJECT_BINARY_INPUT,
      Binary_Input_Init,
      Binary_Input_Count,
      Binary_Input_Index_To_Instance,
      Binary_Input_Valid_Instance,
      Binary_Input_Object_Name,
      Binary_Input_Read_Property,
      Binary_Input_Write_Property,
      Binary_Input_Property_Lists,
      NULL, NULL,
      Binary_Input_Encode_Value_List,
      NULL, NULL, NULL, NULL, NULL,
      Binary_Input_Create,
      Binary_Input_Delete,
      NULL },

    /* Binary Output */
    { OBJECT_BINARY_OUTPUT,
      Binary_Output_Init,
      Binary_Output_Count,
      Binary_Output_Index_To_Instance,
      Binary_Output_Valid_Instance,
      Binary_Output_Object_Name,
      Binary_Output_Read_Property,
      Binary_Output_Write_Property,
      Binary_Output_Property_Lists,
      NULL, NULL,
      Binary_Output_Encode_Value_List,
      NULL, NULL, NULL, NULL, NULL,
      Binary_Output_Create,
      Binary_Output_Delete,
      NULL },

    /* Binary Value */
    { OBJECT_BINARY_VALUE,
      Binary_Value_Init,
      Binary_Value_Count,
      Binary_Value_Index_To_Instance,
      Binary_Value_Valid_Instance,
      Binary_Value_Object_Name,
      Binary_Value_Read_Property,
      Binary_Value_Write_Property,
      Binary_Value_Property_Lists,
      NULL, NULL,
      Binary_Value_Encode_Value_List,
      NULL, NULL, NULL, NULL, NULL,
      Binary_Value_Create,
      Binary_Value_Delete,
      NULL },

    /* Multi-State Input */
    { OBJECT_MULTI_STATE_INPUT,
      Multistate_Input_Init,
      Multistate_Input_Count,
      Multistate_Input_Index_To_Instance,
      Multistate_Input_Valid_Instance,
      Multistate_Input_Object_Name,
      Multistate_Input_Read_Property,
      Multistate_Input_Write_Property,
      Multistate_Input_Property_Lists,
      NULL, NULL,
      Multistate_Input_Encode_Value_List,
      NULL, NULL, NULL, NULL, NULL,
      Multistate_Input_Create,
      Multistate_Input_Delete,
      NULL },

    /* Multi-State Output */
    { OBJECT_MULTI_STATE_OUTPUT,
      Multistate_Output_Init,
      Multistate_Output_Count,
      Multistate_Output_Index_To_Instance,
      Multistate_Output_Valid_Instance,
      Multistate_Output_Object_Name,
      Multistate_Output_Read_Property,
      Multistate_Output_Write_Property,
      Multistate_Output_Property_Lists,
      NULL, NULL,
      Multistate_Output_Encode_Value_List,
      NULL, NULL, NULL, NULL, NULL,
      Multistate_Output_Create,
      Multistate_Output_Delete,
      NULL },

    /* Multi-State Value */
    { OBJECT_MULTI_STATE_VALUE,
      Multistate_Value_Init,
      Multistate_Value_Count,
      Multistate_Value_Index_To_Instance,
      Multistate_Value_Valid_Instance,
      Multistate_Value_Object_Name,
      Multistate_Value_Read_Property,
      Multistate_Value_Write_Property,
      Multistate_Value_Property_Lists,
      NULL, NULL,
      Multistate_Value_Encode_Value_List,
      NULL, NULL, NULL, NULL, NULL,
      Multistate_Value_Create,
      Multistate_Value_Delete,
      NULL },

    /* Schedule - utilise l'implémentation standard de la librairie */
    { OBJECT_SCHEDULE,
      Schedule_Init_Empty,
      Schedule_Count,
      Schedule_Index_To_Instance,
      Schedule_Valid_Instance,
      Schedule_Object_Name,
      Schedule_Read_Property,
      Schedule_Write_Property,
      Schedule_Property_Lists,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
      NULL,
      NULL,
      NULL },

    { OBJECT_TRENDLOG,
      Trend_Log_Init_Empty,
      Trend_Log_Count,
      Trend_Log_Index_To_Instance,
      Trend_Log_Valid_Instance,
      Trend_Log_Object_Name,
      Trend_Log_Read_Property,
      Trend_Log_Write_Property,
      Trend_Log_Property_Lists,
      TrendLogGetRRInfo,          
      NULL,                        
      NULL,                        
      rr_trend_log_encode,        
      NULL, NULL, NULL, NULL,     
      NULL,                        
      NULL,                        
      NULL },                      


    /* Terminator */
    { MAX_BACNET_OBJECT_TYPE,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

/* Déclaration forward */
static int apply_config_from_json(const char *json_text);

/* AJOUT: Fonction pour supprimer tous les objets d'un type donné */
static void delete_all_objects_of_type(BACNET_OBJECT_TYPE obj_type)
{
    unsigned count, i;
    uint32_t instance;
    
    switch (obj_type) {
        case OBJECT_ANALOG_INPUT:
            count = Analog_Input_Count();
            if (count > 0) printf("Deleting %u Analog Input(s)...\n", count);
            for (i = count; i > 0; i--) {
                instance = Analog_Input_Index_To_Instance(i - 1);
                if (Analog_Input_Delete(instance)) {
                    printf("  Deleted AI #%u\n", instance);
                }
            }
            break;
        case OBJECT_ANALOG_OUTPUT:
            count = Analog_Output_Count();
            if (count > 0) printf("Deleting %u Analog Output(s)...\n", count);
            for (i = count; i > 0; i--) {
                instance = Analog_Output_Index_To_Instance(i - 1);
                if (Analog_Output_Delete(instance)) {
                    printf("  Deleted AO #%u\n", instance);
                }
            }
            break;
        case OBJECT_ANALOG_VALUE:
            count = Analog_Value_Count();
            if (count > 0) printf("Deleting %u Analog Value(s)...\n", count);
            for (i = count; i > 0; i--) {
                instance = Analog_Value_Index_To_Instance(i - 1);
                if (Analog_Value_Delete(instance)) {
                    printf("  Deleted AV #%u\n", instance);
                }
            }
            break;
        case OBJECT_BINARY_INPUT:
            count = Binary_Input_Count();
            if (count > 0) printf("Deleting %u Binary Input(s)...\n", count);
            for (i = count; i > 0; i--) {
                instance = Binary_Input_Index_To_Instance(i - 1);
                if (Binary_Input_Delete(instance)) {
                    printf("  Deleted BI #%u\n", instance);
                }
            }
            break;
        case OBJECT_BINARY_OUTPUT:
            count = Binary_Output_Count();
            if (count > 0) printf("Deleting %u Binary Output(s)...\n", count);
            for (i = count; i > 0; i--) {
                instance = Binary_Output_Index_To_Instance(i - 1);
                if (Binary_Output_Delete(instance)) {
                    printf("  Deleted BO #%u\n", instance);
                }
            }
            break;
        case OBJECT_BINARY_VALUE:
            count = Binary_Value_Count();
            if (count > 0) printf("Deleting %u Binary Value(s)...\n", count);
            for (i = count; i > 0; i--) {
                instance = Binary_Value_Index_To_Instance(i - 1);
                if (Binary_Value_Delete(instance)) {
                    printf("  Deleted BV #%u\n", instance);
                }
            }
            break;
        case OBJECT_MULTI_STATE_INPUT:
            count = Multistate_Input_Count();
            if (count > 0) printf("Deleting %u Multi-State Input(s)...\n", count);
            for (i = count; i > 0; i--) {
                instance = Multistate_Input_Index_To_Instance(i - 1);
                if (Multistate_Input_Delete(instance)) {
                    printf("  Deleted MSI #%u\n", instance);
                }
            }
            break;
        case OBJECT_MULTI_STATE_OUTPUT:
            count = Multistate_Output_Count();
            if (count > 0) printf("Deleting %u Multi-State Output(s)...\n", count);
            for (i = count; i > 0; i--) {
                instance = Multistate_Output_Index_To_Instance(i - 1);
                if (Multistate_Output_Delete(instance)) {
                    printf("  Deleted MSO #%u\n", instance);
                }
            }
            break;
        case OBJECT_MULTI_STATE_VALUE:
            count = Multistate_Value_Count();
            if (count > 0) printf("Deleting %u Multi-State Value(s)...\n", count);
            for (i = count; i > 0; i--) {
                instance = Multistate_Value_Index_To_Instance(i - 1);
                if (Multistate_Value_Delete(instance)) {
                    printf("  Deleted MSV #%u\n", instance);
                }
            }
            break;
        default:
            break;
    }
}

static void delete_all_objects(void)
{
    printf("=== Clearing all existing objects before applying new configuration ===\n");
    delete_all_objects_of_type(OBJECT_ANALOG_INPUT);
    delete_all_objects_of_type(OBJECT_ANALOG_OUTPUT);
    delete_all_objects_of_type(OBJECT_ANALOG_VALUE);
    delete_all_objects_of_type(OBJECT_BINARY_INPUT);
    delete_all_objects_of_type(OBJECT_BINARY_OUTPUT);
    delete_all_objects_of_type(OBJECT_BINARY_VALUE);
    delete_all_objects_of_type(OBJECT_MULTI_STATE_INPUT);
    delete_all_objects_of_type(OBJECT_MULTI_STATE_OUTPUT);
    delete_all_objects_of_type(OBJECT_MULTI_STATE_VALUE);

    printf("Schedules not auto-initialized - will be created from JSON if present\n");

    printf("=== All objects cleared ===\n");
}

/* ===== Initialisation des Schedules ===== */
static void Init_Schedules(void)
{
    unsigned int count = Schedule_Count();
    printf("Schedule support initialized: %u schedule(s) available\n", count);
    if (count > 0) {
        unsigned int i;
        printf("  Schedule instances: ");
        for (i = 0; i < count; i++) {
            printf("%u", Schedule_Index_To_Instance(i));
            if (i < count - 1) printf(", ");
        }
        printf("\n  Configure via JSON or BACnet WriteProperty\n");
    } else {
        printf("  No schedules available. Add -DMAX_SCHEDULES=N to Makefile to enable.\n");
    }
}

/* ===== Fonctions JSON ===== */

/* Sauvegarder la configuration actuelle dans un fichier JSON */
static int save_current_config(void)
{
    json_t *root;
    json_t *objects_array;
    unsigned int ai_count, ao_count, av_count, bi_count, bo_count, bv_count;
    unsigned int msi_count, mso_count, msv_count, sch_count;
    unsigned int i;
    
    if (!g_config_file[0]) {
        return 0;
    }

    root = json_object();
    objects_array = json_array();
    
    /* Device info */
    json_object_set_new(root, "deviceId", json_integer(Device_Object_Instance_Number()));
    json_object_set_new(root, "deviceName", json_string(Device_Object_Name_ANSI()));
    
    /* Analog Inputs */
    ai_count = Analog_Input_Count();
    for (i = 0; i < ai_count; i++) {
        uint32_t inst;
        json_t *obj;
        BACNET_CHARACTER_STRING name_str;
        char name_buf[256];
        float pv;
        bool has_name;
        
        inst = Analog_Input_Index_To_Instance(i);
        obj = json_object();
        json_object_set_new(obj, "type", json_string("analog-input"));
        json_object_set_new(obj, "instance", json_integer(inst));
        
        has_name = Analog_Input_Object_Name(inst, &name_str);
        if (has_name) {
            memset(name_buf, 0, sizeof(name_buf));
            characterstring_ansi_copy(name_buf, sizeof(name_buf) - 1, &name_str);
            if (name_buf[0] != '\0') {
                json_object_set_new(obj, "name", json_string(name_buf));
            }
        }
        
        pv = Analog_Input_Present_Value(inst);
        json_object_set_new(obj, "presentValue", json_real(pv));
        
        json_array_append_new(objects_array, obj);
    }
    
    /* Analog Outputs */
    ao_count = Analog_Output_Count();
    for (i = 0; i < ao_count; i++) {
        uint32_t inst;
        json_t *obj;
        BACNET_CHARACTER_STRING name_str;
        char name_buf[256];
        float pv;
        bool has_name;

        inst = Analog_Output_Index_To_Instance(i);
        pv = Analog_Output_Present_Value(inst);

        obj = json_object();
        json_object_set_new(obj, "type", json_string("analog-output"));
        json_object_set_new(obj, "instance", json_integer(inst));

        has_name = Analog_Output_Object_Name(inst, &name_str);
        if (has_name) {
            memset(name_buf, 0, sizeof(name_buf));
            characterstring_ansi_copy(name_buf, sizeof(name_buf) - 1, &name_str);
            if (name_buf[0] != '\0') {
                json_object_set_new(obj, "name", json_string(name_buf));
            }
        }

        json_object_set_new(obj, "presentValue", json_real(pv));

        json_array_append_new(objects_array, obj);
    }
    
    /* Analog Values */
    av_count = Analog_Value_Count();
    for (i = 0; i < av_count; i++) {
        uint32_t inst;
        json_t *obj;
        BACNET_CHARACTER_STRING name_str;
        char name_buf[256];
        float pv;
        bool has_name;
        
        inst = Analog_Value_Index_To_Instance(i);
        obj = json_object();
        json_object_set_new(obj, "type", json_string("analog-value"));
        json_object_set_new(obj, "instance", json_integer(inst));
        
        has_name = Analog_Value_Object_Name(inst, &name_str);
        if (has_name) {
            memset(name_buf, 0, sizeof(name_buf));
            characterstring_ansi_copy(name_buf, sizeof(name_buf) - 1, &name_str);
            if (name_buf[0] != '\0') {
                json_object_set_new(obj, "name", json_string(name_buf));
            }
        }
        
        pv = Analog_Value_Present_Value(inst);
        json_object_set_new(obj, "presentValue", json_real(pv));
        
        json_array_append_new(objects_array, obj);
    }
    
    /* Binary Inputs */
    bi_count = Binary_Input_Count();
    for (i = 0; i < bi_count; i++) {
        uint32_t inst;
        json_t *obj;
        BACNET_CHARACTER_STRING name_str;
        char name_buf[256];
        BACNET_BINARY_PV pv;
        bool has_name;
        
        inst = Binary_Input_Index_To_Instance(i);
        obj = json_object();
        json_object_set_new(obj, "type", json_string("binary-input"));
        json_object_set_new(obj, "instance", json_integer(inst));
        
        has_name = Binary_Input_Object_Name(inst, &name_str);
        if (has_name) {
            memset(name_buf, 0, sizeof(name_buf));
            characterstring_ansi_copy(name_buf, sizeof(name_buf) - 1, &name_str);
            if (name_buf[0] != '\0') {
                json_object_set_new(obj, "name", json_string(name_buf));
            }
        }
        
        pv = Binary_Input_Present_Value(inst);
        json_object_set_new(obj, "presentValue", json_integer(pv));
        
        json_array_append_new(objects_array, obj);
    }
    
    /* Binary Outputs */
    bo_count = Binary_Output_Count();
    for (i = 0; i < bo_count; i++) {
        uint32_t inst;
        json_t *obj;
        BACNET_CHARACTER_STRING name_str;
        char name_buf[256];
        BACNET_BINARY_PV pv;
        bool has_name;
        
        inst = Binary_Output_Index_To_Instance(i);
        obj = json_object();
        json_object_set_new(obj, "type", json_string("binary-output"));
        json_object_set_new(obj, "instance", json_integer(inst));
        
        has_name = Binary_Output_Object_Name(inst, &name_str);
        if (has_name) {
            memset(name_buf, 0, sizeof(name_buf));
            characterstring_ansi_copy(name_buf, sizeof(name_buf) - 1, &name_str);
            if (name_buf[0] != '\0') {
                json_object_set_new(obj, "name", json_string(name_buf));
            }
        }
        
        pv = Binary_Output_Present_Value(inst);
        json_object_set_new(obj, "presentValue", json_integer(pv));
        
        json_array_append_new(objects_array, obj);
    }
    
    /* Binary Values */
    bv_count = Binary_Value_Count();
    for (i = 0; i < bv_count; i++) {
        uint32_t inst;
        json_t *obj;
        BACNET_CHARACTER_STRING name_str;
        char name_buf[256];
        BACNET_BINARY_PV pv;
        bool has_name;
        
        inst = Binary_Value_Index_To_Instance(i);
        obj = json_object();
        json_object_set_new(obj, "type", json_string("binary-value"));
        json_object_set_new(obj, "instance", json_integer(inst));
        
        has_name = Binary_Value_Object_Name(inst, &name_str);
        if (has_name) {
            memset(name_buf, 0, sizeof(name_buf));
            characterstring_ansi_copy(name_buf, sizeof(name_buf) - 1, &name_str);
            if (name_buf[0] != '\0') {
                json_object_set_new(obj, "name", json_string(name_buf));
            }
        }
        
        pv = Binary_Value_Present_Value(inst);
        json_object_set_new(obj, "presentValue", json_integer(pv));
        
        json_array_append_new(objects_array, obj);
    }
    
    /* Multi-State Inputs */
    msi_count = Multistate_Input_Count();
    for (i = 0; i < msi_count; i++) {
        uint32_t inst;
        json_t *obj;
        BACNET_CHARACTER_STRING name_str;
        char name_buf[256];
        uint32_t pv;
        bool has_name;
        
        inst = Multistate_Input_Index_To_Instance(i);
        obj = json_object();
        json_object_set_new(obj, "type", json_string("multi-state-input"));
        json_object_set_new(obj, "instance", json_integer(inst));
        
        has_name = Multistate_Input_Object_Name(inst, &name_str);
        if (has_name) {
            memset(name_buf, 0, sizeof(name_buf));
            characterstring_ansi_copy(name_buf, sizeof(name_buf) - 1, &name_str);
            if (name_buf[0] != '\0') {
                json_object_set_new(obj, "name", json_string(name_buf));
            }
        }
        
        pv = Multistate_Input_Present_Value(inst);
        json_object_set_new(obj, "presentValue", json_integer(pv));
        
        json_array_append_new(objects_array, obj);
    }
    
    /* Multi-State Outputs */
    mso_count = Multistate_Output_Count();
    for (i = 0; i < mso_count; i++) {
        uint32_t inst;
        json_t *obj;
        BACNET_CHARACTER_STRING name_str;
        char name_buf[256];
        uint32_t pv;
        bool has_name;
        
        inst = Multistate_Output_Index_To_Instance(i);
        obj = json_object();
        json_object_set_new(obj, "type", json_string("multi-state-output"));
        json_object_set_new(obj, "instance", json_integer(inst));
        
        has_name = Multistate_Output_Object_Name(inst, &name_str);
        if (has_name) {
            memset(name_buf, 0, sizeof(name_buf));
            characterstring_ansi_copy(name_buf, sizeof(name_buf) - 1, &name_str);
            if (name_buf[0] != '\0') {
                json_object_set_new(obj, "name", json_string(name_buf));
            }
        }
        
        pv = Multistate_Output_Present_Value(inst);
        json_object_set_new(obj, "presentValue", json_integer(pv));
        
        json_array_append_new(objects_array, obj);
    }
    
    /* Multi-State Values */
    msv_count = Multistate_Value_Count();
    for (i = 0; i < msv_count; i++) {
        uint32_t inst;
        json_t *obj;
        BACNET_CHARACTER_STRING name_str;
        char name_buf[256];
        uint32_t pv;
        bool has_name;
        
        inst = Multistate_Value_Index_To_Instance(i);
        obj = json_object();
        json_object_set_new(obj, "type", json_string("multi-state-value"));
        json_object_set_new(obj, "instance", json_integer(inst));
        
        has_name = Multistate_Value_Object_Name(inst, &name_str);
        if (has_name) {
            memset(name_buf, 0, sizeof(name_buf));
            characterstring_ansi_copy(name_buf, sizeof(name_buf) - 1, &name_str);
            if (name_buf[0] != '\0') {
                json_object_set_new(obj, "name", json_string(name_buf));
            }
        }
        
        pv = Multistate_Value_Present_Value(inst);
        json_object_set_new(obj, "presentValue", json_integer(pv));
        
        json_array_append_new(objects_array, obj);
    }
    
    /* Schedules - SAUVEGARDE COMPLÈTE */
 sch_count = Schedule_Count();
for (i = 0; i < sch_count; i++) {
    uint32_t inst;
    json_t *obj;
    BACNET_CHARACTER_STRING name_str;
    char name_buf[256];
    bool has_name;
    json_t *weekly_array;
    unsigned day_idx;
    BACNET_APPLICATION_DATA_VALUE default_val;
    BACNET_READ_PROPERTY_DATA rpdata;
    uint8_t apdu[MAX_APDU];
    int apdu_len;
    
    inst = Schedule_Index_To_Instance(i);
    obj = json_object();
    json_object_set_new(obj, "type", json_string("schedule"));
    json_object_set_new(obj, "instance", json_integer(inst));
    
    /* Nom */
    has_name = Schedule_Object_Name(inst, &name_str);
    if (has_name) {
        memset(name_buf, 0, sizeof(name_buf));
        characterstring_ansi_copy(name_buf, sizeof(name_buf) - 1, &name_str);
        if (name_buf[0] != '\0') {
            json_object_set_new(obj, "name", json_string(name_buf));
        }
    }
    
    /* ======== MODIFICATION: Default Value avec support BOOLEAN ======== */
    memset(&default_val, 0, sizeof(default_val));
    memset(&rpdata, 0, sizeof(rpdata));
    
    rpdata.object_type = OBJECT_SCHEDULE;
    rpdata.object_instance = inst;
    rpdata.object_property = PROP_SCHEDULE_DEFAULT;
    rpdata.array_index = BACNET_ARRAY_ALL;
    rpdata.application_data = &apdu[0];
    rpdata.application_data_len = sizeof(apdu);
    
    apdu_len = Schedule_Read_Property(&rpdata);
    if (apdu_len > 0) {
        int len;
        len = bacapp_decode_application_data(rpdata.application_data, 
                                            (uint8_t)rpdata.application_data_len, 
                                            &default_val);
        if (len > 0) {
            /* ORDRE IMPORTANT: Vérifier BOOLEAN en premier */
            if (default_val.tag == BACNET_APPLICATION_TAG_BOOLEAN) {
                json_object_set_new(obj, "defaultValue", 
                                  json_boolean(default_val.type.Boolean));
            } else if (default_val.tag == BACNET_APPLICATION_TAG_ENUMERATED) {
                json_object_set_new(obj, "defaultValue", 
                                  json_integer(default_val.type.Enumerated));
            } else if (default_val.tag == BACNET_APPLICATION_TAG_REAL) {
                json_object_set_new(obj, "defaultValue", 
                                  json_real(default_val.type.Real));
            } else if (default_val.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT) {
                json_object_set_new(obj, "defaultValue", 
                                  json_integer(default_val.type.Unsigned_Int));
            } else if (default_val.tag == BACNET_APPLICATION_TAG_SIGNED_INT) {
                json_object_set_new(obj, "defaultValue", 
                                  json_integer(default_val.type.Signed_Int));
            }
        }
    }
    
    /* Priority for Writing */
    memset(&rpdata, 0, sizeof(rpdata));
    rpdata.object_type = OBJECT_SCHEDULE;
    rpdata.object_instance = inst;
    rpdata.object_property = PROP_PRIORITY_FOR_WRITING;
    rpdata.array_index = BACNET_ARRAY_ALL;
    rpdata.application_data = &apdu[0];
    rpdata.application_data_len = sizeof(apdu);
    
    apdu_len = Schedule_Read_Property(&rpdata);
    if (apdu_len > 0) {
        BACNET_APPLICATION_DATA_VALUE priority_val;
        int len = bacapp_decode_application_data(rpdata.application_data,
                                                (uint8_t)rpdata.application_data_len,
                                                &priority_val);
        if (len > 0 && priority_val.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT) {
            uint8_t priority = (uint8_t)priority_val.type.Unsigned_Int;
            if (priority > 0 && priority <= 16) {
                json_object_set_new(obj, "priority", json_integer(priority));
            }
        }
    }
    
    /* ======== MODIFICATION: Weekly Schedule avec support BOOLEAN ======== */
    weekly_array = json_array();
    for (day_idx = 0; day_idx < 7; day_idx++) {
        json_t *day_array = json_array();
        
        memset(&rpdata, 0, sizeof(rpdata));
        rpdata.object_type = OBJECT_SCHEDULE;
        rpdata.object_instance = inst;
        rpdata.object_property = PROP_WEEKLY_SCHEDULE;
        rpdata.array_index = day_idx + 1;
        rpdata.application_data = &apdu[0];
        rpdata.application_data_len = sizeof(apdu);
        
        apdu_len = Schedule_Read_Property(&rpdata);
        if (apdu_len > 0) {
            int decode_len = 0;
            int total_len = 0;
            uint8_t *apdu_ptr = rpdata.application_data;
            
            if (decode_is_opening_tag_number(apdu_ptr, 0)) {
                total_len++;
                apdu_ptr++;
                
                while (total_len < apdu_len) {
                    BACNET_APPLICATION_DATA_VALUE time_val;
                    BACNET_APPLICATION_DATA_VALUE value_val;
                    
                    if (decode_is_closing_tag_number(apdu_ptr, 0)) {
                        break;
                    }
                    
                    decode_len = bacapp_decode_application_data(apdu_ptr, 
                                                               apdu_len - total_len, 
                                                               &time_val);
                    if (decode_len <= 0) break;
                    total_len += decode_len;
                    apdu_ptr += decode_len;
                    
                    decode_len = bacapp_decode_application_data(apdu_ptr, 
                                                               apdu_len - total_len, 
                                                               &value_val);
                    if (decode_len <= 0) break;
                    total_len += decode_len;
                    apdu_ptr += decode_len;
                    
                    if (time_val.tag == BACNET_APPLICATION_TAG_TIME) {
                        json_t *time_value_obj = json_object();
                        char time_str[16];
                        
                        snprintf(time_str, sizeof(time_str), "%u:%02u",
                                time_val.type.Time.hour,
                                time_val.type.Time.min);
                        json_object_set_new(time_value_obj, "time", json_string(time_str));
                        
                        /* ORDRE IMPORTANT: Vérifier BOOLEAN en premier */
                        if (value_val.tag == BACNET_APPLICATION_TAG_BOOLEAN) {
                            json_object_set_new(time_value_obj, "value", 
                                              json_boolean(value_val.type.Boolean));
                        } else if (value_val.tag == BACNET_APPLICATION_TAG_ENUMERATED) {
                            json_object_set_new(time_value_obj, "value", 
                                              json_integer(value_val.type.Enumerated));
                        } else if (value_val.tag == BACNET_APPLICATION_TAG_REAL) {
                            json_object_set_new(time_value_obj, "value", 
                                              json_real(value_val.type.Real));
                        } else if (value_val.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT) {
                            json_object_set_new(time_value_obj, "value", 
                                              json_integer(value_val.type.Unsigned_Int));
                        } else if (value_val.tag == BACNET_APPLICATION_TAG_SIGNED_INT) {
                            json_object_set_new(time_value_obj, "value", 
                                              json_integer(value_val.type.Signed_Int));
                        }
                        
                        json_array_append_new(day_array, time_value_obj);
                    }
                }
            }
        }
        
        json_array_append_new(weekly_array, day_array);
    }
    json_object_set_new(obj, "weeklySchedule", weekly_array);
    
    json_array_append_new(objects_array, obj);
}


    
    json_object_set_new(root, "objects", objects_array);
    
    /* Écrire dans le fichier */
    if (json_dump_file(root, g_config_file, JSON_INDENT(2)) != 0) {
        fprintf(stderr, "Failed to write config to %s\n", g_config_file);
        json_decref(root);
        return -1;
    }
    
    json_decref(root);
    return 0;
}

/* Charger la configuration depuis le fichier */
static int load_config_from_file(void)
{
    json_error_t jerr;
    json_t *root;
    char *json_str;
    int result;
    
    if (!g_config_file[0]) {
        return 0;
    }

    root = json_load_file(g_config_file, 0, &jerr);
    if (!root) {
        printf("No existing config file or parse error: %s\n", jerr.text);
        return 0;
    }

    printf("Loading configuration from %s...\n", g_config_file);
    
    json_str = json_dumps(root, 0);
    if (!json_str) {
        json_decref(root);
        return -1;
    }
    
    result = apply_config_from_json(json_str);
    free(json_str);
    json_decref(root);
    
    if (result == 0) {
        printf("Configuration loaded successfully\n");
    }
    
    return result;
}

static int apply_config_from_json(const char *json_text)
{
    json_error_t jerr;
    json_t *root = json_loads(json_text, 0, &jerr);
    json_t *objs;
    size_t i, n;

    if (!root) {
        fprintf(stderr, "CFGJSON: parse error at %d:%d: %s\n", jerr.line, jerr.column, jerr.text);
        return -1;
    }

    /* MODIFICATION: Supprimer tous les objets existants avant d'appliquer la nouvelle configuration */
    delete_all_objects();

    /* Device */
    if (json_is_integer(json_object_get(root, "deviceId"))) {
        Device_Set_Object_Instance_Number((uint32_t)json_integer_value(json_object_get(root, "deviceId")));
    }
    if (json_is_string(json_object_get(root, "deviceName"))) {
        const char *dn = json_string_value(json_object_get(root, "deviceName"));
        if (dn) {
            Device_Object_Name_ANSI_Init(dn);
        }
    }

    objs = json_object_get(root, "objects");
    if (!json_is_array(objs)) { 
        json_decref(root); 
        return 0;
    }

    n = json_array_size(objs);
    printf("Creating %u objects from JSON...\n", (unsigned int)n);
    
    for (i = 0; i < n; i++) {
        json_t *it = json_array_get(objs, i);
        const char *typ;
        json_t *jinst;
        uint32_t inst;
        const char *name;
        json_t *jpv;

        if (!json_is_object(it)) { continue; }

        typ   = json_string_value(json_object_get(it, "type"));
        jinst = json_object_get(it, "instance");
        inst  = (uint32_t)json_integer_value(jinst);
        name  = json_string_value(json_object_get(it, "name"));
        jpv   = json_object_get(it, "presentValue");

        if (!typ || !json_is_integer(jinst)) {
            continue;
        }

        if (strcmp(typ, "analog-input") == 0) {
            bool exists = Analog_Input_Valid_Instance(inst);
            if (!exists) {
                uint32_t result = Analog_Input_Create(inst);
                if (result != BACNET_MAX_INSTANCE) {
                    printf("Created Analog Input %u\n", inst);
                } else {
                    printf("Failed to create Analog Input %u\n", inst);
                    continue;
                }
            } else {
                printf("Updating existing Analog Input %u\n", inst);
            }
            if (name) { 
                char *name_copy = strdup(name);
                if (name_copy) {
                    set_object_name(OBJECT_ANALOG_INPUT, inst, name_copy);
                }
            }
            if (json_is_number(jpv)) {
                Analog_Input_Present_Value_Set(inst, (float)json_number_value(jpv));
            }
            Analog_Input_Out_Of_Service_Set(inst, true);
        } 
        else if (strcmp(typ, "analog-value") == 0) {
            bool exists = Analog_Value_Valid_Instance(inst);
            if (!exists) {
                uint32_t result = Analog_Value_Create(inst);
                if (result != BACNET_MAX_INSTANCE) {
                    printf("Created Analog Value %u\n", inst);
                } else {
                    printf("Failed to create Analog Value %u\n", inst);
                    continue;
                }
            } else {
                printf("Updating existing Analog Value %u\n", inst);
            }
            if (name) { 
                char *name_copy = strdup(name);
                if (name_copy) {
                    set_object_name(OBJECT_ANALOG_VALUE, inst, name_copy);
                }
            }
            if (json_is_number(jpv)) {
                Analog_Value_Present_Value_Set(inst, (float)json_number_value(jpv), BACNET_MAX_PRIORITY);
            }
        } 
        else if (strcmp(typ, "analog-output") == 0) {
            bool exists = Analog_Output_Valid_Instance(inst);
            if (!exists) {
                uint32_t result = Analog_Output_Create(inst);
                if (result != BACNET_MAX_INSTANCE) {
                    printf("Created Analog Output %u\n", inst);
                } else {
                    printf("Failed to create Analog Output %u\n", inst);
                    continue;
                }
            } else {
                printf("Updating existing Analog Output %u\n", inst);
            }
            if (name) { 
                char *name_copy = strdup(name);
                if (name_copy) {
                    set_object_name(OBJECT_ANALOG_OUTPUT, inst, name_copy);
                }
            }
            if (json_is_number(jpv)) {
                Analog_Output_Present_Value_Set(inst, (float)json_number_value(jpv), BACNET_MAX_PRIORITY);
            }
            Analog_Output_Out_Of_Service_Set(inst, true);
        }
        else if (strcmp(typ, "binary-input") == 0) {
            bool exists = Binary_Input_Valid_Instance(inst);
            if (!exists) {
                uint32_t result = Binary_Input_Create(inst);
                if (result != BACNET_MAX_INSTANCE) {
                    printf("Created Binary Input %u\n", inst);
                } else {
                    printf("Failed to create Binary Input %u\n", inst);
                    continue;
                }
            } else {
                printf("Updating existing Binary Input %u\n", inst);
            }
            if (name) { 
                char *name_copy = strdup(name);
                if (name_copy) {
                    set_object_name(OBJECT_BINARY_INPUT, inst, name_copy);
                }
            }
            if (json_is_integer(jpv)) {
                Binary_Input_Present_Value_Set(inst, (BACNET_BINARY_PV)json_integer_value(jpv));
            }
            Binary_Input_Out_Of_Service_Set(inst, true);
        }
        else if (strcmp(typ, "binary-output") == 0) {
            bool exists = Binary_Output_Valid_Instance(inst);
            if (!exists) {
                uint32_t result = Binary_Output_Create(inst);
                if (result != BACNET_MAX_INSTANCE) {
                    printf("Created Binary Output %u\n", inst);
                } else {
                    printf("Failed to create Binary Output %u\n", inst);
                    continue;
                }
            } else {
                printf("Updating existing Binary Output %u\n", inst);
            }
            if (name) { 
                char *name_copy = strdup(name);
                if (name_copy) {
                    set_object_name(OBJECT_BINARY_OUTPUT, inst, name_copy);
                }
            }
            if (json_is_integer(jpv)) {
                Binary_Output_Present_Value_Set(inst, (BACNET_BINARY_PV)json_integer_value(jpv), BACNET_MAX_PRIORITY);
            }
            Binary_Output_Out_Of_Service_Set(inst, true);
        }
        else if (strcmp(typ, "binary-value") == 0) {
            bool exists = Binary_Value_Valid_Instance(inst);
            if (!exists) {
                uint32_t result = Binary_Value_Create(inst);
                if (result != BACNET_MAX_INSTANCE) {
                    printf("Created Binary Value %u\n", inst);
                } else {
                    printf("Failed to create Binary Value %u\n", inst);
                    continue;
                }
            } else {
                printf("Updating existing Binary Value %u\n", inst);
            }
            if (name) { 
                char *name_copy = strdup(name);
                if (name_copy) {
                    set_object_name(OBJECT_BINARY_VALUE, inst, name_copy);
                }
            }
            if (json_is_integer(jpv)) {
                Binary_Value_Present_Value_Set(inst, (BACNET_BINARY_PV)json_integer_value(jpv));
            }
        }
        else if (strcmp(typ, "multi-state-input") == 0) {
            bool exists = Multistate_Input_Valid_Instance(inst);
            if (!exists) {
                uint32_t result = Multistate_Input_Create(inst);
                if (result != BACNET_MAX_INSTANCE) {
                    printf("Created Multi-State Input %u\n", inst);
                } else {
                    printf("Failed to create Multi-State Input %u\n", inst);
                    continue;
                }
            } else {
                printf("Updating existing Multi-State Input %u\n", inst);
            }
            if (name) { 
                char *name_copy = strdup(name);
                if (name_copy) {
                    set_object_name(OBJECT_MULTI_STATE_INPUT, inst, name_copy);
                }
            }
            if (json_is_integer(jpv)) {
                Multistate_Input_Present_Value_Set(inst, (uint32_t)json_integer_value(jpv));
            }
            Multistate_Input_Out_Of_Service_Set(inst, true);
        }
        else if (strcmp(typ, "multi-state-output") == 0) {
            bool exists = Multistate_Output_Valid_Instance(inst);
            if (!exists) {
                uint32_t result = Multistate_Output_Create(inst);
                if (result != BACNET_MAX_INSTANCE) {
                    printf("Created Multi-State Output %u\n", inst);
                } else {
                    printf("Failed to create Multi-State Output %u\n", inst);
                    continue;
                }
            } else {
                printf("Updating existing Multi-State Output %u\n", inst);
            }
            if (name) { 
                char *name_copy = strdup(name);
                if (name_copy) {
                    set_object_name(OBJECT_MULTI_STATE_OUTPUT, inst, name_copy);
                }
            }
            if (json_is_integer(jpv)) {
                Multistate_Output_Present_Value_Set(inst, (uint32_t)json_integer_value(jpv), BACNET_MAX_PRIORITY);
            }
            Multistate_Output_Out_Of_Service_Set(inst, true);
        }
        else if (strcmp(typ, "multi-state-value") == 0) {
            bool exists = Multistate_Value_Valid_Instance(inst);
            if (!exists) {
                uint32_t result = Multistate_Value_Create(inst);
                if (result != BACNET_MAX_INSTANCE) {
                    printf("Created Multi-State Value %u\n", inst);
                } else {
                    printf("Failed to create Multi-State Value %u\n", inst);
                    continue;
                }
            } else {
                printf("Updating existing Multi-State Value %u\n", inst);
            }
            if (name) { 
                char *name_copy = strdup(name);
                if (name_copy) {
                    set_object_name(OBJECT_MULTI_STATE_VALUE, inst, name_copy);
                }
            }
            if (json_is_integer(jpv)) {
                Multistate_Value_Present_Value_Set(inst, (uint32_t)json_integer_value(jpv));
            }
        }
        else if (strcmp(typ, "schedule") == 0) {
            bool exists;
            size_t day_idx;
            json_t *weekly_schedule;
            json_t *default_value;
            json_t *priority;
            double val;
            
            exists = Schedule_Valid_Instance(inst);
            if (!exists) {
                printf("Schedule %u does not exist. MAX_SCHEDULES may be too low or instance out of range.\n", inst);
                printf("  Schedules available: 0 to %u\n", Schedule_Count() > 0 ? Schedule_Count() - 1 : 0);
                continue;
            }
            
            printf("Configuring Schedule %u\n", inst);
            
            if (name) {
                char *name_copy = strdup(name);
                if (name_copy) {
                    set_object_name(OBJECT_SCHEDULE, inst, name_copy);
                    printf("  Schedule name: '%s'\n", name);
                }
            }
            
            default_value = json_object_get(it, "defaultValue");
            if (default_value && !json_is_null(default_value)) {
                BACNET_APPLICATION_DATA_VALUE app_value;
                BACNET_WRITE_PROPERTY_DATA wp_data;
                uint8_t apdu[MAX_APDU];
                int apdu_len;
                
                memset(&app_value, 0, sizeof(app_value));
                memset(&wp_data, 0, sizeof(wp_data));
                
                if (json_is_boolean(default_value)) {
                    app_value.tag = BACNET_APPLICATION_TAG_BOOLEAN;
                    app_value.type.Boolean = json_boolean_value(default_value);
                    printf("  Setting default value: %s (BOOLEAN)\n", 
                        app_value.type.Boolean ? "true" : "false");
                }
                else if (json_is_real(default_value)) {
                    app_value.tag = BACNET_APPLICATION_TAG_REAL;
                    app_value.type.Real = (float)json_real_value(default_value);
                    printf("  Setting default value: %f (REAL)\n", app_value.type.Real);
                }
                else if (json_is_integer(default_value)) {
                    app_value.tag = BACNET_APPLICATION_TAG_ENUMERATED;
                    app_value.type.Enumerated = (uint32_t)json_integer_value(default_value);
                    printf("  Setting default value: %u (ENUMERATED)\n", 
                        app_value.type.Enumerated);
                }
                
                apdu_len = bacapp_encode_application_data(&apdu[0], &app_value);
                
                wp_data.object_type = OBJECT_SCHEDULE;
                wp_data.object_instance = inst;
                wp_data.object_property = PROP_SCHEDULE_DEFAULT;
                wp_data.array_index = BACNET_ARRAY_ALL;
                wp_data.application_data_len = apdu_len;
                memcpy(wp_data.application_data, &apdu[0], apdu_len);
                wp_data.priority = BACNET_NO_PRIORITY;
                wp_data.error_code = ERROR_CODE_SUCCESS;
                
                apdu_len = Schedule_Write_Property(&wp_data);
                if (apdu_len > 0 && wp_data.error_code == ERROR_CODE_SUCCESS) {
                    printf("  Default value set successfully\n");
                } else {
                    printf("  Failed to set default value (error: %d)\n", wp_data.error_code);
                }
            }
            
            priority = json_object_get(it, "priority");
            if (json_is_integer(priority)) {
                BACNET_APPLICATION_DATA_VALUE app_value;
                BACNET_WRITE_PROPERTY_DATA wp_data;
                uint8_t apdu[MAX_APDU];
                int apdu_len;
                uint8_t prio;
                
                prio = (uint8_t)json_integer_value(priority);
                
                if (prio > 0 && prio <= 16) {
                    memset(&app_value, 0, sizeof(app_value));
                    memset(&wp_data, 0, sizeof(wp_data));
                    
                    app_value.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
                    app_value.type.Unsigned_Int = prio;
                    
                    apdu_len = bacapp_encode_application_data(&apdu[0], &app_value);
                    
                    wp_data.object_type = OBJECT_SCHEDULE;
                    wp_data.object_instance = inst;
                    wp_data.object_property = PROP_PRIORITY_FOR_WRITING;
                    wp_data.array_index = BACNET_ARRAY_ALL;
                    wp_data.application_data_len = apdu_len;
                    memcpy(wp_data.application_data, &apdu[0], apdu_len);
                    wp_data.priority = BACNET_NO_PRIORITY;
                    wp_data.error_code = ERROR_CODE_SUCCESS;
                    
                    apdu_len = Schedule_Write_Property(&wp_data);
                    if (apdu_len > 0 && wp_data.error_code == ERROR_CODE_SUCCESS) {
                        printf("  Priority set to: %u\n", prio);
                    } else {
                        printf("  Failed to set priority (error: %d)\n", wp_data.error_code);
                    }
                }
            }
            
            /* ======== MODIFICATION: weeklySchedule avec support BOOLEAN ======== */
            /* Configuration du weekly schedule */
            weekly_schedule = json_object_get(it, "weeklySchedule");
            if (json_is_array(weekly_schedule)) {
                printf("  Configuring weekly schedule...\n");
                for (day_idx = 0; day_idx < json_array_size(weekly_schedule) && day_idx < 7; day_idx++) {
                    json_t *day_schedule = json_array_get(weekly_schedule, day_idx);
                    if (json_is_array(day_schedule)) {
                        BACNET_DAILY_SCHEDULE daily;
                        size_t time_idx;
                        
                        daily.TV_Count = 0;
                        
                        for (time_idx = 0; time_idx < json_array_size(day_schedule) && time_idx < 50; time_idx++) {
                            json_t *time_value = json_array_get(day_schedule, time_idx);
                            json_t *jtime = json_object_get(time_value, "time");
                            json_t *jvalue = json_object_get(time_value, "value");
                            
                            if (json_is_string(jtime) && (json_is_boolean(jvalue) || json_is_number(jvalue))) {
                                const char *time_str = json_string_value(jtime);
                                int hour, minute;
                                
                                if (sscanf(time_str, "%d:%d", &hour, &minute) == 2) {
                                    daily.Time_Values[time_idx].Time.hour = (uint8_t)hour;
                                    daily.Time_Values[time_idx].Time.min = (uint8_t)minute;
                                    daily.Time_Values[time_idx].Time.sec = 0;
                                    daily.Time_Values[time_idx].Time.hundredths = 0;
                                    
                                    if (json_is_boolean(jvalue)) {
                                        daily.Time_Values[time_idx].Value.tag = BACNET_APPLICATION_TAG_BOOLEAN;
                                        daily.Time_Values[time_idx].Value.type.Boolean = json_boolean_value(jvalue);
                                    }
                                    else if (json_is_real(jvalue)) {
                                        daily.Time_Values[time_idx].Value.tag = BACNET_APPLICATION_TAG_REAL;
                                        daily.Time_Values[time_idx].Value.type.Real = (float)json_real_value(jvalue);
                                    }
                                    else if (json_is_integer(jvalue)) {
                                        daily.Time_Values[time_idx].Value.tag = BACNET_APPLICATION_TAG_ENUMERATED;
                                        daily.Time_Values[time_idx].Value.type.Enumerated = (uint32_t)json_integer_value(jvalue);
                                    }
                                    
                                    daily.TV_Count++;
                                }
                            }
                        }
                        
                        if (daily.TV_Count > 0) {
                            bool status = Schedule_Weekly_Schedule_Set(inst, (uint8_t)day_idx, &daily);
                            if (status) {
                                printf("    Day %u: %d time values configured\n", 
                                    (unsigned int)day_idx, daily.TV_Count);
                            } else {
                                printf("    Day %u: Configuration failed\n", (unsigned int)day_idx);
                            }
                        }
                    }
                }
            }
            
            printf("  Schedule %u configuration complete\n", inst);
            
            {
                SCHEDULE_DESCR *desc = Schedule_Object(inst);
                if (desc) {
                    desc->Start_Date.year = 1900;
                    desc->Start_Date.month = 1;
                    desc->Start_Date.day = 1;
                    desc->Start_Date.wday = BACNET_WEEKDAY_MONDAY;
                    
                    desc->End_Date.year = 2154;
                    desc->End_Date.month = 12;
                    desc->End_Date.day = 31;
                    desc->End_Date.wday = BACNET_WEEKDAY_SUNDAY;
                    
                    printf("  Effective period FORCED: always active (1900-2154)\n");
                    
                    {
                        BACNET_TIME time_of_day;
                        BACNET_WEEKDAY wday;
                        time_t now;
                        struct tm *lt;
                        
                        now = time(NULL);
                        lt = localtime(&now);
                        
                        time_of_day.hour = (uint8_t)lt->tm_hour;
                        time_of_day.min = (uint8_t)lt->tm_min;
                        time_of_day.sec = (uint8_t)lt->tm_sec;
                        time_of_day.hundredths = 0;
                        

                        if (lt->tm_wday == 0) {
                            wday = (BACNET_WEEKDAY)7; 
                        } else {
                            wday = (BACNET_WEEKDAY)lt->tm_wday;  
                        }
                        
                        Schedule_Recalculate_PV(desc, wday, &time_of_day);
                        
                        if (desc->Present_Value.tag == BACNET_APPLICATION_TAG_BOOLEAN) {
                            printf("  Initial PV: %s (BOOLEAN) at %02u:%02u wday=%u\n",
                                desc->Present_Value.type.Boolean ? "true" : "false",
                                time_of_day.hour, time_of_day.min, wday);
                        } else if (desc->Present_Value.tag == BACNET_APPLICATION_TAG_ENUMERATED) {
                            printf("  Initial PV: %u (ENUM) at %02u:%02u wday=%u\n",
                                desc->Present_Value.type.Enumerated,
                                time_of_day.hour, time_of_day.min, wday);
                        } else if (desc->Present_Value.tag == BACNET_APPLICATION_TAG_REAL) {
                            printf("  Initial PV: %.1f (REAL) at %02u:%02u wday=%u\n",
                                desc->Present_Value.type.Real,
                                time_of_day.hour, time_of_day.min, wday);
                        }
                    }
                }
            }
        } else if (strcmp(typ, "trendlog") == 0) {
            /* Déclarations des pointeurs JSON */
            json_t *j_desc, *j_enable, *j_linked, *j_interval;
            json_t *j_buffer, *j_trigger, *j_cov, *j_stop_full, *j_align;
            
            /* Déclarations des variables */
            uint32_t tl_instance;
            const char *tl_name;
            const char *tl_desc;
            bool tl_enable;
            uint32_t log_interval;
            uint32_t buffer_size;
            const char *trigger_type;
            BACNET_OBJECT_TYPE source_type;
            uint32_t source_instance;
            
            /* Initialisation */
            source_type = OBJECT_ANALOG_VALUE;
            source_instance = 0;
            
            /* Récupérer instance et name (déjà extraits) */
            tl_instance = inst;
            tl_name = name;
            
            /* Récupérer les autres champs du Trendlog */
            j_desc = json_object_get(it, "description");
            j_enable = json_object_get(it, "enable");
            
            /* Support "linkedObject" (camelCase) et "linked_object" (snake_case) */
            j_linked = json_object_get(it, "linkedObject");
            if (!j_linked) {
                j_linked = json_object_get(it, "linked_object");
            }
            
            /* Support "logInterval" (camelCase) et "log_interval" (snake_case) */
            j_interval = json_object_get(it, "logInterval");
            if (!j_interval) {
                j_interval = json_object_get(it, "log_interval");
            }
            
            /* Support "bufferSize" (camelCase) et "buffer_size" (snake_case) */
            j_buffer = json_object_get(it, "bufferSize");
            if (!j_buffer) {
                j_buffer = json_object_get(it, "buffer_size");
            }
            
            /* Support "triggerType" (camelCase) et "trigger_type" (snake_case) */
            j_trigger = json_object_get(it, "triggerType");
            if (!j_trigger) {
                j_trigger = json_object_get(it, "trigger_type");
            }
            
            /* Propriétés non encore supportées */
            j_cov = json_object_get(it, "cov_increment");
            j_stop_full = json_object_get(it, "stop_when_full");
            j_align = json_object_get(it, "align_intervals");
            
            /* Valeurs par défaut */
            tl_desc = j_desc ? json_string_value(j_desc) : "";
            tl_enable = j_enable ? json_boolean_value(j_enable) : true;
            log_interval = j_interval ? (uint32_t)json_integer_value(j_interval) : 300;
            buffer_size = j_buffer ? (uint32_t)json_integer_value(j_buffer) : 100;
            trigger_type = j_trigger ? json_string_value(j_trigger) : "periodic";
            
            /* Éviter warnings pour variables non utilisées */
            (void)j_cov;
            (void)j_stop_full;
            (void)j_align;
            
            /* Parser l'objet lié (linkedObject) */
            if (j_linked && json_is_object(j_linked)) {
                json_t *j_type = json_object_get(j_linked, "type");
                json_t *j_obj_inst = json_object_get(j_linked, "instance");
                
                if (j_type && json_is_string(j_type)) {
                    const char *type_str = json_string_value(j_type);
                    
                    /* Support format avec tiret: "analog-input" ET "ANALOG_INPUT" */
                    if (strcmp(type_str, "analog-input") == 0 || 
                        strcmp(type_str, "ANALOG_INPUT") == 0) {
                        source_type = OBJECT_ANALOG_INPUT;
                    } else if (strcmp(type_str, "analog-output") == 0 || 
                               strcmp(type_str, "ANALOG_OUTPUT") == 0) {
                        source_type = OBJECT_ANALOG_OUTPUT;
                    } else if (strcmp(type_str, "analog-value") == 0 || 
                               strcmp(type_str, "ANALOG_VALUE") == 0) {
                        source_type = OBJECT_ANALOG_VALUE;
                    } else if (strcmp(type_str, "binary-input") == 0 || 
                               strcmp(type_str, "BINARY_INPUT") == 0) {
                        source_type = OBJECT_BINARY_INPUT;
                    } else if (strcmp(type_str, "binary-output") == 0 || 
                               strcmp(type_str, "BINARY_OUTPUT") == 0) {
                        source_type = OBJECT_BINARY_OUTPUT;
                    } else if (strcmp(type_str, "binary-value") == 0 || 
                               strcmp(type_str, "BINARY_VALUE") == 0) {
                        source_type = OBJECT_BINARY_VALUE;
                    } else if (strcmp(type_str, "multi-state-input") == 0 || 
                               strcmp(type_str, "MULTI_STATE_INPUT") == 0) {
                        source_type = OBJECT_MULTI_STATE_INPUT;
                    } else if (strcmp(type_str, "multi-state-output") == 0 || 
                               strcmp(type_str, "MULTI_STATE_OUTPUT") == 0) {
                        source_type = OBJECT_MULTI_STATE_OUTPUT;
                    } else if (strcmp(type_str, "multi-state-value") == 0 || 
                               strcmp(type_str, "MULTI_STATE_VALUE") == 0) {
                        source_type = OBJECT_MULTI_STATE_VALUE;
                    }
                }
                
                if (j_obj_inst) {
                    source_instance = (uint32_t)json_integer_value(j_obj_inst);
                }
            }
            
            /* Affichage et création */
            printf("\n========================================\n");
            printf("Trendlog %u: %s\n", tl_instance, tl_name ? tl_name : "(no name)");
            printf("========================================\n");
            printf("  Description: %s\n", tl_desc);
            printf("  Source: %s[%u]\n", 
                   bactext_object_type_name(source_type), source_instance);
            printf("  Interval: %u seconds\n", log_interval);
            printf("  Trigger: %s\n", trigger_type);
            printf("  Enabled: %s\n", tl_enable ? "YES" : "NO");
            
            /* Créer et configurer le Trendlog */
            if (create_trendlog(tl_instance, tl_name, source_type, source_instance,
                               log_interval, buffer_size, tl_enable)) {
                printf("✓ Trendlog %u configured successfully\n", tl_instance);
            } else {
                printf("✗ Failed to configure Trendlog %u\n", tl_instance);
            }
            printf("========================================\n");
        }
}
    json_decref(root);
    printf("Object creation complete.\n");
    printf("  AI: %u, AO: %u, AV: %u\n", 
           Analog_Input_Count(), Analog_Output_Count(), Analog_Value_Count());
    printf("  BI: %u, BO: %u, BV: %u\n", 
           Binary_Input_Count(), Binary_Output_Count(), Binary_Value_Count());
    printf("  MSI: %u, MSO: %u, MSV: %u\n", 
           Multistate_Input_Count(), Multistate_Output_Count(), Multistate_Value_Count());
    printf("  SCH: %u, TL: %u\n", Schedule_Count(), Trend_Log_Count()); 
    fflush(stdout);
    
    save_current_config();
    
    Send_I_Am(&Rx_Buf[0]);
    printf("I-Am re-broadcasted after object creation\n");
    fflush(stdout);
    
    return 0;
}


/* ===== Socket utilitaires ===== */
static int socket_listen_local(int port)
{
    int fd, flags, yes;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void*)&yes, sizeof(yes));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -2;
    }
    if (listen(fd, 1) < 0) {
        close(fd);
        return -3;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fd;
}

static void socket_close_all(void)
{
    if (g_client_fd >= 0) { close(g_client_fd); g_client_fd = -1; }
    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
}

static void trim_newlines(char *s)
{
    size_t n;
    if (!s) return;
    n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[n-1] = '\0';
        n--;
    }
}

static void write_pidfile_if_needed(void)
{
    if (g_pidfile[0]) {
        FILE *f = fopen(g_pidfile, "w");
        if (f) {
            fprintf(f, "%d\n", (int)getpid());
            fclose(f);
        }
    }
}

/* ========================================
 * Commande: trendlogs
 * Liste tous les Trendlogs
 * ========================================*/
static int handle_cmd_trendlogs(void)
{
    json_t *root = json_object();
    json_t *trendlogs_array = json_array();
    unsigned int i, count;
    
    count = Trend_Log_Count();
    
    printf("========== Trendlogs Status ==========\n");
    printf("Total Trendlogs configured: %u / %d\n\n", count, MAX_TREND_LOGS);
    
    if (count == 0) {
        printf("No Trendlogs configured.\n");
        json_object_set_new(root, "trendlogs", trendlogs_array);
        json_object_set_new(root, "count", json_integer(0));
        
        char *json_str = json_dumps(root, JSON_INDENT(2));
        printf("\n%s\n", json_str);
        free(json_str);
        json_decref(root);
        return 0;
    }
    
    for (i = 0; i < count; i++) {
        uint32_t instance = Trend_Log_Index_To_Instance(i);
        json_t *tl_obj = json_object();
        
        /* Lire les propriétés du Trendlog */
        BACNET_READ_PROPERTY_DATA rpdata = {0};
        uint8_t apdu[MAX_APDU] = {0};
        int apdu_len;
        
        /* Instance */
        json_object_set_new(tl_obj, "instance", json_integer(instance));
        
        /* ENABLE */
        rpdata.object_type = OBJECT_TRENDLOG;
        rpdata.object_instance = instance;
        rpdata.object_property = PROP_ENABLE;
        rpdata.array_index = BACNET_ARRAY_ALL;
        rpdata.application_data = apdu;
        rpdata.application_data_len = sizeof(apdu);
        
        apdu_len = Trend_Log_Read_Property(&rpdata);
        if (apdu_len > 0) {
            BACNET_APPLICATION_DATA_VALUE value;
            int len = bacapp_decode_application_data(rpdata.application_data,
                                                    rpdata.application_data_len,
                                                    &value);
            if (len > 0 && value.tag == BACNET_APPLICATION_TAG_BOOLEAN) {
                json_object_set_new(tl_obj, "enabled", json_boolean(value.type.Boolean));
                printf("TL[%u] %s ", instance, value.type.Boolean ? "✓" : "✗");
            }
        }
        
        /* RECORD_COUNT */
        rpdata.object_property = PROP_RECORD_COUNT;
        rpdata.application_data = apdu;
        rpdata.application_data_len = sizeof(apdu);
        
        apdu_len = Trend_Log_Read_Property(&rpdata);
        if (apdu_len > 0) {
            BACNET_APPLICATION_DATA_VALUE value;
            int len = bacapp_decode_application_data(rpdata.application_data,
                                                    rpdata.application_data_len,
                                                    &value);
            if (len > 0 && value.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT) {
                json_object_set_new(tl_obj, "record_count", json_integer(value.type.Unsigned_Int));
                printf("Records: %u ", (unsigned int)value.type.Unsigned_Int);
            }
        }
        
        /* LOG_INTERVAL */
        rpdata.object_property = PROP_LOG_INTERVAL;
        rpdata.application_data = apdu;
        rpdata.application_data_len = sizeof(apdu);
        
        apdu_len = Trend_Log_Read_Property(&rpdata);
        if (apdu_len > 0) {
            BACNET_APPLICATION_DATA_VALUE value;
            int len = bacapp_decode_application_data(rpdata.application_data,
                                                    rpdata.application_data_len,
                                                    &value);
            if (len > 0 && value.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT) {
                uint32_t interval_cs = value.type.Unsigned_Int;
                uint32_t interval_s = interval_cs / 100;
                json_object_set_new(tl_obj, "log_interval_seconds", json_integer(interval_s));
                printf("Interval: %us ", interval_s);
            }
        }
        
        /* LOG_DEVICE_OBJECT_PROPERTY (objet source) */
        rpdata.object_property = PROP_LOG_DEVICE_OBJECT_PROPERTY;
        rpdata.application_data = apdu;
        rpdata.application_data_len = sizeof(apdu);
        
        apdu_len = Trend_Log_Read_Property(&rpdata);
        if (apdu_len > 0) {
            BACNET_DEVICE_OBJECT_PROPERTY_REFERENCE source_ref;
            int len = bacnet_device_object_property_reference_decode(
                rpdata.application_data,
                rpdata.application_data_len,
                &source_ref);
            
            if (len > 0) {
                json_t *source_obj = json_object();
                json_object_set_new(source_obj, "type", 
                    json_string(bactext_object_type_name(source_ref.objectIdentifier.type)));
                json_object_set_new(source_obj, "instance", 
                    json_integer(source_ref.objectIdentifier.instance));
                json_object_set_new(tl_obj, "linked_object", source_obj);
                
                printf("← %s[%u]",
                       bactext_object_type_name(source_ref.objectIdentifier.type),
                       source_ref.objectIdentifier.instance);
            }
        }
        
        printf("\n");
        json_array_append_new(trendlogs_array, tl_obj);
    }
    
    json_object_set_new(root, "trendlogs", trendlogs_array);
    json_object_set_new(root, "count", json_integer(count));
    
    /* Afficher le JSON */
    char *json_str = json_dumps(root, JSON_INDENT(2));
    printf("\n%s\n", json_str);
    free(json_str);
    json_decref(root);
    
    return 0;
}

/* ========================================
 * Commande: trendlog <instance>
 * Détails d'un Trendlog spécifique
 * ========================================*/
static int handle_cmd_trendlog(uint32_t instance)
{
    json_t *root;
    BACNET_READ_PROPERTY_DATA rpdata;
    uint8_t apdu[MAX_APDU];
    int apdu_len;
    char *json_str;
    size_t i;  /* CORRECTION : Déclarer i ici, pas dans le for */
    
    struct {
        BACNET_PROPERTY_ID prop_id;
        const char *prop_name;
        const char *display_name;
    } properties[] = {
        {PROP_OBJECT_NAME, "object_name", "Name"},
        {PROP_ENABLE, "enabled", "Enabled"},
        {PROP_STOP_WHEN_FULL, "stop_when_full", "Stop When Full"},
        {PROP_BUFFER_SIZE, "buffer_size", "Buffer Size"},
        {PROP_RECORD_COUNT, "record_count", "Record Count"},
        {PROP_TOTAL_RECORD_COUNT, "total_record_count", "Total Records"},
        {PROP_LOGGING_TYPE, "logging_type", "Logging Type"},
        {PROP_LOG_INTERVAL, "log_interval", "Log Interval"},
        {PROP_ALIGN_INTERVALS, "align_intervals", "Align Intervals"},
        {PROP_LOG_DEVICE_OBJECT_PROPERTY, "linked_object", "Linked Object"}
    };
    
    root = json_object();
    
    if (!Trend_Log_Valid_Instance(instance)) {
        fprintf(stderr, "ERROR: Trendlog instance %u not valid\n", instance);
        json_object_set_new(root, "error", json_string("Invalid instance"));
        
        json_str = json_dumps(root, JSON_INDENT(2));
        printf("%s\n", json_str);
        free(json_str);
        json_decref(root);
        return -1;
    }
    
    memset(&rpdata, 0, sizeof(rpdata));
    memset(apdu, 0, sizeof(apdu));
    
    printf("========== Trendlog %u Details ==========\n", instance);
    json_object_set_new(root, "instance", json_integer(instance));
    
    /* CORRECTION : Pas de size_t i dans le for */
    for (i = 0; i < sizeof(properties) / sizeof(properties[0]); i++) {
        rpdata.object_type = OBJECT_TRENDLOG;
        rpdata.object_instance = instance;
        rpdata.object_property = properties[i].prop_id;
        rpdata.array_index = BACNET_ARRAY_ALL;
        rpdata.application_data = apdu;
        rpdata.application_data_len = sizeof(apdu);
        
        apdu_len = Trend_Log_Read_Property(&rpdata);
        if (apdu_len > 0) {
            BACNET_APPLICATION_DATA_VALUE value;
            int len = bacapp_decode_application_data(rpdata.application_data,
                                                    rpdata.application_data_len,
                                                    &value);
            
            if (len > 0) {
                printf("  %-20s: ", properties[i].display_name);
                
                switch (value.tag) {
                    case BACNET_APPLICATION_TAG_BOOLEAN:
                        json_object_set_new(root, properties[i].prop_name, 
                                          json_boolean(value.type.Boolean));
                        printf("%s\n", value.type.Boolean ? "TRUE" : "FALSE");
                        break;
                        
                    case BACNET_APPLICATION_TAG_UNSIGNED_INT:
                        if (properties[i].prop_id == PROP_LOG_INTERVAL) {
                            uint32_t seconds = value.type.Unsigned_Int / 100;
                            json_object_set_new(root, properties[i].prop_name, 
                                              json_integer(seconds));
                            printf("%u seconds (%u cs)\n", seconds, 
                                   (unsigned int)value.type.Unsigned_Int);
                        } else {
                            json_object_set_new(root, properties[i].prop_name, 
                                              json_integer(value.type.Unsigned_Int));
                            printf("%u\n", (unsigned int)value.type.Unsigned_Int);
                        }
                        break;
                        
                    case BACNET_APPLICATION_TAG_ENUMERATED:
                        json_object_set_new(root, properties[i].prop_name, 
                                          json_integer(value.type.Enumerated));
                        if (properties[i].prop_id == PROP_LOGGING_TYPE) {
                            const char *type_str = "UNKNOWN";
                            switch (value.type.Enumerated) {
                                case LOGGING_TYPE_POLLED: type_str = "POLLED"; break;
                                case LOGGING_TYPE_COV: type_str = "COV"; break;
                                case LOGGING_TYPE_TRIGGERED: type_str = "TRIGGERED"; break;
                            }
                            printf("%s (%u)\n", type_str, (unsigned int)value.type.Enumerated);
                        } else {
                            printf("%u\n", (unsigned int)value.type.Enumerated);
                        }
                        break;
                        
                    case BACNET_APPLICATION_TAG_CHARACTER_STRING:
                        {
                            char name_buf[256];
                            characterstring_ansi_copy(name_buf, sizeof(name_buf), 
                                                     &value.type.Character_String);
                            json_object_set_new(root, properties[i].prop_name, 
                                              json_string(name_buf));
                            printf("%s\n", name_buf);
                        }
                        break;
                        
                    default:
                        if (properties[i].prop_id == PROP_LOG_DEVICE_OBJECT_PROPERTY) {
                            BACNET_DEVICE_OBJECT_PROPERTY_REFERENCE source_ref;
                            len = bacnet_device_object_property_reference_decode(
                                rpdata.application_data,
                                rpdata.application_data_len,
                                &source_ref);
                            
                            if (len > 0) {
                                json_t *source_obj = json_object();
                                json_object_set_new(source_obj, "type", 
                                    json_string(bactext_object_type_name(source_ref.objectIdentifier.type)));
                                json_object_set_new(source_obj, "instance", 
                                    json_integer(source_ref.objectIdentifier.instance));
                                json_object_set_new(root, properties[i].prop_name, source_obj);
                                
                                printf("%s[%u].PRESENT_VALUE\n",
                                       bactext_object_type_name(source_ref.objectIdentifier.type),
                                       source_ref.objectIdentifier.instance);
                            }
                        }
                        break;
                }
            }
        }
    }
    
    json_str = json_dumps(root, JSON_INDENT(2));
    printf("\n%s\n", json_str);
    free(json_str);
    json_decref(root);
    
    return 0;
}

/* ========================================
 * Commande: trendlog-data <instance> [count]
 * Récupérer les données loggées
 * ========================================*/
static int handle_cmd_trendlog_data(uint32_t instance, int count)
{
    json_t *root = json_object();
    json_t *data_array = json_array();
    
    if (!Trend_Log_Valid_Instance(instance)) {
        fprintf(stderr, "ERROR: Trendlog instance %u not valid\n", instance);
        json_object_set_new(root, "error", json_string("Invalid instance"));
        
        char *json_str = json_dumps(root, JSON_INDENT(2));
        printf("%s\n", json_str);
        free(json_str);
        json_decref(root);
        return -1;
    }
    
    if (count <= 0) count = 10;  /* Par défaut, 10 dernières entrées */
    if (count > 100) count = 100;  /* Maximum 100 entrées */
    
    printf("========== Trendlog %u Data (last %d entries) ==========\n", instance, count);
    
    json_object_set_new(root, "instance", json_integer(instance));
    json_object_set_new(root, "count", json_integer(count));
    
    /* Utiliser ReadRange pour récupérer les données */
    BACNET_READ_RANGE_DATA rr_data = {0};
    uint8_t apdu[MAX_APDU] = {0};
    
    rr_data.object_type = OBJECT_TRENDLOG;
    rr_data.object_instance = instance;
    rr_data.object_property = PROP_LOG_BUFFER;
    rr_data.array_index = BACNET_ARRAY_ALL;
    
    /* Lire par position : les dernières 'count' entrées */
    rr_data.RequestType = RR_BY_POSITION;
    rr_data.Range.RefIndex = -count;  /* Négatif = depuis la fin */
    rr_data.Count = count;
    rr_data.Overhead = 50;  /* Overhead APDU */
    
    int len = rr_trend_log_encode(apdu, &rr_data);
    
    if (len > 0) {
        printf("Retrieved %u entries:\n\n", rr_data.ItemCount);
        json_object_set_new(root, "retrieved_count", json_integer(rr_data.ItemCount));
        
        /* TODO: Parser les données du buffer APDU */
        /* Format complexe, nécessite décodage complet */
        printf("(Full data parsing requires APDU decoding - see LOG_BUFFER property)\n");
        
    } else {
        printf("No data available or error reading buffer\n");
        json_object_set_new(root, "retrieved_count", json_integer(0));
    }
    
    json_object_set_new(root, "data", data_array);
    
    /* Afficher le JSON */
    char *json_str = json_dumps(root, JSON_INDENT(2));
    printf("\n%s\n", json_str);
    free(json_str);
    json_decref(root);
    
    return 0;
}

/* ========================================
 * Commande: trendlog-enable <instance> <true|false>
 * Activer/désactiver un Trendlog
 * ========================================*/
static int handle_cmd_trendlog_enable(uint32_t instance, bool enable)
{
    json_t *root;
    BACNET_WRITE_PROPERTY_DATA wp_data;
    BACNET_APPLICATION_DATA_VALUE value;
    int len;
    char *json_str;
    
    root = json_object();
    
    if (!Trend_Log_Valid_Instance(instance)) {
        fprintf(stderr, "ERROR: Trendlog instance %u not valid\n", instance);
        json_object_set_new(root, "error", json_string("Invalid instance"));
        
        json_str = json_dumps(root, JSON_INDENT(2));
        printf("%s\n", json_str);
        free(json_str);
        json_decref(root);
        return -1;
    }
    
    /* Initialiser les structures */
    memset(&wp_data, 0, sizeof(wp_data));
    memset(&value, 0, sizeof(value));
    
    /* Préparer la valeur à écrire */
    value.tag = BACNET_APPLICATION_TAG_BOOLEAN;
    value.type.Boolean = enable;
    
    /* Encoder directement dans wp_data.application_data */
    len = bacapp_encode_application_data(wp_data.application_data, &value);
    
    /* Configurer le Write Property */
    wp_data.object_type = OBJECT_TRENDLOG;
    wp_data.object_instance = instance;
    wp_data.object_property = PROP_ENABLE;
    wp_data.array_index = BACNET_ARRAY_ALL;
    wp_data.application_data_len = len;
    
    /* Écrire la propriété */
    if (Trend_Log_Write_Property(&wp_data)) {
        printf("✓ Trendlog %u %s successfully\n", instance, enable ? "ENABLED" : "DISABLED");
        json_object_set_new(root, "success", json_boolean(true));
        json_object_set_new(root, "instance", json_integer(instance));
        json_object_set_new(root, "enabled", json_boolean(enable));
    } else {
        fprintf(stderr, "✗ Failed to %s Trendlog %u\n", enable ? "enable" : "disable", instance);
        fprintf(stderr, "  Error: class=%d code=%d\n", wp_data.error_class, wp_data.error_code);
        json_object_set_new(root, "success", json_boolean(false));
        json_object_set_new(root, "error_class", json_integer(wp_data.error_class));
        json_object_set_new(root, "error_code", json_integer(wp_data.error_code));
    }
    
    json_str = json_dumps(root, JSON_INDENT(2));
    printf("\n%s\n", json_str);
    free(json_str);
    json_decref(root);
    
    return 0;
}

/* ========================================
 * Commande: trendlog-clear <instance>
 * Effacer le buffer d'un Trendlog
 * ========================================*/
static int handle_cmd_trendlog_clear(uint32_t instance)
{
    json_t *root;
    BACNET_WRITE_PROPERTY_DATA wp_data;
    BACNET_APPLICATION_DATA_VALUE value;
    int len;
    char *json_str;
    
    root = json_object();
    
    if (!Trend_Log_Valid_Instance(instance)) {
        fprintf(stderr, "ERROR: Trendlog instance %u not valid\n", instance);
        json_object_set_new(root, "error", json_string("Invalid instance"));
        
        json_str = json_dumps(root, JSON_INDENT(2));
        printf("%s\n", json_str);
        free(json_str);
        json_decref(root);
        return -1;
    }
    
    /* Initialiser les structures */
    memset(&wp_data, 0, sizeof(wp_data));
    memset(&value, 0, sizeof(value));
    
    /* Préparer la valeur à écrire (0 = effacer le buffer) */
    value.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
    value.type.Unsigned_Int = 0;
    
    /* Encoder directement dans wp_data.application_data */
    len = bacapp_encode_application_data(wp_data.application_data, &value);
    
    /* Configurer le Write Property */
    wp_data.object_type = OBJECT_TRENDLOG;
    wp_data.object_instance = instance;
    wp_data.object_property = PROP_RECORD_COUNT;
    wp_data.array_index = BACNET_ARRAY_ALL;
    wp_data.application_data_len = len;
    
    /* Écrire la propriété */
    if (Trend_Log_Write_Property(&wp_data)) {
        printf("✓ Trendlog %u buffer cleared successfully\n", instance);
        json_object_set_new(root, "success", json_boolean(true));
        json_object_set_new(root, "instance", json_integer(instance));
        json_object_set_new(root, "message", json_string("Buffer cleared"));
    } else {
        fprintf(stderr, "✗ Failed to clear Trendlog %u buffer\n", instance);
        fprintf(stderr, "  Error: class=%d code=%d\n", wp_data.error_class, wp_data.error_code);
        json_object_set_new(root, "success", json_boolean(false));
        json_object_set_new(root, "error_class", json_integer(wp_data.error_class));
        json_object_set_new(root, "error_code", json_integer(wp_data.error_code));
    }
    
    json_str = json_dumps(root, JSON_INDENT(2));
    printf("\n%s\n", json_str);
    free(json_str);
    json_decref(root);
    
    return 0;
}


static int handle_socket_line(const char *line)
{
    char cmd[64];
    
    if (sscanf(line, "%63s", cmd) != 1) {
        return 0;
    }
    if (strncmp(line, "PING", 4) == 0) {
        (void)write(g_client_fd, "PONG\n", 5);
        return 0;
    }
    if (strncmp(line, "QUIT", 4) == 0) {
        (void)write(g_client_fd, "BYE\n", 4);
        return 1;
    }
    if (strncmp(line, "PIDFILE ", 8) == 0) {
        const char *path = line + 8;
        while (*path == ' ') path++;
        if (*path) {
            strncpy(g_pidfile, path, sizeof(g_pidfile)-1);
            g_pidfile[sizeof(g_pidfile)-1] = '\0';
            write_pidfile_if_needed();
            (void)write(g_client_fd, "OK\n", 3);
        } else {
            (void)write(g_client_fd, "ERR missing path\n", 17);
        }
        return 0;
    }
    if (strncmp(line, "CFGJSON ", 8) == 0) {
        const char *json = line + 8;
        int rc = apply_config_from_json(json);
        if (rc == 0) (void)write(g_client_fd, "OK\n", 3);
        else         (void)write(g_client_fd, "ERR\n", 4);
        return 0;
    }
    if (strncmp(line, "STATUS", 6) == 0) {
        char buf[1024];
        snprintf(buf, sizeof(buf),
                 "{\"deviceId\":%u,\"deviceName\":\"%s\",\"objects\":{\"AI\":%u,\"AO\":%u,\"AV\":%u,\"BI\":%u,\"BO\":%u,\"BV\":%u,\"MSI\":%u,\"MSO\":%u,\"MSV\":%u,\"SCH\":%u}}\n",
                 Device_Object_Instance_Number(),
                 Device_Object_Name_ANSI(),
                 Analog_Input_Count(),
                 Analog_Output_Count(),
                 Analog_Value_Count(),
                 Binary_Input_Count(),
                 Binary_Output_Count(),
                 Binary_Value_Count(),
                 Multistate_Input_Count(),
                 Multistate_Output_Count(),
                 Multistate_Value_Count(),
                 Schedule_Count());
        (void)write(g_client_fd, buf, strlen(buf));
        return 0;
    }
     if (strcmp(cmd, "trendlogs") == 0) {
        handle_cmd_trendlogs();
        return false;
    }
    
    if (strcmp(cmd, "trendlog") == 0) {
        uint32_t instance = 0;
        if (sscanf(line, "trendlog %u", &instance) == 1) {
            handle_cmd_trendlog(instance);
        } else {
            printf("Usage: trendlog <instance>\n");
        }
        return false;
    }
    
    if (strcmp(cmd, "trendlog-data") == 0) {
        uint32_t instance = 0;
        int count = 10;
        int n = sscanf(line, "trendlog-data %u %d", &instance, &count);
        if (n >= 1) {
            handle_cmd_trendlog_data(instance, count);
        } else {
            printf("Usage: trendlog-data <instance> [count]\n");
        }
        return false;
    }
    
    if (strcmp(cmd, "trendlog-enable") == 0) {
        uint32_t instance = 0;
        char enable_str[10];
        if (sscanf(line, "trendlog-enable %u %9s", &instance, enable_str) == 2) {
            bool enable = (strcmp(enable_str, "true") == 0 || strcmp(enable_str, "1") == 0);
            handle_cmd_trendlog_enable(instance, enable);
        } else {
            printf("Usage: trendlog-enable <instance> <true|false>\n");
        }
        return false;
    }
    
    if (strcmp(cmd, "trendlog-clear") == 0) {
        uint32_t instance = 0;
        if (sscanf(line, "trendlog-clear %u", &instance) == 1) {
            handle_cmd_trendlog_clear(instance);
        } else {
            printf("Usage: trendlog-clear <instance>\n");
        }
        return false;
    }
    (void)write(g_client_fd, "ERR unknown\n", 12);
    return 0;
}

static void process_socket_io(void)
{
    if (g_listen_fd >= 0 && g_client_fd < 0) {
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd >= 0) {
            g_client_fd = cfd;
            g_cmd_len = 0;
        }
    }
    if (g_client_fd >= 0) {
        char buf[1024];
        ssize_t r = read(g_client_fd, buf, sizeof(buf));
        if (r == 0) {
            close(g_client_fd);
            g_client_fd = -1;
            g_cmd_len = 0;
        } else if (r > 0) {
            size_t i;
            for (i = 0; i < (size_t)r; i++) {
                char ch = buf[i];
                if (g_cmd_len + 1 < sizeof(g_cmd_buf)) {
                    g_cmd_buf[g_cmd_len++] = ch;
                    g_cmd_buf[g_cmd_len] = '\0';
                }
                if (ch == '\n') {
                    char line[8192];
                    size_t linelen = g_cmd_len;
                    if (linelen > 0) linelen--;
                    if (linelen >= sizeof(line)) linelen = sizeof(line)-1;
                    memcpy(line, g_cmd_buf, linelen);
                    line[linelen] = '\0';
                    trim_newlines(line);

                    if (handle_socket_line(line)) {
                        close(g_client_fd);
                        g_client_fd = -1;
                    }
                    g_cmd_len = 0;
                }
            }
        }
    }
}

/* ===== Initialisation ===== */
static void Init_Service_Handlers(void)
{
    Device_Init(My_Object_Table);
    
    Init_Schedules();
    
    printf("BACnet server initialized (0 objects)\n");
    printf("  AI: %u, AO: %u, AV: %u\n", 
           Analog_Input_Count(), Analog_Output_Count(), Analog_Value_Count());
    printf("  BI: %u, BO: %u, BV: %u\n", 
           Binary_Input_Count(), Binary_Output_Count(), Binary_Value_Count());
    printf("  MSI: %u, MSO: %u, MSV: %u\n", 
           Multistate_Input_Count(), Multistate_Output_Count(), Multistate_Value_Count());
    printf("  SCH: %u\n", Schedule_Count());

    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, handler_who_has);
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);

    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE, handler_read_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROPERTY, my_handler_write_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROP_MULTIPLE, handler_write_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_RANGE, handler_read_range);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_REINITIALIZE_DEVICE, handler_reinitialize_device);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_UTC_TIME_SYNCHRONIZATION, handler_timesync_utc);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_TIME_SYNCHRONIZATION, handler_timesync);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV, handler_cov_subscribe);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_COV_NOTIFICATION, handler_ucov_notification);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_DEVICE_COMMUNICATION_CONTROL, handler_device_communication_control);

    mstimer_set(&BACNET_Task_Timer, 1000UL);
    mstimer_set(&Schedule_PV_Timer, 60UL * 1000UL);
    mstimer_set(&BACNET_TSM_Timer, 50UL);
    mstimer_set(&BACNET_Address_Timer, 60UL * 1000UL);
    mstimer_set(&Config_Save_Timer, 30UL * 1000UL);
}

/* ===== CLI ===== */
static void print_usage(const char *filename)
{
    printf("Usage: %s [device-instance [device-name]] [--socketport N] [--pid PATH] [--config PATH]\n", filename);
    printf("  device-instance: BACnet Device Instance Number (default: 260001)\n");
    printf("  device-name: BACnet Device Name (default: bacnetStackServer)\n");
    printf("Options:\n");
    printf("  --socketport N:  Port TCP pour socket JSON (défaut: 55031)\n");
    printf("  --pid PATH:      Fichier PID\n");
    printf("  --config PATH:   Fichier de configuration JSON pour persistance\n");
}

static void clear_all_objects(void)
{
    unsigned int i;
    
    printf("=== Clearing initialization objects ===\n");
    
    /* Vider les Schedules */
    printf("Clearing all %d Schedules...\n", MAX_SCHEDULES);
    for (i = 0; i < MAX_SCHEDULES; i++) {
        memset(&Schedule_Descr[i], 0, sizeof(SCHEDULE_DESCR));
    }
    printf("  Schedules cleared: Schedule_Count() = %u\n", Schedule_Count());
    
    /* AJOUT : Vider les Trendlogs */
    printf("Clearing all %d Trendlogs...\n", MAX_TREND_LOGS);
    /* Note: Trend_Log n'a pas de tableau externe comme Schedule_Descr
     * Donc on ne peut pas faire memset directement.
     * Heureusement, avec Trend_Log_Init_Empty(), ils ne sont pas initialisés !
     */
    printf("  Trendlogs cleared: Trend_Log_Count() = %u\n", Trend_Log_Count());
    
    printf("=== Initialization objects cleared ===\n");
}
/* ===== MAIN ===== */
int main(int argc, char *argv[])
{
    BACNET_ADDRESS src = { 0 };
    uint16_t pdu_len = 0;
    unsigned timeout = 100;
    const char *device_name = "bacnetStackServer";
    uint32_t device_instance = 260001;
    const char *envp;
    int argi = 1;
    char buf[256];
    static uint16_t trendlog_seconds = 0; 
    printf("BACnet Stack Server (Jeedom)\n");
    printf("Version: %s\n", BACNET_VERSION_TEXT);

    if (argi < argc && argv[argi][0] != '-') {
        device_instance = strtoul(argv[argi], NULL, 0);
        argi++;
    }
    if (argi < argc && argv[argi][0] != '-') {
        device_name = argv[argi];
        argi++;
    }

    while (argi < argc) {
        if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[argi], "--socketport") == 0 && (argi + 1) < argc) {
            g_socket_port = atoi(argv[++argi]);
        } else if (strcmp(argv[argi], "--pid") == 0 && (argi + 1) < argc) {
            strncpy(g_pidfile, argv[++argi], sizeof(g_pidfile)-1);
            g_pidfile[sizeof(g_pidfile)-1] = '\0';
        } else if (strcmp(argv[argi], "--config") == 0 && (argi + 1) < argc) {
            strncpy(g_config_file, argv[++argi], sizeof(g_config_file)-1);
            g_config_file[sizeof(g_config_file)-1] = '\0';
            printf("Config file path set to: %s\n", g_config_file);
        }
        argi++;
    }

    envp = getenv("BACSTACK_SOCKET_PORT");
    if (envp) {
        g_socket_port = atoi(envp);
    }

    Device_Set_Object_Instance_Number(device_instance);  
    snprintf(buf, sizeof(buf), "Device ID: %u", device_instance);
    print_timestamp_log(buf);

    dlenv_init();
    Init_Service_Handlers();
    atexit(datalink_cleanup);

    Device_Object_Name_ANSI_Init(device_name);
    snprintf(buf, sizeof(buf), "Device Name: %s", device_name);
    print_timestamp_log(buf);

    g_listen_fd = socket_listen_local(g_socket_port);
    if (g_listen_fd >= 0) {
        printf("Control socket: 127.0.0.1:%d\n", g_socket_port);
    } else {
        printf("Control socket disabled (port %d bind error)\n", g_socket_port);
    }

    write_pidfile_if_needed();

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    if (g_config_file[0]) {
        clear_all_objects();
        
        load_config_from_file();
    }

    Send_I_Am(&Rx_Buf[0]);
    printf("I-Am broadcasted\n");

    printf("Entering main loop...\n");
    while (!g_shutdown) {
        pdu_len = datalink_receive(&src, &Rx_Buf[0], MAX_MPDU, timeout);
        if (pdu_len) {
            npdu_handler(&src, &Rx_Buf[0], pdu_len);
        }

        if (mstimer_expired(&BACNET_Task_Timer)) {
            mstimer_reset(&BACNET_Task_Timer);
        }
        if (mstimer_expired(&BACNET_TSM_Timer)) {
            mstimer_reset(&BACNET_TSM_Timer);
            tsm_timer_milliseconds(mstimer_interval(&BACNET_TSM_Timer));
        }
        if (mstimer_expired(&BACNET_Address_Timer)) {
            mstimer_reset(&BACNET_Address_Timer);
            address_cache_timer(mstimer_interval(&BACNET_Address_Timer));
        }
        if (mstimer_expired(&Config_Save_Timer)) {
            mstimer_reset(&Config_Save_Timer);
            if (g_config_file[0]) {
                save_current_config();
            }
        }
        if (mstimer_expired(&Schedule_PV_Timer)) {
            BACNET_TIME time_of_day;
            BACNET_WEEKDAY wday;
            unsigned sc_count;
            unsigned i;
            time_t now;
            struct tm *lt;

            mstimer_reset(&Schedule_PV_Timer);

            now = time(NULL);
            lt = localtime(&now);
            
            time_of_day.hour = (uint8_t)lt->tm_hour;
            time_of_day.min = (uint8_t)lt->tm_min;
            time_of_day.sec = (uint8_t)lt->tm_sec;
            time_of_day.hundredths = 0;
            
            if (lt->tm_wday == 0) {
                wday = (BACNET_WEEKDAY)7;
            } else {
                wday = (BACNET_WEEKDAY)lt->tm_wday; 
            }

            sc_count = Schedule_Count();
            for (i = 0; i < sc_count; i++) {
                uint32_t inst = Schedule_Index_To_Instance(i);
                SCHEDULE_DESCR *desc = Schedule_Object(inst);
                if (desc) {
                    Schedule_Recalculate_PV(desc, wday, &time_of_day);
                }
            }
        }

        trendlog_seconds++;
        if (trendlog_seconds >= 60) {
            trend_log_timer(60); 
            trendlog_seconds = 0;
        }




        process_socket_io();
    }

    printf("Shutting down...\n");
    socket_close_all();
    if (g_pidfile[0]) {
        unlink(g_pidfile);
    }

    return EXIT_SUCCESS;
}