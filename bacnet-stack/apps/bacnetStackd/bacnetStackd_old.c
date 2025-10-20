/**
 * @file
 * @brief BACnet server (Jeedom) + socket JSON runtime config
 * Démarrage sans objets, build minimal (AI/AO/AV), options --socketport/--pid
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <jansson.h>

/* BACnet Stack defines - first */
#include "bacnet/bacdef.h"
/* BACnet Stack API */
#include "bacnet/apdu.h"
#include "bacnet/bacdcode.h"
#include "bacnet/bacapp.h"
#include "bacnet/bacstr.h"
#include "bacnet/bactext.h"
#include "bacnet/dcc.h"
#include "bacnet/getevent.h"
#include "bacnet/iam.h"
#include "bacnet/npdu.h"
#include "bacnet/version.h"
#include "bacnet/wp.h"
#include "bacnet/datetime.h"
/* services/infra */
#include "bacnet/basic/binding/address.h"
#include "bacnet/basic/services.h"
#include "bacnet/basic/sys/filename.h"
#include "bacnet/basic/sys/mstimer.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/datalink/dlenv.h"
/* Device */
#include "bacnet/basic/object/device.h"

/* ====== Objets gérés (build minimal) ====== */
#include "bacnet/basic/object/ai.h"
#include "bacnet/basic/object/ao.h"
#include "bacnet/basic/object/av.h"

/* =========================
 *  Timers / buffers BACnet
 * ========================= */
static const char *BACnet_Version = BACNET_VERSION_TEXT;
static struct mstimer BACNET_Task_Timer;
static struct mstimer BACNET_TSM_Timer;
static struct mstimer BACNET_Address_Timer;
static struct mstimer BACNET_Object_Timer;
static uint8_t Rx_Buf[MAX_MPDU] = {0};

/* =========================
 *  Socket de config (local)
 * ========================= */
static int g_listen_fd = -1;
static int g_client_fd = -1;
static int g_socket_port = 55031; /* par défaut */
static char g_cmd_buf[8192];
static size_t g_cmd_len = 0;
static char g_pidfile[512] = {0};

/* Protos */
static void Init_Service_Handlers(void);
static void purge_all_objects(void);
static int  write_string_property(BACNET_OBJECT_TYPE t, uint32_t inst, BACNET_PROPERTY_ID p, const char *s);
static int  write_real_property  (BACNET_OBJECT_TYPE t, uint32_t inst, BACNET_PROPERTY_ID p, float v);
static int  write_bool_property  (BACNET_OBJECT_TYPE t, uint32_t inst, BACNET_PROPERTY_ID p, int b);
static int  apply_config_from_json(const char *json_text);
static int  socket_listen_local(int port);
static void socket_close_all(void);
static void trim_newlines(char *s);
static int  handle_socket_line(const char *line);
static void process_socket_io(void);
static void write_pidfile_if_needed(void);

/* ===== Helpers WriteProperty ===== */
static int write_string_property(BACNET_OBJECT_TYPE type, uint32_t instance,
                                 BACNET_PROPERTY_ID prop, const char *s)
{
    uint8_t apdu[MAX_APDU];
    int len;
    BACNET_APPLICATION_DATA_VALUE av;
    BACNET_WRITE_PROPERTY_DATA wp;

    memset(&av, 0, sizeof(av));
    memset(&wp, 0, sizeof(wp));

    av.tag = BACNET_APPLICATION_TAG_CHARACTER_STRING;
    characterstring_init_ansi(&av.type.Character_String, s);
    len = bacapp_encode_application_data(&apdu[0], &av);

    wp.object_type = type;
    wp.object_instance = instance;
    wp.object_property = prop;
    wp.array_index = BACNET_ARRAY_ALL;
    wp.priority = 16;
    wp.application_data_len = len;
    memcpy(wp.application_data, apdu, (size_t)len);

    return Device_Write_Property(&wp);
}

static int write_real_property(BACNET_OBJECT_TYPE type, uint32_t instance,
                               BACNET_PROPERTY_ID prop, float v)
{
    uint8_t apdu[MAX_APDU];
    int len;
    BACNET_APPLICATION_DATA_VALUE av;
    BACNET_WRITE_PROPERTY_DATA wp;

    memset(&av, 0, sizeof(av));
    memset(&wp, 0, sizeof(wp));

    av.tag = BACNET_APPLICATION_TAG_REAL;
    av.type.Real = v;
    len = bacapp_encode_application_data(&apdu[0], &av);

    wp.object_type = type;
    wp.object_instance = instance;
    wp.object_property = prop;
    wp.array_index = BACNET_ARRAY_ALL;
    wp.priority = 16;
    wp.application_data_len = len;
    memcpy(wp.application_data, apdu, (size_t)len);

    return Device_Write_Property(&wp);
}

static int write_bool_property(BACNET_OBJECT_TYPE type, uint32_t instance,
                               BACNET_PROPERTY_ID prop, int b)
{
    uint8_t apdu[MAX_APDU];
    int len;
    BACNET_APPLICATION_DATA_VALUE av;
    BACNET_WRITE_PROPERTY_DATA wp;

    memset(&av, 0, sizeof(av));
    memset(&wp, 0, sizeof(wp));

    av.tag = BACNET_APPLICATION_TAG_BOOLEAN;
    av.type.Boolean = b ? 1 : 0;
    len = bacapp_encode_application_data(&apdu[0], &av);

    wp.object_type = type;
    wp.object_instance = instance;
    wp.object_property = prop;
    wp.array_index = BACNET_ARRAY_ALL;
    wp.priority = 16;
    wp.application_data_len = len;
    memcpy(wp.application_data, apdu, (size_t)len);

    return Device_Write_Property(&wp);
}

/* ===== Purge complet des objets =====
   (sur build minimal, c’est suffisant) */
// static void purge_all_objects(void)
// {
//     unsigned count;
//     while ((count = Analog_Input_Count()))  { Analog_Input_Delete(Analog_Input_Index_To_Instance(0)); }
//     while ((count = Analog_Output_Count())) { Analog_Output_Delete(Analog_Output_Index_To_Instance(0)); }
//     while ((count = Analog_Value_Count()))  { Analog_Value_Delete(Analog_Value_Index_To_Instance(0)); }
// }

static void purge_all_objects(void)
{
    unsigned count, i;
    uint32_t instance;

    /* Analogiques */
#if defined(OBJECT_ANALOG_INPUT)
    count = Analog_Input_Count();
    for (i = 0; i < count; i++) {
        instance = Analog_Input_Index_To_Instance(0);
        printf("Purging Analog Input %u\n", instance);
        Analog_Input_Delete(instance);
    }
#endif

#if defined(OBJECT_ANALOG_OUTPUT)
    count = Analog_Output_Count();
    for (i = 0; i < count; i++) {
        instance = Analog_Output_Index_To_Instance(0);
        printf("Purging Analog Output %u\n", instance);
        Analog_Output_Delete(instance);
    }
#endif

#if defined(OBJECT_ANALOG_VALUE)
    count = Analog_Value_Count();
    for (i = 0; i < count; i++) {
        instance = Analog_Value_Index_To_Instance(0);
        printf("Purging Analog Value %u\n", instance);
        Analog_Value_Delete(instance);
    }
#endif

    /* Binaires */
#if defined(OBJECT_BINARY_INPUT)
    count = Binary_Input_Count();
    for (i = 0; i < count; i++) {
        instance = Binary_Input_Index_To_Instance(0);
        printf("Purging Binary Input %u\n", instance);
        Binary_Input_Delete(instance);
    }
#endif

#if defined(OBJECT_BINARY_OUTPUT)
    count = Binary_Output_Count();
    for (i = 0; i < count; i++) {
        instance = Binary_Output_Index_To_Instance(0);
        printf("Purging Binary Output %u\n", instance);
        Binary_Output_Delete(instance);
    }
#endif

#if defined(OBJECT_BINARY_VALUE)
    count = Binary_Value_Count();
    for (i = 0; i < count; i++) {
        instance = Binary_Value_Index_To_Instance(0);
        printf("Purging Binary Value %u\n", instance);
        Binary_Value_Delete(instance);
    }
#endif

    /* Multistate */
#if defined(OBJECT_MULTI_STATE_INPUT)
    count = Multistate_Input_Count();
    for (i = 0; i < count; i++) {
        instance = Multistate_Input_Index_To_Instance(0);
        printf("Purging Multi-State Input %u\n", instance);
        Multistate_Input_Delete(instance);
    }
#endif

#if defined(OBJECT_MULTI_STATE_OUTPUT)
    count = Multistate_Output_Count();
    for (i = 0; i < count; i++) {
        instance = Multistate_Output_Index_To_Instance(0);
        printf("Purging Multi-State Output %u\n", instance);
        Multistate_Output_Delete(instance);
    }
#endif

#if defined(OBJECT_MULTI_STATE_VALUE)
    count = Multistate_Value_Count();
    for (i = 0; i < count; i++) {
        instance = Multistate_Value_Index_To_Instance(0);
        printf("Purging Multi-State Value %u\n", instance);
        Multistate_Value_Delete(instance);
    }
#endif

    /* Eclairages / Channel */
#if defined(OBJECT_CHANNEL)
    count = Channel_Count();
    for (i = 0; i < count; i++) {
        instance = Channel_Index_To_Instance(0);
        printf("Purging Channel %u\n", instance);
        Channel_Delete(instance);
    }
#endif

#if defined(OBJECT_LIGHTING_OUTPUT)
    count = Lighting_Output_Count();
    for (i = 0; i < count; i++) {
        instance = Lighting_Output_Index_To_Instance(0);
        printf("Purging Lighting Output %u\n", instance);
        Lighting_Output_Delete(instance);
    }
#endif

#if defined(OBJECT_BINARY_LIGHTING_OUTPUT)
    count = Binary_Lighting_Output_Count();
    for (i = 0; i < count; i++) {
        instance = Binary_Lighting_Output_Index_To_Instance(0);
        printf("Purging Binary Lighting Output %u\n", instance);
        Binary_Lighting_Output_Delete(instance);
    }
#endif

    /* Valeurs spéciales */
#if defined(OBJECT_BITSTRING_VALUE)
    count = BitString_Value_Count();
    for (i = 0; i < count; i++) {
        instance = BitString_Value_Index_To_Instance(0);
        printf("Purging BitString Value %u\n", instance);
        BitString_Value_Delete(instance);
    }
#endif

#if defined(OBJECT_CHARACTERSTRING_VALUE)
    count = CharacterString_Value_Count();
    for (i = 0; i < count; i++) {
        instance = CharacterString_Value_Index_To_Instance(0);
        printf("Purging CharacterString Value %u\n", instance);
        CharacterString_Value_Delete(instance);
    }
#endif

#if defined(OBJECT_OCTETSTRING_VALUE)
    count = OctetString_Value_Count();
    for (i = 0; i < count; i++) {
        instance = OctetString_Value_Index_To_Instance(0);
        printf("Purging OctetString Value %u\n", instance);
        OctetString_Value_Delete(instance);
    }
#endif

#if defined(OBJECT_POSITIVE_INTEGER_VALUE)
    count = PositiveInteger_Value_Count();
    for (i = 0; i < count; i++) {
        instance = PositiveInteger_Value_Index_To_Instance(0);
        printf("Purging Positive Integer Value %u\n", instance);
        PositiveInteger_Value_Delete(instance);
    }
#endif

#if defined(OBJECT_INTEGER_VALUE)
    count = Integer_Value_Count();
    for (i = 0; i < count; i++) {
        instance = Integer_Value_Index_To_Instance(0);
        printf("Purging Integer Value %u\n", instance);
        Integer_Value_Delete(instance);
    }
#endif

#if defined(OBJECT_TIME_VALUE)
    count = Time_Value_Count();
    for (i = 0; i < count; i++) {
        instance = Time_Value_Index_To_Instance(0);
        printf("Purging Time Value %u\n", instance);
        Time_Value_Delete(instance);
    }
#endif

    /* Planif / Journal / Commande */
#if defined(OBJECT_SCHEDULE)
    count = Schedule_Count();
    for (i = 0; i < count; i++) {
        instance = Schedule_Index_To_Instance(0);
        printf("Purging Schedule %u\n", instance);
        Schedule_Delete(instance);
    }
#endif

#if defined(OBJECT_TRENDLOG)
    count = Trend_Log_Count();
    for (i = 0; i < count; i++) {
        instance = Trend_Log_Index_To_Instance(0);
        printf("Purging Trend Log %u\n", instance);
        Trend_Log_Delete(instance);
    }
#endif

#if defined(OBJECT_COMMAND)
    count = Command_Count();
    for (i = 0; i < count; i++) {
        instance = Command_Index_To_Instance(0);
        printf("Purging Command %u\n", instance);
        Command_Delete(instance);
    }
#endif

#if defined(OBJECT_CALENDAR)
    count = Calendar_Count();
    for (i = 0; i < count; i++) {
        instance = Calendar_Index_To_Instance(0);
        printf("Purging Calendar %u\n", instance);
        Calendar_Delete(instance);
    }
#endif

#if defined(OBJECT_PROGRAM)
    count = Program_Count();
    for (i = 0; i < count; i++) {
        instance = Program_Index_To_Instance(0);
        printf("Purging Program %u\n", instance);
        Program_Delete(instance);
    }
#endif

#if defined(OBJECT_ACCUMULATOR)
    count = Accumulator_Count();
    for (i = 0; i < count; i++) {
        instance = Accumulator_Index_To_Instance(0);
        printf("Purging Accumulator %u\n", instance);
        Accumulator_Delete(instance);
    }
#endif

    /* Sécurité vie (si inclus) */
#if defined(OBJECT_LIFE_SAFETY_POINT)
    count = Life_Safety_Point_Count();
    for (i = 0; i < count; i++) {
        instance = Life_Safety_Point_Index_To_Instance(0);
        printf("Purging Life Safety Point %u\n", instance);
        Life_Safety_Point_Delete(instance);
    }
#endif

#if defined(OBJECT_LIFE_SAFETY_ZONE)
    count = Life_Safety_Zone_Count();
    for (i = 0; i < count; i++) {
        instance = Life_Safety_Zone_Index_To_Instance(0);
        printf("Purging Life Safety Zone %u\n", instance);
        Life_Safety_Zone_Delete(instance);
    }
#endif

    /* Couleur (rév. 24) */
#if defined(OBJECT_COLOR)
    count = Color_Count();
    for (i = 0; i < count; i++) {
        instance = Color_Index_To_Instance(0);
        printf("Purging Color %u\n", instance);
        Color_Delete(instance);
    }
#endif

#if defined(OBJECT_COLOR_TEMPERATURE)
    count = Color_Temperature_Count();
    for (i = 0; i < count; i++) {
        instance = Color_Temperature_Index_To_Instance(0);
        printf("Purging Color Temperature %u\n", instance);
        Color_Temperature_Delete(instance);
    }
#endif

    /* NE PAS toucher à Network Port (requis par BACnet/IP) ni Device */
    /* Afficher un résumé après purge */
    printf("Purge complete. Remaining objects:\n");
#if defined(OBJECT_ANALOG_INPUT)
    printf("Analog Input: %u\n", Analog_Input_Count());
#endif
#if defined(OBJECT_ANALOG_OUTPUT)
    printf("Analog Output: %u\n", Analog_Output_Count());
#endif
#if defined(OBJECT_ANALOG_VALUE)
    printf("Analog Value: %u\n", Analog_Value_Count());
#endif
}


/* ===== JSON -> objets =====
 * JSON attendu:
 * { "deviceId":123, "deviceName":"X",
 *   "objects":[
 *     {"type":"analog-input","instance":1,"name":"AI1","presentValue":12.3},
 *     {"type":"analog-value","instance":2,"name":"AV2","presentValue":45.6}
 *   ]
 * }
 */
static int apply_config_from_json(const char *json_text)
{
    json_error_t jerr;
    json_t *root = json_loads(json_text, 0, &jerr);
    json_t *objs;
    size_t i, n;

    if (!root) {
        fprintf(stderr, "CFGJSON: parse error at %d:%d: %s\n", jerr.line, jerr.column, jerr.text);
        return -1;
    }

    /* Device */
    if (json_is_integer(json_object_get(root, "deviceId"))) {
        Device_Set_Object_Instance_Number((uint32_t)json_integer_value(json_object_get(root, "deviceId")));
    }
    if (json_is_string(json_object_get(root, "deviceName"))) {
        const char *dn = json_string_value(json_object_get(root, "deviceName"));
        if (dn) {
            Device_Object_Name_ANSI_Init(dn);
        }
    }

    objs = json_object_get(root, "objects");
    if (!json_is_array(objs)) { json_decref(root); return -2; }

    /* Purge ancien modèle */
    purge_all_objects();

    n = json_array_size(objs);
    for (i = 0; i < n; i++) {
        json_t *it = json_array_get(objs, i);
        const char *typ;
        json_t *jinst;
        uint32_t inst;
        const char *name;
        json_t *jpv;

        if (!json_is_object(it)) { continue; }

        typ   = json_string_value(json_object_get(it, "type"));
        jinst = json_object_get(it, "instance");
        inst  = (uint32_t)json_integer_value(jinst);
        name  = json_string_value(json_object_get(it, "name"));
        jpv   = json_object_get(it, "presentValue");

        if (!typ || !json_is_integer(jinst)) {
            continue;
        }

        if (strcmp(typ, "analog-input") == 0) {
            if (Analog_Input_Create(inst)) {
                if (name) { write_string_property(OBJECT_ANALOG_INPUT, inst, PROP_OBJECT_NAME, name); }
                /* rendre PV writable */
                write_bool_property(OBJECT_ANALOG_INPUT, inst, PROP_OUT_OF_SERVICE, 1);
                if (json_is_number(jpv)) {
                    write_real_property(OBJECT_ANALOG_INPUT, inst, PROP_PRESENT_VALUE, (float)json_number_value(jpv));
                }
            }
        } else if (strcmp(typ, "analog-value") == 0) {
            if (Analog_Value_Create(inst)) {
                if (name) { write_string_property(OBJECT_ANALOG_VALUE, inst, PROP_OBJECT_NAME, name); }
                if (json_is_number(jpv)) {
                    write_real_property(OBJECT_ANALOG_VALUE, inst, PROP_PRESENT_VALUE, (float)json_number_value(jpv));
                }
            }
        } else if (strcmp(typ, "analog-output") == 0) {
            if (Analog_Output_Create(inst)) {
                if (name) { write_string_property(OBJECT_ANALOG_OUTPUT, inst, PROP_OBJECT_NAME, name); }
                if (json_is_number(jpv)) {
                    write_real_property(OBJECT_ANALOG_OUTPUT, inst, PROP_PRESENT_VALUE, (float)json_number_value(jpv));
                }
            }
        }
    }

    json_decref(root);
    return 0;
}

/* =========================
 *  Socket utilitaires
 * ========================= */
static int socket_listen_local(int port)
{
    int fd, flags, yes;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void*)&yes, sizeof(yes));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 */
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -2;
    }
    if (listen(fd, 1) < 0) {
        close(fd);
        return -3;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fd;
}

static void socket_close_all(void)
{
    if (g_client_fd >= 0) { close(g_client_fd); g_client_fd = -1; }
    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
}

static void trim_newlines(char *s)
{
    size_t n;
    if (!s) return;
    n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[n-1] = '\0';
        n--;
    }
}

static void write_pidfile_if_needed(void)
{
    if (g_pidfile[0]) {
        FILE *f = fopen(g_pidfile, "w");
        if (f) {
            fprintf(f, "%d\n", (int)getpid());
            fclose(f);
        }
    }
}

static int handle_socket_line(const char *line)
{
    /* 0 = continuer ; 1 = fermer la connexion */
    if (strncmp(line, "PING", 4) == 0) {
        (void)write(g_client_fd, "PONG\n", 5);
        return 0;
    }
    if (strncmp(line, "QUIT", 4) == 0) {
        (void)write(g_client_fd, "BYE\n", 4);
        return 1;
    }
    if (strncmp(line, "PIDFILE ", 8) == 0) {
        const char *path = line + 8;
        while (*path == ' ') path++;
        if (*path) {
            strncpy(g_pidfile, path, sizeof(g_pidfile)-1);
            g_pidfile[sizeof(g_pidfile)-1] = '\0';
            write_pidfile_if_needed();
            (void)write(g_client_fd, "OK\n", 3);
        } else {
            (void)write(g_client_fd, "ERR missing path\n", 17);
        }
        return 0;
    }
    if (strncmp(line, "CFGJSON ", 8) == 0) {
        const char *json = line + 8;
        int rc = apply_config_from_json(json);
        if (rc == 0) (void)write(g_client_fd, "OK\n", 3);
        else         (void)write(g_client_fd, "ERR\n", 4);
        return 0;
    }
    (void)write(g_client_fd, "ERR unknown\n", 12);
    return 0;
}

static void process_socket_io(void)
{
    if (g_listen_fd >= 0 && g_client_fd < 0) {
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd >= 0) {
            int flags = fcntl(cfd, F_GETFL, 0);
            if (flags >= 0) fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
            g_client_fd = cfd;
            g_cmd_len = 0;
        }
    }
    if (g_client_fd >= 0) {
        char buf[1024];
        ssize_t r = read(g_client_fd, buf, sizeof(buf));
        if (r == 0) {
            close(g_client_fd);
            g_client_fd = -1;
            g_cmd_len = 0;
        } else if (r > 0) {
            size_t i;
            for (i = 0; i < (size_t)r; i++) {
                char ch = buf[i];
                if (g_cmd_len + 1 < sizeof(g_cmd_buf)) {
                    g_cmd_buf[g_cmd_len++] = ch;
                    g_cmd_buf[g_cmd_len] = '\0';
                }
                if (ch == '\n') {
                    char line[8192];
                    size_t linelen = g_cmd_len;
                    if (linelen > 0) linelen--;
                    if (linelen >= sizeof(line)) linelen = sizeof(line)-1;
                    memcpy(line, g_cmd_buf, linelen);
                    line[linelen] = '\0';
                    trim_newlines(line);

                    if (handle_socket_line(line)) {
                        close(g_client_fd);
                        g_client_fd = -1;
                    }
                    g_cmd_len = 0;
                }
            }
        } else {
            /* EAGAIN/EWOULDBLOCK => rien */
        }
    }
}

/* ===== Handlers/services ===== */
/* Approche simplifiée pour l'initialisation sans objets */
static void Init_Service_Handlers(void)
{
    /* Initialiser le device uniquement */
    printf("Initializing Device object...\n");
    Device_Init(NULL);

    /* Avec MAX_*=0, les Init() ne créent pas d'objets par défaut */
#if defined(OBJECT_ANALOG_INPUT)
    Analog_Input_Init(); 
#endif

#if defined(OBJECT_ANALOG_OUTPUT)
    Analog_Output_Init(); 
#endif

#if defined(OBJECT_ANALOG_VALUE)
    Analog_Value_Init(); 
#endif
    
    /* Afficher le nombre d'objets au démarrage */
    printf("Objects at startup:\n");
#if defined(OBJECT_ANALOG_INPUT)
    printf("  Analog Input: %u\n", Analog_Input_Count());
#endif
#if defined(OBJECT_ANALOG_OUTPUT)
    printf("  Analog Output: %u\n", Analog_Output_Count());
#endif
#if defined(OBJECT_ANALOG_VALUE)
    printf("  Analog Value: %u\n", Analog_Value_Count());
#endif

    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is_who_am_i_unicast);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, handler_who_has);
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);

    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY,          handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE,     handler_read_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROPERTY,         handler_write_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROP_MULTIPLE,    handler_write_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_RANGE,             handler_read_range);
#if defined(BACFILE)
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_ATOMIC_READ_FILE,       handler_atomic_read_file);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_ATOMIC_WRITE_FILE,      handler_atomic_write_file);
#endif
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_REINITIALIZE_DEVICE,    handler_reinitialize_device);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_UTC_TIME_SYNCHRONIZATION, handler_timesync_utc);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_TIME_SYNCHRONIZATION,     handler_timesync);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_YOU_ARE,            handler_you_are_json_print);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV,          handler_cov_subscribe);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_COV_NOTIFICATION,   handler_ucov_notification);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_DEVICE_COMMUNICATION_CONTROL, handler_device_communication_control);

    /* timers */
    mstimer_set(&BACNET_Task_Timer, 1000UL);
    mstimer_set(&BACNET_TSM_Timer, 50UL);
    mstimer_set(&BACNET_Address_Timer, 60UL * 1000UL);
    mstimer_set(&BACNET_Object_Timer, 100UL);
}

/* ===== CLI help ===== */
static void print_usage(const char *filename)
{
    printf("Usage: %s [device-instance [device-name]] [--socketport N] [--pid PATH]\n", filename);
    printf("       [--version][--help]\n");
}
static void print_help(const char *filename)
{
    printf("Simulate a BACnet server device (minimal AI/AO/AV)\n"
           "device-instance: BACnet Device Object Instance number\n"
           "device-name:     Device object-name\n"
           "--socketport N:  port TCP local pour CFGJSON (def 55031)\n"
           "--pid PATH:      fichier PID a ecrire\n");
}

/* ===== main ===== */
int main(int argc, char *argv[])
{
    BACNET_ADDRESS src; /* address where message came from */
    uint16_t pdu_len = 0;
    uint32_t elapsed_milliseconds = 0;
    uint32_t elapsed_seconds = 0;
    BACNET_CHARACTER_STRING DeviceName;
#if defined(BACNET_TIME_MASTER)
    BACNET_DATE_TIME bdatetime;
#endif
    int argi;
    const char *filename = NULL;
    const char *envp;

    memset(&src, 0, sizeof(src));
    filename = filename_remove_path(argv[0]);

    /* options de base */
    for (argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--help") == 0) {
            print_usage(filename);
            print_help(filename);
            return 0;
        }
        if (strcmp(argv[argi], "--version") == 0) {
            printf("%s %s\n", filename, BACNET_VERSION_TEXT);
            return 0;
        }
    }

    /* device id / name si présents en positionnelle */
    if (argc > 1 && argv[1][0] != '-') {
        Device_Set_Object_Instance_Number(strtoul(argv[1], NULL, 0));
    }
    if (argc > 2 && argv[2][0] != '-') {
        Device_Object_Name_ANSI_Init(argv[2]);
    }

    /* options longues */
    for (argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--socketport") == 0 && (argi + 1) < argc) {
            g_socket_port = atoi(argv[++argi]);
        } else if (strcmp(argv[argi], "--pid") == 0 && (argi + 1) < argc) {
            strncpy(g_pidfile, argv[++argi], sizeof(g_pidfile)-1);
            g_pidfile[sizeof(g_pidfile)-1] = '\0';
        }
    }
    envp = getenv("BACSTACK_SOCKET_PORT");
    if (envp && *envp) {
        g_socket_port = atoi(envp);
    }

    printf("BACnet Server (minimal)\n"
           "BACnet Stack Version %s\n"
           "BACnet Device ID: %u\n"
           "Max APDU: %d\n",
           BACnet_Version, Device_Object_Instance_Number(), MAX_APDU);

    /* init BACnet - AVEC PRÉCAUTIONS POUR ÉVITER LES OBJETS PAR DÉFAUT */
    printf("=== Starting BACnet initialization with clean slate approach ===\n");
    
    /* 1. Purge préventive (au cas où) */
    printf("1. Preventive object purge\n");
    purge_all_objects();
    
    /* 2. Initialisation des services réseau */
    printf("2. Network services initialization\n");
    address_init();
    
    /* 3. Initialisation des handlers (notre version customisée) */
    printf("3. Custom service handler initialization (NO default objects)\n");
    Init_Service_Handlers();
    handler_timesync_set_callback_set(&datetime_timesync);

    if (Device_Object_Name(Device_Object_Instance_Number(), &DeviceName)) {
        printf("BACnet Device Name: %s\n", DeviceName.value);
    }

    /* 4. Initialisation de la datalink (peut créer des objets) */
    printf("4. Data link initialization (with careful monitoring)\n");
    dlenv_init();
    atexit(datalink_cleanup);
    
    /* 5. Vérification supplémentaire qu'il n'y a pas d'objets créés par défaut */
    printf("5. Final verification of object purge...\n");
    purge_all_objects();
    
    /* Afficher le compte d'objets pour vérification */
#if defined(OBJECT_ANALOG_INPUT)
    printf("Analog Input Count: %u\n", Analog_Input_Count());
    if (Analog_Input_Count() > 0) {
        printf("CRITICAL ERROR: Failed to purge all Analog Input objects!\n");
        return 1; /* Échec fatal si on n'arrive pas à purger */
    }
#endif
#if defined(OBJECT_ANALOG_OUTPUT)
    printf("Analog Output Count: %u\n", Analog_Output_Count());
    if (Analog_Output_Count() > 0) {
        printf("CRITICAL ERROR: Failed to purge all Analog Output objects!\n");
        return 1; /* Échec fatal si on n'arrive pas à purger */
    }
#endif
#if defined(OBJECT_ANALOG_VALUE)
    printf("Analog Value Count: %u\n", Analog_Value_Count());
    if (Analog_Value_Count() > 0) {
        printf("CRITICAL ERROR: Failed to purge all Analog Value objects!\n");
        return 1; /* Échec fatal si on n'arrive pas à purger */
    }
#endif
    
    printf("Server starting with ZERO objects as requested...\n");

    /* socket de config */
    g_listen_fd = socket_listen_local(g_socket_port);
    if (g_listen_fd >= 0) {
        printf("Config socket: 127.0.0.1:%d\n", g_socket_port);
    } else {
        printf("Config socket disabled (port %d bind error)\n", g_socket_port);
    }
    write_pidfile_if_needed();

    for (;;) {
        /* Socket local */
        process_socket_io();

        /* BACnet input */
        pdu_len = datalink_receive(&src, &Rx_Buf[0], MAX_MPDU, 1 /*ms*/);

        /* BACnet process */
        if (pdu_len) {
            npdu_handler(&src, &Rx_Buf[0], pdu_len);
        }

        /* Timers 1s */
        if (mstimer_expired(&BACNET_Task_Timer)) {
            mstimer_reset(&BACNET_Task_Timer);
            elapsed_milliseconds = mstimer_interval(&BACNET_Task_Timer);
            elapsed_seconds = elapsed_milliseconds / 1000;

            dcc_timer_seconds(elapsed_seconds);
            datalink_maintenance_timer(elapsed_seconds);
            dlenv_maintenance_timer(elapsed_seconds);
            handler_cov_timer_seconds(elapsed_seconds);
        }

        if (mstimer_expired(&BACNET_TSM_Timer)) {
            mstimer_reset(&BACNET_TSM_Timer);
            elapsed_milliseconds = mstimer_interval(&BACNET_TSM_Timer);
            tsm_timer_milliseconds(elapsed_milliseconds);
        }
        if (mstimer_expired(&BACNET_Address_Timer)) {
            mstimer_reset(&BACNET_Address_Timer);
            elapsed_milliseconds = mstimer_interval(&BACNET_Address_Timer);
            elapsed_seconds = elapsed_milliseconds / 1000;
            address_cache_timer(elapsed_seconds);
        }
    }

    socket_close_all();
    return 0;
}
