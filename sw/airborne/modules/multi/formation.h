/** \file formation.h
 *  \brief Formation flight library
 *
 */


#ifndef FORMATION_H
#define FORMATION_H

#include "firmwares/fixedwing/nav.h"
#include "modules/multi/traffic_info.h"

#define FORM_MODE_GLOBAL 0
#define FORM_MODE_COURSE 1

extern float coef_form_alt, coef_form_pos, coef_form_speed, coef_form_course;
extern float form_prox;
extern uint8_t form_mode;
extern uint8_t leader_id;

enum slot_status {UNSET, ACTIVE, IDLE, LOST};

struct slot_ {
  enum slot_status status;
  float east;
  float north;
  float alt;
};

extern struct slot_ formation[NB_ACS];

extern int formation_init(void);

extern int add_slot(uint8_t _id, float slot_e, float slot_n, float slot_a);

static inline void updateSlot(uint8_t id, float se, float sn, float sa) {
  formation[ti_acs_id[id]].east = se;
  formation[ti_acs_id[id]].north = sn;
  formation[ti_acs_id[id]].alt = sa;
}

static inline updateFormationStatus( uint8_t id, uint8_t status) { formation[ti_acs_id[id]].status = status; }

static inline void parseFormationStatus(void) {
  uint8_t ac_id = DL_FORMATION_STATUS_ac_id(dl_buffer);
  uint8_t leader = DL_FORMATION_STATUS_leader_id(dl_buffer);
  uint8_t status = DL_FORMATION_STATUS_status(dl_buffer);
  if (ac_id == AC_ID) leader_id = leader;
  else if (leader == leader_id) { updateFormationStatus(ac_id,status); }
  else { updateFormationStatus(ac_id,UNSET); }
}

static inline void parseFormationSlot(void) {
  uint8_t ac_id = DL_FORMATION_SLOT_ac_id(dl_buffer);
  uint8_t mode = DL_FORMATION_SLOT_mode(dl_buffer);
  float slot_east = DL_FORMATION_SLOT_slot_east(dl_buffer);
  float slot_north = DL_FORMATION_SLOT_slot_north(dl_buffer);
  float slot_alt = DL_FORMATION_SLOT_slot_alt(dl_buffer);
  updateSlot(ac_id, slot_east, slot_north, slot_alt);
  if (ac_id == leader_id) form_mode = mode;
}

extern int start_formation(void);
extern int stop_formation(void);
extern int formation_flight(void);
extern void formation_pre_call(void);

#endif /* FORMATION_H */
