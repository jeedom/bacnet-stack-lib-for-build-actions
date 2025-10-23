/**
 * @file schedule_override.c
 * @brief Stub minimal de Schedule_Delete() pour BACnet Stack
 * @author Custom implementation for bacnetStackServer plugin
 * @date 2025
 * 
 * Version minimaliste qui permet la compilation sans erreur.
 * Les Schedules seront réinitialisés via Schedule_Init() avant
 * de charger la nouvelle configuration.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "bacnet/bacdef.h"
#include "bacnet/basic/object/schedule.h"
#include "schedule_override.h"

/**
 * @brief Stub pour Schedule_Delete
 * 
 * Cette fonction est un placeholder qui permet au code de compiler.
 * La suppression réelle des Schedules se fait via Schedule_Init()
 * avant de charger la nouvelle configuration.
 */
bool Schedule_Delete(uint32_t object_instance)
{
    if (Schedule_Valid_Instance(object_instance)) {
        printf("Schedule_Delete: Schedule #%u will be reset via Schedule_Init()\n", 
               object_instance);
        return true;
    }
    return false;
}

/**
 * @brief Stub pour Schedule_Delete_All
 * 
 * Pour vraiment supprimer tous les Schedules, appelez Schedule_Init()
 * avant de charger la nouvelle configuration.
 */
int Schedule_Delete_All(void)
{
    unsigned int count;
    unsigned int i;
    int processed = 0;
    
    printf("Schedule_Delete_All: Processing all Schedules...\n");
    
    count = Schedule_Count();
    for (i = 0; i < count; i++) {
        uint32_t instance = Schedule_Index_To_Instance(i);
        if (Schedule_Valid_Instance(instance)) {
            Schedule_Delete(instance);
            processed++;
        }
    }
    
    printf("Schedule_Delete_All: Call Schedule_Init() to fully reset all Schedules\n");
    return processed;
}