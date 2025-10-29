/**
 * @file trendlog_override.h
 * @brief Header pour les fonctions d'override des Trendlogs
 */

#ifndef TRENDLOG_OVERRIDE_H
#define TRENDLOG_OVERRIDE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Vide tous les Trendlogs
 */
void clear_all_trendlogs(void);

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

#endif /* TRENDLOG_OVERRIDE_H */