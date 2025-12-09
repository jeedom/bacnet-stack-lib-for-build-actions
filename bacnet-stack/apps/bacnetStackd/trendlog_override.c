/**
 * @file trendlog_override.c
 * @brief Implémentation personnalisée pour la gestion des Trendlogs BACnet Stack
 */

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
#include "trendlog_override.h"

/**
 * @brief Lecture sécurisée d'une propriété d'objet pour les trendlogs
 * @return longueur des données lues, ou -1 en cas d'erreur
 */
static int safe_read_property_for_trendlog(
    uint8_t *value,
    const BACNET_DEVICE_OBJECT_PROPERTY_REFERENCE *Source,
    BACNET_ERROR_CLASS *error_class,
    BACNET_ERROR_CODE *error_code)
{
    int len = 0;
    BACNET_READ_PROPERTY_DATA rpdata;
    bool object_exists = false;

    if (value == NULL) {
        *error_class = ERROR_CLASS_SERVICES;
        *error_code = ERROR_CODE_OTHER;
        return -1;
    }

    /* Verify source object exists */
    switch (Source->objectIdentifier.type) {
        case OBJECT_ANALOG_INPUT:
            object_exists = Analog_Input_Valid_Instance(Source->objectIdentifier.instance);
            break;
        case OBJECT_ANALOG_OUTPUT:
            object_exists = Analog_Output_Valid_Instance(Source->objectIdentifier.instance);
            break;
        case OBJECT_ANALOG_VALUE:
            object_exists = Analog_Value_Valid_Instance(Source->objectIdentifier.instance);
            break;
        case OBJECT_BINARY_INPUT:
            object_exists = Binary_Input_Valid_Instance(Source->objectIdentifier.instance);
            break;
        case OBJECT_BINARY_OUTPUT:
            object_exists = Binary_Output_Valid_Instance(Source->objectIdentifier.instance);
            break;
        case OBJECT_BINARY_VALUE:
            object_exists = Binary_Value_Valid_Instance(Source->objectIdentifier.instance);
            break;
        default:
            object_exists = false;
            break;
    }
    
    
    if (!object_exists) {
        *error_class = ERROR_CLASS_OBJECT;
        *error_code = ERROR_CODE_UNKNOWN_OBJECT;
        return -1;
    }
    
    /* Setup read property structure */
    memset(&rpdata, 0, sizeof(rpdata));
    rpdata.application_data = value;
    rpdata.application_data_len = MAX_APDU;
    rpdata.object_type = Source->objectIdentifier.type;
    rpdata.object_instance = Source->objectIdentifier.instance;
    rpdata.object_property = Source->propertyIdentifier;
    rpdata.array_index = Source->arrayIndex;
    
    /* Read property */
    len = Device_Read_Property(&rpdata);
    
    if (len < 0) {
        *error_class = rpdata.error_class;
        *error_code = rpdata.error_code;
        return -1;
    }
    
    return len;
}

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

/**
 * @brief Teste si un trendlog peut lire sa source sans crasher
 * @param instance Instance du Trendlog à tester
 * @return true si la lecture fonctionne, false sinon
 */
bool Trendlog_Test_Source_Read(uint32_t instance)
{
    uint8_t test_buffer[MAX_APDU];
    BACNET_ERROR_CLASS error_class;
    BACNET_ERROR_CODE error_code;
    TL_LOG_INFO *log_info;
    int len;
    
    if (!Trend_Log_Valid_Instance(instance)) {
        printf("✗ Trendlog %u: Invalid instance\n", instance);
        return false;
    }
    
    log_info = Trend_Log_Get_Info(instance);
    if (log_info == NULL) {
        printf("✗ Trendlog %u: Could not get log info\n", instance);
        return false;
    }
    
    /* Test read of source property */
    len = safe_read_property_for_trendlog(
        test_buffer,
        &log_info->Source,
        &error_class,
        &error_code
    );
    
    if (len < 0) {
        printf("✗ Trendlog %u: Cannot read source (error class=%d, code=%d)\n",
               instance, error_class, error_code);
        return false;
    }
    
    printf("✓ Trendlog %u: Source read test passed (%d bytes)\n", instance, len);
    return true;
}

/**
 * @brief Force le recalcul des timestamps pour tous les trendlogs actifs
 * Note: Avec les corrections du bug centisecondes/secondes, cette fonction
 * n'est normalement plus nécessaire mais conservée pour compatibilité
 */
void Trendlog_Fix_Timestamps(void)
{
    uint32_t i;
    time_t current_time;
    struct tm *timeptr;
    
    current_time = time(NULL);
    timeptr = localtime(&current_time);
    
    for (i = 0; i < MAX_TREND_LOGS; i++) {
        if (Trend_Log_Valid_Instance(i) && TL_Is_Enabled(i)) {
            TL_LOG_INFO *log_info = Trend_Log_Get_Info(i);
            
            if (log_info != NULL) {
                /* Ne plus forcer tLastDataTime - laissé géré par trend_log_timer() */
                /* log_info->tLastDataTime = current_time; */
                
                /* Si ucTimeFlags indique que les dates sont wildcard, les corriger */
                if (log_info->ucTimeFlags & (TL_T_START_WILD | TL_T_STOP_WILD)) {
                    /* Définir StartTime à maintenant */
                    log_info->StartTime.date.year = (uint16_t)(timeptr->tm_year + 1900);
                    log_info->StartTime.date.month = (uint8_t)(timeptr->tm_mon + 1);
                    log_info->StartTime.date.day = (uint8_t)timeptr->tm_mday;
                    log_info->StartTime.date.wday = (uint8_t)((timeptr->tm_wday == 0) ? 7 : timeptr->tm_wday);
                    
                    log_info->StartTime.time.hour = (uint8_t)timeptr->tm_hour;
                    log_info->StartTime.time.min = (uint8_t)timeptr->tm_min;
                    log_info->StartTime.time.sec = (uint8_t)timeptr->tm_sec;
                    log_info->StartTime.time.hundredths = 0;
                    
                    log_info->tStartTime = current_time;
                    log_info->ucTimeFlags &= ~TL_T_START_WILD;
                }
            }
        }
    }
}
/**
 * @brief Encode ReadRange response for Trend Log with proper time filtering
 * @note Cette fonction remplace la version buguée du BACnet Stack
 */
int rr_trend_log_encode(
    uint8_t *apdu,
    BACNET_READ_RANGE_DATA *pRequest)
{
    int iLen = 0;
    uint32_t instance;
    TL_LOG_INFO *info;
    TL_DATA_REC *rec;
    BACNET_BIT_STRING TempBits;
    int32_t i, j;
    int32_t log_index = 0;
    int32_t count = 0;
    BACNET_DATE_TIME TempTime;
    
    instance = pRequest->object_instance;
    
    /* Vérifier que l'instance est valide */
    if (!Trend_Log_Valid_Instance(instance)) {
        pRequest->error_class = ERROR_CLASS_OBJECT;
        pRequest->error_code = ERROR_CODE_UNKNOWN_OBJECT;
        return BACNET_STATUS_ERROR;
    }
    
    info = Trend_Log_Get_Info(instance);
    if (!info) {
        pRequest->error_class = ERROR_CLASS_OBJECT;
        pRequest->error_code = ERROR_CODE_UNKNOWN_OBJECT;
        return BACNET_STATUS_ERROR;
    }
    
    /* === Log vide === */
    if (info->ulRecordCount == 0) {
        printf("[READRANGE] TL[%u]: Log empty, returning 0 items\n", instance);
        
        pRequest->ItemCount = 0;
        pRequest->FirstSequence = 0;
        
        bitstring_init(&pRequest->ResultFlags);
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_FIRST_ITEM, true);
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_LAST_ITEM, true);
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_MORE_ITEMS, false);
        
        iLen = encode_context_object_id(&apdu[0], 0, OBJECT_TRENDLOG, instance);
        iLen += encode_context_unsigned(&apdu[iLen], 1, PROP_LOG_BUFFER);
        iLen += encode_context_bitstring(&apdu[iLen], 2, &pRequest->ResultFlags);
        iLen += encode_context_unsigned(&apdu[iLen], 3, 0);
        iLen += encode_opening_tag(&apdu[iLen], 4);
        iLen += encode_closing_tag(&apdu[iLen], 4);
        
        return iLen;
    }
    
    /* === Déterminer la plage selon le type de requête === */
    switch (pRequest->RequestType) {
        case RR_BY_POSITION:
            printf("[READRANGE] TL[%u]: ReadRange By Position (index=%d, count=%d)\n",
                   instance, pRequest->Range.RefIndex, pRequest->Count);
            
            log_index = pRequest->Range.RefIndex;
            count = pRequest->Count;
            
            /* Valider l'index (1-based dans BACnet) */
            if (log_index < 1 || log_index > (int32_t)info->ulRecordCount) {
                pRequest->error_class = ERROR_CLASS_PROPERTY;
                pRequest->error_code = ERROR_CODE_INVALID_ARRAY_INDEX;
                return BACNET_STATUS_ERROR;
            }
            
            /* Convertir en 0-based */
            log_index--;
            
            /* Gérer count négatif (lecture arrière) */
            if (count < 0) {
                count = -count;
                if (count > log_index + 1) {
                    count = log_index + 1;
                }
                log_index = log_index - count + 1;
            } else {
                /* Lecture avant */
                if (log_index + count > (int32_t)info->ulRecordCount) {
                    count = info->ulRecordCount - log_index;
                }
            }
            break;
            
        case RR_BY_TIME:
            /* === CORRECTION DU BUG === */
            {
                time_t ref_time;
                bacnet_time_t ref_seconds;
                int found_start = -1;
                int found_end = -1;
                
                /* Convertir le timestamp BACnet en time_t */
                ref_seconds = datetime_seconds_since_epoch(&pRequest->Range.RefTime);
                ref_time = (time_t)ref_seconds;
                
                printf("[READRANGE] TL[%u]: ReadRange By Time\n", instance);
                printf("  RefTime: %s", ctime(&ref_time));
                printf("  Count: %d\n", pRequest->Count);
                printf("  Total records: %u\n", info->ulRecordCount);
                
                if (pRequest->Count < 0) {
                    /* === Compter EN ARRIÈRE depuis RefTime === */
                    count = -pRequest->Count;
                    
                    printf("  → Searching backwards from RefTime\n");
                    
                    /* Parcourir de la fin vers le début */
                    for (i = (int32_t)info->ulRecordCount - 1; i >= 0; i--) {
                        /* ===  UTILISER L'API PUBLIQUE === */
                        rec = Trend_Log_Get_Record(instance, i);
                        if (!rec) continue;
                        
                        /* Comparer le timestamp */
                        if (rec->tTimeStamp <= ref_time) {
                            if (found_end == -1) {
                                found_end = i;
                                printf("  Found end at index %d (timestamp: %s", i, ctime(&rec->tTimeStamp));
                            }
                            found_start = i;
                            
                            /* Arrêter quand on a assez d'enregistrements */
                            if (found_end - found_start + 1 >= count) {
                                printf("  Found start at index %d (got %d records)\n", i, count);
                                break;
                            }
                        }
                    }
                } else {
                    /* === Compter EN AVANT depuis RefTime === */
                    count = pRequest->Count;
                    
                    printf("  → Searching forwards from RefTime\n");
                    
                    /* Parcourir du début vers la fin */
                    for (i = 0; i < (int32_t)info->ulRecordCount; i++) {
                        /* ===  UTILISER L'API PUBLIQUE === */
                        rec = Trend_Log_Get_Record(instance, i);
                        if (!rec) continue;
                        
                        /* Comparer le timestamp */
                        if (rec->tTimeStamp >= ref_time) {
                            if (found_start == -1) {
                                found_start = i;
                                printf("  Found start at index %d (timestamp: %s", i, ctime(&rec->tTimeStamp));
                            }
                            found_end = i;
                            
                            if (found_end - found_start + 1 >= count) {
                                printf("  Found end at index %d (got %d records)\n", i, count);
                                break;
                            }
                        }
                    }
                }
                
                /* Aucun enregistrement trouvé dans la plage */
                if (found_start == -1) {
                    printf("  ✗ No records found in time range!\n");
                    
                    pRequest->ItemCount = 0;
                    pRequest->FirstSequence = 0;
                    
                    bitstring_init(&pRequest->ResultFlags);
                    bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_FIRST_ITEM, true);
                    bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_LAST_ITEM, true);
                    bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_MORE_ITEMS, false);
                    
                    iLen = encode_context_object_id(&apdu[0], 0, OBJECT_TRENDLOG, instance);
                    iLen += encode_context_unsigned(&apdu[iLen], 1, PROP_LOG_BUFFER);
                    iLen += encode_context_bitstring(&apdu[iLen], 2, &pRequest->ResultFlags);
                    iLen += encode_context_unsigned(&apdu[iLen], 3, 0);
                    iLen += encode_opening_tag(&apdu[iLen], 4);
                    iLen += encode_closing_tag(&apdu[iLen], 4);
                    
                    return iLen;
                }
                
                log_index = found_start;
                count = found_end - found_start + 1;
                
                printf("  ✓ Returning %d records (index %d to %d)\n", count, found_start, found_end);
            }
            break;
            
        case RR_BY_SEQUENCE:
            /* Similaire à BY_POSITION mais avec sequence numbers */
            printf("[READRANGE] TL[%u]: ReadRange By Sequence (not fully implemented)\n", instance);
            log_index = 0;
            count = (pRequest->Count < 0) ? -pRequest->Count : pRequest->Count;
            if (count > (int32_t)info->ulRecordCount) {
                count = info->ulRecordCount;
            }
            break;
            
        default:
            pRequest->error_class = ERROR_CLASS_SERVICES;
            pRequest->error_code = ERROR_CODE_INVALID_PARAMETER_DATA_TYPE;
            return BACNET_STATUS_ERROR;
    }
    
    /* === Vérifier qu'on a des données === */
    if (count <= 0) {
        printf("[READRANGE] TL[%u]: Count is 0 after filtering\n", instance);
        
        pRequest->ItemCount = 0;
        pRequest->FirstSequence = 0;
        
        bitstring_init(&pRequest->ResultFlags);
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_FIRST_ITEM, true);
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_LAST_ITEM, true);
        bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_MORE_ITEMS, false);
        
        iLen = encode_context_object_id(&apdu[0], 0, OBJECT_TRENDLOG, instance);
        iLen += encode_context_unsigned(&apdu[iLen], 1, PROP_LOG_BUFFER);
        iLen += encode_context_bitstring(&apdu[iLen], 2, &pRequest->ResultFlags);
        iLen += encode_context_unsigned(&apdu[iLen], 3, 0);
        iLen += encode_opening_tag(&apdu[iLen], 4);
        iLen += encode_closing_tag(&apdu[iLen], 4);
        
        return iLen;
    }
    
    /* === Préparer la réponse === */
    pRequest->ItemCount = count;
    pRequest->FirstSequence = info->ulTotalRecordCount - info->ulRecordCount + log_index + 1;
    
    /* Définir les flags de résultat */
    bitstring_init(&pRequest->ResultFlags);
    bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_FIRST_ITEM, 
                     (log_index == 0));
    bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_LAST_ITEM, 
                     (log_index + count >= (int32_t)info->ulRecordCount));
    bitstring_set_bit(&pRequest->ResultFlags, RESULT_FLAG_MORE_ITEMS, 
                     (log_index + count < (int32_t)info->ulRecordCount));
    
    printf("[READRANGE] TL[%u]: Encoding %d items (first=%d, last=%d, more=%d)\n",
           instance, count,
           (log_index == 0) ? 1 : 0,
           (log_index + count >= (int32_t)info->ulRecordCount) ? 1 : 0,
           (log_index + count < (int32_t)info->ulRecordCount) ? 1 : 0);
    
    /* === Encoder l'en-tête === */
    iLen = encode_context_object_id(&apdu[0], 0, OBJECT_TRENDLOG, instance);
    iLen += encode_context_unsigned(&apdu[iLen], 1, PROP_LOG_BUFFER);
    iLen += encode_context_bitstring(&apdu[iLen], 2, &pRequest->ResultFlags);
    iLen += encode_context_unsigned(&apdu[iLen], 3, pRequest->ItemCount);
    
    /* Opening tag pour itemData [4] */
    iLen += encode_opening_tag(&apdu[iLen], 4);
    
    /* === Encoder chaque enregistrement === */
    for (i = 0; i < count; i++) {
        j = log_index + i;
        
        /* ===  UTILISER L'API PUBLIQUE === */
        rec = Trend_Log_Get_Record(instance, j);
        if (!rec) {
            printf("[READRANGE] TL[%u]: Failed to get record %d\n", instance, j);
            continue;
        }
        
        /* Opening tag pour un log-record [0] */
        iLen += encode_opening_tag(&apdu[iLen], 0);
        
        /* timestamp [0] */
        TL_Local_Time_To_BAC(&TempTime, rec->tTimeStamp);
        iLen += encode_context_datetime(&apdu[iLen], 0, &TempTime);
        
        /* logData [1] - choice */
        iLen += encode_opening_tag(&apdu[iLen], 1);
        
        /* Encoder la valeur selon son type */
        switch (rec->ucRecType) {
            case TL_TYPE_SIGN:
                iLen += encode_application_signed(&apdu[iLen], rec->Datum.lSValue);
                break;
            case TL_TYPE_UNSIGN:
                iLen += encode_application_unsigned(&apdu[iLen], rec->Datum.ulUValue);
                break;
            case TL_TYPE_REAL:
                iLen += encode_application_real(&apdu[iLen], rec->Datum.fReal);
                break;
            case TL_TYPE_ENUM:
                iLen += encode_application_enumerated(&apdu[iLen], rec->Datum.ulEnum);
                break;
            case TL_TYPE_BOOL:
                iLen += encode_application_boolean(&apdu[iLen], rec->Datum.ucBoolean);
                break;
            case TL_TYPE_NULL:
                iLen += encode_application_null(&apdu[iLen]);
                break;
            default:
                iLen += encode_application_null(&apdu[iLen]);
                break;
        }
        
        iLen += encode_closing_tag(&apdu[iLen], 1);
        
        /* statusFlags [2] */
        bitstring_init(&TempBits);
        bitstring_set_bit(&TempBits, STATUS_FLAG_IN_ALARM, 
                         (rec->ucStatus & 1) ? true : false);
        bitstring_set_bit(&TempBits, STATUS_FLAG_FAULT, 
                         (rec->ucStatus & 2) ? true : false);
        bitstring_set_bit(&TempBits, STATUS_FLAG_OVERRIDDEN, 
                         (rec->ucStatus & 4) ? true : false);
        bitstring_set_bit(&TempBits, STATUS_FLAG_OUT_OF_SERVICE, 
                         (rec->ucStatus & 8) ? true : false);
        iLen += encode_context_bitstring(&apdu[iLen], 2, &TempBits);
        
        /* Closing tag pour log-record */
        iLen += encode_closing_tag(&apdu[iLen], 0);
    }
    
    /* Closing tag pour itemData */
    iLen += encode_closing_tag(&apdu[iLen], 4);
    
    /* firstSequenceNumber [5] (optionnel mais recommandé) */
    if (pRequest->FirstSequence > 0) {
        iLen += encode_context_unsigned(&apdu[iLen], 5, pRequest->FirstSequence);
    }
    
    printf("[READRANGE] TL[%u]: Encoded %d bytes total\n", instance, iLen);
    
    return iLen;
}