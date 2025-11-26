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
    bool object_exists = false;

    if (value == NULL) {
        *error_class = ERROR_CLASS_SERVICES;
        *error_code = ERROR_CODE_OTHER;
        return -1;
    }

    /* Verify source object exists */
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
            object_exists = false;
            break;
    }
    
    
    if (!object_exists) {
        *error_class = ERROR_CLASS_OBJECT;
        *error_code = ERROR_CODE_UNKNOWN_OBJECT;
        return -1;
    }
    
    /* Setup read property structure */
    memset(&rpdata, 0, sizeof(rpdata));
    rpdata.application_data = value;
    rpdata.application_data_len = MAX_APDU;
    rpdata.object_type = Source->objectIdentifier.type;
    rpdata.object_instance = Source->objectIdentifier.instance;
    rpdata.object_property = Source->propertyIdentifier;
    rpdata.array_index = Source->arrayIndex;
    
    /* Read property */
    len = Device_Read_Property(&rpdata);
    
    if (len < 0) {
        *error_class = rpdata.error_class;
        *error_code = rpdata.error_code;
        return -1;
    }
    
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
    
    if (!Trend_Log_Valid_Instance(instance)) {
        printf("✗ Trendlog %u: Invalid instance\n", instance);
        return false;
    }
    
    log_info = Trend_Log_Get_Info(instance);
    if (log_info == NULL) {
        printf("✗ Trendlog %u: Could not get log info\n", instance);
        return false;
    }
    
    /* Test read of source property */
    len = safe_read_property_for_trendlog(
        test_buffer,
        &log_info->Source,
        &error_class,
        &error_code
    );
    
    if (len < 0) {
        printf("✗ Trendlog %u: Cannot read source (error class=%d, code=%d)\n",
               instance, error_class, error_code);
        return false;
    }
    
    printf("✓ Trendlog %u: Source read test passed (%d bytes)\n", instance, len);
    return true;
}

/**
 * @brief Force le recalcul des timestamps pour tous les trendlogs actifs
 * À appeler périodiquement pour corriger les timestamps à 1900
 */
void Trendlog_Fix_Timestamps(void)
{
    uint32_t i;
    time_t current_time;
    struct tm *timeptr;
    
    current_time = time(NULL);
    timeptr = localtime(&current_time);
    
    for (i = 0; i < MAX_TREND_LOGS; i++) {
        if (Trend_Log_Valid_Instance(i) && TL_Is_Enabled(i)) {
            TL_LOG_INFO *log_info = Trend_Log_Get_Info(i);
            
            if (log_info != NULL) {
                /* Mettre à jour tLastDataTime avec le timestamp actuel */
                log_info->tLastDataTime = current_time;
                
                /* Si ucTimeFlags indique que les dates sont wildcard, les corriger */
                if (log_info->ucTimeFlags & (TL_T_START_WILD | TL_T_STOP_WILD)) {
                    /* Définir StartTime à maintenant */
                    log_info->StartTime.date.year = (uint16_t)(timeptr->tm_year + 1900);
                    log_info->StartTime.date.month = (uint8_t)(timeptr->tm_mon + 1);
                    log_info->StartTime.date.day = (uint8_t)timeptr->tm_mday;
                    log_info->StartTime.date.wday = (uint8_t)((timeptr->tm_wday == 0) ? 7 : timeptr->tm_wday);
                    
                    log_info->StartTime.time.hour = (uint8_t)timeptr->tm_hour;
                    log_info->StartTime.time.min = (uint8_t)timeptr->tm_min;
                    log_info->StartTime.time.sec = (uint8_t)timeptr->tm_sec;
                    log_info->StartTime.time.hundredths = 0;
                    
                    log_info->tStartTime = current_time;
                    log_info->ucTimeFlags &= ~TL_T_START_WILD;
                }
            }
        }
    }
}
