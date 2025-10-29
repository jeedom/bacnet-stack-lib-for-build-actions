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
#include "trendlog_override.h"
#include "bacnet/datalink/datalink.h"

/* Référence externe au tableau des Trendlogs (après modification de trendlog.c) */
extern TREND_LOG_DESCR Trend_Logs[];

/**
 * @brief Vide un Trendlog spécifique
 * @param index Index du Trendlog à vider
 */
static void clear_single_trendlog(uint32_t index)
{
    if (index >= MAX_TREND_LOGS) {
        return;
    }

    TREND_LOG_DESCR *CurrentTL = &Trend_Logs[index];
    
    /* Réinitialiser les propriétés principales */
    CurrentTL->Instance = 0;
    CurrentTL->bEnable = false;
    CurrentTL->ucTimeFlags = 0;
    CurrentTL->ulRecordCount = 0;
    CurrentTL->ulTotalRecordCount = 0;
    CurrentTL->tStartTime.date.year = 0;
    CurrentTL->tStartTime.date.month = 0;
    CurrentTL->tStartTime.date.day = 0;
    CurrentTL->tStartTime.date.wday = 0;
    CurrentTL->tStartTime.time.hour = 0;
    CurrentTL->tStartTime.time.min = 0;
    CurrentTL->tStartTime.time.sec = 0;
    CurrentTL->tStartTime.time.hundredths = 0;
    CurrentTL->tStopTime = CurrentTL->tStartTime;
    
    /* Réinitialiser le tampon de log */
    CurrentTL->iIndex = 0;
    if (CurrentTL->Logs != NULL) {
        memset(CurrentTL->Logs, 0, TL_MAX_ENTRIES * sizeof(TL_DATA_REC));
    }
    
    /* Réinitialiser la source des données */
    CurrentTL->Source.arrayIndex = 0;
    CurrentTL->Source.deviceIdentifier.type = OBJECT_NONE;
    CurrentTL->Source.deviceIdentifier.instance = 0;
    CurrentTL->Source.objectIdentifier.type = OBJECT_NONE;
    CurrentTL->Source.objectIdentifier.instance = 0;
    CurrentTL->Source.objectProperty = 0;
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