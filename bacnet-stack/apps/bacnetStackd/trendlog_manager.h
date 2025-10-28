/**
 * @file trendlog_manager.h
 * @brief Gestion des Trendlogs BACnet pour bacnetStackServer
 * @author Plugin Jeedom bacnetStackServer
 * @date 2025
 */

#ifndef TRENDLOG_MANAGER_H
#define TRENDLOG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "bacnet/bacdef.h"
#include "bacnet/bacdcode.h"
#include "bacnet/trendlog.h"
#include "bacnet/datetime.h"

/* Configuration maximale */
#define MAX_TRENDLOGS 50
#define DEFAULT_BUFFER_SIZE 2016  /* 7 jours à 5min d'intervalle */
#define MAX_BUFFER_SIZE 10000

/* Types de triggers */
typedef enum {
    TRENDLOG_TRIGGER_PERIODIC = 0,
    TRENDLOG_TRIGGER_COV = 1,
    TRENDLOG_TRIGGER_TRIGGERED = 2
} TRENDLOG_TRIGGER_TYPE;

/* Structure de configuration d'un Trendlog */
typedef struct {
    uint32_t instance;
    char name[64];
    char description[128];
    bool enable;
    
    /* Objet lié */
    BACNET_OBJECT_TYPE linked_object_type;
    uint32_t linked_object_instance;
    
    /* Configuration de logging */
    TRENDLOG_TRIGGER_TYPE trigger_type;
    uint32_t log_interval;        /* En secondes */
    uint32_t buffer_size;
    float cov_increment;          /* Pour trigger COV */
    bool stop_when_full;
    bool align_intervals;
    
    /* Période de démarrage */
    BACNET_DATE_TIME start_time;
    
    /* Runtime data */
    time_t last_log_time;
    uint32_t record_count;
    float last_value;
    bool is_running;
    
} TRENDLOG_CONFIG;

/* Structure pour stocker les enregistrements du Trendlog */
typedef struct {
    BACNET_DATE_TIME timestamp;
    float value;
    uint8_t status_flags;
} TRENDLOG_RECORD;

/* Structure globale du gestionnaire de Trendlogs */
typedef struct {
    TRENDLOG_CONFIG configs[MAX_TRENDLOGS];
    uint32_t count;
    
    /* Buffers circulaires pour chaque Trendlog */
    TRENDLOG_RECORD *buffers[MAX_TRENDLOGS];
    uint32_t write_index[MAX_TRENDLOGS];
    
} TRENDLOG_MANAGER;

/* Fonctions publiques */

/**
 * @brief Initialise le gestionnaire de Trendlogs
 */
void TrendLog_Manager_Init(void);

/**
 * @brief Charge la configuration depuis JSON
 * @param json_data Chaîne JSON contenant la configuration
 * @return true si succès, false sinon
 */
bool TrendLog_Load_Config(const char *json_data);

/**
 * @brief Ajoute un Trendlog depuis une configuration
 * @param config Pointeur vers la configuration
 * @return true si succès, false sinon
 */
bool TrendLog_Add(TRENDLOG_CONFIG *config);

/**
 * @brief Supprime tous les Trendlogs
 */
void TrendLog_Clear_All(void);

/**
 * @brief Enregistre une valeur dans le Trendlog
 * @param trendlog_instance Instance du Trendlog
 * @param value Valeur à enregistrer
 * @param status_flags Flags de statut
 * @return true si enregistré, false sinon
 */
bool TrendLog_Record_Value(uint32_t trendlog_instance, float value, uint8_t status_flags);

/**
 * @brief Vérifie et enregistre les valeurs périodiques
 * Appeler cette fonction régulièrement (ex: toutes les secondes)
 */
void TrendLog_Process_Periodic(void);

/**
 * @brief Vérifie le COV et enregistre si nécessaire
 * @param object_type Type d'objet surveillé
 * @param object_instance Instance de l'objet
 * @param new_value Nouvelle valeur
 */
void TrendLog_Process_COV(BACNET_OBJECT_TYPE object_type, uint32_t object_instance, float new_value);

/**
 * @brief Récupère le nombre d'enregistrements d'un Trendlog
 * @param trendlog_instance Instance du Trendlog
 * @return Nombre d'enregistrements
 */
uint32_t TrendLog_Get_Record_Count(uint32_t trendlog_instance);

/**
 * @brief Récupère un enregistrement spécifique
 * @param trendlog_instance Instance du Trendlog
 * @param record_index Index de l'enregistrement
 * @param record Pointeur vers la structure de sortie
 * @return true si succès, false sinon
 */
bool TrendLog_Get_Record(uint32_t trendlog_instance, uint32_t record_index, TRENDLOG_RECORD *record);

/**
 * @brief Active/désactive un Trendlog
 * @param trendlog_instance Instance du Trendlog
 * @param enable true pour activer, false pour désactiver
 */
void TrendLog_Set_Enable(uint32_t trendlog_instance, bool enable);

/**
 * @brief Vide le buffer d'un Trendlog
 * @param trendlog_instance Instance du Trendlog
 */
void TrendLog_Clear_Buffer(uint32_t trendlog_instance);

/**
 * @brief Exporte les données d'un Trendlog en CSV
 * @param trendlog_instance Instance du Trendlog
 * @param filename Nom du fichier de sortie
 * @return true si succès, false sinon
 */
bool TrendLog_Export_CSV(uint32_t trendlog_instance, const char *filename);

/**
 * @brief Affiche l'état de tous les Trendlogs (debug)
 */
void TrendLog_Print_Status(void);

/**
 * @brief Recherche un Trendlog par son objet lié
 * @param object_type Type d'objet
 * @param object_instance Instance de l'objet
 * @return Index du Trendlog ou -1 si non trouvé
 */
int TrendLog_Find_By_Object(BACNET_OBJECT_TYPE object_type, uint32_t object_instance);

/* Callbacks pour l'intégration BACnet Stack */

/**
 * @brief Callback ReadProperty pour Trendlog objects
 */
int TrendLog_Read_Property(
    BACNET_READ_PROPERTY_DATA *rpdata);

/**
 * @brief Callback WriteProperty pour Trendlog objects
 */
bool TrendLog_Write_Property(
    BACNET_WRITE_PROPERTY_DATA *wp_data);

/**
 * @brief Retourne le nombre total de Trendlogs
 */
unsigned TrendLog_Count(void);

/**
 * @brief Retourne l'instance d'un Trendlog par index
 */
uint32_t TrendLog_Index_To_Instance(unsigned index);

/**
 * @brief Vérifie si une instance existe
 */
bool TrendLog_Valid_Instance(uint32_t object_instance);

/**
 * @brief Retourne le nom d'un Trendlog
 */
bool TrendLog_Object_Name(
    uint32_t object_instance,
    BACNET_CHARACTER_STRING *object_name);

#endif /* TRENDLOG_MANAGER_H */