/**
 * @file trendlog_override.c
 * @brief Implémentation personnalisée pour la gestion des Trendlogs BACnet Stack
 * @author Custom implementation for bacnetStackServer plugin
 * @date 2025
 * 
 * Ce fichier fournit des fonctions pour gérer dynamiquement les Trendlogs
 * et désactiver leur initialisation automatique par défaut.
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
 * @param instance Instance du Trendlog à vider
 */
static void clear_single_trendlog(uint32_t instance)
{
    BACNET_WRITE_PROPERTY_DATA wp_data;
    BACNET_APPLICATION_DATA_VALUE value;
    int len;
    
    if (!Trend_Log_Valid_Instance(instance)) {
        return;
    }
    
    /* Initialiser les structures */
    memset(&wp_data, 0, sizeof(wp_data));
    memset(&value, 0, sizeof(value));
    
    /* Configuration de base du Write Property */
    wp_data.object_type = OBJECT_TRENDLOG;
    wp_data.object_instance = instance;
    wp_data.array_index = BACNET_ARRAY_ALL;
    
    /* 1. Désactiver le Trendlog */
    value.tag = BACNET_APPLICATION_TAG_BOOLEAN;
    value.type.Boolean = false;
    
    len = bacapp_encode_application_data(wp_data.application_data, &value);
    wp_data.object_property = PROP_ENABLE;
    wp_data.application_data_len = len;
    
    Trend_Log_Write_Property(&wp_data);
    
    /* 2. Effacer le buffer (RECORD_COUNT = 0) */
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
 * 
 * Cette fonction réinitialise tous les Trendlogs à leur état vide.
 * Doit être appelée avant de charger une nouvelle configuration.
 */
void clear_all_trendlogs(void)
{
    uint32_t i;
    
    for (i = 0; i < MAX_TREND_LOGS; i++) {
        clear_single_trendlog(i);
    }
}

/**
 * @brief Fonction stub pour Trendlog_Delete (si nécessaire dans le futur)
 * @param object_instance Instance du Trendlog à supprimer
 * @return true si la suppression a réussi, false sinon
 * 
 * Note: Cette fonction est un stub car la librairie BACnet Stack ne fournit pas
 * de fonction de suppression. Pour l'instant, on utilise clear_all_trendlogs()
 * pour réinitialiser tous les Trendlogs avant de charger une nouvelle config.
 */
bool Trendlog_Delete(uint32_t object_instance)
{
    (void)object_instance; /* Éviter warning unused parameter */
    
    /* Pour l'instant, cette fonction ne fait rien */
    /* Si besoin dans le futur, on pourra implémenter une vraie suppression */
    return false;
}

/**
 * @brief Fonction stub pour supprimer tous les Trendlogs
 * 
 * Note: Utiliser clear_all_trendlogs() à la place
 */
void Trendlog_Delete_All(void)
{
    clear_all_trendlogs();
}