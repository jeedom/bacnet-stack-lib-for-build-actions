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

/* Prototype pour initialisation schedules */
static void Init_Schedules(void);

/* Prototype nécessaire pour save_current_config */
static int save_current_config(void);

static void Schedule_Init_Empty(void)
{
    printf("Schedule_Init_Empty: Schedules will be created from JSON only\n");
    /* Ne rien faire - pas d'initialisation des Schedules */
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
        
        /* Default Value - ENUMERATED EN PREMIER pour les booléens */
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
                /* IMPORTANT: Vérifier ENUMERATED EN PREMIER pour les booléens */
                if (default_val.tag == BACNET_APPLICATION_TAG_ENUMERATED) {
                    json_object_set_new(obj, "defaultValue", json_integer(default_val.type.Enumerated));
                } else if (default_val.tag == BACNET_APPLICATION_TAG_REAL) {
                    json_object_set_new(obj, "defaultValue", json_real(default_val.type.Real));
                } else if (default_val.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT) {
                    json_object_set_new(obj, "defaultValue", json_integer(default_val.type.Unsigned_Int));
                } else if (default_val.tag == BACNET_APPLICATION_TAG_SIGNED_INT) {
                    json_object_set_new(obj, "defaultValue", json_integer(default_val.type.Signed_Int));
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
        
        /* Weekly Schedule - avec gestion correcte des booléens */
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
                            
                            /* ENUMERATED en premier pour les booléens */
                            if (value_val.tag == BACNET_APPLICATION_TAG_ENUMERATED) {
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
/* ============================================
 * SECTION SCHEDULE dans apply_config_from_json()
 * REMPLACEZ tout le bloc "else if (strcmp(typ, "schedule") == 0)"
 * ============================================ */

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
                printf("  Schedule name: '%s' (names must be set via BACnet WriteProperty)\n", name);
            }
            
            /* Configuration de defaultValue - MAINTENANT POSSIBLE ! */
            default_value = json_object_get(it, "defaultValue");
            if (json_is_number(default_value)) {
                BACNET_APPLICATION_DATA_VALUE app_value;
                BACNET_WRITE_PROPERTY_DATA wp_data;
                uint8_t apdu[MAX_APDU];
                int apdu_len;
                
                memset(&app_value, 0, sizeof(app_value));
                memset(&wp_data, 0, sizeof(wp_data));
                
                val = json_number_value(default_value);
                
                /* Détecter si c'est un booléen (0 ou 1) */
                if ((val == 0.0 || val == 1.0) && json_is_integer(default_value)) {
                    app_value.tag = BACNET_APPLICATION_TAG_ENUMERATED;
                    app_value.type.Enumerated = (uint32_t)val;
                    printf("  Setting default value: %u (BOOLEAN/ENUMERATED)\n", app_value.type.Enumerated);
                } else if (json_is_real(default_value)) {
                    app_value.tag = BACNET_APPLICATION_TAG_REAL;
                    app_value.type.Real = (float)val;
                    printf("  Setting default value: %f (REAL)\n", app_value.type.Real);
                } else {
                    app_value.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
                    app_value.type.Unsigned_Int = (uint32_t)val;
                    printf("  Setting default value: %lu (UNSIGNED)\n", (unsigned long)app_value.type.Unsigned_Int);
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
            
            /* Configuration de priority - MAINTENANT POSSIBLE ! */
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
                            
                            if (json_is_string(jtime) && json_is_number(jvalue)) {
                                const char *time_str = json_string_value(jtime);
                                int hour, minute;
                                
                                if (sscanf(time_str, "%d:%d", &hour, &minute) == 2) {
                                    daily.Time_Values[time_idx].Time.hour = (uint8_t)hour;
                                    daily.Time_Values[time_idx].Time.min = (uint8_t)minute;
                                    daily.Time_Values[time_idx].Time.sec = 0;
                                    daily.Time_Values[time_idx].Time.hundredths = 0;
                                    
                                    /* Détecter le type de valeur */
                                    val = json_number_value(jvalue);
                                    if ((val == 0.0 || val == 1.0) && json_is_integer(jvalue)) {
                                        /* Valeur booléenne: utiliser ENUMERATED */
                                        daily.Time_Values[time_idx].Value.tag = BACNET_APPLICATION_TAG_ENUMERATED;
                                        daily.Time_Values[time_idx].Value.type.Enumerated = (uint32_t)val;
                                    } else if (json_is_real(jvalue)) {
                                        daily.Time_Values[time_idx].Value.tag = BACNET_APPLICATION_TAG_REAL;
                                        daily.Time_Values[time_idx].Value.type.Real = (float)val;
                                    } else if (json_is_integer(jvalue)) {
                                        daily.Time_Values[time_idx].Value.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
                                        daily.Time_Values[time_idx].Value.type.Unsigned_Int = (uint32_t)val;
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
                    /* FORCER une période "toujours active" car 2155 = invalide/futur */
                    desc->Start_Date.year = 1900;
                    desc->Start_Date.month = 1;
                    desc->Start_Date.day = 1;
                    desc->Start_Date.wday = BACNET_WEEKDAY_MONDAY;
                    
                    desc->End_Date.year = 2154;
                    desc->End_Date.month = 12;
                    desc->End_Date.day = 31;
                    desc->End_Date.wday = BACNET_WEEKDAY_SUNDAY;
                    
                    printf("  Effective period FORCED: always active (1900-2154)\n");
                    printf("  Effective period FORCED: always active (1900-2154)\n");
                    
                    /* Forcer le premier calcul du Present_Value avec l'heure RÉELLE */
                    {
                        BACNET_TIME time_of_day;
                        BACNET_WEEKDAY wday;
                        time_t now;
                        struct tm *lt;
                        
                        /* Obtenir l'heure SYSTÈME directement */
                        now = time(NULL);
                        lt = localtime(&now);
                        
                        /* Remplir time_of_day avec l'heure réelle */
                        time_of_day.hour = (uint8_t)lt->tm_hour;
                        time_of_day.min = (uint8_t)lt->tm_min;
                        time_of_day.sec = (uint8_t)lt->tm_sec;
                        time_of_day.hundredths = 0;
                        
                        /* Calculer le jour de la semaine */
                        if (lt->tm_wday == 0) {
                            wday = (BACNET_WEEKDAY)7;  /* Dimanche */
                        } else {
                            wday = (BACNET_WEEKDAY)lt->tm_wday;  /* Lundi=1...Samedi=6 */
                        }
                        
                        /* Calculer le Present_Value */
                        Schedule_Recalculate_PV(desc, wday, &time_of_day);
                        
                        /* Afficher le résultat */
                        if (desc->Present_Value.tag == BACNET_APPLICATION_TAG_ENUMERATED) {
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
    printf("  SCH: %u\n", Schedule_Count());
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

static int handle_socket_line(const char *line)
{
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

static void clear_all_schedules(void)
{
    unsigned int i;
    
    printf("Clearing all %d Schedules...\n", MAX_SCHEDULES);
    printf("  Before clear: Schedule_Count() = %u\n", Schedule_Count());
    
    for (i = 0; i < MAX_SCHEDULES; i++) {
        memset(&Schedule_Descr[i], 0, sizeof(SCHEDULE_DESCR));
    }
    
    printf("  After clear: Schedule_Count() = %u\n", Schedule_Count());
    printf("All Schedules cleared\n");
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
        /* Vider les 10 Schedules créés par défaut au démarrage */
        clear_all_schedules();
        
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

/* Mise à jour minute des PRESENT_VALUE des Schedule */
/* Mise à jour minute des PRESENT_VALUE des Schedule */
if (mstimer_expired(&Schedule_PV_Timer)) {
    BACNET_TIME time_of_day;
    BACNET_WEEKDAY wday;
    unsigned sc_count;
    unsigned i;
    time_t now;
    struct tm *lt;

    mstimer_reset(&Schedule_PV_Timer);

    /* Obtenir l'heure SYSTÈME directement (datetime_local ne fonctionne pas) */
    now = time(NULL);
    lt = localtime(&now);
    
    /* Remplir time_of_day avec l'heure réelle */
    time_of_day.hour = (uint8_t)lt->tm_hour;
    time_of_day.min = (uint8_t)lt->tm_min;
    time_of_day.sec = (uint8_t)lt->tm_sec;
    time_of_day.hundredths = 0;
    
    /* Calculer le jour de la semaine */
    if (lt->tm_wday == 0) {
        wday = (BACNET_WEEKDAY)7;  /* Dimanche */
    } else {
        wday = (BACNET_WEEKDAY)lt->tm_wday;  /* Lundi=1...Samedi=6 */
    }

    /* Recalcule Present_Value pour tous les schedules */
    sc_count = Schedule_Count();
    for (i = 0; i < sc_count; i++) {
        uint32_t inst = Schedule_Index_To_Instance(i);
        SCHEDULE_DESCR *desc = Schedule_Object(inst);
        if (desc) {
            Schedule_Recalculate_PV(desc, wday, &time_of_day);
        }
    }
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
