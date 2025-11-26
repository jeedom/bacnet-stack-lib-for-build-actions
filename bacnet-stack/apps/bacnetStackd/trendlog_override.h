/**
 * @file trendlog_override.h
 * @brief Header pour les fonctions d'override des Trendlogs
 */

#ifndef TRENDLOG_OVERRIDE_H
#define TRENDLOG_OVERRIDE_H

#include <stdbool.h>
#include <stdint.h>
#include "bacnet/basic/object/trendlog.h"

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

#endif /* TRENDLOG_OVERRIDE_H */