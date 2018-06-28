#include "ch.h"
#include "hal.h"

#include "debug.h"
#include "threads.h"
#include "config.h"
#include "radio.h"
#include "aprs.h"
#include "sleep.h"
#include "chprintf.h"
#include <string.h>
#include <math.h>
#include "watchdog.h"

/*
 *
 */
THD_FUNCTION(bcnThread, arg) {
  bcn_app_conf_t* conf = (bcn_app_conf_t *)arg;

  // Start data collector (if not running yet)
  init_data_collector();

  // Start position thread
  TRACE_INFO("BCN  > Startup beacon thread");

  // Set telemetry configuration transmission variables
  // Each beacon send configuration data as the call signs may differ
  sysinterval_t last_conf_transmission =
      chVTGetSystemTime() - conf_sram.tel_enc_cycle;
  sysinterval_t time = chVTGetSystemTime();

  /* Now wait for our delay before starting. */

  chThdSleepUntil(chVTGetSystemTime() + conf->beacon.init_delay);

  while(true) {

    char code_s[100];
    pktDisplayFrequencyCode(conf->radio_conf.freq, code_s, sizeof(code_s));
    TRACE_INFO("POS  > Do module BEACON cycle for %s on %s",
               conf->call, code_s);
    extern thread_t *collector_thd;
    /*
     *  Pass pointer to beacon config to the collector thread.
     */
    dataPoint_t *dataPoint =
        (dataPoint_t *)chMsgSend(collector_thd, (msg_t)conf);
    if(!p_sleep(&conf->beacon.sleep_conf)) {

      if(!isPositionValid(dataPoint) || dataPoint == NULL) {
            TRACE_INFO("BCN  > Waiting for position data for"
                " %s (GPS state=%d)", conf->call, dataPoint->gps_state);
            chThdSleep(TIME_S2I(60));
            continue;
      }

      // Telemetry encoding parameter transmissions
      if(conf_sram.tel_enc_cycle != 0
    		  && chVTTimeElapsedSinceX(last_conf_transmission)
      	  	  	  >= conf_sram.tel_enc_cycle) {
        TRACE_INFO("BCN  > Transmit telemetry configuration");

        // Encode and transmit telemetry config packet
        for(uint8_t type = 0; type < APRS_NUM_TELEM_GROUPS; type++) {
          packet_t packet = aprs_encode_telemetry_configuration(
              conf->call,
              conf->path,
              conf->call,
              type);
          if(packet == NULL) {
            TRACE_WARN("BCN  > No free packet objects for"
                " telemetry config transmission");
          } else {
            if(!transmitOnRadio(packet,
                                conf->radio_conf.freq,
                                0,
                                0,
                                conf->radio_conf.pwr,
                                conf->radio_conf.mod,
                                conf->radio_conf.cca)) {
              TRACE_ERROR("BCN  > Failed to transmit telemetry config");
            }
          }
          chThdSleep(TIME_S2I(5));
        }
        last_conf_transmission += conf_sram.tel_enc_cycle;
      }

      TRACE_INFO("BCN  > Transmit position and telemetry");

      // Encode/Transmit position packet
      packet_t packet = aprs_encode_position_and_telemetry(conf->call,
                                                           conf->path,
                                                           conf->symbol,
                                                           dataPoint, true);
      if(packet == NULL) {
        TRACE_ERROR("BCN  > No free packet objects"
            " for position transmission");
      } else {
        if(!transmitOnRadio(packet,
                            conf->radio_conf.freq,
                            0,
                            0,
                            conf->radio_conf.pwr,
                            conf->radio_conf.mod,
                            conf->radio_conf.cca)) {
          TRACE_ERROR("BCN  > failed to transmit beacon data");
        }
        chThdSleep(TIME_S2I(5));
      }

      TRACE_INFO("BCN  > Transmit recently heard direct");
      /*
       * Encode/Transmit APRSD packet.
       * This is a tracker originated message (not a reply to a request).
       * The message will be addressed to the base station if set.
       * Else send it to device identity.
       */
      char *call = conf_sram.base.enabled
          ? conf_sram.base.call : conf->call;
      char *path = conf_sram.base.enabled
          ? conf_sram.base.path : conf->path;
      /*
       * Send message from this device.
       * Use call sign and path as specified in base config.
       * There is no acknowledgment requested.
       */
      packet = aprs_compose_aprsd_message(conf->call, path, call);
      if(packet == NULL) {
        TRACE_ERROR("BCN  > No free packet objects "
            "or badly formed APRSD message");
      } else {
        if(!transmitOnRadio(packet,
                            conf->radio_conf.freq,
                            0,
                            0,
                            conf->radio_conf.pwr,
                            conf->radio_conf.mod,
                            conf->radio_conf.cca
        )) {
          TRACE_ERROR("BCN  > Failed to transmit APRSD data");
        }
        chThdSleep(TIME_S2I(5));
      }
    } /* psleep */
    if(conf->run_once)
      chThdExit(MSG_OK);
    time = waitForTrigger(time, conf->beacon.cycle);
  }
}

/*
 *
 */
thread_t * start_beacon_thread(bcn_app_conf_t *conf, const char *name) {
  thread_t *th = chThdCreateFromHeap(NULL, THD_WORKING_AREA_SIZE(10*1024),
                                     name, LOWPRIO, bcnThread, conf);
  if(!th) {
    // Print startup error, do not start watchdog for this thread
    TRACE_ERROR("BCN  > Could not start thread (insufficient memory)");
  }
  return th;
}

