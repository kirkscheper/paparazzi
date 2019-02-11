/*
 * Copyright (C) Bas Pijnacker Hordijk
 *
 * This file is part of paparazzi
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, see
 * <http://www.gnu.org/licenses/>.
 */
/**
 * @file "modules/event_optic_flow/event_optic_flow.c"
 * @author Bas Pijnacker Hordijk
 * Event based optic flow detection and control using the Dynamic Vision Sensor (DVS).
 * Implementation is based on the following:
 * - The MAV used in this application is a customized MavTec drone on which the
 *    DVS is mounted, facing downwards.
 * - The DVS is connected through USB to an Odroid XU4 board which reads
 *    the raw event input.
 * - The Odroid processes and filters the input into 'optic flow events'.
 * - These new events are sent through UART to the Paparazzi autopilot.
 * - Real-time data is logged in Paparazzi to an SD card by the 'high speed logger' module.
 */

#include "event_optic_flow.h"

#include "mcu_periph/uart.h"
#include "mcu_periph/pipe.h"
#include "mcu_periph/sys_time.h"
#include "subsystems/abi.h"
#include "subsystems/datalink/telemetry.h"
#include "math/pprz_algebra.h"
#include "math/pprz_algebra_float.h"
#include "state.h"
#include "autopilot.h"
#include "filters/median_filter.h"

#include "paparazzi.h"
#include "firmwares/rotorcraft/stabilization.h"
#include "generated/modules.h"

#include <stdio.h>
//#include "firmwares/rotorcraft/guidance/guidance_v.h"


//#ifndef DVS_PORT
//#error Please define UART port connected to the DVS128 event-based camera. e.g <define name="DVS_PORT" value="uart0"/>
//#endif

// Module settings
#ifndef EOF_ENABLE_DEROTATION
#define EOF_ENABLE_DEROTATION 0
#endif
PRINT_CONFIG_VAR(EOF_ENABLE_DEROTATION)

#ifndef EOF_FILTER_TIME_CONST
#define EOF_FILTER_TIME_CONST 0.04f
#endif
PRINT_CONFIG_VAR(EOF_FILTER_TIME_CONST)

#ifndef EOF_FILTER_RETENTION_TIME_CONST
#define EOF_FILTER_RETENTION_TIME_CONST 0.04f
#endif
PRINT_CONFIG_VAR(EOF_FILTER_RETENTION_TIME_CONST)

#ifndef EOF_INLIER_MAX_DIFF
#define EOF_INLIER_MAX_DIFF 0.3f
#endif
PRINT_CONFIG_VAR(EOF_INLIER_MAX_DIFF)

#ifndef EOF_MIN_EVENT_RATE
#define EOF_MIN_EVENT_RATE 1000.f
#endif
PRINT_CONFIG_VAR(EOF_MIN_EVENT_RATE)

#ifndef EOF_MIN_POSITION_VARIANCE
#define EOF_MIN_POSITION_VARIANCE 0.1f
#endif
PRINT_CONFIG_VAR(EOF_MIN_POSITION_VARIANCE)

#ifndef EOF_MIN_R2
#define EOF_MIN_R2 0.05f    // was 0.01f
#endif
PRINT_CONFIG_VAR(EOF_MIN_R2)

#ifndef EOF_SEND_VEL
#define EOF_SEND_VEL 1
#endif

#define IR_LEDS_SWITCH 0
#define RECORD_SWITCH 0

#ifndef DVS_BODY_TO_CAM_PHI
#define DVS_BODY_TO_CAM_PHI 0.f
#endif
PRINT_CONFIG_VAR(DVS_BODY_TO_CAM_PHI)

#ifndef DVS_BODY_TO_CAM_THETA
#define DVS_BODY_TO_CAM_THETA 0.f
#endif
PRINT_CONFIG_VAR(DVS_BODY_TO_CAM_THETA)

#ifndef DVS_BODY_TO_CAM_PSI
#define DVS_BODY_TO_CAM_PSI 0.f
#endif
PRINT_CONFIG_VAR(DVS_BODY_TO_CAM_PSI)

// only apply rotation if we have to, save some computation
static const bool ROTATE_CAM_2_BODY = (fabsf(DVS_BODY_TO_CAM_PHI) > 1e-5f
    || fabsf(DVS_BODY_TO_CAM_THETA) > 1e-5f
    || fabsf(DVS_BODY_TO_CAM_PSI) > 1e-5f);

// NED
#ifndef DVS_BODY_TO_CAM_Z
//0.03f
#define DVS_BODY_TO_CAM_Z 0.f
#endif
PRINT_CONFIG_VAR(DVS_BODY_TO_CAM_Z)

/* Default sonar/agl to use */
#ifndef DVS_AGL_ID
#define DVS_AGL_ID ABI_BROADCAST
#endif
PRINT_CONFIG_VAR(DVS_AGL_ID)

/* Default gps to use */
#ifndef DVS_GPS_ID
#define DVS_GPS_ID ABI_BROADCAST
#endif
PRINT_CONFIG_VAR(DVS_GPS_ID)

/* Default gyro to use */
#ifndef DVS_GYRO_ID
#define DVS_GYRO_ID ABI_BROADCAST
#endif
PRINT_CONFIG_VAR(DVS_GYRO_ID)

/**************
 * DEFINITIONS *
 ***************/

// State definition
struct module_state eofState;

// Sensing parameters
uint8_t enableDerotation = 0;//EOF_ENABLE_DEROTATION;
float filterTimeConstant = EOF_FILTER_TIME_CONST;
float kfTimeConst = EOF_FILTER_RETENTION_TIME_CONST;
float inlierMaxDiff = EOF_INLIER_MAX_DIFF;

// Confidence thresholds
float minPosVariance = EOF_MIN_POSITION_VARIANCE;
float minEventRate = EOF_MIN_EVENT_RATE;
float minR2 = EOF_MIN_R2;

// Logging controls
bool irLedSwitch = IR_LEDS_SWITCH;

// Logging controls
bool record_switch = RECORD_SWITCH;

// Constants
#define EVENT_SEPARATOR 255
#define INT16_TO_FLOAT 0.001f
//static const float LENS_DISTANCE_TO_CENTER = 0.13f; // approximate distance of lens focal length to center of OptiTrack markers

#define EVENT_BYTE_SIZE 13 // +1 for separator
static const float power = 1.f;
static uint16_t mode;

struct MedianFilter3Float rate_filter;
struct MedianFilter3Float vel_filter;
// SWITCH THIS ON TO ENABLE CONTROL THROTTLE
//static const bool ASSIGN_CONTROL = false; //TODO Strange, had to rename this to make code compatible with optical_flow_landing

struct __attribute__((aligned)) flow_msg{
  int16_t xu;
  int16_t yu;
  int16_t u;
  int16_t v;
};

// Internal function declarations (definitions below)
enum updateStatus processInput(struct flowStats* s, int32_t *N);
static void sendFlowFieldState(struct transport_tx *trans, struct link_device *dev);
int16_t uartGetInt16(struct uart_periph *p);
int32_t uartGetInt32(struct uart_periph *p);

static struct FloatRMat body_to_cam;         ///< IMU to camera rotation

static float agl = 0.f;
static abi_event agl_ev; ///< The altitude ABI event
static void agl_cb(__attribute__((unused)) uint8_t sender_id, float distance)
{
  agl = distance - DVS_BODY_TO_CAM_Z;
}

static float gps_alt = 0.1f;
static abi_event gps_ev;
static void gps_cb(uint8_t sender_id,
                   uint32_t stamp __attribute__((unused)),
                   struct GpsState *gps_s)
{
  if(bit_is_set(gps_s->valid_fields, GPS_VALID_HMSL_BIT)){
    gps_alt = (float)gps_s->hmsl / 1000.f;
    eofState.z_NED = -gps_alt; // for downlink
#ifndef USE_SONAR
    agl = gps_alt - DVS_BODY_TO_CAM_Z;
#endif
  }

  if(bit_is_set(gps_s->valid_fields, GPS_VALID_VEL_ECEF_BIT)){
    struct NedCoor_i vel_ned_i;
    struct Int32Vect3 vel_body_i;
    struct FloatVect3 vel_body_f, vel_cam;
    ned_of_ecef_vect_i(&vel_ned_i, &state.ned_origin_i , &gps_s->ecef_vel);
    int32_rmat_vmult(&vel_body_i, stateGetNedToBodyRMat_i(), (struct Int32Vect3*)&vel_ned_i);
    vel_body_f.x = vel_body_i.x / 100.f;
    vel_body_f.y = vel_body_i.y / 100.f;
    vel_body_f.z = vel_body_i.z / 100.f;

    if(ROTATE_CAM_2_BODY){
      float_rmat_vmult(&vel_cam, &body_to_cam, &vel_body_f);
    } else {
     VECT3_COPY(vel_cam, vel_body_f);
    }

    // Update height/ground truth speeds from Optitrack
    eofState.wxTruth = vel_cam.x / (gps_alt + 0.01f);
    eofState.wyTruth = vel_cam.y / (gps_alt + 0.01f);
    eofState.DTruth  = vel_cam.z / (gps_alt + 0.01f);
  }
}

static bool new_gyro_meas = false;
static abi_event gyro_ev; ///< The gyro ABI event
static void gyro_cb(uint8_t __attribute__((unused)) sender_id,
                    uint32_t __attribute__((unused)) stamp,
                    struct Int32Rates __attribute__((unused))*gyro) {
  new_gyro_meas = true;
}

/*************************
 * MAIN SENSING FUNCTIONS *
 *************************/
void event_optic_flow_init(void) {
  struct FloatEulers euler = {DVS_BODY_TO_CAM_PHI, DVS_BODY_TO_CAM_THETA, DVS_BODY_TO_CAM_PSI};
  float_rmat_of_eulers(&body_to_cam, &euler);

  register_periodic_telemetry(DefaultPeriodic, PPRZ_MSG_ID_EVENT_OPTIC_FLOW_EST, sendFlowFieldState);

  // Subscribe to the altitude above ground level ABI messages
  AbiBindMsgAGL(DVS_AGL_ID, &agl_ev, agl_cb);
  AbiBindMsgIMU_GYRO_INT32(DVS_GYRO_ID, &gyro_ev, gyro_cb);
  AbiBindMsgGPS(DVS_GPS_ID, &gps_ev, gps_cb);
}

void event_optic_flow_start(void) {
  // Timing
  eofState.lastTime = get_sys_time_float();

  // Reset low pass filter for rates
  eofState.rates.p = 0.f;
  eofState.rates.q = 0.f;
  eofState.rates.r = 0.f;

  // (Re-)initialization
  eofState.moduleFrequency = 100.0f;
  eofState.z_NED = 0.0f;
  eofState.wxTruth = 0.0f;
  eofState.wyTruth = 0.0f;
  eofState.DTruth = 0.0f;
  struct flowField field = {0.f, 0.f, 0.f, 0.f, 0};
  eofState.field = field;
  flowStatsInit(&eofState.stats);
  eofState.NNew = 0;

  InitMedianFilterRatesFloat(rate_filter, 5);
  InitMedianFilterVect3Float(vel_filter, 5);

  mode = autopilot_get_mode();
}

// there is a delay between the incoming rate and the flow
#define NUM_RATES 10
void event_optic_flow_event(void) {
  static uint8_t old_rates_write = 0;
  static uint8_t old_rates_read = 0;
  static struct FloatRates old_rates[NUM_RATES];
  // Update body rates
  if (new_gyro_meas){
    // get and rotate body rates into camera frame
    if(ROTATE_CAM_2_BODY){
      float_rmat_ratemult(&old_rates[old_rates_write], &body_to_cam, stateGetBodyRates_f());
    } else {
     old_rates[old_rates_write].p = stateGetBodyRates_f()->p;
     old_rates[old_rates_write].q = stateGetBodyRates_f()->q;
     old_rates[old_rates_write].r = stateGetBodyRates_f()->r;
    }
    old_rates_write = (old_rates_write + 1) % NUM_RATES;

    UpdateMedianFilterRatesFloat(rate_filter, old_rates[old_rates_read]);
    if (old_rates_write == old_rates_read){
      old_rates_read = (old_rates_read + 1) % NUM_RATES;
    }

    GetMedianFilterRatesFloat(rate_filter, eofState.rates);

    eofState.rates = old_rates[old_rates_read];

    new_gyro_meas = false;
  }
}

void event_optic_flow_periodic(void) {
  // poll if record setting has changed
  set_record();

  if (mode != autopilot_get_mode()){
    // reset parameters when changing modes
    eofState.field.wx = 0.f;
    eofState.field.wy = 0.f;
    eofState.field.D  = 0.f;
    eofState.field.wx_filtered = 0.f;
    eofState.field.wy_filtered = 0.f;
    eofState.field.D_filtered  = 0.f;
    mode = autopilot_get_mode();
  }
  // Timing bookkeeping, do this after the most uncertain computations,
  // but before operations where timing info is necessary
  float currentTime = get_sys_time_float();
  float dt = currentTime - eofState.lastTime;
  if (dt <= 1e-5f){
    return;
  }
  eofState.moduleFrequency = 1.0f/dt;
  eofState.lastTime = currentTime;

  float filterFactor = dt/(filterTimeConstant + dt);
  Bound(filterFactor, 0.f, 1.f);

  float kfFactor = dt/(kfTimeConst + dt);
  Bound(kfFactor, 0.f, 1.f);

  // Obtain UART data if available
  eofState.NNew = 0;
  struct flowStats new_stats;
  flowStatsInit(&new_stats);
  processInput(&new_stats, &eofState.NNew);

  // Update statistics with low pass filter, updated with s1 = s0 + (n - s0)*dt/(tau + dt)
  for (uint16_t i = 0; i < N_FIELD_DIRECTIONS; i++) {
    eofState.stats.sumS[i]  += (new_stats.sumS[i]  - eofState.stats.sumS[i] ) * kfFactor;
    eofState.stats.sumSS[i] += (new_stats.sumSS[i] - eofState.stats.sumSS[i]) * kfFactor;
    eofState.stats.sumV[i]  += (new_stats.sumV[i]  - eofState.stats.sumV[i] ) * kfFactor;
    eofState.stats.sumVV[i] += (new_stats.sumVV[i] - eofState.stats.sumVV[i]) * kfFactor;
    eofState.stats.sumSV[i] += (new_stats.sumSV[i] - eofState.stats.sumSV[i]) * kfFactor;
    eofState.stats.N[i]     += (new_stats.N[i]     - eofState.stats.N[i]    ) * kfFactor;
  }
  eofState.stats.eventRate += (eofState.NNew/dt - eofState.stats.eventRate) * kfFactor;

  eofState.status = recomputeFlowField(&eofState.field, &eofState.stats, filterFactor,
              inlierMaxDiff, minEventRate, minPosVariance, minR2, power);

  struct FloatVect3 body_flow, body_flow_filtered;
  struct FloatVect3 cam_flow_filtered = {eofState.field.wx_filtered, eofState.field.wy_filtered, eofState.field.D_filtered};
  struct FloatVect3 cam_flow = {eofState.field.wx, eofState.field.wy, eofState.field.D};
  if(ROTATE_CAM_2_BODY){
    float_rmat_transp_vmult(&body_flow, &body_to_cam, &cam_flow);
    float_rmat_transp_vmult(&body_flow_filtered, &body_to_cam, &cam_flow_filtered);
  } else {
    VECT3_COPY(body_flow, cam_flow);
    VECT3_COPY(body_flow_filtered, cam_flow_filtered);
  }

  AbiSendMsgOPTICAL_FLOW(FLOW_DVS_ID, get_sys_time_usec(),
      body_flow_filtered.x,
      body_flow_filtered.y,
      body_flow_filtered.x,
      body_flow_filtered.y,
      eofState.field.confidence,
      body_flow_filtered.z);

  if(eofState.status == UPDATE_SUCCESS){
    // the imu already has it's own filter so just remove outliers with median filter
    UpdateMedianFilterVect3Float(vel_filter, body_flow);

    if (agl > 0.f && agl < 10.f){
      AbiSendMsgVELOCITY_ESTIMATE(VEL_DVS_ID, get_sys_time_usec(),
          agl*body_flow.x, agl*body_flow.y, agl*body_flow.z,
          eofState.field.confidence, eofState.field.confidence, -1.f);//eofState.field.confidence);
    }
  }
}

/***********************
 * SUPPORTING FUNCTIONS
 ***********************/
int16_t uartGetInt16(struct uart_periph *p) {
  int16_t out = 0;
  out |= uart_getch(p);
  out |= uart_getch(p) << 8;
  return out;
}

int32_t uartGetInt32(struct uart_periph *p) {
  int32_t out = 0;
  out |= uart_getch(p);
  out |= uart_getch(p) << 8;
  out |= uart_getch(p) << 16;
  out |= uart_getch(p) << 24;
  return out;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
enum updateStatus processInput(struct flowStats* s, int32_t *N) {
  enum updateStatus returnStatus = UPDATE_NONE;

  struct flowEvent e;

  *N = 0;
#ifdef DVS_PORT
  // Now scan across received data and extract events
  static bool synchronized = false;
  while(uart_char_available(&DVS_PORT) >= EVENT_BYTE_SIZE && *N < 500) {
    // Timestamp syncing at first event reception, by generating artificial event rate
    if (synchronized) {
      uint8_t separator;
      int16_t x,y,u,v;
      // Next set of bytes contains a new event
      x = uartGetInt16(&DVS_PORT);
      y = uartGetInt16(&DVS_PORT);
      e.t = uartGetInt32(&DVS_PORT);
      u = uartGetInt16(&DVS_PORT);
      v = uartGetInt16(&DVS_PORT);
      separator = uart_getch(&DVS_PORT);
      if (separator == EVENT_SEPARATOR) {
        // Full event received - we can process this further
        // TODO add timestamp checking - reject events that are outdated

        // Extract floating point position and velocity
        e.x = (float) x * INT16_TO_FLOAT;
        e.y = (float) y * INT16_TO_FLOAT;
        e.u = (float) u * INT16_TO_FLOAT;
        e.v = (float) v * INT16_TO_FLOAT;`

        flowStatsUpdate(s, e);
        returnStatus = UPDATE_STATS;
        (*N)++;
      } else {
        // we are apparently out of sync - do not process event
        synchronized = false;
      }
    } else {
      // (Re)synchronize at next separator
      if (uart_getch(&DVS_PORT) == EVENT_SEPARATOR) {
        synchronized = true;
      }
    }
  }
#endif
#ifdef DVS_PIPE
  static char __attribute__((aligned)) data[PIPE_RX_BUFFER_SIZE];
  uint16_t i = 0;
  while(pipe_char_available(&DVS_PIPE))
  {
    data[i++] = pipe_getch(&DVS_PIPE);
  }

  uint16_t nb_msgs = i / sizeof(struct flow_msg);
  struct flow_msg *flow_msgs = (struct flow_msg*)data;
  
  for (i=0; i<nb_msgs; i++)
  {
    e.x = (float)flow_msgs[i].xu * INT16_TO_FLOAT;
    e.y = (float)flow_msgs[i].yu * INT16_TO_FLOAT;
    e.u = (float)flow_msgs[i].u * INT16_TO_FLOAT;
    e.v = (float)flow_msgs[i].v * INT16_TO_FLOAT;
    flowStatsUpdate(s, e);
    (*N)++;
  }

  if (nb_msgs){
    returnStatus = UPDATE_STATS;
  }
#endif
  return returnStatus;
}
#pragma GCC diagnostic pop

void set_record(void)
{
  static uint8_t msg[] = {0x99, 0};
  static bool local_switch = RECORD_SWITCH;
  if (local_switch == record_switch){
    return;
  }
  local_switch = record_switch;
#ifdef DVS_PIPE
  if (record_switch) {
    msg[1] = '1';
  } else {
    msg[1] = '0';
  }
  pipe_send_raw(&DVS_PIPE, 0, msg, 2);
  if (record_switch) {
    logger_file_file_logger_periodic_status = MODULES_START;
  } else {
    logger_file_file_logger_periodic_status = MODULES_STOP;
  }
#endif
}

static void sendFlowFieldState(struct transport_tx *trans, struct link_device *dev) {
  float fps = eofState.moduleFrequency;
  uint8_t status = (uint8_t) eofState.status;
  float confidence = eofState.field.confidence;
  float eventRate = eofState.stats.eventRate;
  uint32_t nNew = eofState.NNew;
  float wx = eofState.field.wx_filtered;
  float wy = eofState.field.wy_filtered;
  float D  = eofState.field.D_filtered;
  float p = eofState.rates.p;
  float q = eofState.rates.q;
  float wxTruth = eofState.wxTruth;
  float wyTruth = eofState.wyTruth;
  float DTruth = eofState.DTruth;

  pprz_msg_send_EVENT_OPTIC_FLOW_EST(trans, dev, AC_ID,
      &fps, &status, &confidence, &eventRate, &nNew, &wx, &wy, &D, &p, &q,
      &wxTruth,&wyTruth,&DTruth);

}
