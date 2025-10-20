/**
 * @file
 * @brief Remplacements pour les fonctions d'initialisation des objets BACnet
 * Ce fichier doit être inclus AVANT les fichiers d'objets BACnet pour remplacer leurs fonctions
 */

#ifndef NO_OBJECTS_OVERRIDE_H
#define NO_OBJECTS_OVERRIDE_H

#include <stdio.h>

/* Définir les constantes pour empêcher la création d'objets */
#define MAX_ANALOG_INPUTS 0
#define MAX_ANALOG_OUTPUTS 0
#define MAX_ANALOG_VALUES 0
#define MAX_BINARY_INPUTS 0
#define MAX_BINARY_OUTPUTS 0
#define MAX_BINARY_VALUES 0
#define MAX_MULTI_STATE_INPUTS 0
#define MAX_MULTI_STATE_OUTPUTS 0
#define MAX_MULTI_STATE_VALUES 0
#define MAX_CHARACTERSTRING_VALUES 0
#define MAX_DEVICE_OBJECTS 1  /* Nous avons besoin d'au moins un objet Device */
#define NO_INIT_OBJECTS 1

/* Fonction de remplacement pour Device_Init */
static void custom_device_init(void *data) {
    printf("[OVERRIDE] Device_Init called with custom implementation\n");
    /* Nous ne faisons que créer l'objet Device sans créer d'objets par défaut */
    /* L'implémentation originale serait appelée ici, mais avec des paramètres 
       qui empêchent la création d'objets par défaut */
}
#define Device_Init custom_device_init

/* Fonctions vides de remplacement pour les fonctions d'initialisation */
/* Remplacer les fonctions d'initialisation */
static void empty_analog_input_init(void) {
    printf("[OVERRIDE] Empty Analog_Input_Init called\n");
}
#define Analog_Input_Init empty_analog_input_init

static void empty_analog_output_init(void) {
    printf("[OVERRIDE] Empty Analog_Output_Init called\n");
}
#define Analog_Output_Init empty_analog_output_init

static void empty_analog_value_init(void) {
    printf("[OVERRIDE] Empty Analog_Value_Init called\n");
}
#define Analog_Value_Init empty_analog_value_init

static void empty_binary_input_init(void) {
    printf("[OVERRIDE] Empty Binary_Input_Init called\n");
}
#define Binary_Input_Init empty_binary_input_init

static void empty_binary_output_init(void) {
    printf("[OVERRIDE] Empty Binary_Output_Init called\n");
}
#define Binary_Output_Init empty_binary_output_init

static void empty_binary_value_init(void) {
    printf("[OVERRIDE] Empty Binary_Value_Init called\n");
}
#define Binary_Value_Init empty_binary_value_init

static void empty_multistate_input_init(void) {
    printf("[OVERRIDE] Empty MultiState_Input_Init called\n");
}
#define MultiState_Input_Init empty_multistate_input_init

static void empty_multistate_output_init(void) {
    printf("[OVERRIDE] Empty MultiState_Output_Init called\n");
}
#define MultiState_Output_Init empty_multistate_output_init

static void empty_multistate_value_init(void) {
    printf("[OVERRIDE] Empty MultiState_Value_Init called\n");
}
#define MultiState_Value_Init empty_multistate_value_init

static void empty_characterstring_value_init(void) {
    printf("[OVERRIDE] Empty CharacterString_Value_Init called\n");
}
#define CharacterString_Value_Init empty_characterstring_value_init

/* Fonctions de création qui retournent des valeurs */
static uint32_t empty_analog_input_create(uint32_t instance) {
    printf("[OVERRIDE] Empty Analog_Input_Create(%u) called\n", (unsigned int)instance);
    return 0;
}
#define Analog_Input_Create empty_analog_input_create

static uint32_t empty_analog_output_create(uint32_t instance) {
    printf("[OVERRIDE] Empty Analog_Output_Create(%u) called\n", (unsigned int)instance);
    return 0;
}
#define Analog_Output_Create empty_analog_output_create

static uint32_t empty_analog_value_create(uint32_t instance) {
    printf("[OVERRIDE] Empty Analog_Value_Create(%u) called\n", (unsigned int)instance);
    return 0;
}
#define Analog_Value_Create empty_analog_value_create

static uint32_t empty_binary_input_create(uint32_t instance) {
    printf("[OVERRIDE] Empty Binary_Input_Create(%u) called\n", (unsigned int)instance);
    return 0;
}
#define Binary_Input_Create empty_binary_input_create

static uint32_t empty_binary_output_create(uint32_t instance) {
    printf("[OVERRIDE] Empty Binary_Output_Create(%u) called\n", (unsigned int)instance);
    return 0;
}
#define Binary_Output_Create empty_binary_output_create

static uint32_t empty_binary_value_create(uint32_t instance) {
    printf("[OVERRIDE] Empty Binary_Value_Create(%u) called\n", (unsigned int)instance);
    return 0;
}
#define Binary_Value_Create empty_binary_value_create

static uint32_t empty_multistate_input_create(uint32_t instance) {
    printf("[OVERRIDE] Empty MultiState_Input_Create(%u) called\n", (unsigned int)instance);
    return 0;
}
#define MultiState_Input_Create empty_multistate_input_create

static uint32_t empty_multistate_output_create(uint32_t instance) {
    printf("[OVERRIDE] Empty MultiState_Output_Create(%u) called\n", (unsigned int)instance);
    return 0;
}
#define MultiState_Output_Create empty_multistate_output_create

static uint32_t empty_multistate_value_create(uint32_t instance) {
    printf("[OVERRIDE] Empty MultiState_Value_Create(%u) called\n", (unsigned int)instance);
    return 0;
}
#define MultiState_Value_Create empty_multistate_value_create

/* Remplace d'autres fonctions qui pourraient créer des objets par défaut */
#define Device_Objects_Present(type) ((type == OBJECT_DEVICE) ? 1 : 0)

#endif /* NO_OBJECTS_OVERRIDE_H */
