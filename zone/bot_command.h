/*	EQEMu: Everquest Server Emulator
	Copyright (C) 2001-2015 EQEMu Development Team (http://eqemulator.org)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; version 2 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY except by those people which sell it, which
	are required to give you total support for your newly bought product;
	without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE. See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/


#ifndef BOT_COMMAND_H
#define BOT_COMMAND_H

class Client;
class Seperator;

#include "../common/types.h"
#include "bot.h"


class BCEnum
{
public:
	typedef enum SpellType {
		ST_None,
		ST_BindAffinity,
		ST_Charm,
		ST_Cure,
		ST_Defensive,
		ST_Depart,
		ST_Escape,
		ST_Grow,
		ST_Identify,
		ST_Invisibility,
		ST_Levitation,
		ST_Lull,
		ST_Mesmerize,
		ST_MovementSpeed,
		ST_Resistance,
		ST_Resurrect,
		ST_Rune,
		ST_SendHome,
		ST_Shrink,
		ST_SummonCorpse,
		ST_WaterBreathing
	} SType;
	static const int SpellTypeFirst = ST_BindAffinity;
	static const int SpellTypeLast = ST_WaterBreathing;

	typedef enum TargetType {
		TT_None,
		TT_Animal,
		TT_Undead,
		TT_Summoned,
		TT_Plant,
		TT_Single,
		TT_GroupV1,
		TT_GroupV2,
		TT_AECaster,
		TT_AEBard,
		TT_AETarget,
		TT_Corpse
	} TType;

	typedef enum TargetMask {
		TM_None = 0,
		TM_Animal = 1,
		TM_Undead = 2,
		TM_Summoned = 4,
		TM_Plant = 8,
		TM_Single = 31, // currently, 2^4 + 2^{0..3})
		TM_GroupV1 = 32,
		TM_GroupV2 = 64,
		TM_AECaster = 128,
		TM_AEBard = 256,
		TM_AETarget = 512,
		TM_Corpse = 1024
	} TMask;

	typedef enum AppearanceFailType {
		AFT_None,
		AFT_Value,
		AFT_GenderRace,
		AFT_Race
	} AFType;

	typedef enum AilmentMask {
		AM_None = 0,
		AM_Blindness = 1,	// SE: 20
		AM_Disease = 2,		// SE: 35
		AM_Poison = 4,		// SE: 36
		AM_Curse = 8,		// SE: 116
		AM_Corruption = 16	// SE: 369
	} AMask;

	typedef enum AilmentType {
		AT_None,
		AT_Blindness,
		AT_Disease,
		AT_Poison,
		AT_Curse,
		AT_Corruption
	} AType;
	static const int AilmentTypeCount = 5;

	typedef enum InvisibilityType {
		IT_None,
		IT_Animal,
		IT_Undead,
		IT_Living,
		IT_See
	} IType;

	typedef enum ResistanceMask {
		RM_None = 0,
		RM_Fire = 1,		// SE: 46
		RM_Cold = 2,		// SE: 47
		RM_Poison = 4,		// SE: 48
		RM_Disease = 8,		// SE: 49
		RM_Magic = 16,		// SE: 50
		RM_Corruption = 32	// SE: 370
	} RMask;

	typedef enum ResistanceType {
		RT_None,
		RT_Fire,
		RT_Cold,
		RT_Poison,
		RT_Disease,
		RT_Magic,
		RT_Corruption
	} RType;
	static const int ResistanceTypeCount = 6;

	static std::string SpellTypeEnumToString(BCEnum::SType spell_type) {
		switch (spell_type) {
		case ST_BindAffinity:
			return "ST_BindAffinity";
		case ST_Charm:
			return "ST_Charm";
		case ST_Cure:
			return "ST_Cure";
		case ST_Defensive:
			return "ST_Defensive";
		case ST_Depart:
			return "ST_Depart";
		case ST_Escape:
			return "ST_Escape";
		case ST_Grow:
			return "ST_Grow";
		case ST_Identify:
			return "ST_Identify";
		case ST_Invisibility:
			return "ST_Invisibility";
		case ST_Levitation:
			return "ST_Levitation";
		case ST_Lull:
			return "ST_Lull";
		case ST_Mesmerize:
			return "ST_Mesmerize";
		case ST_MovementSpeed:
			return "ST_MovementSpeed";
		case ST_Resistance:
			return "ST_Resistance";
		case ST_Resurrect:
			return "ST_Resurrect";
		case ST_Rune:
			return "ST_Rune";
		case ST_SendHome:
			return "ST_SendHome";
		case ST_Shrink:
			return "ST_Shrink";
		case ST_SummonCorpse:
			return "ST_SummonCorpse";
		case ST_WaterBreathing:
			return "ST_WaterBreathing";
		default:
			return "ST_None";
		}
	}

	static std::string TargetTypeEnumToString(BCEnum::TType target_type) {
		switch (target_type) {
		case TT_Animal:
			return "TT_Animal";
		case TT_Undead:
			return "TT_Undead";
		case TT_Summoned:
			return "TT_Summoned";
		case TT_Plant:
			return "TT_Plant";
		case TT_Single:
			return "TT_Single";
		case TT_GroupV1:
			return "TT_GroupV1";
		case TT_GroupV2:
			return "TT_GroupV2";
		case TT_AECaster:
			return "TT_AECaster";
		case TT_AEBard:
			return "TT_AEBard";
		case TT_AETarget:
			return "TT_AETarget";
		case TT_Corpse:
			return "TT_Corpse";
		default:
			return "TT_None";
		}
	}
};


class STBaseEntry;
class STCureEntry;
class STDepartEntry;
class STEscapeEntry;
class STInvisibilityEntry;
class STResistanceEntry;

class STBaseEntry {
public:
	int spell_id;
	uint8 spell_level;
	uint8 caster_class;
	BCEnum::TType target_type;
	BCEnum::SType bcst;

	STBaseEntry() {
		spell_id = 0;
		spell_level = 255;
		caster_class = 255;
		target_type = BCEnum::TT_None;
		bcst = BCEnum::ST_None;
	}
	STBaseEntry(STBaseEntry* prototype) {
		spell_id = prototype->spell_id;
		spell_level = 255;
		caster_class = 255;
		target_type = prototype->target_type;
		bcst = prototype->bcst;
	}
	virtual ~STBaseEntry() { return; };

	bool IsCure() const { return (bcst == BCEnum::ST_Cure); }
	bool IsDepart() const { return (bcst == BCEnum::ST_Depart); }
	bool IsEscape() const { return (bcst == BCEnum::ST_Escape); }
	bool IsInvisibility() const { return (bcst == BCEnum::ST_Invisibility); }
	bool IsResistance() const { return (bcst == BCEnum::ST_Resistance); }

	virtual STCureEntry* SafeCastToCure() { return nullptr; }
	virtual STDepartEntry* SafeCastToDepart() { return nullptr; }
	virtual STEscapeEntry* SafeCastToEscape() { return nullptr; }
	virtual STInvisibilityEntry* SafeCastToInvisibility() { return nullptr; }
	virtual STResistanceEntry* SafeCastToResistance() { return nullptr; }
};

class STCureEntry : public STBaseEntry {
public:
	int cure_mask;
	int cure_value[BCEnum::AilmentTypeCount];

	STCureEntry() {
		cure_mask = 0;
		memset(&cure_value, 0, (sizeof(int) * BCEnum::AilmentTypeCount));
	}
	STCureEntry(STCureEntry* prototype) : STBaseEntry(prototype) {
		cure_mask = prototype->cure_mask;
		memcpy(&cure_value, prototype->cure_value, (sizeof(int) * BCEnum::AilmentTypeCount));
	}
	virtual ~STCureEntry() { return; };

	virtual STCureEntry* SafeCastToCure() { return ((bcst == BCEnum::ST_Cure) ? (static_cast<STCureEntry*>(this)) : (nullptr)); }
};

class STDepartEntry : public STBaseEntry {
public:
	std::string long_name;

	STDepartEntry() { long_name.clear(); }
	STDepartEntry(STDepartEntry* prototype) : STBaseEntry(prototype) { long_name = prototype->long_name; }
	virtual ~STDepartEntry() { return; };

	virtual STDepartEntry* SafeCastToDepart() { return ((bcst == BCEnum::ST_Depart) ? (static_cast<STDepartEntry*>(this)) : (nullptr)); }
};

class STEscapeEntry : public STBaseEntry {
public:
	bool lesser;

	STEscapeEntry() { lesser = false; }
	STEscapeEntry(STEscapeEntry* prototype) : STBaseEntry(prototype) { lesser = prototype->lesser; }
	virtual ~STEscapeEntry() { return; };

	virtual STEscapeEntry* SafeCastToEscape() { return ((bcst == BCEnum::ST_Escape) ? (static_cast<STEscapeEntry*>(this)) : (nullptr)); }
};

class STInvisibilityEntry : public STBaseEntry {
public:
	BCEnum::IType invis_type;

	STInvisibilityEntry() { invis_type = BCEnum::IT_None; }
	STInvisibilityEntry(STInvisibilityEntry* prototype) : STBaseEntry(prototype) { invis_type = prototype->invis_type; }
	virtual ~STInvisibilityEntry() { return; };

	virtual STInvisibilityEntry* SafeCastToInvisibility() { return ((bcst == BCEnum::ST_Invisibility) ? (static_cast<STInvisibilityEntry*>(this)) : (nullptr)); }
};

class STResistanceEntry : public STBaseEntry {
public:
	int resist_mask;
	int resist_value[BCEnum::ResistanceTypeCount];

	STResistanceEntry() {
		resist_mask = 0;
		memset(&resist_value, 0, (sizeof(int) * BCEnum::ResistanceTypeCount));
	}
	STResistanceEntry(STResistanceEntry* prototype) : STBaseEntry(prototype) {
		resist_mask = prototype->resist_mask;
		memcpy(&resist_value, prototype->resist_value, (sizeof(int) * BCEnum::ResistanceTypeCount));
	}
	virtual ~STResistanceEntry() { return; };

	virtual STResistanceEntry* SafeCastToResistance() { return ((bcst == BCEnum::ST_Resistance) ? (static_cast<STResistanceEntry*>(this)) : (nullptr)); }
};


#define	BOT_COMMAND_CHAR '^'

typedef void (*BotCmdFuncPtr)(Client *,const Seperator *);

typedef struct {
	int access;
	const char *desc;			// description of bot command
	BotCmdFuncPtr function;		// null means perl function
} BotCommandRecord;

extern int (*bot_command_dispatch)(Client *,char const*);
extern int bot_command_count;	// number of bot commands loaded


// the bot command system:
int bot_command_init(void);
void bot_command_deinit(void);
int bot_command_add(std::string bot_command_name, const char *desc, int access, BotCmdFuncPtr function);
int bot_command_not_avail(Client *c, const char *message);
int bot_command_real_dispatch(Client *c, char const *message);
void bot_command_log_command(Client *c, const char *message);


// bot commands
void bot_command_bind_affinity(Client *c, const Seperator *sep);
void bot_command_bot(Client *c, const Seperator *sep);
void bot_command_botgroup(Client *c, const Seperator *sep);
#ifdef BUGTRACK
void bot_command_bug(Client *c, const Seperator *sep);
#endif
void bot_command_charm(Client *c, const Seperator *sep);
void bot_command_cure(Client *c, const Seperator *sep);
void bot_command_defensive(Client *c, const Seperator *sep);
void bot_command_depart(Client *c, const Seperator *sep);
void bot_command_escape(Client *c, const Seperator *sep);
void bot_command_find_aliases(Client *c, const Seperator *sep);
void bot_command_follow_distance(Client *c, const Seperator *sep);
void bot_command_group(Client *c, const Seperator *sep);
void bot_command_grow(Client *c, const Seperator *sep);
void bot_command_heal_rotation(Client *c, const Seperator *sep);
void bot_command_help(Client *c, const Seperator *sep);
void bot_command_identify(Client *c, const Seperator *sep);
void bot_command_inventory(Client *c, const Seperator *sep);
void bot_command_invisibility(Client *c, const Seperator *sep);
void bot_command_item(Client *c, const Seperator *sep);
void bot_command_levitation(Client *c, const Seperator *sep);
void bot_command_lull(Client *c, const Seperator *sep);
void bot_command_mesmerize(Client *c, const Seperator *sep);
void bot_command_movement_speed(Client *c, const Seperator *sep);
#ifdef PACKET_PROFILER
void bot_command_packet_profile(Client *c, const Seperator *sep);
#endif
void bot_command_pet(Client *c, const Seperator *sep);
void bot_command_pick_lock(Client *c, const Seperator *sep);
#ifdef EQPROFILE
void bot_command_profile_dump(Client *c, const Seperator *sep);
void bot_command_profile_reset(Client *c, const Seperator *sep);
#endif
void bot_command_pull(Client *c, const Seperator *sep);
void bot_command_resistance(Client *c, const Seperator *sep);
void bot_command_resurrect(Client *c, const Seperator *sep);
void bot_command_rune(Client *c, const Seperator *sep);
void bot_command_send_home(Client *c, const Seperator *sep);
void bot_command_shrink(Client *c, const Seperator *sep);
void bot_command_summon_corpse(Client *c, const Seperator *sep);
void bot_command_taunt(Client *c, const Seperator *sep);
void bot_command_track(Client *c, const Seperator *sep);
void bot_command_water_breathing(Client *c, const Seperator *sep);


// bot subcommands
void bot_subcommand_bot_appearance(Client *c, const Seperator *sep);
void bot_subcommand_bot_beard_color(Client *c, const Seperator *sep);
void bot_subcommand_bot_beard_style(Client *c, const Seperator *sep);
void bot_subcommand_bot_camp(Client *c, const Seperator *sep);
void bot_subcommand_bot_clone(Client *c, const Seperator *sep);
void bot_subcommand_bot_create(Client *c, const Seperator *sep);
void bot_subcommand_bot_delete(Client *c, const Seperator *sep);
void bot_subcommand_bot_details(Client *c, const Seperator *sep);
void bot_subcommand_bot_dye_armor(Client *c, const Seperator *sep);
void bot_subcommand_bot_eyes(Client *c, const Seperator *sep);
void bot_subcommand_bot_face(Client *c, const Seperator *sep);
void bot_subcommand_bot_hair_color(Client *c, const Seperator *sep);
void bot_subcommand_bot_hairstyle(Client *c, const Seperator *sep);
void bot_subcommand_bot_heritage(Client *c, const Seperator *sep);
void bot_subcommand_bot_inspect_message(Client *c, const Seperator *sep);
void bot_subcommand_bot_list(Client *c, const Seperator *sep);
void bot_subcommand_bot_out_of_combat(Client *c, const Seperator *sep);
void bot_subcommand_bot_report(Client *c, const Seperator *sep);
void bot_subcommand_bot_spawn(Client *c, const Seperator *sep);
void bot_subcommand_bot_stance(Client *c, const Seperator *sep);
void bot_subcommand_bot_summon(Client *c, const Seperator *sep);
void bot_subcommand_bot_tattoo(Client *c, const Seperator *sep);
void bot_subcommand_bot_toggle_archer(Client *c, const Seperator *sep);
void bot_subcommand_bot_toggle_helm(Client *c, const Seperator *sep);
void bot_subcommand_bot_update(Client *c, const Seperator *sep);
void bot_subcommand_botgroup_add(Client *c, const Seperator *sep);
void bot_subcommand_botgroup_attack(Client *c, const Seperator *sep);
void bot_subcommand_botgroup_create(Client *c, const Seperator *sep);
void bot_subcommand_botgroup_delete(Client *c, const Seperator *sep);
void bot_subcommand_botgroup_disband(Client *c, const Seperator *sep);
void bot_subcommand_botgroup_follow(Client *c, const Seperator *sep);
void bot_subcommand_botgroup_guard(Client *c, const Seperator *sep);
void bot_subcommand_botgroup_list(Client *c, const Seperator *sep);
void bot_subcommand_botgroup_load(Client *c, const Seperator *sep);
void bot_subcommand_botgroup_remove(Client *c, const Seperator *sep);
void bot_subcommand_botgroup_save(Client *c, const Seperator *sep);
void bot_subcommand_botgroup_summon(Client *c, const Seperator *sep);
void bot_subcommand_circle(Client *c, const Seperator *sep);
void bot_subcommand_evacuate(Client *c, const Seperator *sep);
void bot_subcommand_group_attack(Client *c, const Seperator *sep);
void bot_subcommand_group_follow(Client *c, const Seperator *sep);
void bot_subcommand_group_guard(Client *c, const Seperator *sep);
void bot_subcommand_group_summon(Client *c, const Seperator *sep);
void bot_subcommand_heal_rotation_add_member(Client *c, const Seperator *sep);
void bot_subcommand_heal_rotation_add_target(Client *c, const Seperator *sep);
void bot_subcommand_heal_rotation_clear_targets(Client *c, const Seperator *sep);
void bot_subcommand_heal_rotation_create(Client *c, const Seperator *sep);
void bot_subcommand_heal_rotation_fast_heals(Client *c, const Seperator *sep);
void bot_subcommand_heal_rotation_list(Client *c, const Seperator *sep);
void bot_subcommand_heal_rotation_remove_member(Client *c, const Seperator *sep);
void bot_subcommand_heal_rotation_remove_target(Client *c, const Seperator *sep);
void bot_subcommand_heal_rotation_start(Client *c, const Seperator *sep);
void bot_subcommand_heal_rotation_stop(Client *c, const Seperator *sep);
void bot_subcommand_inventory_list(Client *c, const Seperator *sep);
void bot_subcommand_inventory_remove(Client *c, const Seperator *sep);
void bot_subcommand_item_augment(Client *c, const Seperator *sep);
void bot_subcommand_item_give(Client *c, const Seperator *sep);
void bot_subcommand_pet_remove(Client *c, const Seperator *sep);
void bot_subcommand_pet_set_type(Client *c, const Seperator *sep);
void bot_subcommand_portal(Client *c, const Seperator *sep);
void bot_subcommand_succor(Client *c, const Seperator *sep);


// bot command helpers
bool helper_bot_appearance_fail(Client *c, Bot *b, BCEnum::AFType fail_type, const char* type_desc);
void helper_bot_appearance_final(Client *c, Bot *b);
void helper_bot_appearance_send_current(Bot *b);
uint32 helper_bot_create(Client *c, std::string bot_name, uint8 bot_class, uint16 bot_race, uint8 bot_gender);
#endif
