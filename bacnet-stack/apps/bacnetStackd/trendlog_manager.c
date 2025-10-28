/**
 * @file trendlog_manager.c
 * @brief Implémentation de la gestion des Trendlogs BACnet
 */

#include "trendlog_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cJSON.h"

/* Gestionnaire global */
static TRENDLOG_MANAGER trendlog_manager = {0};

/* Fonction utilitaire pour obtenir le temps actuel en BACNET_DATE_TIME */
static void get_current_datetime(BACNET_DATE_TIME *datetime)
{
    time_t now;
    struct tm *tm_info;
    
    time(&now);
    tm_info = localtime(&now);
    
    datetime->date.year = tm_info->tm_year + 1900;
    datetime->date.month = tm_info->tm_mon + 1;
    datetime->date.day = tm_info->tm_mday;
    datetime->date.wday = tm_info->tm_wday == 0 ? 7 : tm_info->tm_wday;
    
    datetime->time.hour = tm_info->tm_hour;
    datetime->time.min = tm_info->tm_min;
    datetime->time.sec = tm_info->tm_sec;
    datetime->time.hundredths = 0;
}

/**
 * @brief Initialise le gestionnaire de Trendlogs
 */
void TrendLog_Manager_Init(void)
{
    memset(&trendlog_manager, 0, sizeof(TRENDLOG_MANAGER));
    printf("[TrendLog] Manager initialized\n");
}

/**
 * @brief Ajoute un Trendlog depuis une configuration
 */
bool TrendLog_Add(TRENDLOG_CONFIG *config)
{
    if (trendlog_manager.count >= MAX_TRENDLOGS) {
        fprintf(stderr, "[TrendLog] Maximum number of Trendlogs reached (%d)\n", MAX_TRENDLOGS);
        return false;
    }
    
    if (config->buffer_size > MAX_BUFFER_SIZE) {
        fprintf(stderr, "[TrendLog] Buffer size too large: %d (max: %d)\n", 
                config->buffer_size, MAX_BUFFER_SIZE);
        return false;
    }
    
    uint32_t idx = trendlog_manager.count;
    
    /* Copie de la configuration */
    memcpy(&trendlog_manager.configs[idx], config, sizeof(TRENDLOG_CONFIG));
    
    /* Allocation du buffer circulaire */
    trendlog_manager.buffers[idx] = calloc(config->buffer_size, sizeof(TRENDLOG_RECORD));
    if (!trendlog_manager.buffers[idx]) {
        fprintf(stderr, "[TrendLog] Failed to allocate buffer for instance %d\n", config->instance);
        return false;
    }
    
    trendlog_manager.write_index[idx] = 0;
    trendlog_manager.configs[idx].record_count = 0;
    trendlog_manager.configs[idx].last_log_time = 0;
    trendlog_manager.configs[idx].is_running = config->enable;
    
    trendlog_manager.count++;
    
    printf("[TrendLog] Added instance %d: %s (buffer: %d, interval: %ds, trigger: %d)\n",
           config->instance, config->name, config->buffer_size, 
           config->log_interval, config->trigger_type);
    
    return true;
}

/**
 * @brief Parse la configuration JSON des Trendlogs
 */
bool TrendLog_Load_Config(const char *json_data)
{
    cJSON *root = cJSON_Parse(json_data);
    if (!root) {
        fprintf(stderr, "[TrendLog] Failed to parse JSON configuration\n");
        return false;
    }
    
    cJSON *trendlogs = cJSON_GetObjectItem(root, "trendlogs");
    if (!trendlogs || !cJSON_IsArray(trendlogs)) {
        fprintf(stderr, "[TrendLog] No 'trendlogs' array found in JSON\n");
        cJSON_Delete(root);
        return false;
    }
    
    int array_size = cJSON_GetArraySize(trendlogs);
    printf("[TrendLog] Loading %d Trendlog(s) from configuration...\n", array_size);
    
    for (int i = 0; i < array_size; i++) {
        cJSON *item = cJSON_GetArrayItem(trendlogs, i);
        if (!item) continue;
        
        TRENDLOG_CONFIG config = {0};
        
        /* Lecture des paramètres */
        cJSON *instance = cJSON_GetObjectItem(item, "instance");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *description = cJSON_GetObjectItem(item, "description");
        cJSON *enable = cJSON_GetObjectItem(item, "enable");
        cJSON *log_interval = cJSON_GetObjectItem(item, "log_interval");
        cJSON *buffer_size = cJSON_GetObjectItem(item, "buffer_size");
        cJSON *trigger_type = cJSON_GetObjectItem(item, "trigger_type");
        cJSON *cov_increment = cJSON_GetObjectItem(item, "cov_increment");
        cJSON *stop_when_full = cJSON_GetObjectItem(item, "stop_when_full");
        cJSON *align_intervals = cJSON_GetObjectItem(item, "align_intervals");
        
        if (!instance || !name) {
            fprintf(stderr, "[TrendLog] Missing required fields in trendlog %d\n", i);
            continue;
        }
        
        config.instance = instance->valueint;
        strncpy(config.name, name->valuestring, sizeof(config.name) - 1);
        
        if (description) {
            strncpy(config.description, description->valuestring, sizeof(config.description) - 1);
        }
        
        config.enable = enable ? cJSON_IsTrue(enable) : true;
        config.log_interval = log_interval ? log_interval->valueint : 300;
        config.buffer_size = buffer_size ? buffer_size->valueint : DEFAULT_BUFFER_SIZE;
        config.cov_increment = cov_increment ? (float)cov_increment->valuedouble : 0.5;
        config.stop_when_full = stop_when_full ? cJSON_IsTrue(stop_when_full) : false;
        config.align_intervals = align_intervals ? cJSON_IsTrue(align_intervals) : true;
        
        /* Parse trigger type */
        if (trigger_type && cJSON_IsString(trigger_type)) {
            if (strcmp(trigger_type->valuestring, "PERIODIC") == 0) {
                config.trigger_type = TRENDLOG_TRIGGER_PERIODIC;
            } else if (strcmp(trigger_type->valuestring, "COV") == 0) {
                config.trigger_type = TRENDLOG_TRIGGER_COV;
            } else {
                config.trigger_type = TRENDLOG_TRIGGER_TRIGGERED;
            }
        } else {
            config.trigger_type = TRENDLOG_TRIGGER_PERIODIC;
        }
        
        /* Parse linked object */
        cJSON *linked_obj = cJSON_GetObjectItem(item, "linked_object");
        if (linked_obj) {
            cJSON *type = cJSON_GetObjectItem(linked_obj, "type");
            cJSON *obj_instance = cJSON_GetObjectItem(linked_obj, "instance");
            
            if (type && cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "ANALOG_VALUE") == 0) {
                    config.linked_object_type = OBJECT_ANALOG_VALUE;
                } else if (strcmp(type->valuestring, "BINARY_VALUE") == 0) {
                    config.linked_object_type = OBJECT_BINARY_VALUE;
                } else if (strcmp(type->valuestring, "MULTI_STATE_VALUE") == 0) {
                    config.linked_object_type = OBJECT_MULTI_STATE_VALUE;
                } else if (strcmp(type->valuestring, "ANALOG_INPUT") == 0) {
                    config.linked_object_type = OBJECT_ANALOG_INPUT;
                } else if (strcmp(type->valuestring, "BINARY_INPUT") == 0) {
                    config.linked_object_type = OBJECT_BINARY_INPUT;
                }
            }
            
            if (obj_instance) {
                config.linked_object_instance = obj_instance->valueint;
            }
        }
        
        /* Initialise start_time à maintenant */
        get_current_datetime(&config.start_time);
        
        /* Ajoute le Trendlog */
        if (!TrendLog_Add(&config)) {
            fprintf(stderr, "[TrendLog] Failed to add Trendlog instance %d\n", config.instance);
        }
    }
    
    cJSON_Delete(root);
    printf("[TrendLog] Configuration loaded successfully\n");
    return true;
}

/**
 * @brief Supprime tous les Trendlogs
 */
void TrendLog_Clear_All(void)
{
    for (uint32_t i = 0; i < trendlog_manager.count; i++) {
        if (trendlog_manager.buffers[i]) {
            free(trendlog_manager.buffers[i]);
            trendlog_manager.buffers[i] = NULL;
        }
    }
    
    memset(&trendlog_manager, 0, sizeof(TRENDLOG_MANAGER));
    printf("[TrendLog] All Trendlogs cleared\n");
}

/**
 * @brief Trouve l'index d'un Trendlog par son instance
 */
static int find_trendlog_index(uint32_t instance)
{
    for (uint32_t i = 0; i < trendlog_manager.count; i++) {
        if (trendlog_manager.configs[i].instance == instance) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Enregistre une valeur dans le Trendlog
 */
bool TrendLog_Record_Value(uint32_t trendlog_instance, float value, uint8_t status_flags)
{
    int idx = find_trendlog_index(trendlog_instance);
    if (idx < 0) {
        return false;
    }
    
    TRENDLOG_CONFIG *config = &trendlog_manager.configs[idx];
    
    if (!config->is_running) {
        return false;
    }
    
    /* Vérifie si le buffer est plein et stop_when_full est activé */
    if (config->stop_when_full && config->record_count >= config->buffer_size) {
        return false;
    }
    
    /* Enregistre la valeur */
    uint32_t write_idx = trendlog_manager.write_index[idx];
    TRENDLOG_RECORD *record = &trendlog_manager.buffers[idx][write_idx];
    
    get_current_datetime(&record->timestamp);
    record->value = value;
    record->status_flags = status_flags;
    
    /* Met à jour les index (buffer circulaire) */
    trendlog_manager.write_index[idx] = (write_idx + 1) % config->buffer_size;
    
    if (config->record_count < config->buffer_size) {
        config->record_count++;
    }
    
    config->last_value = value;
    config->last_log_time = time(NULL);
    
    return true;
}

/**
 * @brief Traitement périodique des Trendlogs
 */
void TrendLog_Process_Periodic(void)
{
    time_t now = time(NULL);
    
    for (uint32_t i = 0; i < trendlog_manager.count; i++) {
        TRENDLOG_CONFIG *config = &trendlog_manager.configs[i];
        
        if (!config->is_running || config->trigger_type != TRENDLOG_TRIGGER_PERIODIC) {
            continue;
        }
        
        /* Vérifie si l'intervalle est écoulé */
        if (config->last_log_time == 0 || 
            (now - config->last_log_time) >= config->log_interval) {
            
            /* Lit la valeur actuelle de l'objet lié */
            /* NOTE: Cette partie doit être adaptée selon votre implémentation */
            /* Pour l'instant, on utilise last_value comme placeholder */
            
            TrendLog_Record_Value(config->instance, config->last_value, 0);
        }
    }
}

/**
 * @brief Traitement COV des Trendlogs
 */
void TrendLog_Process_COV(BACNET_OBJECT_TYPE object_type, uint32_t object_instance, float new_value)
{
    for (uint32_t i = 0; i < trendlog_manager.count; i++) {
        TRENDLOG_CONFIG *config = &trendlog_manager.configs[i];
        
        if (!config->is_running || config->trigger_type != TRENDLOG_TRIGGER_COV) {
            continue;
        }
        
        /* Vérifie si c'est l'objet surveillé */
        if (config->linked_object_type == object_type && 
            config->linked_object_instance == object_instance) {
            
            /* Vérifie le COV increment */
            if (config->last_log_time == 0 || 
                fabs(new_value - config->last_value) >= config->cov_increment) {
                
                TrendLog_Record_Value(config->instance, new_value, 0);
            }
        }
    }
}

/**
 * @brief Recherche un Trendlog par son objet lié
 */
int TrendLog_Find_By_Object(BACNET_OBJECT_TYPE object_type, uint32_t object_instance)
{
    for (uint32_t i = 0; i < trendlog_manager.count; i++) {
        TRENDLOG_CONFIG *config = &trendlog_manager.configs[i];
        
        if (config->linked_object_type == object_type && 
            config->linked_object_instance == object_instance) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Récupère le nombre d'enregistrements
 */
uint32_t TrendLog_Get_Record_Count(uint32_t trendlog_instance)
{
    int idx = find_trendlog_index(trendlog_instance);
    if (idx < 0) {
        return 0;
    }
    
    return trendlog_manager.configs[idx].record_count;
}

/**
 * @brief Récupère un enregistrement spécifique
 */
bool TrendLog_Get_Record(uint32_t trendlog_instance, uint32_t record_index, TRENDLOG_RECORD *record)
{
    int idx = find_trendlog_index(trendlog_instance);
    if (idx < 0 || !record) {
        return false;
    }
    
    TRENDLOG_CONFIG *config = &trendlog_manager.configs[idx];
    
    if (record_index >= config->record_count) {
        return false;
    }
    
    /* Calcul de l'index réel dans le buffer circulaire */
    uint32_t real_index;
    if (config->record_count < config->buffer_size) {
        real_index = record_index;
    } else {
        uint32_t start_idx = trendlog_manager.write_index[idx];
        real_index = (start_idx + record_index) % config->buffer_size;
    }
    
    memcpy(record, &trendlog_manager.buffers[idx][real_index], sizeof(TRENDLOG_RECORD));
    return true;
}

/**
 * @brief Active/désactive un Trendlog
 */
void TrendLog_Set_Enable(uint32_t trendlog_instance, bool enable)
{
    int idx = find_trendlog_index(trendlog_instance);
    if (idx < 0) {
        return;
    }
    
    trendlog_manager.configs[idx].is_running = enable;
    printf("[TrendLog] Instance %d %s\n", trendlog_instance, enable ? "enabled" : "disabled");
}

/**
 * @brief Vide le buffer d'un Trendlog
 */
void TrendLog_Clear_Buffer(uint32_t trendlog_instance)
{
    int idx = find_trendlog_index(trendlog_instance);
    if (idx < 0) {
        return;
    }
    
    TRENDLOG_CONFIG *config = &trendlog_manager.configs[idx];
    
    memset(trendlog_manager.buffers[idx], 0, config->buffer_size * sizeof(TRENDLOG_RECORD));
    trendlog_manager.write_index[idx] = 0;
    config->record_count = 0;
    config->last_log_time = 0;
    
    printf("[TrendLog] Buffer cleared for instance %d\n", trendlog_instance);
}

/**
 * @brief Exporte en CSV
 */
bool TrendLog_Export_CSV(uint32_t trendlog_instance, const char *filename)
{
    int idx = find_trendlog_index(trendlog_instance);
    if (idx < 0) {
        return false;
    }
    
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "[TrendLog] Failed to open file: %s\n", filename);
        return false;
    }
    
    TRENDLOG_CONFIG *config = &trendlog_manager.configs[idx];
    
    fprintf(fp, "Timestamp,Value,Status\n");
    
    for (uint32_t i = 0; i < config->record_count; i++) {
        TRENDLOG_RECORD record;
        if (TrendLog_Get_Record(trendlog_instance, i, &record)) {
            fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02d,%.2f,%d\n",
                    record.timestamp.date.year,
                    record.timestamp.date.month,
                    record.timestamp.date.day,
                    record.timestamp.time.hour,
                    record.timestamp.time.min,
                    record.timestamp.time.sec,
                    record.value,
                    record.status_flags);
        }
    }
    
    fclose(fp);
    printf("[TrendLog] Exported %d records to %s\n", config->record_count, filename);
    return true;
}

/**
 * @brief Affiche l'état de tous les Trendlogs
 */
void TrendLog_Print_Status(void)
{
    printf("\n========== TRENDLOG STATUS ==========\n");
    printf("Total Trendlogs: %d\n\n", trendlog_manager.count);
    
    for (uint32_t i = 0; i < trendlog_manager.count; i++) {
        TRENDLOG_CONFIG *config = &trendlog_manager.configs[i];
        
        printf("Instance: %d - %s\n", config->instance, config->name);
        printf("  Description: %s\n", config->description);
        printf("  Status: %s\n", config->is_running ? "RUNNING" : "STOPPED");
        printf("  Linked Object: Type=%d, Instance=%d\n", 
               config->linked_object_type, config->linked_object_instance);
        printf("  Trigger: %s\n", 
               config->trigger_type == TRENDLOG_TRIGGER_PERIODIC ? "PERIODIC" :
               config->trigger_type == TRENDLOG_TRIGGER_COV ? "COV" : "TRIGGERED");
        printf("  Log Interval: %ds\n", config->log_interval);
        printf("  Records: %d / %d (%.1f%% full)\n", 
               config->record_count, config->buffer_size,
               (float)config->record_count / config->buffer_size * 100.0);
        
        if (config->record_count > 0) {
            printf("  Last Value: %.2f\n", config->last_value);
            printf("  Last Log: %ld\n", config->last_log_time);
        }
        printf("\n");
    }
    printf("====================================\n\n");
}

/* Callbacks BACnet Stack */

unsigned TrendLog_Count(void)
{
    return trendlog_manager.count;
}

uint32_t TrendLog_Index_To_Instance(unsigned index)
{
    if (index < trendlog_manager.count) {
        return trendlog_manager.configs[index].instance;
    }
    return BACNET_MAX_INSTANCE;
}

bool TrendLog_Valid_Instance(uint32_t object_instance)
{
    return (find_trendlog_index(object_instance) >= 0);
}

bool TrendLog_Object_Name(uint32_t object_instance, BACNET_CHARACTER_STRING *object_name)
{
    int idx = find_trendlog_index(object_instance);
    if (idx < 0 || !object_name) {
        return false;
    }
    
    return characterstring_init_ansi(object_name, trendlog_manager.configs[idx].name);
}

/* Implémentation simplifiée de Read/Write Property */
/* À adapter selon votre architecture BACnet Stack */

int TrendLog_Read_Property(BACNET_READ_PROPERTY_DATA *rpdata)
{
    /* Implementation selon vos besoins */
    return 0;
}

bool TrendLog_Write_Property(BACNET_WRITE_PROPERTY_DATA *wp_data)
{
    /* Implementation selon vos besoins */
    return false;
}