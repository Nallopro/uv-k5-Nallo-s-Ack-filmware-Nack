/* Copyright 2023 Dual Tachyon



 * https://github.com/DualTachyon



 *



 * Licensed under the Apache License, Version 2.0 (the "License");



 * you may not use this file except in compliance with the License.



 * You may obtain a copy of the License at



 *



 *     http://www.apache.org/licenses/LICENSE-2.0



 *



 *     Unless required by applicable law or agreed to in writing, software



 *     distributed under the License is distributed on an "AS IS" BASIS,



 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.



 *     See the License for the specific language governing permissions and



 *     limitations under the License.



 */





#include <assert.h>



#include <stdint.h>



#include <string.h>





#include "am_fix.h"



#include "app/action.h"





#ifdef ENABLE_AIRCOPY



    #include "app/aircopy.h"



#endif



#include "app/app.h"



#include "app/chFrScanner.h"



#ifdef ENABLE_FLASHLIGHT



    #include "app/flashlight.h"



#endif



#ifdef ENABLE_FMRADIO



    #include "app/fm.h"



#endif



#include "app/generic.h"



#include "app/main.h"



#include "app/menu.h"



#include "app/scanner.h"



#ifdef ENABLE_UART



    #include "app/uart.h"



#endif



#include "ARMCM0.h"



#include "audio.h"



#include "board.h"



#include "bsp/dp32g030/gpio.h"



#include "driver/backlight.h"



#ifdef ENABLE_FMRADIO



    #include "driver/bk1080.h"



#endif



#include "driver/bk4819.h"

#include "driver/crc.h"



#include "driver/gpio.h"



#include "driver/keyboard.h"



#include "driver/st7565.h"



#include "driver/system.h"

#include "external/printf/printf.h"



#include "frequencies.h"



#include "functions.h"



#include "helper/battery.h"



#include "misc.h"



#include "radio.h"



#include "settings.h"





#if defined(ENABLE_OVERLAY)



    #include "sram-overlay.h"



#endif



#include "ui/battery.h"



#include "ui/inputbox.h"



#include "ui/main.h"



#include "ui/menu.h"



#include "ui/status.h"



#include "ui/ui.h"





static bool flagSaveVfo;



static bool flagSaveSettings;



static bool flagSaveChannel;



// ----------------------------------------------------



// Nallo SqlAck



// ----------------------------------------------------





typedef struct



{



    bool     sawOpen;



    bool     pending;





    uint16_t rxTime_10ms;



    uint16_t delay_10ms;



    uint16_t window_10ms;



    uint16_t cooldown_10ms;





    // Canal que estaba cargado cuando se recibió.



    // Si el usuario cambia de canal mientras espera el ACK,



    // cancelaremos ese ACK.



    uint8_t  channel;



} SQLACK_State_t;





static SQLACK_State_t gSqlAck[2];



#define SQLACK_MIN_RX_10MS  70   // 700 ms





#define SQLACK_INITIAL_MIN_10MS   300     // 3.00 s



#define SQLACK_INITIAL_MAX_10MS   500     // 5.00 s





#define SQLACK_RETRY_MAX_10MS      50     // 0.00 - 0.50 s





#define SQLACK_WINDOW_10MS       1000     // 10 segundos máximo



#define SQLACK_COOLDOWN_10MS     2000     // 20 segundos



#define DUAL_RX_RESPOND_HOLD_10MS 1050





static void ProcessKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);





static uint16_t SQLACK_RandomDelay(uint16_t min, uint16_t max)



{



    const uint16_t span = (max - min) + 1;





    const uint16_t seed =



        (uint16_t)gFlashLightBlinkCounter ^



        BK4819_GetRSSI();





    return min + (seed % span);



}





// ---------------------------------------------------------------------------
// Nallo digital protocol - fixed 14-byte frame at 1200 bps 2FSK
// [0] MAGIC
// [1] TYPE
// [2..5] source ID
// [6..9] destination ID ("0000" for broadcast)
// [10] value (SNR 0..40 where used)
// [11] reserved
// [12..13] CRC16 over bytes 0..11
// ---------------------------------------------------------------------------
#define NACK_MAGIC                0xA7u
#define NACK_TYPE_ACK_ID          0x01u
#define NACK_TYPE_SNR_REQ         0x02u
#define NACK_TYPE_SNR_RESP        0x03u
#define NACK_TYPE_EMERGENCY       0x04u
#define NACK_TYPE_EMERGENCY_ACK   0x05u
#define NACK_PACKET_BYTES         14u
#define NACK_PACKET_WORDS         7u
#define NACK_RESPONSE_WINDOW_10MS 1000u
#define NACK_EMG_COOLDOWN_10MS    2000u
#define NACK_ALERT_TIME_10MS      3000u

typedef enum {
    NACK_PENDING_NONE = 0,
    NACK_PENDING_SNR_RESP,
    NACK_PENDING_EMG_ACK
} NACK_PendingType_t;

typedef struct {
    NACK_PendingType_t type;
    uint8_t vfo;
    uint8_t channel;
    uint8_t value;
    char target[4];
    uint16_t delay_10ms;
    uint16_t window_10ms;
} NACK_PendingResponse_t;

static uint16_t gNackFskWords[NACK_PACKET_WORDS];
static uint8_t gNackFskWriteIndex;
static uint8_t gNackRxSnr;
static uint16_t gNackEmgCooldown_10ms[2];
static NACK_PendingResponse_t gNackPending;
static uint16_t gNackEmergencyAlert_10ms;
static bool gNackEmergencySound;

static bool NACK_IsValidIdChar(const char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
}

static bool NACK_IdEquals(const char a[4], const char b[4])
{
    return memcmp(a, b, 4) == 0;
}

static bool NACK_IsOwnId(const char id[4])
{
    return NACK_IdEquals(id, gEeprom.ACK_ID);
}

static uint8_t NACK_ReadSnr(void)
{
    // BK4819 does not expose a direct dB SNR register in this firmware.
    // Its extended-noise indicator is 0=clean .. 127=noisy, so map it
    // to the requested user scale 40..0 dB.
    const uint8_t noise = BK4819_GetExNoiceIndicator();
    const uint8_t loss = (uint8_t)(((uint16_t)noise * 40u + 63u) / 127u);
    return (loss >= 40u) ? 0u : (uint8_t)(40u - loss);
}

static void NACK_BuildPacket(uint8_t packet[NACK_PACKET_BYTES], uint8_t type,
                             const char target[4], uint8_t value)
{
    memset(packet, 0, NACK_PACKET_BYTES);
    packet[0] = NACK_MAGIC;
    packet[1] = type;

    for (unsigned int i = 0; i < 4; i++) {
        char c = gEeprom.ACK_ID[i];
        if (!NACK_IsValidIdChar(c)) c = '0';
        packet[2 + i] = (uint8_t)c;
    }

    for (unsigned int i = 0; i < 4; i++) {
        char c = target ? target[i] : '0';
        if (!NACK_IsValidIdChar(c)) c = '0';
        packet[6 + i] = (uint8_t)c;
    }

    packet[10] = value;
    packet[11] = 0;
    const uint16_t crc = CRC_Calculate(packet, 12);
    packet[12] = (uint8_t)(crc & 0xFFu);
    packet[13] = (uint8_t)(crc >> 8);
}

static bool NACK_SendPacketOnVfo(const uint8_t vfo, uint8_t type,
                                 const char target[4], uint8_t value,
                                 uint8_t expectedChannel)
{
    if (vfo > 1 || gMonitor || gPttIsPressed || gCurrentFunction == FUNCTION_TRANSMIT)
        return false;

    VFO_Info_t *pVfo = &gEeprom.VfoInfo[vfo];
    if (expectedChannel != 0xFFu && pVfo->CHANNEL_SAVE != expectedChannel)
        return false;
    if ((gEeprom.RX_VFO & 1u) != vfo && expectedChannel != 0xFFu)
        return false;

    gRxVfo = pVfo;
    const bool oldRxActive = gRxVfoIsActive;
    gRxVfoIsActive = true;

    FUNCTION_Select(FUNCTION_FOREGROUND);
    RADIO_PrepareTX();
    if (gCurrentFunction != FUNCTION_TRANSMIT) {
        gRxVfoIsActive = oldRxActive;
        return false;
    }

    uint8_t packet[NACK_PACKET_BYTES];
    NACK_BuildPacket(packet, type, target, value);
    SYSTEM_DelayMs(60);
    BK4819_SendNackFSK(packet);
    RADIO_SetupRegisters(false);
    FUNCTION_Select(FUNCTION_FOREGROUND);

    gRxVfoIsActive = oldRxActive;
    gUpdateStatus = true;
    gUpdateDisplay = true;
    return true;
}

static void NACK_QueueResponse(NACK_PendingType_t type, uint8_t vfo,
                               const char target[4], uint8_t value)
{
    if (gNackPending.type != NACK_PENDING_NONE)
        return;

    gNackPending.type = type;
    gNackPending.vfo = vfo;
    gNackPending.channel = gEeprom.VfoInfo[vfo].CHANNEL_SAVE;
    gNackPending.value = value;
    memcpy(gNackPending.target, target, 4);
    gNackPending.delay_10ms = SQLACK_RandomDelay(SQLACK_INITIAL_MIN_10MS,
                                                 SQLACK_INITIAL_MAX_10MS);
    gNackPending.window_10ms = NACK_RESPONSE_WINDOW_10MS;
}

static void NACK_StopEmergencyAlert(void)
{
    // Always force the outputs off.  This is also called exactly when the
    // countdown reaches zero, so an early return here would leave LIGHT on.
    gNackEmergencyAlert_10ms = 0;
    gNackEmergencySound = false;
#ifdef ENABLE_FLASHLIGHT
    gFlashLightState = FLASHLIGHT_OFF;
    GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
#endif
    gUpdateDisplay = true;
}

void APP_NackEmergencyTick10ms(void)
{
    // Emergency alerts intentionally have no automatic timeout.
    // They stay active until the user presses any key.
}

static void NACK_StartEmergencyAlert(void)
{
    gNackEmergencyAlert_10ms = 1u;
    gNackEmergencySound = (gEeprom.EMG_RX_MODE == EMG_RX_SOUND ||
                           gEeprom.EMG_RX_MODE == EMG_RX_LIGHT_SOUND);
#ifdef ENABLE_FLASHLIGHT
    if (gEeprom.EMG_RX_MODE == EMG_RX_LIGHT ||
        gEeprom.EMG_RX_MODE == EMG_RX_LIGHT_SOUND) {
        gFlashLightState = FLASHLIGHT_BLINK;
    }
#endif
}

static void NACK_ProcessReceivedPacket(void)
{
    uint8_t packet[NACK_PACKET_BYTES];
    if (gNackFskWriteIndex < NACK_PACKET_WORDS)
        return;

    for (unsigned int i = 0; i < NACK_PACKET_WORDS; i++) {
        packet[i * 2] = (uint8_t)(gNackFskWords[i] & 0xFFu);
        packet[i * 2 + 1] = (uint8_t)(gNackFskWords[i] >> 8);
    }

    if (packet[0] != NACK_MAGIC)
        return;
    const uint16_t rxCrc = (uint16_t)packet[12] | ((uint16_t)packet[13] << 8);
    if (CRC_Calculate(packet, 12) != rxCrc)
        return;

    for (unsigned int i = 2; i < 10; i++)
        if (!NACK_IsValidIdChar((char)packet[i]))
            return;

    const char *source = (const char *)&packet[2];
    const char *target = (const char *)&packet[6];
    const uint8_t vfo = gEeprom.RX_VFO & 1u;

    // A recognised digital packet must never produce the normal SqlAck.
    gSqlAck[vfo].sawOpen = false;
    gSqlAck[vfo].rxTime_10ms = 0;
    gSqlAck[vfo].pending = false;

    if (NACK_IsOwnId(source))
        return;

    switch (packet[1]) {
        case NACK_TYPE_ACK_ID:
            UI_MAIN_NotifyAckId(source);
            break;

        case NACK_TYPE_SNR_REQ:
            // No cooldown: every valid SNR request may receive a response.
            NACK_QueueResponse(NACK_PENDING_SNR_RESP, vfo, source, gNackRxSnr);
            break;

        case NACK_TYPE_SNR_RESP:
            if (NACK_IsOwnId(target))
                UI_MAIN_NotifySnr(source, packet[10] > 40 ? 40 : packet[10]);
            break;

        case NACK_TYPE_EMERGENCY:
            UI_MAIN_NotifyEmergency(source);
            NACK_StartEmergencyAlert();
            // MUTE means never acknowledge, including emergency.
            // OFF/BEEP/ID all send the digital emergency acknowledgement.
            if (gEeprom.VfoInfo[vfo].SQL_ACK_MODE != SQL_ACK_MUTE &&
                gNackEmgCooldown_10ms[vfo] == 0)
                NACK_QueueResponse(NACK_PENDING_EMG_ACK, vfo, source, 0);
            break;

        case NACK_TYPE_EMERGENCY_ACK:
            if (NACK_IsOwnId(target))
                UI_MAIN_NotifyAckId(source);
            break;

        default:
            break;
    }
}

void NACK_ActionSnrRequest(void)
{
    const uint8_t vfo = gEeprom.TX_VFO & 1u;
    if (!NACK_SendPacketOnVfo(vfo, NACK_TYPE_SNR_REQ, NULL, 0, 0xFFu))
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
}

void NACK_ActionEmergency(void)
{
    const uint8_t vfo = gEeprom.TX_VFO & 1u;
    if (!NACK_SendPacketOnVfo(vfo, NACK_TYPE_EMERGENCY, NULL, 0, 0xFFu))
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
}

void (*ProcessKeysFunctions[])(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld) = {



    [DISPLAY_MAIN] = &MAIN_ProcessKeys,



    [DISPLAY_MENU] = &MENU_ProcessKeys,



    [DISPLAY_SCANNER] = &SCANNER_ProcessKeys,





#ifdef ENABLE_FMRADIO



    [DISPLAY_FM] = &FM_ProcessKeys,



#endif





#ifdef ENABLE_AIRCOPY



    [DISPLAY_AIRCOPY] = &AIRCOPY_ProcessKeys,



#endif



};





static_assert(ARRAY_SIZE(ProcessKeysFunctions) == DISPLAY_N_ELEM);





static bool SQLACK_SendBeep(const uint8_t vfo)



{



    if (vfo > 1)



        return false;





    VFO_Info_t *pVfo = &gEeprom.VfoInfo[vfo];





    // Volver a comprobar la opción DEL CANAL receptor



    if (pVfo->SQL_ACK_MODE != SQL_ACK_BEEP && pVfo->SQL_ACK_MODE != SQL_ACK_ID)



        return false;





    if (gEeprom.SQUELCH_LEVEL == 0)



        return false;





    if (gMonitor)



        return false;





    if (gPttIsPressed)



        return false;





    if (gCurrentFunction == FUNCTION_TRANSMIT)



        return false;





    // Si durante los 3-10 segundos el usuario ha cambiado



    // ese VFO a otro canal, no mandar un ACK en el canal nuevo.



    if (pVfo->CHANNEL_SAVE != gSqlAck[vfo].channel)



        return false;





    /*



     * MUY IMPORTANTE:



     * solo transmitimos cuando el receptor está actualmente



     * situado en el mismo VFO que originó el ACK.



     */



    if ((gEeprom.RX_VFO & 1u) != vfo)



        return false;





    /*



     * gRxVfo tiene que señalar exactamente al VFO receptor.



     * NO modificamos gEeprom.TX_VFO, así que no cambiamos



     * cuál es el MAIN del usuario.



     */



    gRxVfo = &gEeprom.VfoInfo[vfo];





    /*



     * Evita que RADIO_PrepareTX(), con Dual Watch activo,



     * fuerce la transmisión al TX_VFO principal.



     */



    const bool oldRxActive = gRxVfoIsActive;



    gRxVfoIsActive = true;





    FUNCTION_Select(FUNCTION_FOREGROUND);





    RADIO_PrepareTX();





    if (gCurrentFunction != FUNCTION_TRANSMIT)



    {



        gRxVfoIsActive = oldRxActive;



        return false;



    }





    SYSTEM_DelayMs(100);





    // beep 1



    BK4819_TransmitTone(false, 1000);



    BK4819_ExitTxMute();



    SYSTEM_DelayMs(70);





    // pausa muy corta



    BK4819_EnterTxMute();



    SYSTEM_DelayMs(45);





    // beep 2



    BK4819_TransmitTone(false, 1000);



    BK4819_ExitTxMute();



    SYSTEM_DelayMs(70);





    BK4819_EnterTxMute();





    /*



     * Volver a RX en ese mismo VFO.



     * Dual Watch continuará después normalmente.



     */



    RADIO_SetupRegisters(false);



    FUNCTION_Select(FUNCTION_FOREGROUND);





    gRxVfoIsActive = oldRxActive;





    gUpdateStatus  = true;



    gUpdateDisplay = true;





    return true;



}





static bool SQLACK_SendId(const uint8_t vfo)
{
    if (vfo > 1) return false;
    VFO_Info_t *pVfo = &gEeprom.VfoInfo[vfo];
    if (pVfo->SQL_ACK_MODE != SQL_ACK_ID) return false;
    if (gEeprom.SQUELCH_LEVEL == 0 || gMonitor || gPttIsPressed || gCurrentFunction == FUNCTION_TRANSMIT) return false;
    if (pVfo->CHANNEL_SAVE != gSqlAck[vfo].channel) return false;
    if ((gEeprom.RX_VFO & 1u) != vfo) return false;

    gRxVfo = &gEeprom.VfoInfo[vfo];
    const bool oldRxActive = gRxVfoIsActive;
    gRxVfoIsActive = true;
    FUNCTION_Select(FUNCTION_FOREGROUND);
    RADIO_PrepareTX();
    if (gCurrentFunction != FUNCTION_TRANSMIT) {
        gRxVfoIsActive = oldRxActive;
        return false;
    }

    uint8_t packet[NACK_PACKET_BYTES];
    NACK_BuildPacket(packet, NACK_TYPE_ACK_ID, NULL, 0);
    SYSTEM_DelayMs(60);
    BK4819_SendNackFSK(packet);
    RADIO_SetupRegisters(false);
    FUNCTION_Select(FUNCTION_FOREGROUND);
    gRxVfoIsActive = oldRxActive;
    gUpdateStatus = true;
    gUpdateDisplay = true;
    return true;
}

static void CheckForIncoming(void)



{



    if (!g_SquelchLost)



        return;          // squelch is closed





    // squelch is open





    if (gScanStateDir == SCAN_OFF)



    {   // not RF scanning



        if (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF)



        {   // dual watch is disabled





            #ifdef ENABLE_NOAA



                if (gIsNoaaMode)



                {



                    gNOAA_Countdown_10ms = NOAA_countdown_3_10ms;



                    gScheduleNOAA        = false;



                }



            #endif





            if (gCurrentFunction != FUNCTION_INCOMING)



            {



                FUNCTION_Select(FUNCTION_INCOMING);



                //gUpdateDisplay = true;



            }





            return;



        }





        // dual watch is enabled and we're RX'ing a signal





        if (gRxReceptionMode != RX_MODE_NONE)



        {



            if (gCurrentFunction != FUNCTION_INCOMING)



            {



                FUNCTION_Select(FUNCTION_INCOMING);



                //gUpdateDisplay = true;



            }



            return;



        }





        gDualWatchCountdown_10ms = dual_watch_count_after_rx_10ms;



        gScheduleDualWatch       = false;





        // let the user see DW is not active



        gDualWatchActive = false;



        gUpdateStatus    = true;



    }



    else



    {   // RF scanning



        if (gRxReceptionMode != RX_MODE_NONE)



        {



            if (gCurrentFunction != FUNCTION_INCOMING)



            {



                FUNCTION_Select(FUNCTION_INCOMING);



                //gUpdateDisplay = true;



            }



            return;



        }





        gScanPauseDelayIn_10ms = scan_pause_delay_in_3_10ms;



        gScheduleScanListen    = false;



    }





    gRxReceptionMode = RX_MODE_DETECTED;





    if (gCurrentFunction != FUNCTION_INCOMING)



    {



        FUNCTION_Select(FUNCTION_INCOMING);



        //gUpdateDisplay = true;



    }



}





static void HandleIncoming(void)



{



    if (!g_SquelchLost) {   // squelch is closed





                // ------------------------------------------------



        // SqlAck: acabamos de recibir una transmisión



        // y ahora el squelch se ha cerrado



        // ------------------------------------------------





        if (gCurrentFunction != FUNCTION_FOREGROUND) {



            FUNCTION_Select(FUNCTION_FOREGROUND);



            gUpdateDisplay = true;



        }



        return;



    }





    bool bFlag = (gScanStateDir == SCAN_OFF && gCurrentCodeType == CODE_TYPE_OFF);





#ifdef ENABLE_NOAA



    if (IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE) && gNOAACountdown_10ms > 0) {



        gNOAACountdown_10ms = 0;



        bFlag               = true;



    }



#endif





    if (g_CTCSS_Lost && gCurrentCodeType == CODE_TYPE_CONTINUOUS_TONE) {



        bFlag       = true;



        gFoundCTCSS = false;



    }





    if (g_CDCSS_Lost && gCDCSSCodeType == CDCSS_POSITIVE_CODE



        && (gCurrentCodeType == CODE_TYPE_DIGITAL || gCurrentCodeType == CODE_TYPE_REVERSE_DIGITAL))



    {



        gFoundCDCSS = false;



    }



    else if (!bFlag)



        return;





    APP_StartListening(gMonitor ? FUNCTION_MONITOR : FUNCTION_RECEIVE);



}





static void HandleReceive(void)



{



    #define END_OF_RX_MODE_SKIP 0



    #define END_OF_RX_MODE_END  1



    #define END_OF_RX_MODE_TTE  2





    uint8_t Mode = END_OF_RX_MODE_SKIP;





    if (gFlagTailToneEliminationComplete) {



        Mode = END_OF_RX_MODE_END;



        goto Skip;



    }





    if (gScanStateDir != SCAN_OFF && IS_FREQ_CHANNEL(gNextMrChannel)) { // we are scanning in the frequency mode



        if (g_SquelchLost)



            return;





        Mode = END_OF_RX_MODE_END;



        goto Skip;



    }





    switch (gCurrentCodeType) {



        default:



        case CODE_TYPE_OFF:



            break;





        case CODE_TYPE_CONTINUOUS_TONE:



            if (gFoundCTCSS && gFoundCTCSSCountdown_10ms == 0) {



                gFoundCTCSS = false;



                gFoundCDCSS = false;



                Mode        = END_OF_RX_MODE_END;



                goto Skip;



            }



            break;





        case CODE_TYPE_DIGITAL:



        case CODE_TYPE_REVERSE_DIGITAL:



            if (gFoundCDCSS && gFoundCDCSSCountdown_10ms == 0) {



                gFoundCTCSS = false;



                gFoundCDCSS = false;



                Mode        = END_OF_RX_MODE_END;



                goto Skip;



            }



            break;



    }





    if (g_SquelchLost) {



        if (!gEndOfRxDetectedMaybe



#ifdef ENABLE_NOAA



        && !IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE)



#endif



        ) {



            switch (gCurrentCodeType) {



                case CODE_TYPE_OFF:



                    if (gEeprom.SQUELCH_LEVEL) {



                        if (g_CxCSS_TAIL_Found) {



                            Mode               = END_OF_RX_MODE_TTE;



                            g_CxCSS_TAIL_Found = false;



                        }



                    }



                    break;





                case CODE_TYPE_CONTINUOUS_TONE:



                    if (g_CTCSS_Lost) {



                        gFoundCTCSS = false;



                    }



                    else if (!gFoundCTCSS) {



                        gFoundCTCSS               = true;



                        gFoundCTCSSCountdown_10ms = 100;   // 1 sec



                    }





                    if (g_CxCSS_TAIL_Found) {



                        Mode               = END_OF_RX_MODE_TTE;



                        g_CxCSS_TAIL_Found = false;



                    }



                    break;





                case CODE_TYPE_DIGITAL:



                case CODE_TYPE_REVERSE_DIGITAL:



                    if (g_CDCSS_Lost && gCDCSSCodeType == CDCSS_POSITIVE_CODE) {



                        gFoundCDCSS = false;



                    }



                    else if (!gFoundCDCSS) {



                        gFoundCDCSS               = true;



                        gFoundCDCSSCountdown_10ms = 100;   // 1 sec



                    }





                    if (g_CxCSS_TAIL_Found) {



                        if (BK4819_GetCTCType() == 1)



                            Mode = END_OF_RX_MODE_TTE;





                        g_CxCSS_TAIL_Found = false;



                    }





                    break;



            }



        }



    }



    else



        Mode = END_OF_RX_MODE_END;





    if (!gEndOfRxDetectedMaybe         &&



         Mode == END_OF_RX_MODE_SKIP   &&



         gNextTimeslice40ms            &&



         gEeprom.TAIL_TONE_ELIMINATION &&



         (gCurrentCodeType == CODE_TYPE_DIGITAL || gCurrentCodeType == CODE_TYPE_REVERSE_DIGITAL) &&



         BK4819_GetCTCType() == 1)



        Mode = END_OF_RX_MODE_TTE;



    else



        gNextTimeslice40ms = false;





Skip:



    switch (Mode) {



        case END_OF_RX_MODE_SKIP:



            break;





        case END_OF_RX_MODE_END:



            RADIO_SetupRegisters(true);





#ifdef ENABLE_NOAA



            if (IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE))



                gNOAACountdown_10ms = 300;         // 3 sec



#endif



            gUpdateDisplay = true;





            if (gScanStateDir != SCAN_OFF) {



                switch (gEeprom.SCAN_RESUME_MODE) {



                    case SCAN_RESUME_TO:



                        break;





                    case SCAN_RESUME_CO:



                        gScanPauseDelayIn_10ms = scan_pause_delay_in_7_10ms;



                        gScheduleScanListen    = false;



                        break;





                    case SCAN_RESUME_SE:



                        CHFRSCANNER_Stop();



                        break;



                }



            }





            break;





        case END_OF_RX_MODE_TTE:



            if (gEeprom.TAIL_TONE_ELIMINATION) {



                AUDIO_AudioPathOff();





                gTailToneEliminationCountdown_10ms = 20;



                gFlagTailToneEliminationComplete   = false;



                gEndOfRxDetectedMaybe = true;



                gEnableSpeaker        = false;



            }



            break;



    }



}





static void HandlePowerSave()



{



    if (!gRxIdleMode) {



        CheckForIncoming();



    }



}





static void (*HandleFunction_fn_table[])(void) = {



    [FUNCTION_FOREGROUND] = &CheckForIncoming,



    [FUNCTION_TRANSMIT] = &FUNCTION_NOP,



    [FUNCTION_MONITOR] = &FUNCTION_NOP,



    [FUNCTION_INCOMING] = &HandleIncoming,



    [FUNCTION_RECEIVE] = &HandleReceive,



    [FUNCTION_POWER_SAVE] = &HandlePowerSave,



    [FUNCTION_BAND_SCOPE] = &FUNCTION_NOP,



};





static_assert(ARRAY_SIZE(HandleFunction_fn_table) == FUNCTION_N_ELEM);





static void HandleFunction(void)



{



    HandleFunction_fn_table[gCurrentFunction]();



}





void APP_StartListening(FUNCTION_Type_t function)



{



    const unsigned int vfo = gEeprom.RX_VFO;





#ifdef ENABLE_FMRADIO



    if (gFmRadioMode)



        BK1080_Init0();



#endif





    // clear the other vfo's rssi level (to hide the antenna symbol)



    gVFO_RSSI_bar_level[!vfo] = 0;





    AUDIO_AudioPathOn();



    gEnableSpeaker = true;





    if (gSetting_backlight_on_tx_rx & BACKLIGHT_ON_TR_RX) {



        BACKLIGHT_TurnOn();



    }





    if (gScanStateDir != SCAN_OFF)



        CHFRSCANNER_Found();





#ifdef ENABLE_NOAA



    if (IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE) && gIsNoaaMode) {



        gRxVfo->CHANNEL_SAVE        = gNoaaChannel + NOAA_CHANNEL_FIRST;



        gRxVfo->pRX->Frequency      = NoaaFrequencyTable[gNoaaChannel];



        gRxVfo->pTX->Frequency      = NoaaFrequencyTable[gNoaaChannel];



        gEeprom.ScreenChannel[vfo] = gRxVfo->CHANNEL_SAVE;





        gNOAA_Countdown_10ms        = 500;   // 5 sec



        gScheduleNOAA               = false;



    }



#endif





    if (gScanStateDir == SCAN_OFF &&



        gEeprom.DUAL_WATCH != DUAL_WATCH_OFF)



    {   // not scanning, dual watch is enabled





        gDualWatchCountdown_10ms = dual_watch_count_after_2_10ms;



        gScheduleDualWatch       = false;





        // when crossband is active only the main VFO should be used for TX



        if(gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF)



            gRxVfoIsActive = true;





        // let the user see DW is not active



        gDualWatchActive = false;



        gUpdateStatus    = true;



    }





    BK4819_WriteRegister(BK4819_REG_48,



        (11u << 12)                |     // ??? .. 0 to 15, doesn't seem to make any difference



        ( 0u << 10)                |     // AF Rx Gain-1



        (gEeprom.VOLUME_GAIN << 4) |     // AF Rx Gain-2



        (gEeprom.DAC_GAIN    << 0));     // AF DAC Gain (after Gain-1 and Gain-2)





#ifdef ENABLE_VOICE



    if (gVoiceWriteIndex == 0)       // AM/FM RX mode will be set when the voice has finished



#endif



        RADIO_SetModulation(gRxVfo->Modulation);  // no need, set it now





    FUNCTION_Select(function);





#ifdef ENABLE_FMRADIO



    if (function == FUNCTION_MONITOR || gFmRadioMode)



#else



    if (function == FUNCTION_MONITOR)



#endif



    {   // squelch is disabled



        if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu



            GUI_SelectNextDisplay(DISPLAY_MAIN);



    }



    else



        gUpdateDisplay = true;





    gUpdateStatus = true;



}





uint32_t APP_SetFreqByStepAndLimits(VFO_Info_t *pInfo, int8_t direction, uint32_t lower, uint32_t upper)



{



    uint32_t Frequency = FREQUENCY_RoundToStep(pInfo->freq_config_RX.Frequency + (direction * pInfo->StepFrequency), pInfo->StepFrequency);





    if (Frequency >= upper)



        Frequency =  lower;



    else if (Frequency < lower)



        Frequency = FREQUENCY_RoundToStep(upper - pInfo->StepFrequency, pInfo->StepFrequency);





    return Frequency;



}





uint32_t APP_SetFrequencyByStep(VFO_Info_t *pInfo, int8_t direction)



{



    return APP_SetFreqByStepAndLimits(pInfo, direction, frequencyBandTable[pInfo->Band].lower, frequencyBandTable[pInfo->Band].upper);



}





#ifdef ENABLE_NOAA



    static void NOAA_IncreaseChannel(void)



    {



        if (++gNoaaChannel > 9)



            gNoaaChannel = 0;



    }



#endif





static void DualwatchAlternate(void)



{



    #ifdef ENABLE_NOAA



        if (gIsNoaaMode)



        {



            if (!IS_NOAA_CHANNEL(gEeprom.ScreenChannel[0]) || !IS_NOAA_CHANNEL(gEeprom.ScreenChannel[1]))



                gEeprom.RX_VFO = (gEeprom.RX_VFO + 1) & 1;



            else



                gEeprom.RX_VFO = 0;





            gRxVfo = &gEeprom.VfoInfo[gEeprom.RX_VFO];





            if (IS_NOAA_CHANNEL(gEeprom.VfoInfo[0].CHANNEL_SAVE))



                NOAA_IncreaseChannel();



        }



        else



    #endif



    {   // toggle between VFO's



        gEeprom.RX_VFO = !gEeprom.RX_VFO;



        gRxVfo         = &gEeprom.VfoInfo[gEeprom.RX_VFO];





        if (!gDualWatchActive)



        {   // let the user see DW is active



            gDualWatchActive = true;



            gUpdateStatus    = true;



        }



    }





    RADIO_SetupRegisters(false);





    #ifdef ENABLE_NOAA



        gDualWatchCountdown_10ms = gIsNoaaMode ? dual_watch_count_noaa_10ms : dual_watch_count_toggle_10ms;



    #else



        gDualWatchCountdown_10ms = dual_watch_count_toggle_10ms;



    #endif



}





static void CheckRadioInterrupts(void)



{



    if (SCANNER_IsScanning())



        return;





    while (BK4819_ReadRegister(BK4819_REG_0C) & 1u) { // BK chip interrupt request



        // clear interrupts



        BK4819_WriteRegister(BK4819_REG_02, 0);



        // fetch interrupt status bits





        union {



            struct {



                uint16_t __UNUSED : 1;



                uint16_t fskRxSync : 1;



                uint16_t sqlLost : 1;



                uint16_t sqlFound : 1;



                uint16_t voxLost : 1;



                uint16_t voxFound : 1;



                uint16_t ctcssLost : 1;



                uint16_t ctcssFound : 1;



                uint16_t cdcssLost : 1;



                uint16_t cdcssFound : 1;



                uint16_t cssTailFound : 1;



                uint16_t __UNUSED_DTMF : 1;



                uint16_t fskFifoAlmostFull : 1;



                uint16_t fskRxFinied : 1;



                uint16_t fskFifoAlmostEmpty : 1;



                uint16_t fskTxFinied : 1;



            };



            uint16_t __raw;



        } interrupts;





        interrupts.__raw = BK4819_ReadRegister(BK4819_REG_02);





        // 0 = no phase shift



        // 1 = 120deg phase shift



        // 2 = 180deg phase shift



        // 3 = 240deg phase shift



//      const uint8_t ctcss_shift = BK4819_GetCTCShift();



//      if (ctcss_shift > 0)



//          g_CTCSS_Lost = true;













        if (interrupts.cssTailFound)



            g_CxCSS_TAIL_Found = true;





        if (interrupts.cdcssLost) {



            g_CDCSS_Lost = true;



            gCDCSSCodeType = BK4819_GetCDCSSCodeType();



        }





        if (interrupts.cdcssFound)



            g_CDCSS_Lost = false;





        if (interrupts.ctcssLost)



            g_CTCSS_Lost = true;





        if (interrupts.ctcssFound)



            g_CTCSS_Lost = false;





#ifdef ENABLE_VOX



        if (interrupts.voxLost) {



            g_VOX_Lost         = true;



            gVoxPauseCountdown = 10;





            if (gEeprom.VOX_SWITCH) {



                if (gCurrentFunction == FUNCTION_POWER_SAVE && !gRxIdleMode) {



                    gPowerSave_10ms            = power_save2_10ms;



                    gPowerSaveCountdownExpired = 0;



                }





                if (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF && (gScheduleDualWatch || gDualWatchCountdown_10ms < dual_watch_count_after_vox_10ms)) {



                    gDualWatchCountdown_10ms = dual_watch_count_after_vox_10ms;



                    gScheduleDualWatch = false;





                    // let the user see DW is not active



                    gDualWatchActive = false;



                    gUpdateStatus    = true;



                }



            }



        }





        if (interrupts.voxFound) {



            g_VOX_Lost         = false;



            gVoxPauseCountdown = 0;



        }



#endif





if (interrupts.sqlLost)



{



    const uint8_t vfo = gEeprom.RX_VFO & 1u;





    g_SquelchLost = true;





    gSqlAck[vfo].sawOpen    = true;



    gSqlAck[vfo].rxTime_10ms = 0;





    BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, true);



}



if (interrupts.sqlFound)



{



    const uint8_t vfo = gEeprom.RX_VFO & 1u;



    VFO_Info_t *pVfo = &gEeprom.VfoInfo[vfo];





    g_SquelchLost = false;





    BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);





    // SOLO se mira la configuración SqlAck del VFO



    // que realmente recibió la transmisión.



    if (gSqlAck[vfo].sawOpen &&



        gSqlAck[vfo].rxTime_10ms >= SQLACK_MIN_RX_10MS &&



        (pVfo->SQL_ACK_MODE == SQL_ACK_BEEP || pVfo->SQL_ACK_MODE == SQL_ACK_ID) &&



        gEeprom.SQUELCH_LEVEL > 0 &&



        !gMonitor &&



        gScanStateDir == SCAN_OFF &&



        gSqlAck[vfo].cooldown_10ms == 0)



    {



        if (!gSqlAck[vfo].pending)



        {



            gSqlAck[vfo].pending = true;





            gSqlAck[vfo].delay_10ms =



                SQLACK_RandomDelay(



                    SQLACK_INITIAL_MIN_10MS,



                    SQLACK_INITIAL_MAX_10MS);





            gSqlAck[vfo].window_10ms = SQLACK_WINDOW_10MS;





            // Recordar exactamente qué canal originó el ACK



            gSqlAck[vfo].channel = pVfo->CHANNEL_SAVE;



        }



    }





    // Siempre resetear la medición de esta recepción



    gSqlAck[vfo].rxTime_10ms = 0;



    gSqlAck[vfo].sawOpen = false;



}





// Nallo ACK-ID decoder. Active regardless of SqlAck mode.
#ifdef ENABLE_AIRCOPY
        if (gScreenToDisplay != DISPLAY_AIRCOPY)
#endif
        {
            if (interrupts.fskRxSync) {
                gNackFskWriteIndex = 0;
                gNackRxSnr = NACK_ReadSnr();
            }
            // The BK4819 FSK FIFO "almost full" interrupt means that 4 words
            // are ready.  Do NOT drain all 7 words here: reading past the four
            // available words corrupts a 14-byte packet.  Read four now and
            // the final three when RX_FINISHED arrives.
            if (interrupts.fskFifoAlmostFull) {
                for (unsigned int i = 0;
                     i < 4 && gNackFskWriteIndex < NACK_PACKET_WORDS;
                     i++)
                    gNackFskWords[gNackFskWriteIndex++] = BK4819_ReadRegister(BK4819_REG_5F);
            }
            if (interrupts.fskRxFinied) {
                while (gNackFskWriteIndex < NACK_PACKET_WORDS)
                    gNackFskWords[gNackFskWriteIndex++] = BK4819_ReadRegister(BK4819_REG_5F);
                NACK_ProcessReceivedPacket();
                gNackFskWriteIndex = 0;
                BK4819_SetupNackFSKReceive();
            }
        }


#ifdef ENABLE_AIRCOPY



        if (interrupts.fskFifoAlmostFull &&



            gScreenToDisplay == DISPLAY_AIRCOPY &&



            gAircopyState == AIRCOPY_TRANSFER &&



            gAirCopyIsSendMode == 0)



        {



            for (unsigned int i = 0; i < 4; i++) {



                g_FSK_Buffer[gFSKWriteIndex++] = BK4819_ReadRegister(BK4819_REG_5F);



            }





            AIRCOPY_StorePacket();



        }



#endif



    }



}





void APP_EndTransmission(void)



{



    // back to RX mode



    RADIO_SendEndOfTransmission();





    gFlagEndTransmission = true;





    if (gMonitor) {



         //turn the monitor back on



        gFlagReconfigureVfos = true;



    }



}





#ifdef ENABLE_VOX



static void HandleVox(void)



{





    if (gVoxResumeCountdown == 0) {



        if (gVoxPauseCountdown)



            return;



    }



    else {



        g_VOX_Lost         = false;



        gVoxPauseCountdown = 0;



    }





#ifdef ENABLE_FMRADIO



    if (gFmRadioMode)



        return;



#endif





    if (gCurrentFunction == FUNCTION_RECEIVE || gCurrentFunction == FUNCTION_MONITOR)



        return;





    if (gScanStateDir != SCAN_OFF)



        return;





    if (gVOX_NoiseDetected) {



        if (g_VOX_Lost)



            gVoxStopCountdown_10ms = vox_stop_count_down_10ms;



        else if (gVoxStopCountdown_10ms == 0)



            gVOX_NoiseDetected = false;





        if (gCurrentFunction == FUNCTION_TRANSMIT && !gPttIsPressed && !gVOX_NoiseDetected) {



            if (gFlagEndTransmission) {



                //if (gCurrentFunction != FUNCTION_FOREGROUND)



                    FUNCTION_Select(FUNCTION_FOREGROUND);



            }



            else {



                APP_EndTransmission();





                if (gEeprom.REPEATER_TAIL_TONE_ELIMINATION == 0) {



                    //if (gCurrentFunction != FUNCTION_FOREGROUND)



                        FUNCTION_Select(FUNCTION_FOREGROUND);



                }



                else



                    gRTTECountdown_10ms = gEeprom.REPEATER_TAIL_TONE_ELIMINATION * 10;



            }





            gUpdateStatus        = true;



            gUpdateDisplay       = true;



            gFlagEndTransmission = false;



        }



        return;



    }





    if (g_VOX_Lost) {



        gVOX_NoiseDetected = true;





        if (gCurrentFunction == FUNCTION_POWER_SAVE)



            FUNCTION_Select(FUNCTION_FOREGROUND);





        if (gCurrentFunction != FUNCTION_TRANSMIT && !SerialConfigInProgress()) {





            RADIO_PrepareTX();



            gUpdateDisplay = true;



        }



    }



}



#endif





void APP_Update(void)



{



#ifdef ENABLE_VOICE



    if (gFlagPlayQueuedVoice) {



            AUDIO_PlayQueuedVoice();



            gFlagPlayQueuedVoice = false;



    }



#endif





    if (gCurrentFunction == FUNCTION_TRANSMIT && (gTxTimeoutReached || SerialConfigInProgress()))



    {   // transmitter timed out or must de-key



        gTxTimeoutReached = false;





        APP_EndTransmission();





        AUDIO_PlayBeep(BEEP_880HZ_60MS_TRIPLE_BEEP);





        RADIO_SetVfoState(VFO_STATE_TIMEOUT);





        GUI_DisplayScreen();



    }





    if (gReducedService)



        return;





    if (gCurrentFunction != FUNCTION_TRANSMIT)



        HandleFunction();





#ifdef ENABLE_FMRADIO



//  if (gFmRadioCountdown_500ms > 0)



    if (gFmRadioMode && gFmRadioCountdown_500ms > 0)    // 1of11



        return;



#endif





#ifdef ENABLE_VOICE



    if (!SCANNER_IsScanning() && gScanStateDir != SCAN_OFF && gScheduleScanListen && !gPttIsPressed && gVoiceWriteIndex == 0)



#else



    if (!SCANNER_IsScanning() && gScanStateDir != SCAN_OFF && gScheduleScanListen && !gPttIsPressed)



#endif



    {   // scanning



        CHFRSCANNER_ContinueScanning();



    }





#ifdef ENABLE_NOAA



#ifdef ENABLE_VOICE



        if (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF && gIsNoaaMode && gScheduleNOAA && gVoiceWriteIndex == 0)



#else



        if (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF && gIsNoaaMode && gScheduleNOAA)



#endif



        {



            NOAA_IncreaseChannel();



            RADIO_SetupRegisters(false);





            gNOAA_Countdown_10ms = 7;      // 70ms



            gScheduleNOAA        = false;



        }



#endif





    // toggle between the VFO's if dual watch is enabled



    if (!SCANNER_IsScanning()



        && gEeprom.DUAL_WATCH != DUAL_WATCH_OFF



        && gScheduleDualWatch



        && gScanStateDir == SCAN_OFF



        && !gPttIsPressed



        && gCurrentFunction != FUNCTION_POWER_SAVE



#ifdef ENABLE_VOICE



        && gVoiceWriteIndex == 0



#endif



#ifdef ENABLE_FMRADIO



        && !gFmRadioMode



#endif





    ) {



        DualwatchAlternate();    // toggle between the two VFO's





        if (gRxVfoIsActive && gScreenToDisplay == DISPLAY_MAIN) {



            GUI_SelectNextDisplay(DISPLAY_MAIN);



        }





        gRxVfoIsActive     = false;



        gScanPauseMode     = false;



        gRxReceptionMode   = RX_MODE_NONE;



        gScheduleDualWatch = false;



    }





#ifdef ENABLE_FMRADIO



    if (gScheduleFM && gFM_ScanState != FM_SCAN_OFF && !FUNCTION_IsRx()) {



        // switch to FM radio mode



        FM_Play();



        gScheduleFM = false;



    }



#endif





#ifdef ENABLE_VOX



    if (gEeprom.VOX_SWITCH)



        HandleVox();



#endif





    if (gSchedulePowerSave) {



        if (gPttIsPressed



            || gKeyBeingHeld



            || gEeprom.BATTERY_SAVE == 0



            || gScanStateDir != SCAN_OFF



            || gCssBackgroundScan



            || gScreenToDisplay != DISPLAY_MAIN



#ifdef ENABLE_FMRADIO



            || gFmRadioMode



#endif





#ifdef ENABLE_NOAA



            || (gIsNoaaMode && (IS_NOAA_CHANNEL(gEeprom.ScreenChannel[0]) || IS_NOAA_CHANNEL(gEeprom.ScreenChannel[1])))



#endif



        ) {



            gBatterySaveCountdown_10ms = battery_save_count_10ms;



        } else {



            FUNCTION_Select(FUNCTION_POWER_SAVE);



        }





        gSchedulePowerSave = false;



    }





    if (gPowerSaveCountdownExpired && gCurrentFunction == FUNCTION_POWER_SAVE



#ifdef ENABLE_VOICE



        && gVoiceWriteIndex == 0



#endif



    ) {



        static bool goToSleep;



        // wake up, enable RX then go back to sleep



        if (gRxIdleMode)



        {



            BK4819_Conditional_RX_TurnOn_and_GPIO6_Enable();





#ifdef ENABLE_VOX



            if (gEeprom.VOX_SWITCH)



                BK4819_EnableVox(gEeprom.VOX1_THRESHOLD, gEeprom.VOX0_THRESHOLD);



#endif





            if (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF &&



                gScanStateDir == SCAN_OFF &&



                !gCssBackgroundScan)



            {   // dual watch mode, toggle between the two VFO's



                DualwatchAlternate();



                goToSleep = false;



            }





            FUNCTION_Init();





            gPowerSave_10ms = power_save1_10ms; // come back here in a bit



            gRxIdleMode     = false;            // RX is awake



        }



        else if (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF || gScanStateDir != SCAN_OFF || gCssBackgroundScan || goToSleep)



        {   // dual watch mode off or scanning or rssi update request



            // go back to sleep





            gPowerSave_10ms = gEeprom.BATTERY_SAVE * 10;



            gRxIdleMode     = true;



            goToSleep = false;





            BK4819_DisableVox();



            BK4819_Sleep();



            BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);





            // Authentic device checked removed





        }



        else {



            // toggle between the two VFO's



            DualwatchAlternate();



            gPowerSave_10ms   = power_save1_10ms;



            goToSleep = true;



        }





        gPowerSaveCountdownExpired = false;



    }



}





// called every 10ms



static void CheckKeys(void)



{





#ifdef ENABLE_AIRCOPY



    if (gScreenToDisplay == DISPLAY_AIRCOPY && gAircopyState != AIRCOPY_READY){



        return;



    }



#endif





// -------------------- PTT ------------------------



    if (gPttIsPressed)



    {



        if (GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT) || SerialConfigInProgress())



        {   // PTT released or serial comms config in progress



            if (++gPttDebounceCounter >= 3 || SerialConfigInProgress())     // 30ms



            {   // stop transmitting



                ProcessKey(KEY_PTT, false, false);



                gPttIsPressed = false;



                if (gKeyReading1 != KEY_INVALID)



                    gPttWasReleased = true;



            }



        }



        else



            gPttDebounceCounter = 0;



    }



    else if (!GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT) && !SerialConfigInProgress())



    {   // PTT pressed



        if (++gPttDebounceCounter >= 3)     // 30ms



        {   // start transmitting



            boot_counter_10ms   = 0;



            gPttDebounceCounter = 0;



            gPttIsPressed       = true;



            ProcessKey(KEY_PTT, true, false);



        }



    }



    else



        gPttDebounceCounter = 0;





// --------------------- OTHER KEYS ----------------------------





    // scan the hardware keys



    KEY_Code_t Key = KEYBOARD_Poll();





    if (Key != KEY_INVALID) // any key pressed



        boot_counter_10ms = 0;   // cancel boot screen/beeps if any key pressed





    if (gKeyReading0 != Key) // new key pressed



    {





        if (gKeyReading0 != KEY_INVALID && Key != KEY_INVALID)



            ProcessKey(gKeyReading1, false, gKeyBeingHeld);  // key pressed without releasing previous key





        gKeyReading0     = Key;



        gDebounceCounter = 0;



        return;



    }





    gDebounceCounter++;





    if (gDebounceCounter == key_debounce_10ms) // debounced new key pressed



    {



        if (Key == KEY_INVALID) //all non PTT keys released



        {



            if (gKeyReading1 != KEY_INVALID) // some button was pressed before



            {



                ProcessKey(gKeyReading1, false, gKeyBeingHeld); // process last button released event



                gKeyReading1 = KEY_INVALID;



            }



        }



        else // process new key pressed



        {



            gKeyReading1 = Key;



            ProcessKey(Key, true, false);



        }





        gKeyBeingHeld = false;



        return;



    }





    if (gDebounceCounter < key_repeat_delay_10ms || Key == KEY_INVALID) // the button is not held long enough for repeat yet, or not really pressed



        return;





    if (gDebounceCounter == key_repeat_delay_10ms) //initial key repeat with longer delay



    {



        if (Key != KEY_PTT)



        {



            gKeyBeingHeld = true;



            ProcessKey(Key, true, true); // key held event



        }



    }



    else //subsequent fast key repeats



    {



        if (Key == KEY_UP || Key == KEY_DOWN) // fast key repeats for up/down buttons



        {



            gKeyBeingHeld = true;



            if ((gDebounceCounter % key_repeat_10ms) == 0)



                ProcessKey(Key, true, true); // key held event



        }





        if (gDebounceCounter < 0xFFFF)



            return;





        gDebounceCounter = key_repeat_delay_10ms+1;



    }



}





void APP_TimeSlice10ms(void)



{



    gNextTimeslice = false;



    gFlashLightBlinkCounter++;

    // Nallo digital-protocol timers
    for (uint8_t i = 0; i < 2; i++) {
        if (gNackEmgCooldown_10ms[i] > 0)
            gNackEmgCooldown_10ms[i]--;
    }

    if (gNackPending.type != NACK_PENDING_NONE) {
        if (gNackPending.window_10ms > 0)
            gNackPending.window_10ms--;
        else
            gNackPending.type = NACK_PENDING_NONE;

        if (gNackPending.type != NACK_PENDING_NONE) {
            if (gNackPending.delay_10ms > 0) {
                gNackPending.delay_10ms--;
            } else {
                const uint8_t vfo = gNackPending.vfo & 1u;
                VFO_Info_t *pVfo = &gEeprom.VfoInfo[vfo];
                if (pVfo->CHANNEL_SAVE != gNackPending.channel) {
                    gNackPending.type = NACK_PENDING_NONE;
                } else if ((gEeprom.RX_VFO & 1u) != vfo || g_SquelchLost || gMonitor ||
                           gPttIsPressed || gCurrentFunction == FUNCTION_TRANSMIT ||
                           gScanStateDir != SCAN_OFF) {
                    gNackPending.delay_10ms = SQLACK_RandomDelay(0, SQLACK_RETRY_MAX_10MS);
                } else {
                    const uint8_t type = (gNackPending.type == NACK_PENDING_SNR_RESP) ?
                                         NACK_TYPE_SNR_RESP : NACK_TYPE_EMERGENCY_ACK;
                    if (NACK_SendPacketOnVfo(vfo, type, gNackPending.target,
                                             gNackPending.value, gNackPending.channel)) {
                        if (gNackPending.type == NACK_PENDING_EMG_ACK)
                            gNackEmgCooldown_10ms[vfo] = NACK_EMG_COOLDOWN_10MS;
                        gNackPending.type = NACK_PENDING_NONE;
                    } else {
                        gNackPending.delay_10ms = SQLACK_RandomDelay(0, SQLACK_RETRY_MAX_10MS);
                    }
                }
            }
        }
    }






        // ------------------------------------------------



    // SqlAck timers



    // ------------------------------------------------





    if (g_SquelchLost)



{



    const uint8_t vfo = gEeprom.RX_VFO & 1u;





    if (gSqlAck[vfo].rxTime_10ms < 1000)



        gSqlAck[vfo].rxTime_10ms++;



}





for (uint8_t vfo = 0; vfo < 2; vfo++)



{



    SQLACK_State_t *ack = &gSqlAck[vfo];



    VFO_Info_t *pVfo = &gEeprom.VfoInfo[vfo];





    // Cooldown independiente A/B



    if (ack->cooldown_10ms > 0)



        ack->cooldown_10ms--;





    if (!ack->pending)



        continue;





    // Si se cambió de canal mientras esperábamos, cancelar.



    if (pVfo->CHANNEL_SAVE != ack->channel)



    {



        ack->pending = false;



        continue;



    }





    // Si SqlAck fue desactivado durante la espera, cancelar.



    if (pVfo->SQL_ACK_MODE != SQL_ACK_BEEP &&
        pVfo->SQL_ACK_MODE != SQL_ACK_ID)



    {



        ack->pending = false;



        continue;



    }





    // Ventana máxima independiente de 10 segundos.



    if (ack->window_10ms > 0)



    {



        ack->window_10ms--;



    }



    else



    {



        ack->pending = false;



        continue;



    }





    // Todavía estamos en la espera 3-5 s / reintento.



    if (ack->delay_10ms > 0)



    {



        ack->delay_10ms--;



        continue;



    }





    /*



     * Para comprobar si el canal concreto está libre,



     * esperamos a que Dual Watch esté actualmente escuchando



     * ese VFO.



     *



     * Así no usamos por error el estado de squelch del otro canal.



     */



    if ((gEeprom.RX_VFO & 1u) != vfo)



    {



        ack->delay_10ms =



            SQLACK_RandomDelay(0, SQLACK_RETRY_MAX_10MS);





        continue;



    }





    const bool canSend =



        !g_SquelchLost &&



        !gMonitor &&



        !gPttIsPressed &&



        gCurrentFunction != FUNCTION_TRANSMIT &&



        gScanStateDir == SCAN_OFF;





    if (!canSend)



    {



        ack->delay_10ms =



            SQLACK_RandomDelay(0, SQLACK_RETRY_MAX_10MS);





        continue;



    }





    /*



     * Intentar ACK por ESTE VFO.



     */



    const bool ackSent =
        (pVfo->SQL_ACK_MODE == SQL_ACK_ID) ? SQLACK_SendId(vfo) : SQLACK_SendBeep(vfo);

    if (ackSent)



    {



        ack->pending = false;





        // 20 segundos SOLO para este canal/VFO.



        ack->cooldown_10ms = SQLACK_COOLDOWN_10MS;





        /*



         * Importante:



         * hemos transmitido en este timeslice.



         * No dejar que el otro VFO empiece otro ACK



         * exactamente a continuación en el mismo recorrido.



         */



        break;



    }



    else



    {



        // No pudo transmitir: volver a intentarlo 0-0.5 s después.



        ack->delay_10ms =



            SQLACK_RandomDelay(0, SQLACK_RETRY_MAX_10MS);



    }



}





#ifdef ENABLE_BOOT_BEEPS



    if (boot_counter_10ms > 0 && (boot_counter_10ms % 25) == 0) {



        AUDIO_PlayBeep(BEEP_880HZ_40MS_OPTIONAL);



    }



#endif





#ifdef ENABLE_AM_FIX



    if (gRxVfo->Modulation == MODULATION_AM) {



        AM_fix_10ms(gEeprom.RX_VFO);



    }



#endif





#ifdef ENABLE_UART



    if (UART_IsCommandAvailable()) {



        __disable_irq();



        UART_HandleCommand();



        __enable_irq();



    }



#endif





    if (gReducedService)



        return;





    if (gCurrentFunction != FUNCTION_POWER_SAVE || !gRxIdleMode)



        CheckRadioInterrupts();





    if (gCurrentFunction == FUNCTION_TRANSMIT)



    {   // transmitting



#ifdef ENABLE_AUDIO_BAR



        if (gSetting_mic_bar && (gFlashLightBlinkCounter % (150 / 10)) == 0) // once every 150ms



            UI_DisplayAudioBar();



#endif



    }





    if (gUpdateDisplay) {



        gUpdateDisplay = false;



        GUI_DisplayScreen();



    }





    if (gUpdateStatus)



        UI_DisplayStatus();





    // Skipping authentic device checks





#ifdef ENABLE_FMRADIO



    if (gFmRadioMode && gFmRadioCountdown_500ms > 0)   // 1of11



        return;



#endif





#ifdef ENABLE_FLASHLIGHT



    FlashlightTimeSlice();



#endif





#ifdef ENABLE_VOX



    if (gVoxResumeCountdown > 0)



        gVoxResumeCountdown--;





    if (gVoxPauseCountdown > 0)



        gVoxPauseCountdown--;



#endif





    if (gCurrentFunction == FUNCTION_TRANSMIT) {



#ifdef ENABLE_ALARM



        if (gAlarmState == ALARM_STATE_TXALARM || gAlarmState == ALARM_STATE_SITE_ALARM) {



            uint16_t Tone;





            gAlarmRunningCounter++;



            gAlarmToneCounter++;





            Tone = 500 + (gAlarmToneCounter * 25);



            if (Tone > 1500) {



                Tone              = 500;



                gAlarmToneCounter = 0;



            }





            BK4819_SetScrambleFrequencyControlWord(Tone);





            if (gEeprom.ALARM_MODE == ALARM_MODE_TONE && gAlarmRunningCounter == 512) {



                gAlarmRunningCounter = 0;





                if (gAlarmState == ALARM_STATE_TXALARM) {



                    gAlarmState = ALARM_STATE_SITE_ALARM;





                    if(gEeprom.TAIL_TONE_ELIMINATION)



                        RADIO_SendCssTail();



                    BK4819_SetupPowerAmplifier(0, 0);



                    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);



                    BK4819_Enable_AfDac_DiscMode_TxDsp();



                    BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);





                    GUI_DisplayScreen();



                }



                else {



                    gAlarmState = ALARM_STATE_TXALARM;





                    GUI_DisplayScreen();





                    BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);



                    RADIO_SetTxParameters();



                    BK4819_TransmitTone(true, 500);



                    SYSTEM_DelayMs(2);



                    AUDIO_AudioPathOn();





                    gEnableSpeaker    = true;



                    gAlarmToneCounter = 0;



                }



            }



        }



#endif



        // repeater tail tone elimination



        if (gRTTECountdown_10ms > 0) {



            if (--gRTTECountdown_10ms == 0) {



                //if (gCurrentFunction != FUNCTION_FOREGROUND)



                    FUNCTION_Select(FUNCTION_FOREGROUND);





                gUpdateStatus  = true;



                gUpdateDisplay = true;



            }



        }



    }





#ifdef ENABLE_FMRADIO



    if (gFmRadioMode && gFM_RestoreCountdown_10ms > 0) {



        if (--gFM_RestoreCountdown_10ms == 0) { 



            FM_Start(); // switch back to FM radio mode



            GUI_SelectNextDisplay(DISPLAY_FM);



        }



    }



#endif





    SCANNER_TimeSlice10ms();





#ifdef ENABLE_AIRCOPY



    if (gScreenToDisplay == DISPLAY_AIRCOPY && gAircopyState == AIRCOPY_TRANSFER && gAirCopyIsSendMode == 1) {



        if (!AIRCOPY_SendMessage()) {



            GUI_DisplayScreen();



        }



    }



#endif





    CheckKeys();



}





void cancelUserInputModes(void)



{

    if (gWasFKeyPressed || gKeyInputCountdown > 0 || gInputBoxIndex > 0)



    {



        gWasFKeyPressed     = false;



        gInputBoxIndex      = 0;



        gKeyInputCountdown  = 0;



        gBeepToPlay         = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;



        gUpdateStatus       = true;



        gUpdateDisplay      = true;



    }



}





// this is called once every 500ms



void APP_TimeSlice500ms(void)



{



    gNextTimeslice_500ms = false;

    if (gNackEmergencyAlert_10ms > 0 && gNackEmergencySound &&
        gCurrentFunction != FUNCTION_TRANSMIT) {
        AUDIO_PlayEmergencySweep();
    }

    bool exit_menu = false;





    // Skipped authentic device check





    if (gKeypadLocked > 0)



        if (--gKeypadLocked == 0)



            gUpdateDisplay = true;





    if (gKeyInputCountdown > 0)



    {



        if (--gKeyInputCountdown == 0)



        {



            cancelUserInputModes();





            if (gBeepToPlay != BEEP_NONE)



            {



                AUDIO_PlayBeep(gBeepToPlay);



                gBeepToPlay = BEEP_NONE;



            }



        }



    }

    if (gMenuCountdown > 0)



        if (--gMenuCountdown == 0)



            exit_menu = (gScreenToDisplay == DISPLAY_MENU); // exit menu mode





    // Skipped authentic device check





#ifdef ENABLE_FMRADIO



    if (gFmRadioCountdown_500ms > 0)



    {



        gFmRadioCountdown_500ms--;



        if (gFmRadioMode)           // 1of11



            return;



    }



#endif





    if (gBacklightCountdown_500ms > 0 && !gAskToSave && !gCssBackgroundScan



        // don't turn off backlight if user is in backlight menu option



        && !(gScreenToDisplay == DISPLAY_MENU && (UI_MENU_GetCurrentMenuId() == MENU_ABR || UI_MENU_GetCurrentMenuId() == MENU_ABR_MAX))



        && --gBacklightCountdown_500ms == 0



        && gEeprom.BACKLIGHT_TIME < (ARRAY_SIZE(gSubMenu_BACKLIGHT) - 1)



    ) {



        BACKLIGHT_TurnOff();



    }





    if (gReducedService)



    {



        BOARD_ADC_GetBatteryInfo(&gBatteryCurrentVoltage, &gBatteryCurrent);





        if (gBatteryCurrent > 500 || gBatteryCalibration[3] < gBatteryCurrentVoltage)



        {



            #ifdef ENABLE_OVERLAY



                overlay_FLASH_RebootToBootloader();



            #else



                NVIC_SystemReset();



            #endif



        }





        return;



    }





    gBatteryCheckCounter++;





    // Skipped authentic device check





    if (gCurrentFunction != FUNCTION_TRANSMIT)



    {





        if ((gBatteryCheckCounter & 1) == 0)



        {



            BOARD_ADC_GetBatteryInfo(&gBatteryVoltages[gBatteryVoltageIndex++], &gBatteryCurrent);



            if (gBatteryVoltageIndex > 3)



                gBatteryVoltageIndex = 0;



            BATTERY_GetReadings(true);



        }



    }





    // regular display updates (once every 2 sec) - if need be



    if ((gBatteryCheckCounter & 3) == 0)



    {



        if (gChargingWithTypeC || gSetting_battery_text > 0)



            gUpdateStatus = true;



        #ifdef ENABLE_SHOW_CHARGE_LEVEL



            if (gChargingWithTypeC)



                gUpdateDisplay = true;



        #endif



    }





    if (!gCssBackgroundScan && gScanStateDir == SCAN_OFF && !SCANNER_IsScanning()



#ifdef ENABLE_FMRADIO



        && (gFM_ScanState == FM_SCAN_OFF || gAskToSave)



#endif



#ifdef ENABLE_AIRCOPY



        && gScreenToDisplay != DISPLAY_AIRCOPY



#endif



    ) {



        if (gEeprom.AUTO_KEYPAD_LOCK && gKeyLockCountdown > 0



            && gScreenToDisplay != DISPLAY_MENU && --gKeyLockCountdown == 0)



        {



            gEeprom.KEY_LOCK = true;     // lock the keyboard



            gUpdateStatus = true;            // lock symbol needs showing



        }





        if (exit_menu) {



            gMenuCountdown = 0;





            if (gEeprom.BACKLIGHT_TIME == 0) {



                BACKLIGHT_TurnOff();



            }





            if (gInputBoxIndex > 0) {



                AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);



            }



/*



            if (SCANNER_IsScanning()) {



                BK4819_StopScan();





                RADIO_ConfigureChannel(0, VFO_CONFIGURE_RELOAD);



                RADIO_ConfigureChannel(1, VFO_CONFIGURE_RELOAD);





                RADIO_SetupRegisters(true);



            }



*/

            gWasFKeyPressed  = false;



            gInputBoxIndex   = 0;





            gAskToSave       = false;



            gAskToDelete     = false;





            gUpdateStatus    = true;



            gUpdateDisplay   = true;





            GUI_DisplayType_t disp = DISPLAY_INVALID;





#ifdef ENABLE_FMRADIO



            if (gFmRadioMode && ! FUNCTION_IsRx()) {



                disp = DISPLAY_FM;



            }



#endif





            if (disp == DISPLAY_INVALID



#ifdef ENABLE_NO_CODE_SCAN_TIMEOUT



                && !SCANNER_IsScanning()



#endif



            ) {



                disp = DISPLAY_MAIN;



            }





            if (disp != DISPLAY_INVALID) {



                GUI_SelectNextDisplay(disp);



            }



        }



    }





    if (!gPttIsPressed && gVFOStateResumeCountdown_500ms > 0 && --gVFOStateResumeCountdown_500ms == 0) {



            RADIO_SetVfoState(VFO_STATE_NORMAL);



#ifdef ENABLE_FMRADIO



        if (gFmRadioMode && !FUNCTION_IsRx()) {



            // switch back to FM radio mode



            FM_Start();



            GUI_SelectNextDisplay(DISPLAY_FM);



        }



#endif



    }





    BATTERY_TimeSlice500ms();



    SCANNER_TimeSlice500ms();



    UI_MAIN_TimeSlice500ms();





}





#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)



static void ALARM_Off(void)



{



    AUDIO_AudioPathOff();



    gEnableSpeaker = false;





    if (gAlarmState == ALARM_STATE_TXALARM || gAlarmState == ALARM_STATE_TX1750) {



        RADIO_SendEndOfTransmission();



    }





    gAlarmState = ALARM_STATE_OFF;





#ifdef ENABLE_VOX



    gVoxResumeCountdown = 80;



#endif





    SYSTEM_DelayMs(5);





    RADIO_SetupRegisters(true);





    if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu



        gRequestDisplayScreen = DISPLAY_MAIN;



}



#endif





static void ProcessKey(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)



{

    if (bKeyPressed && gNackEmergencyAlert_10ms > 0)
        NACK_StopEmergencyAlert();

    if (Key == KEY_EXIT && !BACKLIGHT_IsOn() && gEeprom.BACKLIGHT_TIME > 0)



    {   // just turn the light on for now so the user can see what's what



        BACKLIGHT_TurnOn();



        gBeepToPlay = BEEP_NONE;



        return;



    }





    if (gCurrentFunction == FUNCTION_POWER_SAVE)



        FUNCTION_Select(FUNCTION_FOREGROUND);





    gBatterySaveCountdown_10ms = battery_save_count_10ms;





    if (gEeprom.AUTO_KEYPAD_LOCK)



        gKeyLockCountdown = 30;     // 15 seconds





    if (!bKeyPressed) { // key released



        if (flagSaveVfo) {



            SETTINGS_SaveVfoIndices();



            flagSaveVfo = false;



        }





        if (flagSaveSettings) {



            SETTINGS_SaveSettings();



            flagSaveSettings = false;



        }





#ifdef ENABLE_FMRADIO



        if (gFlagSaveFM) {



            SETTINGS_SaveFM();



            gFlagSaveFM = false;



        }



#endif





        if (flagSaveChannel) {



            SETTINGS_SaveChannel(gTxVfo->CHANNEL_SAVE, gEeprom.TX_VFO, gTxVfo, flagSaveChannel);



            flagSaveChannel = false;





            if (!SCANNER_IsScanning() && gVfoConfigureMode == VFO_CONFIGURE_NONE)



                // gVfoConfigureMode is so as we don't wipe out previously setting this variable elsewhere



                gVfoConfigureMode = VFO_CONFIGURE;



        }



    }



    else { // key pressed or held



        const int m = UI_MENU_GetCurrentMenuId();



        if  (   //not when PTT and the backlight shouldn't turn on on TX



                !(Key == KEY_PTT && !(gSetting_backlight_on_tx_rx & BACKLIGHT_ON_TR_TX))



                // not in the backlight menu



                && !(gScreenToDisplay == DISPLAY_MENU && ( m == MENU_ABR || m == MENU_ABR_MAX || m == MENU_ABR_MIN))



            )



        {



            BACKLIGHT_TurnOn();



        }





        if (Key == KEY_EXIT && bKeyHeld) { // exit key held pressed

            // cancel user input



            cancelUserInputModes();





            if (gMonitor)



                ACTION_Monitor(); //turn off the monitor



#ifdef ENABLE_SCAN_RANGES



            gScanRangeStart = 0;



#endif



        }





        if (gScreenToDisplay == DISPLAY_MENU)       // 1of11



            gMenuCountdown = menu_timeout_500ms;





    }





    bool lowBatPopup = gLowBattery && !gLowBatteryConfirmed &&  gScreenToDisplay == DISPLAY_MAIN;





    if ((gEeprom.KEY_LOCK || lowBatPopup) && gCurrentFunction != FUNCTION_TRANSMIT && Key != KEY_PTT)



    {   // keyboard is locked or low battery popup





        // close low battery popup



        if(Key == KEY_EXIT && bKeyPressed && lowBatPopup) {



            gLowBatteryConfirmed = true;



            gUpdateDisplay = true;



            AUDIO_PlayBeep(BEEP_1KHZ_60MS_OPTIONAL);



            return;



        }





        if (Key == KEY_F) { // function/key-lock key



            if (!bKeyPressed)



                return;





            if (!bKeyHeld) { // keypad is locked, tell the user



                AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);



                gKeypadLocked  = 4;      // 2 seconds



                gUpdateDisplay = true;



                return;



            }



        }



        // KEY_MENU has a special treatment here, because we want to pass hold event to ACTION_Handle



        // but we don't want it to complain when initial press happens



        // we want to react on realese instead



        else if (Key != KEY_SIDE1 && Key != KEY_SIDE2 &&        // pass side buttons



                 !(Key == KEY_MENU && bKeyHeld)) // pass KEY_MENU held



        {



            if ((!bKeyPressed || bKeyHeld || (Key == KEY_MENU && bKeyPressed)) && // prevent released or held, prevent KEY_MENU pressed



                !(Key == KEY_MENU && !bKeyPressed))  // pass KEY_MENU released



                return;





            // keypad is locked, tell the user



            AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);



            gKeypadLocked  = 4;          // 2 seconds



            gUpdateDisplay = true;



            return;



        }



    }





    if (Key <= KEY_9 || Key == KEY_F) {



        if (gScanStateDir != SCAN_OFF || gCssBackgroundScan) { // FREQ/CTCSS/DCS scanning



            if (bKeyPressed && !bKeyHeld)



                AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);



            return;



        }



    }





    bool bFlag = false;



    if (Key == KEY_PTT) {



        if (gPttWasPressed) {



            bFlag = bKeyHeld;



            if (!bKeyPressed) {



                bFlag          = true;



                gPttWasPressed = false;



            }



        }



    }



    else if (gPttWasReleased) {



        if (bKeyHeld)



            bFlag = true;



        if (!bKeyPressed) {



            bFlag           = true;



            gPttWasReleased = false;



        }



    }





    if (gWasFKeyPressed && (Key == KEY_PTT || Key == KEY_EXIT || Key == KEY_SIDE1 || Key == KEY_SIDE2)) { 



        // cancel the F-key



        gWasFKeyPressed = false;



        gUpdateStatus   = true;



    }





    if (bFlag) {



        goto Skip;



    }





    if (gCurrentFunction == FUNCTION_TRANSMIT) {

#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)

        if (gAlarmState == ALARM_STATE_OFF)

#endif

        {

            if (Key == KEY_PTT) {

                GENERIC_Key_PTT(bKeyPressed);

                goto Skip;

            }



#ifdef ENABLE_TX1750

            if (Key == KEY_SIDE2) {

                if (!bKeyPressed || bKeyHeld) {

                    if (!bKeyPressed) {

                        AUDIO_AudioPathOff();

                        gEnableSpeaker = false;



                        if (gCurrentVfo->SCRAMBLING_TYPE == 0 || !gSetting_ScrambleEnable)

                            BK4819_DisableScramble();

                        else

                            BK4819_EnableScramble(gCurrentVfo->SCRAMBLING_TYPE - 1);

                    }

                }

                else {

                    BK4819_DisableScramble();

                    BK4819_TransmitTone(false, 1750);

                }



                goto Skip;

            }

#endif

        }

#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)



        else if ((!bKeyHeld && bKeyPressed) || (gAlarmState == ALARM_STATE_TX1750 && bKeyHeld && !bKeyPressed)) {



            ALARM_Off();





            if (gEeprom.REPEATER_TAIL_TONE_ELIMINATION == 0)



                FUNCTION_Select(FUNCTION_FOREGROUND);



            else



                gRTTECountdown_10ms = gEeprom.REPEATER_TAIL_TONE_ELIMINATION * 10;





            if (Key == KEY_PTT)



                gPttWasPressed  = true;



            else if (!bKeyHeld)



                gPttWasReleased = true;



        }



#endif



    }



    else if (Key != KEY_SIDE1 && Key != KEY_SIDE2 && gScreenToDisplay != DISPLAY_INVALID) {



        ProcessKeysFunctions[gScreenToDisplay](Key, bKeyPressed, bKeyHeld);



    }



    else if (!SCANNER_IsScanning()



#ifdef ENABLE_AIRCOPY



            && gScreenToDisplay != DISPLAY_AIRCOPY



#endif



    ) {



        ACTION_Handle(Key, bKeyPressed, bKeyHeld);



    }



    else if (!bKeyHeld && bKeyPressed) {



        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;



    }





Skip:



    if (gBeepToPlay != BEEP_NONE) {



        AUDIO_PlayBeep(gBeepToPlay);



        gBeepToPlay = BEEP_NONE;



    }





    if (gFlagAcceptSetting) {



        gMenuCountdown = menu_timeout_500ms;





        MENU_AcceptSetting();





        gFlagRefreshSetting = true;



        gFlagAcceptSetting  = false;



    }





    if (gRequestSaveSettings) {



        if (!bKeyHeld)



            SETTINGS_SaveSettings();



        else



            flagSaveSettings = 1;



        gRequestSaveSettings = false;



        gUpdateStatus        = true;



    }





#ifdef ENABLE_FMRADIO



    if (gRequestSaveFM) {



        gRequestSaveFM = false;



        if (!bKeyHeld)



            SETTINGS_SaveFM();



        else



            gFlagSaveFM = true;



    }



#endif





    if (gRequestSaveVFO) {



        gRequestSaveVFO = false;



        if (!bKeyHeld)



            SETTINGS_SaveVfoIndices();



        else



            flagSaveVfo = true;



    }





    if (gRequestSaveChannel > 0) { // TODO: remove the gRequestSaveChannel, why use global variable for that??



        if (!bKeyHeld) {



            SETTINGS_SaveChannel(gTxVfo->CHANNEL_SAVE, gEeprom.TX_VFO, gTxVfo, gRequestSaveChannel);





            if (!SCANNER_IsScanning() && gVfoConfigureMode == VFO_CONFIGURE_NONE)



                // gVfoConfigureMode is so as we don't wipe out previously setting this variable elsewhere



                gVfoConfigureMode = VFO_CONFIGURE;



        }



        else { // this is probably so settings are not saved when up/down button is held and save is postponed to btn release



            flagSaveChannel = gRequestSaveChannel;





            if (gRequestDisplayScreen == DISPLAY_INVALID)



                gRequestDisplayScreen = DISPLAY_MAIN;



        }





        gRequestSaveChannel = 0;



    }





    if (gVfoConfigureMode != VFO_CONFIGURE_NONE) {



        if (gFlagResetVfos) {



            RADIO_ConfigureChannel(0, gVfoConfigureMode);



            RADIO_ConfigureChannel(1, gVfoConfigureMode);



        }



        else



            RADIO_ConfigureChannel(gEeprom.TX_VFO, gVfoConfigureMode);





        if (gRequestDisplayScreen == DISPLAY_INVALID)



            gRequestDisplayScreen = DISPLAY_MAIN;





        gFlagReconfigureVfos = true;



        gVfoConfigureMode    = VFO_CONFIGURE_NONE;



        gFlagResetVfos       = false;



    }





    if (gFlagReconfigureVfos) {



        RADIO_SelectVfos();





#ifdef ENABLE_NOAA



        RADIO_ConfigureNOAA();



#endif





        RADIO_SetupRegisters(true);





        gVFO_RSSI_bar_level[0]      = 0;



        gVFO_RSSI_bar_level[1]      = 0;





        gFlagReconfigureVfos        = false;





        if (gMonitor)



            ACTION_Monitor();   // 1of11



    }





    if (gFlagRefreshSetting) {



        gFlagRefreshSetting = false;



        gMenuCountdown      = menu_timeout_500ms;





        MENU_ShowCurrentSetting();



    }





    if (gFlagPrepareTX) {



        RADIO_PrepareTX();



        gFlagPrepareTX = false;



    }





#ifdef ENABLE_VOICE



    if (gAnotherVoiceID != VOICE_ID_INVALID) {



        if (gAnotherVoiceID < 76)



            AUDIO_SetVoiceID(0, gAnotherVoiceID);



        AUDIO_PlaySingleVoice(false);



        gAnotherVoiceID = VOICE_ID_INVALID;



    }



#endif





    GUI_SelectNextDisplay(gRequestDisplayScreen);



    gRequestDisplayScreen = DISPLAY_INVALID;





    gUpdateDisplay = true;



}
