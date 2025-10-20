#ifndef CUSTOM_INIT_H
#define CUSTOM_INIT_H

/* Ce fichier contient des surcharges pour empêcher l'initialisation 
   automatique des objets BACnet */

/* Définir les constantes pour empêcher la création d'objets */
#ifndef MAX_ANALOG_INPUTS
#define MAX_ANALOG_INPUTS 0
#endif

#ifndef MAX_ANALOG_OUTPUTS
#define MAX_ANALOG_OUTPUTS 0
#endif

#ifndef MAX_ANALOG_VALUES
#define MAX_ANALOG_VALUES 0
#endif

#ifndef NO_INIT_OBJECTS
#define NO_INIT_OBJECTS 1
#endif

/* Note : La redéfinition des fonctions d'init a été retirée
   car elle peut causer des problèmes de compilation.
   Nous utiliserons une approche différente plus compatible. */

#endif /* CUSTOM_INIT_H */
