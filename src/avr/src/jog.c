/******************************************************************************\

                  This file is part of the Buildbotics firmware.

         Copyright (c) 2015 - 2023, Buildbotics LLC, All rights reserved.

          This Source describes Open Hardware and is licensed under the
                                  CERN-OHL-S v2.

          You may redistribute and modify this Source and make products
     using it under the terms of the CERN-OHL-S v2 (https:/cern.ch/cern-ohl).
            This Source is distributed WITHOUT ANY EXPRESS OR IMPLIED
     WARRANTY, INCLUDING OF MERCHANTABILITY, SATISFACTORY QUALITY AND FITNESS
      FOR A PARTICULAR PURPOSE. Please see the CERN-OHL-S v2 for applicable
                                   conditions.

                 Source location: https://github.com/buildbotics

       As per CERN-OHL-S v2 section 4, should You produce hardware based on
     these sources, You must maintain the Source Location clearly visible on
     the external case of the CNC Controller or other product you make using
                                   this Source.

                 For more information, email info@buildbotics.com

\******************************************************************************/

#include "jog.h"

#include "axis.h"
#include "util.h"
#include "exec.h"
#include "state.h"
#include "command.h"
#include "config.h"
#include "SCurve.h"

#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>


typedef struct {
  bool holding;
  bool writing;

  SCurve scurves[AXES];
  float next[AXES];
  float targetV[AXES];

  uint16_t id;
  uint16_t nextID;
  uint16_t lastID;
} jr_t;

jr_t jr = {0};


stat_t jog_exec() {
  bool done = true;

  // Compute per axis velocities and target positions
  float target[AXES] = {0,};
  float velocity_sqr = 0;

  for (int axis = 0; axis < AXES; axis++) {
    if (!axis_is_enabled(axis)) continue;

    // Load next velocity
    if (!jr.writing)
      jr.targetV[axis] = jr.next[axis] * axis_get_velocity_max(axis);

    float p = exec_get_axis_position(axis);
    float vel = jr.scurves[axis].getVelocity();
    float targetV = jr.targetV[axis];
    float min = axis_get_soft_limit(axis, true);
    float max = axis_get_soft_limit(axis, false);
    bool softLimited = min != max && axis_get_homed(axis);

    // Apply soft limits, if enabled and homed
    if (softLimited && MIN_VELOCITY < fabs(targetV)) {
      float dist = jr.scurves[axis].getStoppingDist() *
        (1 + (JOG_STOPPING_UNDERSHOOT / 100.0));

      if (vel < 0 && p - dist <= min) targetV = -MIN_VELOCITY;
      if (0 < vel && max <= p + dist) targetV = MIN_VELOCITY;
    }

    // Compute next velocity
    float v = jr.scurves[axis].next(SEGMENT_TIME, targetV);

    // Don't overshoot soft limits
    float deltaP = v * SEGMENT_TIME;
    if (softLimited && 0 < deltaP && max < p + deltaP) p = max;
    else if (softLimited && deltaP < 0 && p + deltaP < min) p = min;
    else p += deltaP;

    // Not done jogging if still moving
    if (MIN_VELOCITY < fabs(v) || MIN_VELOCITY < fabs(targetV)) done = false;

    velocity_sqr += square(v);
    target[axis] = p;
  }

  // Next jog ID
  if (!jr.writing && jr.id != jr.nextID) {
    jr.lastID = jr.id;
    jr.id = jr.nextID;
  }

  // Check if we are done
  if (done) {
    jr.lastID = jr.id;
    command_reset_position();
    exec_set_velocity(0);
    exec_set_cb(0);
    if (jr.holding) state_holding();
    else state_idle();

    return STAT_NOP; // Done, no move executed
  }

  // Set velocity and target
  exec_set_velocity(sqrt(velocity_sqr));
  exec_move_to_target(target);

  return STAT_OK;
}


void jog_stop() {
  if (state_get() != STATE_JOGGING) return;
  jr.writing = true;
  for (int axis = 0; axis < AXES; axis++) jr.next[axis] = 0;
  jr.writing = false;
}


stat_t command_jog(char *cmd) {
  // Ignore jog commands when not READY, HOLDING or JOGGING
  if (state_get() != STATE_READY && state_get() != STATE_HOLDING &&
      state_get() != STATE_JOGGING)
    return STAT_NOP;

  // Skip over command code
  cmd++;

  // Get ID
  uint16_t id;
  if (!decode_hex_u16(&cmd, &id)) return STAT_BAD_INT;

  // Get velocities
  float velocity[AXES] = {0,};
  stat_t status = decode_axes(&cmd, velocity);
  if (status) return status;

  // Check for end of command
  if (*cmd) return STAT_INVALID_ARGUMENTS;

  // Start jogging
  if (state_get() != STATE_JOGGING) {
    memset((void *)&jr, 0, sizeof(jr));

    jr.holding = state_get() == STATE_HOLDING;

    for (int axis = 0; axis < AXES; axis++)
      if (axis_is_enabled(axis))
        jr.scurves[axis] =
          SCurve(axis_get_velocity_max(axis), axis_get_accel_max(axis),
                 axis_get_jerk_max(axis));

    state_jogging();
    exec_set_cb(jog_exec);
  }

  // Set next velocities
  jr.writing = true;
  for (int axis = 0; axis < AXES; axis++) jr.next[axis] = velocity[axis];
  jr.nextID = id;
  jr.writing = false;

  return STAT_OK;
}


// Variable callbacks
uint16_t get_jog_id() {return jr.lastID;}
