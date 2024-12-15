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

#include "vars.h"

#include "type.h"
#include "status.h"
#include "hardware.h"
#include "config.h"
#include "axis.h"
#include "cpp_magic.h"
#include "report.h"
#include "command.h"

#include <string.h>
#include <stdio.h>

// Format strings
static const char code_fmt[] PROGMEM = "\"%s\":";
static const char indexed_code_fmt[] PROGMEM = "\"%c%s\":";


// Ensure no var code is used more than once
enum {
#define VAR(NAME, CODE, ...) var_code_##CODE,
#include "vars.def"
#undef VAR
  var_code_count
};


// Var forward declarations
#define VAR(NAME, CODE, TYPE, INDEX, SET, ...)          \
  TYPE get_##NAME(IF(INDEX)(int index));                \
  IF(SET)                                               \
  (void set_##NAME(IF(INDEX)(int index,) TYPE value);)

#include "vars.def"
#undef VAR


// Set callback union
typedef union {
  void *ptr;
#define TYPEDEF(TYPE, ...) void (*set_##TYPE)(TYPE);
#include "type.def"
#undef TYPEDEF

#define TYPEDEF(TYPE, ...) void (*set_##TYPE##_index)(int i, TYPE);
#include "type.def"
#undef TYPEDEF
} set_cb_u;


// Get callback union
typedef union {
  void *ptr;
#define TYPEDEF(TYPE, ...) TYPE (*get_##TYPE)();
#include "type.def"
#undef TYPEDEF

#define TYPEDEF(TYPE, ...) TYPE (*get_##TYPE##_index)(int i);
#include "type.def"
#undef TYPEDEF
} get_cb_u;


typedef struct {
  type_t type;
  char name[5];
  int8_t index;
  get_cb_u get;
  set_cb_u set;
} var_info_t;


// Var names
#define VAR(NAME, CODE, TYPE, INDEX, SET, REPORT, ...)       \
  static const char NAME##_name[] PROGMEM = #NAME;

#include "vars.def"
#undef VAR


// Last value
#define VAR(NAME, CODE, TYPE, INDEX, ...)       \
  static TYPE NAME##_state IF(INDEX)([INDEX]);

#include "vars.def"
#undef VAR


// Report
static uint8_t _report_var[(var_code_count >> 3) + 1] = {0,};


static bool _get_report_var(int index) {
  return _report_var[index >> 3] & (1 << (index & 7));
}


static void _set_report_var(int index, bool enable) {
  if (enable) _report_var[index >> 3] |= 1 << (index & 7);
  else _report_var[index >> 3] &= ~(1 << (index & 7));
}


static int _find_code(const char *code) {
#define VAR(NAME, CODE, TYPE, INDEX, ...)                               \
  if (!strcmp(code, #CODE)) return var_code_##CODE;                     \

#include "vars.def"
#undef VAR
  return -1;
}


void vars_init() {
  // Initialize var state
#define VAR(NAME, CODE, TYPE, INDEX, ...)                       \
  IF(INDEX)(for (int i = 0; i < INDEX; i++))                    \
    (NAME##_state)IF(INDEX)([i]) = get_##NAME(IF(INDEX)(i));

#include "vars.def"
#undef VAR

// Report
#define VAR(NAME, CODE, TYPE, INDEX, SET, REPORT, ...)  \
  _set_report_var(var_code_##CODE, REPORT);

#include "vars.def"
#undef VAR
}


void vars_report(bool full) {
  bool reported = false;

#define VAR(NAME, CODE, TYPE, INDEX, ...)                               \
  if (_get_report_var(var_code_##CODE)) {                               \
    IF(INDEX)(for (int i = 0; i < (INDEX ? INDEX : 1); i++)) {          \
      TYPE value = get_##NAME(IF(INDEX)(i));                            \
      TYPE last = (NAME##_state)IF(INDEX)([i]);                         \
                                                                        \
      if (full || (!type_eq_##TYPE(value, last))) {                     \
        (NAME##_state)IF(INDEX)([i]) = value;                           \
                                                                        \
        if (!reported) {                                                \
          reported = true;                                              \
          putchar('{');                                                 \
        } else putchar(',');                                            \
                                                                        \
        printf_P                                                        \
          (IF_ELSE(INDEX)(indexed_code_fmt, code_fmt),                  \
           IF(INDEX)(INDEX##_LABEL[i],) #CODE);                         \
                                                                        \
        type_print_##TYPE(value);                                       \
      }                                                                 \
    }                                                                   \
  }

#include "vars.def"
#undef VAR

  if (reported) printf("}\n");
}

void vars_report_all(bool enable) {
#define VAR(NAME, CODE, TYPE, INDEX, SET, REPORT, ...)                  \
  _set_report_var(var_code_##CODE, enable);

#include "vars.def"
#undef VAR
}


void vars_report_var(const char *code, bool enable) {
  int index = _find_code(code);
  if (index != -1) _set_report_var(index, enable);
}


static char *_resolve_name(const char *_name) {
  unsigned len = strlen(_name);

  if (!len || 4 < len) return 0;

  static char name[5];
  strncpy(name, _name, 4);
  name[4] = 0;

  // Handle axis to motor mapping
  if (2 < len && name[1] == '.') {
    int axis = axis_get_id(name[0]);
    if (axis < 0) return 0;
    int motor = axis_get_motor(axis);
    if (motor < 0) return 0;

    name[0] = MOTORS_LABEL[motor];
    for (int i = 1; _name[i]; i++)
      name[i] = _name[i + 1];
  }

  return name;
}


static int _index(char c, const char *s) {
  const char *index = strchr(s, c);
  return index ? index - s : -1;
}


static bool _find_var(const char *_name, var_info_t *info) {
  char *name = _resolve_name(_name);
  if (!name) return false;

  int i = -1;
  memset(info, 0, sizeof(var_info_t));
  strcpy(info->name, name);

#define VAR(NAME, CODE, TYPE, INDEX, SET, ...)                          \
  if (!strcmp(IF_ELSE(INDEX)(name + 1, name), #CODE)                    \
      IF(INDEX)(&& (i = _index(name[0], INDEX##_LABEL)) != -1)          \
      ) {                                                               \
                                                                        \
    info->type = TYPE_##TYPE;                                           \
    info->index = i;                                                    \
    info->get.IF_ELSE(INDEX)(get_##TYPE##_index, get_##TYPE) =          \
      get_##NAME;                                                       \
                                                                        \
    IF(SET)(info->set.IF_ELSE(INDEX)                                    \
            (set_##TYPE##_index, set_##TYPE) = set_##NAME;)             \
                                                                        \
      return true;                                                      \
  }

#include "vars.def"
#undef VAR

  return false;
}


static type_u _get(type_t type, int8_t index, get_cb_u cb) {
  type_u value;

  switch (type) {
#define TYPEDEF(TYPE, ...)                                              \
    case TYPE_##TYPE:                                                   \
      if (index == -1) value._##TYPE = cb.get_##TYPE();                 \
      value._##TYPE = cb.get_##TYPE##_index(index);                     \
      break;
#include "type.def"
#undef TYPEDEF
  }

  return value;
}


static void _set(type_t type, int8_t index, set_cb_u cb, type_u value) {
  switch (type) {
#define TYPEDEF(TYPE, ...)                                              \
    case TYPE_##TYPE:                                                   \
      if (index == -1) cb.set_##TYPE(value._##TYPE);                    \
      else cb.set_##TYPE##_index(index, value._##TYPE);                 \
      break;
#include "type.def"
#undef TYPEDEF
  }
}


stat_t vars_print(const char *name) {
  var_info_t info;
  if (!_find_var(name, &info)) return STAT_UNRECOGNIZED_NAME;

  printf_P(PSTR("{\"%s\":"), info.name);
  type_print(info.type, _get(info.type, info.index, info.get));
  putchar('}');
  putchar('\n');

  return STAT_OK;
}


stat_t vars_set(const char *name, const char *value) {
  var_info_t info;
  if (!_find_var(name, &info)) return STAT_UNRECOGNIZED_NAME;
  if (!info.set.ptr) return STAT_READ_ONLY;

  stat_t status;
  type_u x = type_parse(info.type, value, &status);
  if (status == STAT_OK) _set(info.type, info.index, info.set, x);

  return status;
}


void vars_print_json() {
  bool first = true;
  static const char fmt[] PROGMEM =
    "\"%s\":{\"name\":\"%" PRPSTR "\",\"type\":\"%" PRPSTR "\"";
  static const char index_fmt[] PROGMEM = ",\"index\":\"%s\"";

#define VAR(NAME, CODE, TYPE, INDEX, ...)                               \
  if (first) first = false; else putchar(',');                          \
  printf_P(fmt, #CODE, NAME##_name, type_get_##TYPE##_name_pgm());      \
  IF(INDEX)(printf_P(index_fmt, INDEX##_LABEL));                        \
  putchar('}');
#include "vars.def"
#undef VAR
}


// Command callbacks
stat_t command_var(char *cmd) {
  cmd++; // Skip command code

  if (*cmd == '$' && !cmd[1]) {
    report_request_full();
    return STAT_OK;
  }

  // Get or set variable
  char *value = strchr(cmd, '=');
  if (value) {
    *value++ = 0;
    stat_t status = vars_set(cmd, value);
    value[-1] = '='; // For error reporting
    return status;
  }

  return vars_print(cmd);
}


typedef struct {
  type_t type;
  int8_t index;
  set_cb_u set;
  type_u value;
} var_cmd_t;


stat_t command_sync_var(char *cmd) {
  // Get value
  char *value = strchr(cmd + 1, '=');
  if (!value) return STAT_INVALID_COMMAND;
  *value++ = 0;

  var_info_t info;
  if (!_find_var(cmd + 1, &info)) return STAT_UNRECOGNIZED_NAME;
  if (!info.set.ptr) return STAT_READ_ONLY;

  stat_t status;
  var_cmd_t buffer;

  buffer.type  = info.type;
  buffer.index = info.index;
  buffer.set   = info.set;
  buffer.value = type_parse(info.type, value, &status);

  if (status == STAT_OK) command_push(*cmd, &buffer);

  return status;
}


unsigned command_sync_var_size() {return sizeof(var_cmd_t);}


void command_sync_var_exec(void *data) {
  var_cmd_t *cmd = (var_cmd_t *)data;
  _set(cmd->type, cmd->index, cmd->set, cmd->value);
}


stat_t command_report(char *cmd) {
  bool enable = cmd[1] != '0';

  if (cmd[2]) vars_report_var(cmd + 2, enable);
  else vars_report_all(enable);

  return STAT_OK;
}
