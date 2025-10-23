/**
 * @file schedule_override.h
 * @brief Header pour l'implémentation personnalisée de Schedule_Delete()
 * @author Custom implementation for bacnetStackServer plugin
 * @date 2025
 */

#ifndef SCHEDULE_OVERRIDE_H
#define SCHEDULE_OVERRIDE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Supprime un objet Schedule
 * @param object_instance - Instance de l'objet Schedule à supprimer
 * @return true si la suppression a réussi, false sinon
 */
bool Schedule_Delete(uint32_t object_instance);

/**
 * @brief Fonction helper pour convertir instance en index
 * @param object_instance - Instance de l'objet
 * @return Index dans le tableau, ou MAX_SCHEDULES si non trouvé
 */
unsigned Schedule_Instance_To_Index(uint32_t object_instance);

/**
 * @brief Supprime tous les objets Schedule (fonction utilitaire)
 * @return Nombre d'objets supprimés
 */
int Schedule_Delete_All(void);

#ifdef __cplusplus
}
#endif

#endif /* SCHEDULE_OVERRIDE_H */