/**
 * @file dcc-stub.c
 * @brief Device Communication Control stub for bacnetStackc
 * 
 * This file provides DCC (Device Communication Control) symbols
 * in case they're not linked from the BACnet library.
 * 
 * By default, communication is ENABLED to allow Who-Is to work.
 */

#include <stdint.h>
#include <stdbool.h>
#include "bacnet/bacdef.h"

/* DCC state variables */
static uint32_t DCC_Time_Duration_Seconds = 0;
static BACNET_COMMUNICATION_ENABLE_DISABLE DCC_Enable_Disable = COMMUNICATION_ENABLE;

/**
 * Returns if network communications is enabled.
 * @return true if communication has been enabled.
 */
bool dcc_communication_enabled(void)
{
    return (DCC_Enable_Disable == COMMUNICATION_ENABLE);
}

/**
 * Returns if network communications is disabled.
 * @return true if communication has been disabled.
 */
bool dcc_communication_disabled(void)
{
    return (DCC_Enable_Disable == COMMUNICATION_DISABLE);
}

/**
 * Returns if initiation of communications is disabled.
 * @return true if disabling initiation is set.
 */
bool dcc_communication_initiation_disabled(void)
{
    return (DCC_Enable_Disable == COMMUNICATION_DISABLE_INITIATION);
}

/**
 * Returns the network communications enable/disable status.
 * @return BACnet communication status
 */
BACNET_COMMUNICATION_ENABLE_DISABLE dcc_enable_status(void)
{
    return DCC_Enable_Disable;
}

/**
 * Returns the time duration in seconds.
 * @return time in seconds (0 indicates either expired or infinite duration)
 */
uint32_t dcc_duration_seconds(void)
{
    return DCC_Time_Duration_Seconds;
}

/**
 * Called every second or so to decrement the duration timer.
 * @param seconds Time passed in seconds since last call.
 */
void dcc_timer_seconds(uint32_t seconds)
{
    if (DCC_Time_Duration_Seconds) {
        if (DCC_Time_Duration_Seconds > seconds) {
            DCC_Time_Duration_Seconds -= seconds;
        } else {
            DCC_Time_Duration_Seconds = 0;
            DCC_Enable_Disable = COMMUNICATION_ENABLE;
        }
    }
}

/**
 * Setup the communication enable/disable and duration.
 * @param status Enable/disable/disable-initiation status
 * @param minutes Duration in minutes (0 = infinite)
 * @return true if the values are set successfully
 */
bool dcc_set_status_duration(
    BACNET_COMMUNICATION_ENABLE_DISABLE status,
    uint16_t minutes)
{
    bool valid = false;

    if (status < MAX_BACNET_COMMUNICATION_ENABLE_DISABLE) {
        DCC_Enable_Disable = status;
        if (minutes == 0) {
            /* infinite - don't change the value if already non-zero */
            if (DCC_Time_Duration_Seconds == 0) {
                DCC_Time_Duration_Seconds = 0;
            }
        } else {
            DCC_Time_Duration_Seconds = (uint32_t)minutes * 60;
        }
        valid = true;
    }

    return valid;
}