/**
 * @file trendlog_override.c
 * @brief Implémentation personnalisée pour la gestion des Trendlogs BACnet Stack
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "bacnet/basic/object/trendlog.h"
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/object/ai.h"
#include "bacnet/basic/object/ao.h"
#include "bacnet/basic/object/av.h"
#include "bacnet/basic/object/bi.h"
#include "bacnet/basic/object/bo.h"
#include "bacnet/basic/object/bv.h"
#include "bacnet/wp.h"
#include "bacnet/rp.h"
#include "bacnet/bacapp.h"
#include "bacnet/bacdcode.h"
#include "trendlog_override.h"

/**
 * @brief Lecture sécurisée d'une propriété d'objet pour les trendlogs
 * @return longueur des données lues, ou -1 en cas d'erreur
 */
static int safe_read_property_for_trendlog(
    uint8_t *value,
    const BACNET_DEVICE_OBJECT_PROPERTY_REFERENCE *Source,
    BACNET_ERROR_CLASS *error_class,
    BACNET_ERROR_CODE *error_code)
{
    int len = 0;
    BACNET_READ_PROPERTY_DATA rpdata;
    

    printf("[TL_OVERRIDE] Reading property from OBJECT_TYPE_%d[%u].PROPERTY_%u\n",
           Source->objectIdentifier.type,
           Source->objectIdentifier.instance,
           Source->propertyIdentifier);
    fflush(stdout);

    if (value == NULL) {
        printf("[TL_OVERRIDE] ERROR: value buffer is NULL\n");
        fflush(stdout);
        *error_class = ERROR_CLASS_SERVICES;
        *error_code = ERROR_CODE_OTHER;
        return -1;
    }

    /* Vérification que l'objet source existe vraiment */
    bool object_exists = false;
    switch (Source->objectIdentifier.type) {
        case OBJECT_ANALOG_INPUT:
            object_exists = Analog_Input_Valid_Instance(Source->objectIdentifier.instance);
            break;
        case OBJECT_ANALOG_OUTPUT:
            object_exists = Analog_Output_Valid_Instance(Source->objectIdentifier.instance);
            break;
        case OBJECT_ANALOG_VALUE:
            object_exists = Analog_Value_Valid_Instance(Source->objectIdentifier.instance);
            break;
        case OBJECT_BINARY_INPUT:
            object_exists = Binary_Input_Valid_Instance(Source->objectIdentifier.instance);
            break;
        case OBJECT_BINARY_OUTPUT:
            object_exists = Binary_Output_Valid_Instance(Source->objectIdentifier.instance);
            break;
        case OBJECT_BINARY_VALUE:
            object_exists = Binary_Value_Valid_Instance(Source->objectIdentifier.instance);
            break;
        default:
            printf("[TL_OVERRIDE] ERROR: Unsupported object type %d\n", 
                   Source->objectIdentifier.type);
            fflush(stdout);
            object_exists = false;
            break;
    }
    
    if (!object_exists) {
        printf("[TL_OVERRIDE] ERROR: Source object does not exist!\n");
        fflush(stdout);
        *error_class = ERROR_CLASS_OBJECT;
        *error_code = ERROR_CODE_UNKNOWN_OBJECT;
        return -1;
    }

    /* Configure la structure de lecture */
    memset(&rpdata, 0, sizeof(rpdata));
    rpdata.application_data = value;
    rpdata.application_data_len = MAX_APDU;
    rpdata.object_type = Source->objectIdentifier.type;
    rpdata.object_instance = Source->objectIdentifier.instance;
    rpdata.object_property = Source->propertyIdentifier;
    rpdata.array_index = Source->arrayIndex;
    
    printf("[TL_OVERRIDE] Calling Device_Read_Property()...\n");
    fflush(stdout);
    
    /* Lecture de la propriété */
    len = Device_Read_Property(&rpdata);
    
    printf("[TL_OVERRIDE] Device_Read_Property() returned len=%d\n", len);
    fflush(stdout);
    
    if (len < 0) {
        printf("[TL_OVERRIDE] ERROR: Device_Read_Property failed, error_class=%d, error_code=%d\n",
               rpdata.error_class, rpdata.error_code);
        fflush(stdout);
        *error_class = rpdata.error_class;
        *error_code = rpdata.error_code;
        return -1;
    }
    
    printf("[TL_OVERRIDE] Successfully read %d bytes\n", len);
    fflush(stdout);
    
    return len;
}

/**
 * @brief Vide un Trendlog spécifique en utilisant l'API publique
 */
static void clear_single_trendlog(uint32_t instance)
{
    BACNET_WRITE_PROPERTY_DATA wp_data;
    BACNET_APPLICATION_DATA_VALUE value;
    int len;
    
    if (!Trend_Log_Valid_Instance(instance)) {
        return;
    }
    
    memset(&wp_data, 0, sizeof(wp_data));
    memset(&value, 0, sizeof(value));
    
    wp_data.object_type = OBJECT_TRENDLOG;
    wp_data.object_instance = instance;
    wp_data.array_index = BACNET_ARRAY_ALL;
    
    /* Désactiver */
    value.tag = BACNET_APPLICATION_TAG_BOOLEAN;
    value.type.Boolean = false;
    
    len = bacapp_encode_application_data(wp_data.application_data, &value);
    wp_data.object_property = PROP_ENABLE;
    wp_data.application_data_len = len;
    
    Trend_Log_Write_Property(&wp_data);
    
    /* Effacer buffer */
    memset(&value, 0, sizeof(value));
    value.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
    value.type.Unsigned_Int = 0;
    
    len = bacapp_encode_application_data(wp_data.application_data, &value);
    wp_data.object_property = PROP_RECORD_COUNT;
    wp_data.application_data_len = len;
    
    Trend_Log_Write_Property(&wp_data);
}

/**
 * @brief Vide tous les Trendlogs
 */
void clear_all_trendlogs(void)
{
    uint32_t i;
    
    for (i = 0; i < MAX_TREND_LOGS; i++) {
        clear_single_trendlog(i);
    }
}

/**
 * @brief Fonction stub pour Trendlog_Delete
 */
bool Trendlog_Delete(uint32_t object_instance)
{
    (void)object_instance;
    return false;
}

/**
 * @brief Fonction stub pour supprimer tous les Trendlogs
 */
void Trendlog_Delete_All(void)
{
    clear_all_trendlogs();
}

/**
 * @brief Teste si un trendlog peut lire sa source sans crasher
 * @param instance Instance du Trendlog à tester
 * @return true si la lecture fonctionne, false sinon
 */
bool Trendlog_Test_Source_Read(uint32_t instance)
{
    uint8_t test_buffer[MAX_APDU];
    BACNET_ERROR_CLASS error_class;
    BACNET_ERROR_CODE error_code;
    TL_LOG_INFO *log_info;
    int len;
    
    printf("\n[TL_OVERRIDE] ========================================\n");
    printf("[TL_OVERRIDE] Testing source read for Trendlog %u\n", instance);
    printf("[TL_OVERRIDE] ========================================\n");
    fflush(stdout);
    
    if (!Trend_Log_Valid_Instance(instance)) {
        printf("[TL_OVERRIDE] ERROR: Invalid trendlog instance\n");
        fflush(stdout);
        return false;
    }
    printf("[TL_OVERRIDE] Instance %u is valid\n", instance);
    fflush(stdout);
    
    printf("[TL_OVERRIDE] Calling Trend_Log_Get_Info(%u)...\n", instance);
    fflush(stdout);
    
    log_info = Trend_Log_Get_Info(instance);
    
    printf("[TL_OVERRIDE] Trend_Log_Get_Info() returned: %p\n", (void*)log_info);
    fflush(stdout);
    
    if (log_info == NULL) {
        printf("[TL_OVERRIDE] ERROR: Could not get log info\n");
        fflush(stdout);
        return false;
    }
    
    printf("[TL_OVERRIDE] Got log info successfully\n");
    fflush(stdout);
    
    printf("[TL_OVERRIDE] Accessing Source structure...\n");
    fflush(stdout);
    
    printf("[TL_OVERRIDE] Source type: %d\n", log_info->Source.objectIdentifier.type);
    fflush(stdout);
    
    printf("[TL_OVERRIDE] Source instance: %u\n", log_info->Source.objectIdentifier.instance);
    fflush(stdout);
    
    printf("[TL_OVERRIDE] Source property: %u\n", log_info->Source.propertyIdentifier);
    fflush(stdout);
    

    printf("[TL_OVERRIDE] Source: OBJECT_TYPE_%d[%u].PROPERTY_%u\n",
           log_info->Source.objectIdentifier.type,
           log_info->Source.objectIdentifier.instance,
           log_info->Source.propertyIdentifier);
    fflush(stdout);
    
    /* Tentative de lecture sécurisée */
    len = safe_read_property_for_trendlog(
        test_buffer,
        &log_info->Source,
        &error_class,
        &error_code
    );
    
    if (len < 0) {
        printf("[TL_OVERRIDE] ✗ TEST FAILED: Cannot read source property\n");
        printf("[TL_OVERRIDE]   Error: class=%d, code=%d\n", error_class, error_code);
        printf("[TL_OVERRIDE] ========================================\n\n");
        fflush(stdout);
        return false;
    }
    
    printf("[TL_OVERRIDE] ✓ TEST PASSED: Successfully read %d bytes\n", len);
    printf("[TL_OVERRIDE] ========================================\n\n");
    fflush(stdout);
    
    return true;
}