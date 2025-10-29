/**
 * @file trendlog_override.c
 * @brief Implémentation personnalisée pour la gestion des Trendlogs BACnet Stack
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "bacnet/basic/object/trendlog.h"
#include "bacnet/wp.h"
#include "bacnet/bacapp.h"
#include "trendlog_override.h"

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