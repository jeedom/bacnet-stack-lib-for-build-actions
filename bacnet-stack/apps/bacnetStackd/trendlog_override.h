/**
 * @file trendlog_override.h
 * @brief Header pour les fonctions d'override des Trendlogs
 */

#ifndef TRENDLOG_OVERRIDE_H
#define TRENDLOG_OVERRIDE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
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
#include "bacnet/readrange.h"
#include "bacnet/datetime.h"
#include "trendlog_override.h"

/**
 * @brief Vide tous les Trendlogs
 */
void clear_all_trendlogs(void);

void Trendlog_Fix_Timestamps(void);

/**
 * @brief Fonction stub pour supprimer un Trendlog spécifique
 * @param object_instance Instance du Trendlog
 * @return true si succès, false sinon
 */
bool Trendlog_Delete(uint32_t object_instance);

/**
 * @brief Fonction stub pour supprimer tous les Trendlogs
 */
void Trendlog_Delete_All(void);

/**
 * @brief Teste si un trendlog peut lire sa source sans crasher
 * @param instance Instance du Trendlog à tester
 * @return true si la lecture fonctionne, false sinon
 */
bool Trendlog_Test_Source_Read(uint32_t instance);

int rr_trend_log_encode(uint8_t *apdu, BACNET_READ_RANGE_DATA *pRequest);

/**
 * @brief Override de Trend_Log_Read_Property pour gérer LOG_BUFFER array_index
 * @param rpdata Structure de données de lecture de propriété
 * @return Longueur des données encodées, ou BACNET_STATUS_ERROR en cas d'erreur
 */
int Trend_Log_Read_Property_Override(BACNET_READ_PROPERTY_DATA *rpdata);

#endif /* TRENDLOG_OVERRIDE_H */