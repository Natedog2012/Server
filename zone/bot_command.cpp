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

/*

	To add a new bot command 3 things must be done:

	1.	At the bottom of bot_command.h you must add a prototype for it.
	2.	Add the function in this file.
	3.	In the bot_command_init function you must add a call to bot_command_add
		for your function.
		
	Notes: If you want an alias for your bot command, add an entry to the
	`bot_command_settings` table in your database. The access level you
	set with bot_command_add is the default setting if the bot command isn't
	listed in the `bot_command_settings` db table.

*/

#include <string.h>
#include <stdlib.h>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <thread>

#ifdef _WINDOWS
#define strcasecmp _stricmp
#endif

#include "../common/global_define.h"
#include "../common/eq_packet.h"
#include "../common/features.h"
#include "../common/guilds.h"
#include "../common/patches/patches.h"
#include "../common/ptimer.h"
#include "../common/rulesys.h"
#include "../common/serverinfo.h"
#include "../common/string_util.h"
#include "../common/eqemu_logsys.h"


#include "bot_command.h"
#include "guild_mgr.h"
#include "map.h"
#include "pathing.h"
#include "qglobals.h"
#include "queryserv.h"
#include "quest_parser_collection.h"
#include "string_ids.h"
#include "titles.h"
#include "water_map.h"
#include "worldserver.h"

extern QueryServ* QServ;
extern WorldServer worldserver;
extern TaskManager *taskmanager;
void CatchSignal(int sig_num);


/*
 * file-scope helper objects
 */
#define BCSTSPELLDUMP

enum { EffectIDFirst = 1, EffectIDLast = 12 };

#define CLASSIDTOINDEX(x) ((x >= WARRIOR && x <= BERSERKER) ? (x - 1) : (0))
#define EFFECTIDTOINDEX(x) ((x >= EffectIDFirst && x <= EffectIDLast) ? (x - 1) : (0))
#define AILMENTIDTOINDEX(x) ((x >= BCEnum::AT_Blindness && x <= BCEnum::AT_Corruption) ? (x - 1) : (0))
#define RESISTANCEIDTOINDEX(x) ((x >= BCEnum::RT_Fire && x <= BCEnum::RT_Corruption) ? (x - 1) : (0))


typedef std::list<STBaseEntry*> bcst_list;
typedef std::map<BCEnum::SType, bcst_list> bcst_map;

typedef std::map<BCEnum::SType, std::string> bcst_required_bot_classes_map;
typedef std::map<BCEnum::SType, std::map<uint8, std::string>> bcst_required_bot_classes_map_by_class;

typedef std::map<uint8, uint8> bcst_levels;
typedef std::map<BCEnum::SType, bcst_levels> bcst_levels_map;

typedef std::map<std::string, std::string> strstr_map;

bcst_map bot_command_spells;
bcst_required_bot_classes_map required_bots_map;
bcst_required_bot_classes_map_by_class required_bots_map_by_class;


class BCSpells
{
#define ACTIVE_BOT_SPELL_RANK 0 // 1, 2 or 3 are valid rank filters .. any other value permits all 3 ranks
#define PREFER_NO_MANA_COST_SPELLS true

public:
	static void Load() {
		bot_command_spells.clear();
		bcst_levels_map bot_levels_map;
		
		for (int i = BCEnum::SpellTypeFirst; i <= BCEnum::SpellTypeLast; ++i) {
			bot_command_spells[static_cast<BCEnum::SType>(i)] = {};
			bot_levels_map[static_cast<BCEnum::SType>(i)] = std::map<uint8, uint8>();
		}
		
		for (int spell_id = 2; spell_id < SPDAT_RECORDS; ++spell_id) {
			if (spells[spell_id].player_1[0] == '\0')
				continue;
			if (spells[spell_id].targettype != ST_Target && spells[spell_id].CastRestriction != 0)
				continue;
			
			BCEnum::TType target_type = BCEnum::TT_None;
			switch (spells[spell_id].targettype) {
			case ST_GroupTeleport:
				target_type = BCEnum::TT_GroupV1;
				break;
			case ST_AECaster:
				target_type = BCEnum::TT_AECaster;
				break;
			case ST_AEBard:
				target_type = BCEnum::TT_AEBard;
				break;
			case ST_Target:
				switch (spells[spell_id].CastRestriction) {
				case 0:
					target_type = BCEnum::TT_Single;
					break;
				case 104:
					target_type = BCEnum::TT_Animal;
					break;
				case 105:
					target_type = BCEnum::TT_Plant;
					break;
				case 118:
					target_type = BCEnum::TT_Summoned;
					break;
				case 120:
					target_type = BCEnum::TT_Undead;
					break;
				default:
					break;
				}
				break;
			case ST_AETarget:
				target_type = BCEnum::TT_AETarget;
				break;
			case ST_Animal:
				target_type = BCEnum::TT_Animal;
				break;
			case ST_Undead:
				target_type = BCEnum::TT_Undead;
				break;
			case ST_Summoned:
				target_type = BCEnum::TT_Summoned;
				break;
			case ST_Corpse:
				target_type = BCEnum::TT_Corpse;
				break;
			case ST_Plant:
				target_type = BCEnum::TT_Plant;
				break;
			case ST_Group:
				target_type = BCEnum::TT_GroupV2;
				break;
			default:
				break;
			}
			if (target_type == BCEnum::TT_None)
				continue;
			
			uint8 class_levels[16] = { 0 };
			bool player_spell = false;
			for (int class_type = WARRIOR; class_type <= BERSERKER; ++class_type) {
				int class_index = CLASSIDTOINDEX(class_type);
				if (spells[spell_id].classes[class_index] == 0 || spells[spell_id].classes[class_index] > HARD_LEVEL_CAP)
					continue;

				class_levels[class_index] = spells[spell_id].classes[class_index];
				player_spell = true;
			}
			if (!player_spell)
				continue;
			
			STBaseEntry* entry_prototype = nullptr;
			while (true) {
				int effect_index = EFFECTIDTOINDEX(1);
				switch (spells[spell_id].effectid[effect_index]) {
				case SE_BindAffinity:
					entry_prototype = new STBaseEntry;
					entry_prototype->bcst = BCEnum::ST_BindAffinity;
					break;
				case SE_Charm:
					entry_prototype = new STBaseEntry;
					entry_prototype->bcst = BCEnum::ST_Charm;
					break;
				case SE_Teleport:
					entry_prototype = new STDepartEntry;
					entry_prototype->bcst = BCEnum::ST_Depart;
					break;
				case SE_Succor:
					if (!strcmp(spells[spell_id].name, "same")) {
						entry_prototype = new STEscapeEntry;
						entry_prototype->bcst = BCEnum::ST_Escape;
						std::string is_lesser = spells[spell_id].name;
						if (is_lesser.find("lesser") != std::string::npos)
							static_cast<STEscapeEntry*>(entry_prototype)->lesser = true;
					}
					else {
						entry_prototype = new STDepartEntry;
						entry_prototype->bcst = BCEnum::ST_Depart;
					}
					break;
				case SE_Translocate:
					if (spells[spell_id].name[0] == '\0') {
						entry_prototype = new STBaseEntry;
						entry_prototype->bcst = BCEnum::ST_SendHome;
					}
					else {
						entry_prototype = new STDepartEntry;
						entry_prototype->bcst = BCEnum::ST_Depart;
					}
					break;
					//case SE_TeleporttoAnchor:
					//case SE_TranslocatetoAnchor:
				case SE_ModelSize:
					if (spells[spell_id].base[effect_index] > 100) {
						entry_prototype = new STBaseEntry;
						entry_prototype->bcst = BCEnum::ST_Grow;
					}
					else if (spells[spell_id].base[effect_index] > 0 && spells[spell_id].base[effect_index] < 100) {
						entry_prototype = new STBaseEntry;
						entry_prototype->bcst = BCEnum::ST_Shrink;
					}
					break;
				case SE_Identify:
					entry_prototype = new STBaseEntry;
					entry_prototype->bcst = BCEnum::ST_Identify;
					break;
				case SE_Invisibility:
					entry_prototype = new STInvisibilityEntry;
					entry_prototype->bcst = BCEnum::ST_Invisibility;
					static_cast<STInvisibilityEntry*>(entry_prototype)->invis_type = BCEnum::IT_Living;
					break;
				case SE_SeeInvis:
					entry_prototype = new STInvisibilityEntry;
					entry_prototype->bcst = BCEnum::ST_Invisibility;
					static_cast<STInvisibilityEntry*>(entry_prototype)->invis_type = BCEnum::IT_See;
					break;
				case SE_InvisVsUndead:
					entry_prototype = new STInvisibilityEntry;
					entry_prototype->bcst = BCEnum::ST_Invisibility;
					static_cast<STInvisibilityEntry*>(entry_prototype)->invis_type = BCEnum::IT_Undead;
					break;
				case SE_InvisVsAnimals:
					entry_prototype = new STInvisibilityEntry;
					entry_prototype->bcst = BCEnum::ST_Invisibility;
					static_cast<STInvisibilityEntry*>(entry_prototype)->invis_type = BCEnum::IT_Animal;
					break;
				case SE_Levitate:
					entry_prototype = new STBaseEntry;
					entry_prototype->bcst = BCEnum::ST_Levitation;
					break;
				case SE_Mez:
					entry_prototype = new STBaseEntry;
					entry_prototype->bcst = BCEnum::ST_Mesmerize;
					break;
				case SE_Revive:
					entry_prototype = new STBaseEntry;
					entry_prototype->bcst = BCEnum::ST_Resurrect;
					break;
				case SE_Rune:
					entry_prototype = new STBaseEntry;
					entry_prototype->bcst = BCEnum::ST_Rune;
					break;
				case SE_SummonCorpse:
					entry_prototype = new STBaseEntry;
					entry_prototype->bcst = BCEnum::ST_SummonCorpse;
					break;
				case SE_WaterBreathing:
					entry_prototype = new STBaseEntry;
					entry_prototype->bcst = BCEnum::ST_WaterBreathing;
					break;
				default:
					break;
				}
				if (entry_prototype)
					break;
				
				effect_index = EFFECTIDTOINDEX(2);
				switch (spells[spell_id].effectid[effect_index]) {
				case SE_MovementSpeed:
					entry_prototype = new STBaseEntry;
					entry_prototype->bcst = BCEnum::ST_MovementSpeed;
					break;
				default:
					break;
				}
				if (entry_prototype)
					break;
				
				effect_index = EFFECTIDTOINDEX(3);
				switch (spells[spell_id].effectid[effect_index]) {
				case SE_Lull:
					entry_prototype = new STBaseEntry;
					entry_prototype->bcst = BCEnum::ST_Lull;
					break;
				default:
					break;
				}
				if (entry_prototype)
					break;
				
				switch (spells[spell_id].descnum) {
				case 27: // by spell id - temporary
					switch (spell_id) {
					case 4499:
					case 4503:
					case 4688:
					case 6663:
					case 6673:
					case 6731:
					case 6741:
					case 7004:
					case 7005:
					case 10965:
					case 11854:
					case 11866:
						entry_prototype = new STBaseEntry;
						entry_prototype->bcst = BCEnum::ST_Defensive;
					default:
						break;
					}
					break;
				default:
					break;
				}
				if (entry_prototype)
					break;
				
				switch (spells[spell_id].SpellAffectIndex) {
				case 1:
					effect_index = EFFECTIDTOINDEX(1);
					switch (spells[spell_id].effectid[effect_index]) {
					case SE_Blind:
					case SE_DiseaseCounter:
					case SE_PoisonCounter:
					case SE_CurseCounter:
					case SE_CorruptionCounter: {
						entry_prototype = new STCureEntry;
						entry_prototype->bcst = BCEnum::ST_Cure;

						bool valid_spell = false;
						for (int i = EffectIDFirst; i <= EffectIDLast; ++i) {
							effect_index = EFFECTIDTOINDEX(i);
							if (spells[spell_id].base[effect_index] >= 0)
								continue;
							
							switch (spells[spell_id].effectid[effect_index]) {
							case SE_Blind:
								static_cast<STCureEntry*>(entry_prototype)->cure_mask |= BCEnum::AM_Blindness;
								static_cast<STCureEntry*>(entry_prototype)->cure_value[AILMENTIDTOINDEX(BCEnum::AT_Blindness)] += spells[spell_id].base[effect_index];
								break;
							case SE_DiseaseCounter:
								static_cast<STCureEntry*>(entry_prototype)->cure_mask |= BCEnum::AM_Disease;
								static_cast<STCureEntry*>(entry_prototype)->cure_value[AILMENTIDTOINDEX(BCEnum::AT_Disease)] += spells[spell_id].base[effect_index];
								break;
							case SE_PoisonCounter:
								static_cast<STCureEntry*>(entry_prototype)->cure_mask |= BCEnum::AM_Poison;
								static_cast<STCureEntry*>(entry_prototype)->cure_value[AILMENTIDTOINDEX(BCEnum::AT_Poison)] += spells[spell_id].base[effect_index];
								break;
							case SE_CurseCounter:
								static_cast<STCureEntry*>(entry_prototype)->cure_mask |= BCEnum::AM_Curse;
								static_cast<STCureEntry*>(entry_prototype)->cure_value[AILMENTIDTOINDEX(BCEnum::AT_Curse)] += spells[spell_id].base[effect_index];
								break;
							case SE_CorruptionCounter:
								static_cast<STCureEntry*>(entry_prototype)->cure_mask |= BCEnum::AM_Corruption;
								static_cast<STCureEntry*>(entry_prototype)->cure_value[AILMENTIDTOINDEX(BCEnum::AT_Corruption)] += spells[spell_id].base[effect_index];
								break;
							default:
								continue;
							}

							valid_spell = true;
						}
						if (!valid_spell) {
							safe_delete(entry_prototype);
							entry_prototype = nullptr;
						}
						break;
					}
					default:
						break;
					}
					break;
				case 2:
					effect_index = EFFECTIDTOINDEX(1);
					switch (spells[spell_id].effectid[effect_index]) {
					case SE_ResistFire:
					case SE_ResistCold:
					case SE_ResistPoison:
					case SE_ResistDisease:
					case SE_ResistMagic:
					case SE_ResistCorruption:{
						entry_prototype = new STResistanceEntry;
						entry_prototype->bcst = BCEnum::ST_Resistance;

						bool valid_spell = false;
						for (int i = EffectIDFirst; i <= EffectIDLast; ++i) {
							effect_index = EFFECTIDTOINDEX(i);
							if (spells[spell_id].base[effect_index] >= 0)
								continue;

							switch (spells[spell_id].effectid[effect_index]) {
							case SE_ResistFire:
								static_cast<STResistanceEntry*>(entry_prototype)->resist_mask |= BCEnum::RM_Fire;
								static_cast<STResistanceEntry*>(entry_prototype)->resist_value[RESISTANCEIDTOINDEX(BCEnum::RT_Fire)] += spells[spell_id].base[effect_index];
								break;
							case SE_ResistCold:
								static_cast<STResistanceEntry*>(entry_prototype)->resist_mask |= BCEnum::RM_Cold;
								static_cast<STResistanceEntry*>(entry_prototype)->resist_value[RESISTANCEIDTOINDEX(BCEnum::RT_Cold)] += spells[spell_id].base[effect_index];
								break;
							case SE_ResistPoison:
								static_cast<STResistanceEntry*>(entry_prototype)->resist_mask |= BCEnum::RM_Poison;
								static_cast<STResistanceEntry*>(entry_prototype)->resist_value[RESISTANCEIDTOINDEX(BCEnum::RT_Poison)] += spells[spell_id].base[effect_index];
								break;
							case SE_ResistDisease:
								static_cast<STResistanceEntry*>(entry_prototype)->resist_mask |= BCEnum::RM_Disease;
								static_cast<STResistanceEntry*>(entry_prototype)->resist_value[RESISTANCEIDTOINDEX(BCEnum::RT_Disease)] += spells[spell_id].base[effect_index];
								break;
							case SE_ResistMagic:
								static_cast<STResistanceEntry*>(entry_prototype)->resist_mask |= BCEnum::RM_Magic;
								static_cast<STResistanceEntry*>(entry_prototype)->resist_value[RESISTANCEIDTOINDEX(BCEnum::RT_Magic)] += spells[spell_id].base[effect_index];
								break;
							case SE_ResistCorruption:
								static_cast<STResistanceEntry*>(entry_prototype)->resist_mask |= BCEnum::RM_Corruption;
								static_cast<STResistanceEntry*>(entry_prototype)->resist_value[RESISTANCEIDTOINDEX(BCEnum::RT_Corruption)] += spells[spell_id].base[effect_index];
								break;
							default:
								continue;
							}

							valid_spell = true;
						}
						if (!valid_spell) {
							safe_delete(entry_prototype);
							entry_prototype = nullptr;
						}
						break;
					}
					default:
						break;
					}
					break;
				default:
					break;
				}
				if (entry_prototype)
					break;

				break;
			}
			if (!entry_prototype)
				continue;
			
			assert(entry_prototype->bcst != BCEnum::ST_None);

			entry_prototype->spell_id = spell_id;
			entry_prototype->target_type = target_type;

			bcst_levels& bot_levels = bot_levels_map[entry_prototype->bcst];
			for (int class_type = WARRIOR; class_type <= BERSERKER; ++class_type) {
				int class_index = CLASSIDTOINDEX(class_type);
				if (!class_levels[class_index])
					continue;

				STBaseEntry* spell_entry = nullptr;
				switch (entry_prototype->bcst) {
				case BCEnum::ST_Cure:
					spell_entry = new STCureEntry(static_cast<STCureEntry*>(entry_prototype));
					break;
				case BCEnum::ST_Depart:
					spell_entry = new STDepartEntry(static_cast<STDepartEntry*>(entry_prototype));
					break;
				case BCEnum::ST_Escape:
					spell_entry = new STEscapeEntry(static_cast<STEscapeEntry*>(entry_prototype));
					break;
				case BCEnum::ST_Invisibility:
					spell_entry = new STInvisibilityEntry(static_cast<STInvisibilityEntry*>(entry_prototype));
					break;
				case BCEnum::ST_Resistance:
					spell_entry = new STResistanceEntry(static_cast<STResistanceEntry*>(entry_prototype));
					break;
				default:
					spell_entry = new STBaseEntry(entry_prototype);
					break;
				}
				
				assert(spell_entry);

				spell_entry->caster_class = class_type;
				spell_entry->spell_level = class_levels[class_index];
				
				bot_command_spells[spell_entry->bcst].push_back(spell_entry);

				if (bot_levels.find(class_type) == bot_levels.end() || bot_levels[class_type] > class_levels[class_index])
					bot_levels[class_type] = class_levels[class_index];
			}

			delete(entry_prototype);
		}
		
		remove_inactive();
		order_all();
		load_teleport_zone_names();
		build_strings(bot_levels_map);
		status_report();

#ifdef BCSTSPELLDUMP
		spell_dump();
#endif

	}

	static void Unload() {
		for (bcst_map::iterator map_iter = bot_command_spells.begin(); map_iter != bot_command_spells.end(); ++map_iter) {
			if (map_iter->second.empty())
				continue;
			for (bcst_list::iterator list_iter = map_iter->second.begin(); list_iter != map_iter->second.end(); ++list_iter)
				safe_delete(*list_iter);
			map_iter->second.clear();
		}
		bot_command_spells.clear();
		required_bots_map.clear();
		required_bots_map_by_class.clear();
	}


private:
	static void build_strings(bcst_levels_map& bot_levels_map) {
		for (int i = BCEnum::SpellTypeFirst; i <= BCEnum::SpellTypeLast; ++i)
			build_required_bots_string(static_cast<BCEnum::SType>(i), bot_levels_map[static_cast<BCEnum::SType>(i)]);
	}

	static void order_all() {
		// 'Ascending' list sorting predicates
		const auto asc_base_id1 = [](const STBaseEntry* l, const STBaseEntry* r) { return (spells[l->spell_id].base[EFFECTIDTOINDEX(1)] < spells[r->spell_id].base[EFFECTIDTOINDEX(1)]); };
		const auto asc_base_id2 = [](const STBaseEntry* l, const STBaseEntry* r) { return (spells[l->spell_id].base[EFFECTIDTOINDEX(2)] < spells[r->spell_id].base[EFFECTIDTOINDEX(2)]); };
		const auto asc_caster_class = [](const STBaseEntry* l, const STBaseEntry* r) { return (l->caster_class < r->caster_class); };
		const auto asc_cure_mask = [](const STBaseEntry* l, const STBaseEntry* r)
		{ return ((l->IsCure() && r->IsCure()) ? (static_cast<const STCureEntry*>(l)->cure_mask < static_cast<const STCureEntry*>(r)->cure_mask) : true); };
		const auto asc_invis_type = [](const STBaseEntry* l, const STBaseEntry* r)
		{ return ((l->IsInvisibility() && r->IsInvisibility()) ? (static_cast<const STInvisibilityEntry*>(l)->invis_type < static_cast<const STInvisibilityEntry*>(r)->invis_type) : true); };
		const auto asc_mana = [](const STBaseEntry* l, const STBaseEntry* r) { return (spells[l->spell_id].mana < spells[r->spell_id].mana); };
		const auto asc_mana_boolean = [](const STBaseEntry* l, const STBaseEntry* r) {return (spells[l->spell_id].mana != 0 && spells[r->spell_id].mana == 0); };
		const auto asc_max_id1 = [](const STBaseEntry* l, const STBaseEntry* r) { return (spells[l->spell_id].max[EFFECTIDTOINDEX(1)] < spells[r->spell_id].max[EFFECTIDTOINDEX(1)]); };
		const auto asc_spell_name = [](const STBaseEntry* l, const STBaseEntry* r) { return (strcasecmp(spells[l->spell_id].name, spells[r->spell_id].name) < 0); };
		const auto asc_resist_diff = [](const STBaseEntry* l, const STBaseEntry* r) { return (spells[l->spell_id].ResistDiff < spells[r->spell_id].ResistDiff); };
		const auto asc_resist_mask = [](const STBaseEntry* l, const STBaseEntry* r)
		{ return ((l->IsResistance() && r->IsResistance()) ? (static_cast<const STResistanceEntry*>(l)->resist_mask < static_cast<const STResistanceEntry*>(r)->resist_mask) : true); };
		const auto asc_spell_id = [](const STBaseEntry* l, const STBaseEntry* r) { return (l->spell_id < r->spell_id); };
		const auto asc_spell_level = [](const STBaseEntry* l, const STBaseEntry* r) { return (l->spell_level < r->spell_level); };
		const auto asc_target_type = [](const STBaseEntry* l, const STBaseEntry* r) { return (l->target_type < r->target_type); };
		const auto asc_zone_type = [](const STBaseEntry* l, const STBaseEntry* r) { return (spells[l->spell_id].zonetype < spells[r->spell_id].zonetype); };

		// 'Descending' list sorting predicates
		const auto dsc_base_id1 = [](const STBaseEntry* l, const STBaseEntry* r) { return (spells[l->spell_id].base[EFFECTIDTOINDEX(1)] > spells[r->spell_id].base[EFFECTIDTOINDEX(1)]); };
		const auto dsc_base_id2 = [](const STBaseEntry* l, const STBaseEntry* r) { return (spells[l->spell_id].base[EFFECTIDTOINDEX(2)] > spells[r->spell_id].base[EFFECTIDTOINDEX(2)]); };
		const auto dsc_caster_class = [](const STBaseEntry* l, const STBaseEntry* r) { return (l->caster_class > r->caster_class); };
		const auto dsc_cure_mask = [](const STBaseEntry* l, const STBaseEntry* r)
		{ return ((l->IsCure() && r->IsCure()) ? (static_cast<const STCureEntry*>(l)->cure_mask > static_cast<const STCureEntry*>(r)->cure_mask) : true); };
		const auto dsc_invis_type = [](const STBaseEntry* l, const STBaseEntry* r)
		{ return ((l->IsInvisibility() && r->IsInvisibility()) ? (static_cast<const STInvisibilityEntry*>(l)->invis_type > static_cast<const STInvisibilityEntry*>(r)->invis_type) : true); };
		const auto dsc_mana = [](const STBaseEntry* l, const STBaseEntry* r) { return (spells[l->spell_id].mana > spells[r->spell_id].mana); };
		const auto dsc_mana_boolean = [](const STBaseEntry* l, const STBaseEntry* r) {return (spells[l->spell_id].mana == 0 && spells[r->spell_id].mana != 0); };
		const auto dsc_max_id1 = [](const STBaseEntry* l, const STBaseEntry* r) { return (spells[l->spell_id].max[EFFECTIDTOINDEX(1)] > spells[r->spell_id].max[EFFECTIDTOINDEX(1)]); };
		const auto dsc_spell_name = [](const STBaseEntry* l, const STBaseEntry* r) { return (strcasecmp(spells[l->spell_id].name, spells[r->spell_id].name) > 0); };
		const auto dsc_resist_diff = [](const STBaseEntry* l, const STBaseEntry* r) { return (spells[l->spell_id].ResistDiff > spells[r->spell_id].ResistDiff); };
		const auto dsc_resist_mask = [](const STBaseEntry* l, const STBaseEntry* r)
		{ return ((l->IsResistance() && r->IsResistance()) ? (static_cast<const STResistanceEntry*>(l)->resist_mask > static_cast<const STResistanceEntry*>(r)->resist_mask) : true); };
		const auto dsc_spell_id = [](const STBaseEntry* l, const STBaseEntry* r) { return (l->spell_id > r->spell_id); };
		const auto dsc_spell_level = [](const STBaseEntry* l, const STBaseEntry* r) { return (l->spell_level > r->spell_level); };
		const auto dsc_target_type = [](const STBaseEntry* l, const STBaseEntry* r) { return (l->target_type > r->target_type); };
		const auto dsc_zone_type = [](const STBaseEntry* l, const STBaseEntry* r) { return (spells[l->spell_id].zonetype > spells[r->spell_id].zonetype); };


		for (int i = BCEnum::SpellTypeFirst; i <= BCEnum::SpellTypeLast; ++i) {
			BCEnum::SType spell_type = static_cast<BCEnum::SType>(i);
			if (bot_command_spells[spell_type].size() < 2)
				continue;

			switch (spell_type) {
			case BCEnum::ST_BindAffinity:
				// ORDER BY `mana` != '0', spell_level, caster_class
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(asc_spell_level);
				bot_command_spells[spell_type].sort(asc_target_type);
				bot_command_spells[spell_type].sort(dsc_mana_boolean);
				continue;
			case BCEnum::ST_Charm:
				// 
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(asc_spell_level);
				bot_command_spells[spell_type].sort(dsc_max_id1);
				bot_command_spells[spell_type].sort(asc_target_type);
				bot_command_spells[spell_type].sort(dsc_resist_diff);
				continue;
			case BCEnum::ST_Cure:
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(dsc_cure_mask);
				bot_command_spells[spell_type].sort(dsc_spell_level);
				bot_command_spells[spell_type].sort(asc_target_type);
				continue;
			case BCEnum::ST_Defensive:
				// ??
				continue;
			case BCEnum::ST_Depart:
				bot_command_spells[spell_type].sort(asc_spell_name);
				bot_command_spells[spell_type].sort(asc_spell_level);
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(asc_target_type);
				continue;
			case BCEnum::ST_Escape:
				bot_command_spells[spell_type].sort(asc_spell_name);
				bot_command_spells[spell_type].sort(asc_spell_level);
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(asc_target_type);
				continue;
			case BCEnum::ST_Grow:
				bot_command_spells[spell_type].sort(asc_spell_name);
				bot_command_spells[spell_type].sort(asc_spell_level);
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(asc_target_type);
				continue;
			case BCEnum::ST_Identify:
				// ??
				continue;
			case BCEnum::ST_Invisibility:
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(asc_spell_name);
				bot_command_spells[spell_type].sort(dsc_spell_level);
				bot_command_spells[spell_type].sort(asc_zone_type);
				bot_command_spells[spell_type].sort(asc_target_type);
				bot_command_spells[spell_type].sort(asc_invis_type);
				continue;
			case BCEnum::ST_Levitation:
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(asc_spell_name);
				bot_command_spells[spell_type].sort(dsc_spell_level);
				bot_command_spells[spell_type].sort(asc_zone_type);
				bot_command_spells[spell_type].sort(asc_target_type);
				continue;
			case BCEnum::ST_Lull:
				bot_command_spells[spell_type].sort(asc_spell_name);
				bot_command_spells[spell_type].sort(asc_spell_level);
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(asc_zone_type);
				bot_command_spells[spell_type].sort(asc_target_type);
				continue;
			case BCEnum::ST_Mesmerize:
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(dsc_spell_level);
				bot_command_spells[spell_type].sort(dsc_max_id1);
				bot_command_spells[spell_type].sort(asc_target_type);
				bot_command_spells[spell_type].sort(dsc_resist_diff);
				continue;
			case BCEnum::ST_MovementSpeed:
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(asc_spell_level);
				bot_command_spells[spell_type].sort(dsc_base_id2);
				bot_command_spells[spell_type].sort(asc_target_type);
				continue;
			case BCEnum::ST_Resistance:
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(dsc_resist_mask);
				bot_command_spells[spell_type].sort(dsc_spell_level);
				bot_command_spells[spell_type].sort(asc_target_type);
				continue;
			case BCEnum::ST_Resurrect:
				bot_command_spells[spell_type].sort(asc_spell_name);
				bot_command_spells[spell_type].sort(asc_spell_id);
				bot_command_spells[spell_type].sort(asc_spell_level);
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(asc_zone_type);
				bot_command_spells[spell_type].sort(asc_target_type);
				bot_command_spells[spell_type].sort(dsc_base_id1);
				continue;
			case BCEnum::ST_Rune:
				bot_command_spells[spell_type].sort(dsc_max_id1);
				bot_command_spells[spell_type].sort(asc_spell_level);
				bot_command_spells[spell_type].sort(asc_target_type);
				continue;
			case BCEnum::ST_SendHome:
				// ??
				continue;
			case BCEnum::ST_Shrink:
				bot_command_spells[spell_type].sort(asc_spell_name);
				bot_command_spells[spell_type].sort(asc_spell_level);
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(asc_target_type);
				continue;
			case BCEnum::ST_SummonCorpse:
				// ??
				continue;
			case BCEnum::ST_WaterBreathing:
				bot_command_spells[spell_type].sort(asc_caster_class);
				bot_command_spells[spell_type].sort(asc_spell_name);
				bot_command_spells[spell_type].sort(asc_spell_level);
				bot_command_spells[spell_type].sort(asc_target_type);
				continue;
			default:
				continue;
			}
		}
	}

	static void load_teleport_zone_names() {
		bcst_list& depart_list = bot_command_spells[BCEnum::ST_Depart];
		if (depart_list.empty())
			return;

		std::string query = "SELECT `short_name`, `long_name` FROM `zone` WHERE '' NOT IN (`short_name`, `long_name`)";
		auto results = database.QueryDatabase(query);
		if (!results.Success()) {
			Log.Out(Logs::General, Logs::Error, "load_teleport_zone_names() - Error in zone names query: %s", results.ErrorMessage().c_str());
			return;
		}

		strstr_map zone_names;
		for (auto row = results.begin(); row != results.end(); ++row)
			zone_names[row[0]] = row[1];

		for (bcst_list::iterator list_iter = depart_list.begin(); list_iter != depart_list.end();) {
			strstr_map::iterator test_iter = zone_names.find(spells[(*list_iter)->spell_id].teleport_zone);
			if (test_iter == zone_names.end()) {
				list_iter = depart_list.erase(list_iter);
				continue;
			}

			static_cast<STDepartEntry*>(*list_iter)->long_name = test_iter->second;
			++list_iter;
		}

	}

	static void remove_inactive() {
		if (bot_command_spells.empty())
			return;

#if(ACTIVE_BOT_SPELL_RANK == 1)
		for (bcst_map::iterator iter_map = bot_command_spells.begin(); iter_map != bot_command_spells.end(); ++iter_map) {
			if (iter_map->second.empty())
				continue;
			for (bcst_list::iterator list_iter = iter_map->second.begin(); list_iter != iter_map->second.end();) {
				STBaseEntry* spell_entry = *list_iter;
				if (!spell_entry) {
					list_iter = iter_map->second.erase(list_iter);
					continue;
				}
				std::string spell_name = spells[spell_entry->spell_id].name;
				size_t str_pos = spell_name.length();
				// remove short name and rank II/III spells
				if (str_pos < 3 || (spell_name[--str_pos] == 'I' && spell_name[--str_pos] == 'I')) {
					safe_delete(*list_iter);
					list_iter = iter_map->second.erase(list_iter);
					continue;
				}
				++list_iter;
			}
		}
#elif(ACTIVE_BOT_SPELL_RANK == 2)
		for (bcst_map::iterator iter_map = bot_command_spells.begin(); iter_map != bot_command_spells.end(); ++iter_map) {
			if (iter_map->second.empty())
				continue;
			std::list<std::string> remove_list;
			size_t str_pos = std::string::npos;
			for (bcst_list::iterator list_iter = iter_map->second.begin(); list_iter != iter_map->second.end();) {
				STBaseEntry* spell_entry = *list_iter;
				if (!spell_entry) {
					list_iter = iter_map->second.erase(list_iter);
					continue;
				}
				std::string spell_name = spells[spell_entry->spell_id].name;
				str_pos = spell_name.length();
				// remove short name spells
				if (str_pos < 3) {
					safe_delete(*list_iter);
					list_iter = iter_map->second.erase(list_iter);
					continue;
				}
				if (spell_name[--str_pos] == 'I' && spell_name[--str_pos] == 'I') {
					// remove rank III spells
					if (spell_name[--str_pos] == 'I') {
						safe_delete(*list_iter);
						list_iter = iter_map->second.erase(list_iter);
						continue;
					}
					if ((str_pos = spell_name.find_last_of("rk. II")) != std::string::npos)
						remove_list.push_back(spell_name.substr(0, str_pos));
					else if ((str_pos = spell_name.find_last_of("rk.II")) != std::string::npos)
						remove_list.push_back(spell_name.substr(0, str_pos));
					else
						remove_list.push_back(spell_name.substr(0, spell_name.length() - 2));
					str_pos = remove_list.back().length();
					while (remove_list.back().length() > 0 && remove_list.back().at(--str_pos) == ' ')
						remove_list.back().erase(str_pos);
					if (remove_list.back().empty())
						remove_list.pop_back();
				}
				++list_iter;
			}
			if (remove_list.empty())
				continue;
			for (bcst_list::iterator list_iter = iter_map->second.begin(); list_iter != iter_map->second.end();) {
				bool entry_removed = false;
				for (std::list<std::string>::iterator remove_iter = remove_list.begin(); remove_iter != remove_list.end();) {
					if (!strcasecmp(spells[(*list_iter)->spell_id].name, (*remove_iter).c_str())) {
						safe_delete(*list_iter);
						list_iter = iter_map->second.erase(list_iter);
						remove_iter = remove_list.end();
						entry_removed = true;
						continue;
					}
					++remove_iter;
				}
				if (entry_removed || list_iter == iter_map->second.end())
					continue;
				++list_iter;
			}
		}
#elif(ACTIVE_BOT_SPELL_RANK == 3)
		for (bcst_map::iterator iter_map = bot_command_spells.begin(); iter_map != bot_command_spells.end(); ++iter_map) {
			if (iter_map->second.empty())
				continue;
			std::list<std::string> remove_list;
			size_t str_pos = std::string::npos;
			for (bcst_list::iterator list_iter = iter_map->second.begin(); list_iter != iter_map->second.end();) {
				STBaseEntry* spell_entry = *list_iter;
				if (!spell_entry) {
					list_iter = iter_map->second.erase(list_iter);
					continue;
				}
				std::string spell_name = spells[spell_entry->spell_id].name;
				str_pos = spell_name.length();
				// remove short name spells
				if (str_pos < 3) {
					safe_delete(*list_iter);
					list_iter = iter_map->second.erase(list_iter);
					continue;
				}
				if (spell_name[--str_pos] == 'I' && spell_name[--str_pos] == 'I') {
					// remove rank II spells
					if (spell_name[--str_pos] != 'I') {
						safe_delete(*list_iter);
						list_iter = iter_map->second.erase(list_iter);
						continue;
					}
					if ((str_pos = spell_name.find_last_of("rk. III")) != std::string::npos)
						remove_list.push_back(spell_name.substr(0, str_pos));
					else if ((str_pos = spell_name.find_last_of("rk.III")) != std::string::npos)
						remove_list.push_back(spell_name.substr(0, str_pos));
					else
						remove_list.push_back(spell_name.substr(0, spell_name.length() - 3));
					str_pos = remove_list.back().length();
					while (remove_list.back().length() > 0 && remove_list.back().at(--str_pos) == ' ')
						remove_list.back().erase(str_pos);
					if (remove_list.back().empty())
						remove_list.pop_back();
				}
				++list_iter;
			}
			if (remove_list.empty())
				continue;
			for (bcst_list::iterator list_iter = iter_map->second.begin(); list_iter != iter_map->second.end();) {
				bool entry_removed = false;
				for (std::list<std::string>::iterator remove_iter = remove_list.begin(); remove_iter != remove_list.end();) {
					if (!strcasecmp(spells[(*list_iter)->spell_id].name, (*remove_iter).c_str())) {
						safe_delete(*list_iter);
						list_iter = iter_map->second.erase(list_iter);
						remove_iter = remove_list.end();
						entry_removed = true;
						continue;
					}
					++remove_iter;
				}
				if (entry_removed || list_iter == iter_map->second.end())
					continue;
				++list_iter;
			}
		}
#endif
	}

	static void status_report() {
		Log.Out(Logs::General, Logs::Commands, "load_bot_command_spells(): - 'ACTIVE_BOT_SPELL_RANK' set to %i.", ACTIVE_BOT_SPELL_RANK);
		if (bot_command_spells.empty()) {
			Log.Out(Logs::General, Logs::Error, "load_bot_command_spells() - 'bot_command_spells' is empty.");
			return;
		}

		for (int i = BCEnum::SpellTypeFirst; i <= BCEnum::SpellTypeLast; ++i)
			Log.Out(Logs::General, Logs::Commands, "load_bot_command_spells(): - '%s' returned %u spells.",
			BCEnum::SpellTypeEnumToString(static_cast<BCEnum::SType>(i)).c_str(), bot_command_spells[static_cast<BCEnum::SType>(i)].size());
	}

	static void build_required_bots_string(BCEnum::SType type_index, bcst_levels& bot_levels) {
		for (int i = WARRIOR; i <= BERSERKER; ++i)
			required_bots_map_by_class[type_index][i] = "Unavailable...";

		if (bot_levels.empty()) {
			required_bots_map[type_index] = "This command is currently unavailable...";
			return;
		}

		required_bots_map[type_index] = "";

		size_t map_size = bot_levels.size();
		while (bot_levels.size()) {
			bcst_levels::iterator test_iter = bot_levels.begin();
			for (bcst_levels::iterator levels_iter = bot_levels.begin(); levels_iter != bot_levels.end(); ++levels_iter) {
				if (levels_iter->second < test_iter->second)
					test_iter = levels_iter;
				if (strcasecmp(Bot::ClassIdToString(levels_iter->first).c_str(), Bot::ClassIdToString(test_iter->first).c_str()) < 0 && levels_iter->second <= test_iter->second)
					test_iter = levels_iter;
			}

			std::string bot_segment;
			if (bot_levels.size() == map_size)
				bot_segment = "%s(%u)";
			else if (bot_levels.size() > 1)
				bot_segment = ", %s(%u)";
			else
				bot_segment = " or %s(%u)";

			required_bots_map[type_index].append(StringFormat(bot_segment.c_str(), Bot::ClassIdToString(test_iter->first).c_str(), test_iter->second));
			required_bots_map_by_class[type_index][test_iter->first] = StringFormat("%s(%u)", Bot::ClassIdToString(test_iter->first).c_str(), test_iter->second);
			bot_levels.erase(test_iter);
		}
	}

#ifdef BCSTSPELLDUMP
	static void spell_dump() {
		std::ofstream spell_dump;
		spell_dump.open(StringFormat("bcs_dump/spell_dump_%i.txt", getpid()), std::ios_base::app | std::ios_base::out);

		if (bot_command_spells.empty()) {
			spell_dump << "BCSpells::spell_dump() - 'bot_command_spells' map is empty.\n";
			spell_dump.close();
			return;
		}

		int entry_count = 0;
		for (int i = BCEnum::SpellTypeFirst; i <= BCEnum::SpellTypeLast; ++i) {
			spell_dump << StringFormat("BCSpells::spell_dump(): - '%s' returned %u spells:\n",
				BCEnum::SpellTypeEnumToString(static_cast<BCEnum::SType>(i)).c_str(), bot_command_spells[static_cast<BCEnum::SType>(i)].size());
			
			bcst_list& map_entry = bot_command_spells[static_cast<BCEnum::SType>(i)];
			for (bcst_list::iterator list_iter = map_entry.begin(); list_iter != map_entry.end(); ++list_iter) {
				STBaseEntry* list_entry = *list_iter;
				int spell_id = list_entry->spell_id;
				spell_dump << StringFormat("*(id)%05i (name)%12s (ttype)%06s (cls)%02s (lvl)%03u",
					spell_id, spells[spell_id].name, BCEnum::TargetTypeEnumToString(list_entry->target_type).c_str(), Bot::ClassIdToString(list_entry->caster_class).c_str(), list_entry->spell_level);

				switch (list_entry->bcst) {
				case BCEnum::ST_Cure:
					spell_dump << StringFormat(" ([d]curemask)%05u", static_cast<STCureEntry*>(list_entry)->cure_mask);
					break;
				case BCEnum::ST_Depart:
					spell_dump << StringFormat(" ([d]longname)%12s", static_cast<STDepartEntry*>(list_entry)->long_name.c_str());
					break;
				case BCEnum::ST_Escape:
					spell_dump << StringFormat(" ([d]lesser)%05s", ((static_cast<STEscapeEntry*>(list_entry)->lesser) ? ("true") : ("false")));
					break;
				case BCEnum::ST_Invisibility:
					spell_dump << StringFormat(" ([d]invistype)%02i", static_cast<STInvisibilityEntry*>(list_entry)->invis_type);
					break;
				case BCEnum::ST_Resistance:
					spell_dump << StringFormat(" ([d]resistmask)%05u", static_cast<STResistanceEntry*>(list_entry)->resist_mask);
					break;
				default:
					break;
				}

				spell_dump << StringFormat(" (mana)%05u (RD)%06i (ztype)%03i (desc#)%05i (SAI)%03u",
					spells[spell_id].mana, spells[spell_id].ResistDiff, spells[spell_id].zonetype, spells[spell_id].descnum, spells[spell_id].SpellAffectIndex);

				for (int i = EffectIDFirst; i <= 3/*EffectIDLast*/; ++i) {
					int effect_index = EFFECTIDTOINDEX(i);
					spell_dump << StringFormat(" (eff%02i)%04i (base%02i)%06i (max%02i)%06i",
						i, spells[spell_id].effectid[effect_index], i, spells[spell_id].base[effect_index], i, spells[spell_id].max[effect_index]);
				}

				spell_dump << "\n";
				++entry_count;
			}
			
			spell_dump << StringFormat("required_bots_map[%s] = \"%s\"\n",
				BCEnum::SpellTypeEnumToString(static_cast<BCEnum::SType>(i)).c_str(), required_bots_map[static_cast<BCEnum::SType>(i)].c_str());

			spell_dump << "\n";
		}

		spell_dump << StringFormat("Total bcs entry count: %i\n", entry_count);
		spell_dump.close();
	}
#endif
};


//struct bcl_struct *bot_command_list;	// the actual linked list of bot commands
int bot_command_count;					// how many bot commands we have

// this is the pointer to the dispatch function, updated once
// init has been performed to point at the real function
int (*bot_command_dispatch)(Client *,char const *) = bot_command_not_avail;

// TODO: Find out what these are for...
void bot_command_bestz(Client *c, const Seperator *message);
void bot_command_pf(Client *c, const Seperator *message);

std::map<std::string, BotCommandRecord *> bot_command_list;
strstr_map bot_command_aliases;

// All allocated BotCommandRecords get put in here so they get deleted on shutdown
LinkedList<BotCommandRecord *> cleanup_bot_command_list;


/*
 * bot_command_not_avail
 * This is the default dispatch function when commands aren't loaded.
 *
 * Parameters:
 *	not used
 *
 */
int bot_command_not_avail(Client *c, const char *message)
{
	c->Message(13, "Bot commands not available.");
	return -1;
}


/**************************************************************************
/* the rest below here could be in a dynamically loaded module eventually *
/*************************************************************************/

/*

Access Levels:

0		Normal
10	* Steward *
20	* Apprentice Guide *
50	* Guide *
80	* QuestTroupe *
81	* Senior Guide *
85	* GM-Tester *
90	* EQ Support *
95	* GM-Staff *
100	* GM-Admin *
150	* GM-Lead Admin *
160	* QuestMaster *
170	* GM-Areas *
180	* GM-Coder *
200	* GM-Mgmt *
250	* GM-Impossible *

*/

/*
 * bot_command_init
 * initializes the bot command list, call at startup
 *
 * Parameters:
 *	none
 *
 * When adding a new bot command, only hard-code 'real' bot commands -
 * all command aliases are added later through a database call
 *
 */
int bot_command_init(void)
{
	bot_command_aliases.clear();

	if (
		bot_command_add("bindaffinity", "Orders a bot to attempt an affinity binding", 0, bot_command_bind_affinity) ||
		bot_command_add("bot", "Lists the available bot management [subcommands]", 0, bot_command_bot) ||
		bot_command_add("botappearance", "Lists the available bot appearance [subcommands]", 0, bot_subcommand_bot_appearance) ||
		bot_command_add("botbeardcolor", "Changes the beard color of a bot", 0, bot_subcommand_bot_beard_color) ||
		bot_command_add("botbeardstyle", "Changes the beard style of a bot", 0, bot_subcommand_bot_beard_style) ||
		bot_command_add("botcamp", "Orders a bot(s) to camp", 0, bot_subcommand_bot_camp) ||
		bot_command_add("botclone", "Creates a copy of a bot", 200, bot_subcommand_bot_clone) ||
		bot_command_add("botcreate", "Creates a new bot", 0, bot_subcommand_bot_create) ||
		bot_command_add("botdelete", "Deletes all record of a bot", 0, bot_subcommand_bot_delete) ||
		bot_command_add("botdetails", "Changes the Drakkin details of a bot", 0, bot_subcommand_bot_details) ||
		bot_command_add("botdyearmor", "Changes the color of a bot's (bots') armor", 0, bot_subcommand_bot_dye_armor) ||
		bot_command_add("boteyes", "Changes the eye colors of a bot", 0, bot_subcommand_bot_eyes) ||
		bot_command_add("botface", "Changes the facial appearance of your bot", 0, bot_subcommand_bot_face) ||
		bot_command_add("botgroup", "Lists the available bot-group [subcommands]", 0, bot_command_botgroup) ||
		bot_command_add("botgroupadd", "Adds a member to a bot-group", 0, bot_subcommand_botgroup_add) ||
		bot_command_add("botgroupattack", "Orders a bot-group to attack", 0, bot_subcommand_botgroup_attack) ||
		bot_command_add("botgroupcreate", "Creates a bot-group and designates a leader", 0, bot_subcommand_botgroup_create) ||
		bot_command_add("botgroupdelete", "Deletes a bot-group and releases its members", 0, bot_subcommand_botgroup_delete) ||
		bot_command_add("botgroupdisband", "Removes a bot from its bot-group or disbands the bot-group, if leader", 0, bot_subcommand_botgroup_disband) ||
		bot_command_add("botgroupfollow", "Orders a bot-group to follow you", 0, bot_subcommand_botgroup_follow) ||
		bot_command_add("botgroupguard", "Orders a bot-group to guard you", 0, bot_subcommand_botgroup_guard) ||
		bot_command_add("botgrouplist", "Lists all of your created bot-groups", 0, bot_subcommand_botgroup_list) ||
		bot_command_add("botgroupload", "Loads all members of a bot-group", 0, bot_subcommand_botgroup_load) ||
		bot_command_add("botgroupremove", "Removes a bot from its bot-group", 0, bot_subcommand_botgroup_remove) ||
		bot_command_add("botgroupsave", "Saves a formed bot-group", 0, bot_subcommand_botgroup_save) ||
		bot_command_add("botgroupsummon", "Summons all members of a bot-group to you", 0, bot_subcommand_botgroup_summon) ||
		bot_command_add("bothaircolor", "Changes the hair color of a bot", 0, bot_subcommand_bot_hair_color) ||
		bot_command_add("bothairstyle", "Changes the hairstyle of a bot", 0, bot_subcommand_bot_hairstyle) ||
		bot_command_add("botheritage", "Changes the Drakkin heritage of a bot", 0, bot_subcommand_bot_heritage) ||
		bot_command_add("botinspectmessage", "Changes the inspect message of a bot", 0, bot_subcommand_bot_inspect_message) ||
		bot_command_add("botlist", "Lists the bots that you own", 0, bot_subcommand_bot_list) ||
		bot_command_add("botoutofcombat", "Toggles your bot between standard and out-of-combat spell/skill use - if any specialized behaviors exist", 0, bot_subcommand_bot_out_of_combat) ||
		bot_command_add("botreport", "Orders a bot to report its readiness", 0, bot_subcommand_bot_report) ||
		bot_command_add("botspawn", "Spawns a created bot", 0, bot_subcommand_bot_spawn) ||
		bot_command_add("botstance", "Changes the stance of a bot", 0, bot_subcommand_bot_stance) ||
		bot_command_add("botsummon", "Summons your bot to a location", 0, bot_subcommand_bot_summon) ||
		bot_command_add("bottattoo", "Changes the Drakkin tattoos of a bot", 0, bot_subcommand_bot_tattoo) ||
		bot_command_add("bottogglearcher", "Toggles a archer bot between melee and ranged weapon use", 0, bot_subcommand_bot_toggle_archer) ||
		bot_command_add("bottogglehelm", "Toggles the helm visibility of a bot between shown and hidden", 0, bot_subcommand_bot_toggle_helm) ||
		bot_command_add("botupdate", "Updates a bot to reflect any level changes that you have made", 0, bot_subcommand_bot_update) ||
		bot_command_add("charm", "Attempts to have a bot charm your target", 0, bot_command_charm) ||
		bot_command_add("circle", "Orders a Druid bot to open a magical doorway to a specified destination", 0, bot_subcommand_circle) ||
		bot_command_add("cure", "Orders a bot to remove any ailments", 0, bot_command_cure) ||
		bot_command_add("defensive", "Orders a bot to use a defensive discipline", 0, bot_command_defensive) ||
		bot_command_add("depart", "Orders a bot to open a magical doorway to a specified destination", 0, bot_command_depart) ||
		bot_command_add("escape", "Orders a bot to send a target group to a safe location within the zone", 0, bot_command_escape) ||
		bot_command_add("findaliases", "Find available aliases for a bot command", 0, bot_command_find_aliases) ||
		bot_command_add("followdistance", "Changes the follow distance(s) of a bot(s)", 0, bot_command_follow_distance) ||
		bot_command_add("group", "Lists the available grouped bot [subcommands]", 0, bot_command_group) ||
		bot_command_add("groupattack", "Orders bots in your group to attack", 0, bot_subcommand_group_attack) ||
		bot_command_add("groupfollow", "Orders bots in your group to follow you", 0, bot_subcommand_group_follow) ||
		bot_command_add("groupguard", "Orders bots in your group to guard you", 0, bot_subcommand_group_guard) ||
		bot_command_add("groupsummon", "Summons bots in your group to you", 0, bot_subcommand_group_summon) ||
		bot_command_add("grow", "Orders a bot to increase a group member's size", 0, bot_command_grow) ||
		bot_command_add("healrotation", "Lists the available bot heal rotation [subcommands]", 0, bot_command_heal_rotation) ||
		bot_command_add("healrotationaddmember", "Adds a bot to a heal rotation instance", 0, bot_subcommand_heal_rotation_add_member) ||
		bot_command_add("healrotationaddtarget", "Adds target to a heal rotation instance", 0, bot_subcommand_heal_rotation_add_target) ||
		bot_command_add("healrotationcleartargets", "Removes all targets from a heal rotation instance", 0, bot_subcommand_heal_rotation_clear_targets) ||
		bot_command_add("healrotationcreate", "Creates a bot heal rotation instance and designates a leader", 0, bot_subcommand_heal_rotation_create) ||
		bot_command_add("healrotationfastheals", "Enables or disables fast heals within the heal rotation instance", 0, bot_subcommand_heal_rotation_fast_heals) ||
		bot_command_add("healrotationlist", "Reports heal rotation instance(s) information", 0, bot_subcommand_heal_rotation_list) ||
		bot_command_add("healrotationremovemember", "Removes a bot from a heal rotation instance", 0, bot_subcommand_heal_rotation_remove_member) ||
		bot_command_add("healrotationremovetarget", "Removes target from a heal rotations instance", 0, bot_subcommand_heal_rotation_remove_target) ||
		bot_command_add("healrotationstart", "Starts a heal rotation(s)", 0, bot_subcommand_heal_rotation_start) ||
		bot_command_add("healrotationstop", "Stops a heal rotation(s)", 0, bot_subcommand_heal_rotation_stop) ||
		bot_command_add("help", "List available commands and their description - specify partial command as argument to search", 0, bot_command_help) ||
		bot_command_add("identify", "Orders a bot to cast an item identification spell", 0, bot_command_identify) ||
		bot_command_add("inventory", "Lists the available bot inventory [subcommands]", 0, bot_command_inventory) ||
		bot_command_add("inventorylist", "Lists all items in a bot's inventory", 0, bot_subcommand_inventory_list) ||
		bot_command_add("inventoryremove", "Removes an item from a bot's inventory", 0, bot_subcommand_inventory_remove) ||
		bot_command_add("invisibility", "Orders a bot to cast a cloak of invisibility, or allow them to be seen", 0, bot_command_invisibility) ||
		bot_command_add("item", "Lists the available bot item [subcommands]", 0, bot_command_item) ||
		bot_command_add("itemaugment", "Allows a bot to augment an item for you", 200, bot_subcommand_item_augment) ||
		bot_command_add("itemgive", "Gives the item on your cursor to a bot", 200, bot_subcommand_item_give) ||
		bot_command_add("levitation", "Orders a bot to cast a levitation spell", 0, bot_command_levitation) ||
		bot_command_add("lull", "Orders a bot to cast a pacification spell", 0, bot_command_lull) ||
		bot_command_add("mesmerize", "Orders a bot to cast a mesmerization spell", 0, bot_command_mesmerize) ||
		bot_command_add("movementspeed", "Orders a bot to cast a movement speed enhancement spell", 0, bot_command_movement_speed) ||
#ifdef PACKET_PROFILER
		bot_command_add("packetprofile", "Dump packet profile for target or self.", 250, bot_command_packet_profile) ||
#endif
		bot_command_add("pet", "Lists the available bot pet [subcommands]", 0, bot_command_pet) ||
		bot_command_add("petremove", "Orders a bot to remove its pet", 0, bot_subcommand_pet_remove) ||
		bot_command_add("petsettype", "Orders a Magician bot to use a specified pet type", 0, bot_subcommand_pet_set_type) ||
		bot_command_add("picklock", "Orders a capable bot to pick the lock of the closest door", 0, bot_command_pick_lock) ||
		bot_command_add("portal", "Orders a Wizard bot to open a magical doorway to a specified destination", 0, bot_subcommand_portal) ||
#ifdef EQPROFILE
		bot_command_add("profiledump", "Dump profiling info to logs", 250, bot_command_profile_dump) ||
		bot_command_add("profilereset", "Reset profiling info", 250, bot_command_profile_reset) ||
#endif
		bot_command_add("pull", "Orders a designated bot to 'pull' an enemy", 0, bot_command_pull) ||
		bot_command_add("resistance", "Orders a bot to cast a specified resistance buff", 0, bot_command_resistance) ||
		bot_command_add("resurrect", "Orders a bot to resurrect a player's (players') corpse(s)", 0, bot_command_resurrect) ||
		bot_command_add("rune", "Orders a bot to cast a rune of protection", 0, bot_command_rune) ||
		bot_command_add("sendhome", "Orders a bot to open a magical doorway home", 0, bot_command_send_home) ||
		bot_command_add("shrink", "Orders a bot to decrease a player's size", 0, bot_command_shrink) ||
		bot_command_add("summoncorpse", "Orders a bot to summon a corpse to its feet", 0, bot_command_summon_corpse) ||
		bot_command_add("taunt", "Toggles taunt use by a bot", 0, bot_command_taunt) ||
		bot_command_add("track", "Orders a capable bot to track enemies", 0, bot_command_track) ||
		bot_command_add("waterbreathing", "Orders a bot to cast a water breathing spell", 0, bot_command_water_breathing)
	) {
		bot_command_deinit();
		return -1;
	}

	std::map<std::string, std::pair<uint8, std::vector<std::string>>> bot_command_settings;
	database.GetBotCommandSettings(bot_command_settings);
	for (std::map<std::string, BotCommandRecord *>::iterator iter_cl = bot_command_list.begin(); iter_cl != bot_command_list.end(); ++iter_cl) {
		std::map<std::string, std::pair<uint8, std::vector<std::string>>>::iterator iter_cs = bot_command_settings.find(iter_cl->first);
		if (iter_cs == bot_command_settings.end()) {
			if (iter_cl->second->access == 0)
				Log.Out(Logs::General, Logs::Commands, "bot_command_init(): Warning: Bot Command '%s' defaulting to access level 0!", iter_cl->first.c_str());
			continue;
		}

		iter_cl->second->access = iter_cs->second.first;
		Log.Out(Logs::General, Logs::Commands, "bot_command_init(): - Bot Command '%s' set to access level %d.", iter_cl->first.c_str(), iter_cs->second.first);
		if (iter_cs->second.second.empty())
			continue;

		for (std::vector<std::string>::iterator iter_aka = iter_cs->second.second.begin(); iter_aka != iter_cs->second.second.end(); ++iter_aka) {
			if (iter_aka->empty())
				continue;
			if (bot_command_list.find(*iter_aka) != bot_command_list.end()) {
				Log.Out(Logs::General, Logs::Commands, "bot_command_init(): Warning: Alias '%s' already exists as a bot command - skipping!", iter_aka->c_str());
				continue;
			}

			bot_command_list[*iter_aka] = iter_cl->second;
			bot_command_aliases[*iter_aka] = iter_cl->first;

			Log.Out(Logs::General, Logs::Commands, "bot_command_init(): - Alias '%s' added to bot command '%s'.", iter_aka->c_str(), bot_command_aliases[*iter_aka].c_str());
		}
	}

	bot_command_dispatch = bot_command_real_dispatch;

	BCSpells::Load();
	
	return bot_command_count;
}


/*
 * bot_command_deinit
 * clears the bot command list, freeing resources
 *
 * Parameters:
 *	none
 *
 */
void bot_command_deinit(void)
{
	bot_command_list.clear();
	bot_command_aliases.clear();

	bot_command_dispatch = bot_command_not_avail;
	bot_command_count = 0;

	BCSpells::Unload();
}


/*
 * bot_command_add
 * adds a bot command to the bot command list; used by bot_command_init
 *
 * Parameters:
 *	bot_command_string	- the command ex: "spawn"
 *	desc				- text description of bot command for #help
 *	access				- default access level required to use command
 *	function			- pointer to function that handles command
 *
 */
int bot_command_add(std::string bot_command_name, const char *desc, int access, BotCmdFuncPtr function)
{
	if (bot_command_name.empty()) {
		Log.Out(Logs::General, Logs::Error, "bot_command_add() - Bot command added with empty name string - check bot_command.cpp.");
		return -1;
	}
	if (function == nullptr) {
		Log.Out(Logs::General, Logs::Error, "bot_command_add() - Bot command '%s' added without a valid function pointer - check bot_command.cpp.", bot_command_name.c_str());
		return -1;
	}
	if (bot_command_list.count(bot_command_name) != 0) {
		Log.Out(Logs::General, Logs::Error, "bot_command_add() - Bot command '%s' is a duplicate bot command name - check bot_command.cpp.", bot_command_name.c_str());
		return -1;
	}
	for (std::map<std::string, BotCommandRecord *>::iterator iter = bot_command_list.begin(); iter != bot_command_list.end(); ++iter) {
		if (iter->second->function != function)
			continue;
		Log.Out(Logs::General, Logs::Error, "bot_command_add() - Bot command '%s' equates to an alias of '%s' - check bot_command.cpp.", bot_command_name.c_str(), iter->first.c_str());
		return -1;
	}

	BotCommandRecord *bcr = new BotCommandRecord;
	bcr->access = access;
	bcr->desc = desc;
	bcr->function = function;

	bot_command_list[bot_command_name] = bcr;
	bot_command_aliases[bot_command_name] = bot_command_name;
	cleanup_bot_command_list.Append(bcr);
	bot_command_count++;

	return 0;
}


/*
 *
 * bot_command_real_dispatch
 * Calls the correct function to process the client's command string.
 * Called from Client::ChannelMessageReceived if message starts with
 * bot command character (^).
 *
 * Parameters:
 *	c			- pointer to the calling client object
 *	message		- what the client typed
 *
 */
int bot_command_real_dispatch(Client *c, const char *message)
{
	Seperator sep(message, ' ', 10, 100, true); // "three word argument" should be considered 1 arg

	bot_command_log_command(c, message);

	std::string cstr(sep.arg[0]+1);

	if(bot_command_list.count(cstr) != 1) {
		return(-2);
	}

	BotCommandRecord *cur = bot_command_list[cstr];
	if(c->Admin() < cur->access){
		c->Message(13,"Your access level is not high enough to use this bot command.");
		return(-1);
	}

	/* QS: Player_Log_Issued_Commands */
	if (RuleB(QueryServ, PlayerLogIssuedCommandes)){
		std::string event_desc = StringFormat("Issued bot command :: '%s' in zoneid:%i instid:%i",  message, c->GetZoneID(), c->GetInstanceID());
		QServ->PlayerLogEvent(Player_Log_Issued_Commands, c->CharacterID(), event_desc);
	}

	if(cur->access >= COMMANDS_LOGGING_MIN_STATUS) {
		Log.Out(Logs::General, Logs::Commands, "%s (%s) used bot command: %s (target=%s)",  c->GetName(), c->AccountName(), message, c->GetTarget()?c->GetTarget()->GetName():"NONE");
	}

	if(cur->function == nullptr) {
		Log.Out(Logs::General, Logs::Error, "Bot command '%s' has a null function\n",  cstr.c_str());
		return(-1);
	} else {
		//dispatch C++ bot command
		cur->function(c, &sep);	// dispatch bot command
	}
	return 0;

}

void bot_command_log_command(Client *c, const char *message)
{
	int admin=c->Admin();

	bool continueevents=false;
	switch (zone->loglevelvar){ //catch failsafe
		case 9: { // log only LeadGM
			if ((admin>= 150) && (admin <200))
				continueevents=true;
			break;
		}
		case 8: { // log only GM
			if ((admin>= 100) && (admin <150))
				continueevents=true;
			break;
		}
		case 1: {
			if ((admin>= 200))
				continueevents=true;
			break;
		}
		case 2: {
			if ((admin>= 150))
				continueevents=true;
			break;
		}
		case 3: {
			if ((admin>= 100))
				continueevents=true;
			break;
		}
		case 4: {
			if ((admin>= 80))
				continueevents=true;
			break;
		}
		case 5: {
			if ((admin>= 20))
				continueevents=true;
			break;
		}
		case 6: {
			if ((admin>= 10))
				continueevents=true;
			break;
		}
		case 7: {
				continueevents=true;
				break;
		}
	}

	if (continueevents)
		database.logevents(
			c->AccountName(),
			c->AccountID(),
			admin,c->GetName(),
			c->GetTarget()?c->GetTarget()->GetName():"None",
			"BotCommand",
			message,
			1
		);
}


/*
 * helper functions by use
 */
bool spell_list_fail(Client *c, bcst_list* spell_list, BCEnum::SType spell_type)
{
	if (!spell_list || spell_list->empty()) {
		c->Message(15, "%s", required_bots_map[spell_type].c_str());
		return true;
	}
	return false;
}

bool command_alias_fail(Client *c, const char* command_handler, const char *alias, const char *command)
{
	std::map<std::string, std::string>::iterator alias_iter = bot_command_aliases.find(&alias[1]);
	if (alias_iter == bot_command_aliases.end() || alias_iter->second.compare(command)) {
		c->Message(15, "Undefined linker usage in %s (%s)", command_handler, &alias[1]);
		return true;
	}
	return false;
}

bool is_help_or_usage(const char* arg)
{
	if (!arg)
		return false;

	if (!strcasecmp(arg, "help") || !strcasecmp(arg, "usage"))
		return true;

	return false;
}

void send_usage_required_bots(Client *bot_owner, BCEnum::SType spell_type, uint8 bot_class = 0)
{
	bot_owner->Message(0, "requires one of the following bot classes:");
	if (bot_class)
		bot_owner->Message(1, "%s", required_bots_map_by_class[spell_type][bot_class].c_str());
	else
		bot_owner->Message(1, "%s", required_bots_map[spell_type].c_str());
}

void send_available_subcommands(Client *bot_owner, const std::list<std::string>& subcommand_list)
{
	for (std::list<std::string>::const_iterator subcommand_iter = subcommand_list.begin(); subcommand_iter != subcommand_list.end(); ++subcommand_iter) {
		std::map<std::string, BotCommandRecord*>::iterator find_iter = bot_command_list.find((*subcommand_iter));
		bot_owner->Message(0, "%c%s - %s", BOT_COMMAND_CHAR, (*subcommand_iter).c_str(), ((find_iter != bot_command_list.end()) ? (find_iter->second->desc) : ("[no description]")));
	}
}


class MyBot
{
public:
	static bool IsMyBot(Client *bot_owner, Bot *my_bot) {
		if (!bot_owner || !my_bot || !my_bot->GetOwner() || !my_bot->GetOwner()->IsClient() || my_bot->GetOwner()->CastToClient() != bot_owner)
			return false;

		return true;
	}
	
	static bool IsMyBotInPlayerGroup(Client *bot_owner, Bot *grouped_bot, Client *grouped_player) {
		if (!bot_owner || !grouped_player || !grouped_bot || !grouped_player->GetGroup() || !grouped_bot->GetGroup() || !IsMyBot(bot_owner, grouped_bot))
			return false;

		return (grouped_player->GetGroup() == grouped_bot->GetGroup());
	}


	static void Populate_ByGroupedBots(Client *bot_owner, std::list<Bot*> &sbl) {
		sbl.clear();
		if (!bot_owner)
			return;

		Group* g = bot_owner->GetGroup();
		if (!g)
			return;

		for (int i = 0; i < MAX_GROUP_MEMBERS; ++i) {
			if (!g->members[i] || !g->members[i]->IsBot())
				continue;
			Bot* grouped_bot = g->members[i]->CastToBot();
			if (!IsMyBot(bot_owner, grouped_bot))
				continue;

			sbl.push_back(grouped_bot);
		}
	}

	static void Populate_ByTargetsGroupedBots(Client *bot_owner, std::list<Bot*> &sbl) {
		sbl.clear();
		if (!bot_owner)
			return;

		Mob* t = bot_owner->GetTarget();
		if (!t)
			return;

		Group* g = nullptr;
		if (t->IsClient())
			g = t->CastToClient()->GetGroup();
		else if (t->IsBot())
			g = t->CastToBot()->GetGroup();
		if (!g)
			return;

		for (int i = 0; i < MAX_GROUP_MEMBERS; ++i) {
			if (!g->members[i] || !g->members[i]->IsBot())
				continue;
			Bot* grouped_bot = g->members[i]->CastToBot();
			if (!IsMyBot(bot_owner, grouped_bot))
				continue;

			sbl.push_back(grouped_bot);
		}
	}

	static void Populate_BySpawnedBots(Client *bot_owner, std::list<Bot*> &sbl) {
		sbl.clear();
		if (!bot_owner)
			return;

		sbl = entity_list.GetBotsByBotOwnerCharacterID(bot_owner->CharacterID());
	}
	
	
	static Bot* AsGroupMember_ByClass(Client *bot_owner, Client *bot_grouped_player, uint8 cls, bool petless = false) {
		if (!bot_owner || !bot_grouped_player)
			return nullptr;

		Group* g = bot_grouped_player->GetGroup();
		if (!g)
			return nullptr;

		for (int i = 0; i < MAX_GROUP_MEMBERS; ++i) {
			if (!g->members[i] || !g->members[i]->IsBot())
				continue;
			Bot* grouped_bot = g->members[i]->CastToBot();
			if (!IsMyBot(bot_owner, grouped_bot))
				continue;
			if (grouped_bot->GetClass() != cls)
				continue;
			if (petless && grouped_bot->GetPet())
				continue;

			return grouped_bot;
		}

		return nullptr;
	}

	static Bot* AsGroupMember_ByMinLevelAndClass(Client *bot_owner, Client *bot_grouped_player, uint8 minlvl, uint8 cls, bool petless = false) {
		// This function can be nixed if we can enforce bot level as owner level..and the level check can then be moved to the spell loop in the command function
		if (!bot_owner || !bot_grouped_player)
			return nullptr;

		Group* g = bot_grouped_player->GetGroup();
		if (!g)
			return nullptr;

		for (int i = 0; i < MAX_GROUP_MEMBERS; ++i) {
			if (!g->members[i] || !g->members[i]->IsBot())
				continue;
			Bot* grouped_bot = g->members[i]->CastToBot();
			if (!IsMyBot(bot_owner, grouped_bot))
				continue;
			if (grouped_bot->GetLevel() < minlvl || grouped_bot->GetClass() != cls)
				continue;
			if (petless && grouped_bot->GetPet())
				continue;

			return grouped_bot;
		}

		return nullptr;
	}
	

	static Bot* AsSpawned_ByClass(Client *bot_owner, std::list<Bot*> &sbl, uint8 cls, bool petless = false) {
		if (!bot_owner)
			return nullptr;

		for (std::list<Bot*>::iterator iter = sbl.begin(); iter != sbl.end(); ++iter) {
			Bot* spawned_bot = *iter;
			if (!IsMyBot(bot_owner, spawned_bot))
				continue;
			if (spawned_bot->GetClass() != cls)
				continue;
			if (petless && spawned_bot->GetPet())
				continue;

			return spawned_bot;
		}

		return nullptr;
	}

	static Bot* AsSpawned_ByMinLevelAndClass(Client *bot_owner, std::list<Bot*> &sbl, uint8 minlvl, uint8 cls, bool petless = false) {
		// This function can be nixed if we can enforce bot level as owner level..and the level check can then be moved to the spell loop in the command function
		if (!bot_owner)
			return nullptr;

		for (std::list<Bot*>::iterator iter = sbl.begin(); iter != sbl.end(); ++iter) {
			Bot* spawned_bot = *iter;
			if (!IsMyBot(bot_owner, spawned_bot))
				continue;
			if (spawned_bot->GetLevel() < minlvl || spawned_bot->GetClass() != cls)
				continue;
			if (petless && spawned_bot->GetPet())
				continue;

			return spawned_bot;
		}

		return nullptr;
	}
	

	static Bot* AsTarget_ByBot(Client *bot_owner) {
		if (!bot_owner || !bot_owner->GetTarget() || !bot_owner->GetTarget()->IsBot())
			return nullptr;

		Bot* targeted_bot = bot_owner->GetTarget()->CastToBot();
		if (!IsMyBot(bot_owner, targeted_bot))
			return nullptr;

		return targeted_bot;
	}
	
	static Bot* AsTarget_ByArcherBot(Client *bot_owner) {
		if (!bot_owner || !bot_owner->GetTarget() || !bot_owner->GetTarget()->IsBot())
			return nullptr;

		Bot* targeted_bot = bot_owner->GetTarget()->CastToBot();
		if (!IsMyBot(bot_owner, targeted_bot) || !targeted_bot->IsBotArcher())
			return nullptr;

		return targeted_bot;
	}


	static Bot* AsNamed_ByBot(Client *bot_owner, std::string bot_name) {
		if (!bot_owner || bot_name.length() < 3 || bot_name.length() >= 64)
			return nullptr;

		std::list<Bot*> sbl;
		Populate_BySpawnedBots(bot_owner, sbl);

		for (std::list<Bot*>::iterator iter = sbl.begin(); iter != sbl.end(); ++iter) {
			Bot* named_bot = *iter;
			if (!bot_name.compare(named_bot->GetCleanName()))
				return named_bot;
		}

		return nullptr;
	}
};

class MyTarget
{
public:
	static bool AmIInPlayerGroup(Client *bot_owner, Client *grouped_player) {
		if (!bot_owner || !grouped_player || !bot_owner->GetGroup() || !grouped_player->GetGroup())
			return false;

		return (bot_owner->GetGroup() == grouped_player->GetGroup());
	}

	
	static Client* AsSingle_ByPlayer(Client *bot_owner, bool return_me_on_null_target = true) {
		if (!bot_owner)
			return nullptr;

		if (!bot_owner->GetTarget()) {
			if (return_me_on_null_target)
				return bot_owner;
			else
				return nullptr;
		}

		if (!bot_owner->GetTarget()->IsClient())
			return nullptr;

		return bot_owner->GetTarget()->CastToClient();
	}

	static Client* AsGroupMember_ByPlayer(Client *bot_owner, bool return_me_on_null_target = true) {
		if (!bot_owner)
			return nullptr;

		if (!bot_owner->GetTarget()) {
			if (return_me_on_null_target)
				return bot_owner;
			else
				return nullptr;
		}

		if (!bot_owner->GetTarget()->IsClient() || !AmIInPlayerGroup(bot_owner, bot_owner->GetTarget()->CastToClient()))
			return nullptr;

		return bot_owner->GetTarget()->CastToClient();
	}

	static Corpse* AsCorpse_ByPlayer(Client *bot_owner) {
		if (!bot_owner || !bot_owner->GetTarget() || !bot_owner->GetTarget()->IsPlayerCorpse())
			return nullptr;

		return bot_owner->GetTarget()->CastToCorpse();
	}

	static Mob* AsSingle_ByAttackable(Client *bot_owner) {
		if (!bot_owner || !bot_owner->GetTarget() || !bot_owner->IsAttackAllowed(bot_owner->GetTarget()))
			return nullptr;

		return bot_owner->GetTarget();
	}

};

class SelectActionableBots
{
public:
	enum SABType { Single, OwnerGroup, TargetGroup, Spawned, All }; // add botgroup and maybe healrotation (added 'by_name' argument below)

	static bool SetActionableBots(Client *c, std::string command_arg, SelectActionableBots::SABType &bot_action_target_type, Bot *&b, std::list<Bot*> &sbl, const char* by_name = nullptr) {
		if (!c)
			return false;

		bot_action_target_type = Single;
		if (!command_arg.compare("ownergroup"))
			bot_action_target_type = OwnerGroup;
		else if (!command_arg.compare("targetgroup"))
			bot_action_target_type = TargetGroup;
		else if (!command_arg.compare("spawned"))
			bot_action_target_type = Spawned;
		else if (!command_arg.compare("all"))
			bot_action_target_type = All;

		b = MyBot::AsTarget_ByBot(c);
		if (!b && (bot_action_target_type == Single || bot_action_target_type == TargetGroup)) {
			c->Message(15, "You must <target> a bot that you own to use this command");
			return false;
		}

		switch (bot_action_target_type) {
		case Single:
			sbl.push_front(b);
			break;
		case OwnerGroup:
			MyBot::Populate_ByGroupedBots(c, sbl);
			break;
		case TargetGroup:
			MyBot::Populate_ByTargetsGroupedBots(c, sbl);
			break;
		case Spawned:
		case All:
			MyBot::Populate_BySpawnedBots(c, sbl);
			break;
		default:
			return false;
		}
		if (sbl.empty() && bot_action_target_type != All) {
			c->Message(15, "You have no spawned bots");
			return false;
		}

		return true;
	}
};


/*
 * bot commands go below here
 */
/*X*/
void bot_command_bind_affinity(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_BindAffinity];
	if (spell_list_fail(c, local_list, BCEnum::ST_BindAffinity) || command_alias_fail(c, "bot_command_bind_affinity", sep->arg[0], "bindaffinity"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_BindAffinity);
		return;
	}

	Client* mtsp = MyTarget::AsSingle_ByPlayer(c);
	if (!mtsp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STBaseEntry* local_entry = *iter_list;
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
			if (!mtsp)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtsp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/* OK */
void bot_command_bot(Client *c, const Seperator *sep)
{
	const std::list<std::string> subcommand_list = {
		"botappearance", "botcamp", "botclone", "botcreate", "botdelete", "botdetails", "botdyearmor", "botinspectmessage", "botlist",
		"botoutofcombat", "botreport", "botspawn", "botstance", "botsummon", "bottogglearcher", "bottogglehelm", "botupdate"
	};
	
	if (command_alias_fail(c, "bot_command_bot", sep->arg[0], "bot"))
		return;

	c->Message(0, "Available bot management subcommands:");
	
	send_available_subcommands(c, subcommand_list);
}

/* OK */
void bot_command_botgroup(Client *c, const Seperator *sep)
{
	const std::list<std::string> subcommand_list = {
		"botgroupadd", "botgroupattack", "botgroupcreate", "botgroupdelete", "botgroupdisband", "botgroupfollow",
		"botgroupguard", "botgrouplist", "botgroupload", "botgroupremove", "botgroupsave", "botgroupsummon"
	};
	
	if (command_alias_fail(c, "bot_command_botgroup", sep->arg[0], "botgroup"))
		return;

	c->Message(0, "Available bot-group management subcommands:");

	send_available_subcommands(c, subcommand_list);
}

/*X*/
void bot_command_charm(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Charm];
	if (spell_list_fail(c, local_list, BCEnum::ST_Charm) || command_alias_fail(c, "bot_command_charm", sep->arg[0], "charm"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Charm);
		return;
	}

	Mob* t = MyTarget::AsSingle_ByAttackable(c);
	if (!t) {
		c->Message(15, "You must <target> an enemey to use this command");
		return;
	}
	if (t->IsCharmed()) {
		c->Message(15, "Your <target> is already charmed");
		return;
	}

	Bot* b = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STBaseEntry* local_entry = *iter_list;
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;
		if (spells[local_entry->spell_id].max[EFFECTIDTOINDEX(1)] < t->GetLevel())
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class, true);
			break;
		case BCEnum::TT_Animal:
			if (t->GetBodyType() != BT_Animal)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class, true);
			break;
		case BCEnum::TT_Undead:
			if (t->GetBodyType() != BT_Undead)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class, true);
			break;
		case BCEnum::TT_Summoned:
			if (t->GetBodyType() != BT_Summoned)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class, true);
			break;
		case BCEnum::TT_Plant:
			if (t->GetBodyType() != BT_Plant)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class, true);
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/*X*/
void bot_command_cure(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Cure];
	if (spell_list_fail(c, local_list, BCEnum::ST_Cure) || command_alias_fail(c, "bot_command_cure", sep->arg[0], "cure"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s [type]", sep->arg[0]);
		c->Message(0, "type: blindness, disease, poison, curse or corruption");
		send_usage_required_bots(c, BCEnum::ST_Cure);
		return;
	}

	Client* mtsp = MyTarget::AsSingle_ByPlayer(c);
	Client* mtgmp = MyTarget::AsGroupMember_ByPlayer(c);
	if (!mtsp && !mtgmp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	std::string ailment = sep->arg[1];

	BCEnum::AMask ailment_mask = BCEnum::AM_None;
	if (!ailment.compare("blindness"))
		ailment_mask = BCEnum::AM_Blindness;
	else if (!ailment.compare("disease"))
		ailment_mask = BCEnum::AM_Disease;
	else if (!ailment.compare("poison"))
		ailment_mask = BCEnum::AM_Poison;
	else if (!ailment.compare("curse"))
		ailment_mask = BCEnum::AM_Curse;
	else if (!ailment.compare("corruption"))
		ailment_mask = BCEnum::AM_Corruption;

	if (ailment_mask == BCEnum::AM_None) {
		c->Message(15, "You must specify a cure [type]");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STCureEntry* local_entry = (*iter_list)->SafeCastToCure();
		if (!local_entry)
			continue;
		
		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;
		if ((local_entry->cure_mask & ailment_mask) != ailment_mask)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
		case BCEnum::TT_GroupV2:
			if (!mtsp)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtsp;
			break;
		case BCEnum::TT_GroupV1:
			if (!mtgmp)
				continue;
			b = MyBot::AsGroupMember_ByMinLevelAndClass(c, mtgmp, local_entry->spell_level, local_entry->caster_class);
			t = mtgmp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/*X*/
void bot_command_defensive(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Defensive];
	if (spell_list_fail(c, local_list, BCEnum::ST_Defensive) || command_alias_fail(c, "bot_command_defensive", sep->arg[0], "defensive"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s ([name])", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Defensive);
		return;
	}
	
	Bot* b = nullptr;
	Bot* actionable_bot = nullptr;
	if (sep->arg[1][0] != '\0' && !sep->IsNumber(1))
		b = MyBot::AsNamed_ByBot(c, sep->arg[1]);
	else
		b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> or [name] a bot that you own to use this command");
		return;
	}

	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STBaseEntry* local_entry = *iter_list;
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		if (local_entry->caster_class != b->GetClass())
			continue;
		if (local_entry->spell_level > b->GetLevel())
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Using '%s'", spells[local_entry->spell_id].name);
		b->UseDiscipline(local_entry->spell_id, b->GetID());
		actionable_bot = b;

		break;
	}
	if (!actionable_bot) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/*X*/
void bot_command_depart(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Depart];
	if (spell_list_fail(c, local_list, BCEnum::ST_Depart) || command_alias_fail(c, "bot_command_depart", sep->arg[0], "depart"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s [list | destination]", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Depart);
		return;
	}

	if (!strcasecmp(sep->arg[1], "list")) {
		c->Message(0, "The following destinations are available:");

		Bot* b1 = MyBot::AsGroupMember_ByClass(c, c, DRUID);
		Bot* b2 = MyBot::AsGroupMember_ByClass(c, c, WIZARD);
		std::string msg;
		std::string text_link;

		int destinations = 0;
		for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
			STDepartEntry* local_entry = (*iter_list)->SafeCastToDepart();
			if (!local_entry)
				continue;

			if (b1 && b1->GetClass() == local_entry->caster_class && b1->GetLevel() >= local_entry->spell_level) {
				msg = StringFormat("%ccircle %s", BOT_COMMAND_CHAR, spells[local_entry->spell_id].teleport_zone);
				text_link = b1->CreateSayLink(c, msg.c_str(), local_entry->long_name.c_str());
				Bot::BotGroupSay(b1, "%s %s", spells[local_entry->spell_id].teleport_zone, text_link.c_str());
				++destinations;
				continue;
			}
			if (b2 && b2->GetClass() == local_entry->caster_class && b2->GetLevel() >= local_entry->spell_level) {
				msg = StringFormat("%cportal %s", BOT_COMMAND_CHAR, spells[local_entry->spell_id].teleport_zone);
				text_link = b2->CreateSayLink(c, msg.c_str(), local_entry->long_name.c_str());
				Bot::BotGroupSay(b2, "%s %s", spells[local_entry->spell_id].teleport_zone, text_link.c_str());
				++destinations;
				continue;
			}
		}
		if (!destinations)
			c->Message(15, "None");

		return;
	}

	Client* mtgmp = MyTarget::AsGroupMember_ByPlayer(c);
	if (!mtgmp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STDepartEntry* local_entry = (*iter_list)->SafeCastToDepart();
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_GroupV1:
			if (!mtgmp)
				continue;
			b = MyBot::AsGroupMember_ByMinLevelAndClass(c, mtgmp, local_entry->spell_level, local_entry->caster_class);
			t = mtgmp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/*X*/
void bot_command_escape(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Escape];
	if (spell_list_fail(c, local_list, BCEnum::ST_Escape) || command_alias_fail(c, "bot_command_escape", sep->arg[0], "escape"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s ([lesser])", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Escape);
		return;
	}

	Client* mtsp = MyTarget::AsSingle_ByPlayer(c);
	if (!mtsp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	bool use_lesser = false;
	if (!strcasecmp(sep->arg[1], "lesser"))
		use_lesser = true;

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STEscapeEntry* local_entry = (*iter_list)->SafeCastToEscape();
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;
		if (local_entry->lesser != use_lesser)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_GroupV2:
			if (!mtsp)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtsp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/* OK */
void bot_command_find_aliases(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_command_find_aliases", sep->arg[0], "findaliases"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s [alias | command]", sep->arg[0]);
		return;
	}
	
	std::map<std::string, std::string>::iterator find_iter = bot_command_aliases.find(sep->arg[1]);
	if (find_iter == bot_command_aliases.end()) {
		c->Message(15, "No bot commands or aliases match '%s'", sep->arg[1]);
		return;
	}

	std::map<std::string, BotCommandRecord *>::iterator command_iter = bot_command_list.find(find_iter->second);
	if (find_iter->second.empty() || command_iter == bot_command_list.end()) {
		c->Message(0, "An unknown condition occurred...");
		return;
	}

	c->Message(0, "Available bot command aliases for '%s':", command_iter->first.c_str());

	int bot_command_aliases_shown = 0;
	for (std::map<std::string, std::string>::iterator alias_iter = bot_command_aliases.begin(); alias_iter != bot_command_aliases.end(); ++alias_iter) {
		if (strcasecmp(find_iter->second.c_str(), alias_iter->second.c_str()) || c->Admin() < command_iter->second->access)
			continue;

		c->Message(0, "%c%s", BOT_COMMAND_CHAR, alias_iter->first.c_str());
		++bot_command_aliases_shown;
	}
	c->Message(0, "%d bot command alias%s listed.", bot_command_aliases_shown, bot_command_aliases_shown != 1 ? "es" : "");
}

/*X*/
void bot_command_follow_distance(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_command_follow_distance", sep->arg[0], "followdistance"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s [set] [distance] ([ownergroup | targetgroup | spawned | all])", sep->arg[0]);
		c->Message(0, "usage: (<target>) %s [clear] ([ownergroup | targetgroup | spawned | all])", sep->arg[0]);
		return;
	}

	uint32 bfd = BOT_DEFAULT_FOLLOW_DISTANCE;
	bool set_flag = false;
	int bat_arg = 2;

	if (!strcasecmp(sep->arg[1], "set")) {
		if (!sep->IsNumber(2)) {
			c->Message(15, "A numeric [value] is required to use this command");
			return;
		}

		bfd = atoi(sep->arg[2]);
		set_flag = true;
		bat_arg = 3;
	}
	else if (strcasecmp(sep->arg[1], "clear")) {
		c->Message(15, "This command requires a [set | clear] argument");
		return;
	}

	Bot* b = nullptr;
	SelectActionableBots::SABType multitarget = SelectActionableBots::Single;
	std::list<Bot*> sbl;
	if (!SelectActionableBots::SetActionableBots(c, sep->arg[bat_arg], multitarget, b, sbl))
		return;

	int bot_count = 0;
	for (std::list<Bot*>::iterator iter = sbl.begin(); iter != sbl.end(); ++iter) {
		if (!(*iter))
			continue;

		(*iter)->SetFollowDistance(bfd);
		// calling Bot::Camp() saves in-memory values for any spawned bot
		// (*iter)->Save();

		++bot_count;
	}

	if (multitarget == SelectActionableBots::All) {
		std::string query = StringFormat(
			"UPDATE `bot_data`"
			" SET `follow_distance` = '%u'"
			" WHERE `owner_id` = '%u'",
			bfd,
			c->CharacterID()
		);
		auto results = database.QueryDatabase(query);
		if (!results.Success()) {
			c->Message(15, "Failed to save follow distance changes for your bots due to unknown cause");
			return;
		}

		c->Message(0, "%s all of your bot follow distances", set_flag ? "Set" : "Cleared");
	}
	else {
		c->Message(0, "%s %i of your spawned bot follow distances", (set_flag ? "Set" : "Cleared"), bot_count);
	}

	// TODO: Finalization
}

/*X*/
void bot_command_grow(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Grow];
	if (spell_list_fail(c, local_list, BCEnum::ST_Grow) || command_alias_fail(c, "bot_command_grow", sep->arg[0], "grow"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Grow);
		return;
	}

	Client* mtsp = MyTarget::AsSingle_ByPlayer(c);
	if (!mtsp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STBaseEntry* local_entry = *iter_list;
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
			if (!mtsp)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtsp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/* OK */
void bot_command_group(Client *c, const Seperator *sep)
{
	const std::list<std::string> subcommand_list = { "groupattack", "groupfollow", "groupguard", "groupsummon"	};
	
	if (command_alias_fail(c, "bot_command_group", sep->arg[0], "group"))
		return;

	c->Message(0, "Available grouped bot management subcommands:");
	
	send_available_subcommands(c, subcommand_list);
}

/* OK */
void bot_command_heal_rotation(Client *c, const Seperator *sep)
{
	const std::list<std::string> subcommand_list = {
		"healrotationaddmember", "healrotationaddtarget", "healrotationcleartargets", "healrotationcreate", "healrotationfastheals",
		"healrotationlist", "healrotationremovemember", "healrotationremovetarget", "healrotationstart", "healrotationstop"
	};
	
	if (command_alias_fail(c, "bot_command_heal_rotation", sep->arg[0], "healrotation"))
		return;

	c->Message(0, "Available bot heal rotation management subcommands:");

	send_available_subcommands(c, subcommand_list);
}

/* OK */
void bot_command_help(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_command_help", sep->arg[0], "help"))
		return;
	
	c->Message(0, "Available EQEMu bot commands:");

	int bot_commands_shown = 0;
	for (std::map<std::string, BotCommandRecord *>::iterator command_iter = bot_command_list.begin(); command_iter != bot_command_list.end(); ++command_iter) {
		if (sep->arg[1][0] && command_iter->first.find(sep->arg[1]) == std::string::npos)
			continue;
		if (c->Admin() < command_iter->second->access)
			continue;

		c->Message(0, "%c%s - %s", BOT_COMMAND_CHAR, command_iter->first.c_str(), command_iter->second->desc == nullptr ? "[no description]" : command_iter->second->desc);
		++bot_commands_shown;
	}
	c->Message(0, "%d bot command%s listed.", bot_commands_shown, bot_commands_shown != 1 ? "s" : "");
}

/*X*/
void bot_command_identify(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Identify];
	if (spell_list_fail(c, local_list, BCEnum::ST_Identify) || command_alias_fail(c, "bot_command_identify", sep->arg[0], "identify"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Identify);
		return;
	}

	Client* mtsp = MyTarget::AsSingle_ByPlayer(c);
	if (!mtsp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STBaseEntry* local_entry = *iter_list;
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
			if (!mtsp)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtsp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/* OK */
void bot_command_inventory(Client *c, const Seperator *sep)
{
	const std::list<std::string> subcommand_list = { "inventorylist", "inventoryremove" };
	
	if (command_alias_fail(c, "bot_command_inventory", sep->arg[0], "inventory"))
		return;

	c->Message(0, "Available bot inventory management subcommands:");
	
	send_available_subcommands(c, subcommand_list);
}

/*X*/
void bot_command_invisibility(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Invisibility];
	if (spell_list_fail(c, local_list, BCEnum::ST_Invisibility) || command_alias_fail(c, "bot_command_invisibility", sep->arg[0], "invisibility"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s [type]", sep->arg[0]);
		c->Message(0, "type: living, undead, animal or see");
		send_usage_required_bots(c, BCEnum::ST_Invisibility);
		return;
	}

	Client* mtsp = MyTarget::AsSingle_ByPlayer(c);
	if (!mtsp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	std::string invisibility = sep->arg[1];

	BCEnum::IType invisibility_type = BCEnum::IT_None;
	if (!invisibility.compare("living"))
		invisibility_type = BCEnum::IT_Living;
	else if (!invisibility.compare("undead"))
		invisibility_type = BCEnum::IT_Undead;
	else if (!invisibility.compare("animal"))
		invisibility_type = BCEnum::IT_Animal;
	else if (!invisibility.compare("see"))
		invisibility_type = BCEnum::IT_See;

	if (invisibility_type == BCEnum::IT_None) {
		c->Message(15, "You must specify an invisibility [type]");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STInvisibilityEntry* local_entry = (*iter_list)->SafeCastToInvisibility();
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;
		if (local_entry->invis_type != invisibility_type)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
		case BCEnum::TT_GroupV2:
			if (!mtsp)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtsp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/* OK */
void bot_command_item(Client *c, const Seperator *sep)
{
	const std::list<std::string> subcommand_list = { "itemaugment", "itemgive" };

	if (command_alias_fail(c, "bot_command_item", sep->arg[0], "item"))
		return;

	c->Message(0, "Available bot item management subcommands:");

	send_available_subcommands(c, subcommand_list);
}

/*X*/
void bot_command_levitation(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Levitation];
	if (spell_list_fail(c, local_list, BCEnum::ST_Levitation) || command_alias_fail(c, "bot_command_levitation", sep->arg[0], "levitation"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Levitation);
		return;
	}

	Client* mtsp = MyTarget::AsSingle_ByPlayer(c);
	if (!mtsp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STBaseEntry* local_entry = *iter_list;
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
		case BCEnum::TT_GroupV2:
			if (!mtsp)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtsp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/*xx*/
void bot_command_lull(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Lull];
	if (spell_list_fail(c, local_list, BCEnum::ST_Lull) || command_alias_fail(c, "bot_command_lull", sep->arg[0], "lull"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Lull);
		return;
	}

	//
	return;
	//

	Mob* t = MyTarget::AsSingle_ByAttackable(c);
	if (!t) {
		c->Message(15, "You must <target> an enemey to use this command");
		return;
	}
	if (t->IsCharmed()) {
		c->Message(15, "Your <target> is already charmed");
		return;
	}


	



	// TT_Single
	// TT_AETarget
	// TT_Animal
	// TT_Undead

	Bot* b = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STBaseEntry* local_entry = *iter_list;
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;
		if (spells[local_entry->spell_id].max[EFFECTIDTOINDEX(1)] < t->GetLevel())
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class, true);
			break;
		case BCEnum::TT_Animal:
			if (t->GetBodyType() != BT_Animal)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class, true);
			break;
		case BCEnum::TT_Undead:
			if (t->GetBodyType() != BT_Undead)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class, true);
			break;
		case BCEnum::TT_Summoned:
			if (t->GetBodyType() != BT_Summoned)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class, true);
			break;
		case BCEnum::TT_Plant:
			if (t->GetBodyType() != BT_Plant)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class, true);
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}








	if (!strcasecmp(sep->arg[1], "target") && !strcasecmp(sep->arg[2], "calm")) {
		Mob *target = c->GetTarget();
		if (target == nullptr || target->IsClient() || target->IsBot() || (target->IsPet() && target->GetOwner() && target->GetOwner()->IsBot()))
			c->Message(15, "You must select a monster!");
		else {
			if (c->IsGrouped()) {
				Group *g = c->GetGroup();
				for (int i = 0; i < MAX_GROUP_MEMBERS; i++) {
					// seperated cleric and chanter so chanter is primary
					if (g && g->members[i] && g->members[i]->IsBot() && (g->members[i]->GetClass() == ENCHANTER)) {
						Bot *pacer = g->members[i]->CastToBot();
						pacer->BotGroupSay(pacer, "Trying to pacify %s.", target->GetCleanName());
						if (pacer->Bot_Command_CalmTarget(target)) {
							if (target->FindType(SE_Lull) || target->FindType(SE_Harmony) || target->FindType(SE_InstantHate))
								c->Message(0, "I have successfully pacified %s.", target->GetCleanName());

							return;
						}
						else
							c->Message(0, "I failed to pacify %s.", target->GetCleanName());
					}
					// seperated cleric and chanter so chanter is primary
					if (g && g->members[i] && g->members[i]->IsBot() && (g->members[i]->GetClass() == CLERIC) && (Bot::GroupHasEnchanterClass(g) == false)) {
						Bot *pacer = g->members[i]->CastToBot();
						pacer->BotGroupSay(pacer, "Trying to pacify %s.", target->GetCleanName());

						if (pacer->Bot_Command_CalmTarget(target)) {
							if (target->FindType(SE_Lull) || target->FindType(SE_Harmony) || target->FindType(SE_InstantHate))
								c->Message(0, "I have successfully pacified %s.", target->GetCleanName());

							return;
						}
						else
							c->Message(0, "I failed to pacify %s.", target->GetCleanName());
					}
				}
			}
		}

		return;
	}

	// TODO: Logging
}

/*xx*/
void bot_command_mesmerize(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Mesmerize];
	if (spell_list_fail(c, local_list, BCEnum::ST_Mesmerize) || command_alias_fail(c, "bot_command_mesmerize", sep->arg[0], "mesmerize"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Mesmerize);
		return;
	}

	//
	return;
	//

	// calls Bot::MesmerizeTarget() .. no command intervention - atm

	// TT_AECaster
	// TT_Single
	// TT_AETarget
	// TT_Undead
	// TT_Summoned



	if (!strcasecmp(sep->arg[1], "mez")) {
		Mob *target = c->GetTarget();
		if (target == nullptr || target == c || target->IsBot() || (target->IsPet() && target->GetOwner() && target->GetOwner()->IsBot())) {
			c->Message(15, "You must select a monster");
			return;
		}

		if (c->IsGrouped()) {
			bool hasmezzer = false;
			Group *g = c->GetGroup();
			for (int i = 0; i < MAX_GROUP_MEMBERS; i++) {
				if (g && g->members[i] && g->members[i]->IsBot() && (g->members[i]->GetClass() == ENCHANTER)) {
					hasmezzer = true;
					Mob *mezzer = g->members[i];
					mezzer->CastToBot()->BotGroupSay(mezzer->CastToBot(), "Trying to mesmerize %s.", target->GetCleanName());
					mezzer->CastToBot()->MesmerizeTarget(target);
				}
			}

			if (!hasmezzer)
				c->Message(15, "You must have an Enchanter in your group.");
		}
		return;
	}

	// TODO: Logging
}

/*X*/
void bot_command_movement_speed(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_MovementSpeed];
	if (spell_list_fail(c, local_list, BCEnum::ST_MovementSpeed) || command_alias_fail(c, "bot_command_movement_speed", sep->arg[0], "movementspeed"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_MovementSpeed);
		return;
	}

	// c->Message(0, "usage: %cspeed ([group])", BOT_COMMAND_CHAR);

	Client* mtsp = MyTarget::AsSingle_ByPlayer(c);
	Client* mtgmp = MyTarget::AsGroupMember_ByPlayer(c);
	if (!mtsp && !mtgmp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STBaseEntry* local_entry = *iter_list;
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
		case BCEnum::TT_GroupV2:
			if (!mtsp)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtsp;
			break;
		case BCEnum::TT_GroupV1:
			if (!mtgmp)
				continue;
			b = MyBot::AsGroupMember_ByMinLevelAndClass(c, mtgmp, local_entry->spell_level, local_entry->caster_class);
			t = mtgmp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/* OK */
void bot_command_pet(Client *c, const Seperator *sep)
{
	const std::list<std::string> subcommand_list = { "petremove", "petsettype" };
	
	if (command_alias_fail(c, "bot_command_pet", sep->arg[0], "pet"))
		return;

	c->Message(0, "Available bot pet management subcommands:");
	
	send_available_subcommands(c, subcommand_list);
}

/*X*/
void bot_command_pick_lock(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_command_pick_lock", sep->arg[0], "picklock"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		c->Message(0, "requires one of the following bot classes:");
		c->Message(1, "Rogue(5) or Bard(40)");
		return;
	}

	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);

	Bot* b1 = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, 5, ROGUE);
	Bot* b2 = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, 40, BARD);
	if (b1 && b2) {
		// this probably needs to be improved upon (bonus modifiers)
		if (b2->GetSkill(SkillPickLock) > b1->GetSkill(SkillPickLock))
			b1 = b2;
	}
	else if (!b1 && b2) {
		b1 = b2;
	}
	else if (!b1 && !b2) {
		c->Message(15, "No spawned bots are capable of performing this action");
		return;
	}

	b1->InterruptSpell();
	b1->BotGroupSay(b1, "Attempting to pick the lock..");
	entity_list.BotPickLock(b1, c);

	// TODO: Finalization
}

/**/
void bot_command_pull(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_command_pull", sep->arg[0], "pull"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s", sep->arg[0]);
		c->Message(0, "requires one of the following bot classes:");
		//c->Message(1, required_bots_map[BCEnum::ST_BindAffinity].c_str());
		return;
	}

	//
	return;
	//
	
	if (!strcasecmp(sep->arg[1], "usage")) {
		if (!strcasecmp(sep->arg[0], "pull"))
			c->Message(0, "usage: %cpull", BOT_COMMAND_CHAR);
		else
			c->Message(15, "Undefined linker usage in bot_command_pull (%c%s)", BOT_COMMAND_CHAR, sep->arg[0]);

		// c->Message(0, "requires one of the following bot classes in your group:");
		// c->Message(1, "Cleric(10), Druid(12), Necromancer(12), Wizard(12), Magician(12), Enchanter(12) or Shaman(14)");

		return;
	}



	if (!strcasecmp(sep->arg[1], "pull")) {
		Mob *target = c->GetTarget();
		if (target == nullptr || target == c || target->IsBot() || (target->IsPet() && target->GetOwner() && target->GetOwner()->IsBot())) {
			c->Message(15, "You must select a monster");
			return;
		}

		if (c->IsGrouped()) {
			bool haspuller = false;
			Group *g = c->GetGroup();
			for (int i = 0; i < MAX_GROUP_MEMBERS; i++) {
				if (g && g->members[i] && g->members[i]->IsBot() && !strcasecmp(g->members[i]->GetName(), sep->arg[2])) {
					haspuller = true;
					Mob *puller = g->members[i];
					if (puller->CastToBot()->IsArcheryRange(target)) {
						puller->CastToBot()->BotGroupSay(puller->CastToBot(), "Trying to pull %s.", target->GetCleanName());
						puller->CastToBot()->BotRangedAttack(target);
					}
					else {
						puller->CastToBot()->BotGroupSay(puller->CastToBot(), "%s is out of range.", target->GetCleanName());
					}
				}
			}
			if (!haspuller) {
				c->Message(15, "You must have an Puller in your group.");
			}
		}
		return;
	}

	// TODO: Finalization
}

/*X*/
void bot_command_resistance(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Resistance];
	if (spell_list_fail(c, local_list, BCEnum::ST_Resistance) || command_alias_fail(c, "bot_command_resistance", sep->arg[0], "resistance"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s [type]", sep->arg[0]);
		c->Message(0, "type: fire, cold, poison, disease, magic or corruption");
		send_usage_required_bots(c, BCEnum::ST_Resistance);
		return;
	}

	Client* mtsp = MyTarget::AsSingle_ByPlayer(c);
	Client* mtgmp = MyTarget::AsGroupMember_ByPlayer(c);
	if (!mtsp && !mtgmp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	std::string resistance = sep->arg[1];

	BCEnum::RMask resistance_mask = BCEnum::RM_None;
	if (!resistance.compare("fire"))
		resistance_mask = BCEnum::RM_Fire;
	else if (!resistance.compare("cold"))
		resistance_mask = BCEnum::RM_Cold;
	else if (!resistance.compare("poison"))
		resistance_mask = BCEnum::RM_Poison;
	else if (!resistance.compare("disease"))
		resistance_mask = BCEnum::RM_Disease;
	else if (!resistance.compare("magic"))
		resistance_mask = BCEnum::RM_Magic;
	else if (!resistance.compare("corruption"))
		resistance_mask = BCEnum::RM_Corruption;

	if (resistance_mask == BCEnum::RM_None) {
		c->Message(15, "You must specify a resistance [type]");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STResistanceEntry* local_entry = (*iter_list)->SafeCastToResistance();
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;
		if ((local_entry->resist_mask & resistance_mask) != resistance_mask)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
		case BCEnum::TT_GroupV2:
			if (!mtsp)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtsp;
			break;
		case BCEnum::TT_GroupV1:
			if (!mtgmp)
				continue;
			b = MyBot::AsGroupMember_ByMinLevelAndClass(c, mtgmp, local_entry->spell_level, local_entry->caster_class);
			t = mtgmp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/*X*/
void bot_command_resurrect(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Resurrect];
	if (spell_list_fail(c, local_list, BCEnum::ST_Resurrect) || command_alias_fail(c, "bot_command_resurrect", sep->arg[0], "resurrect"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <corpse> %s ([ae])", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Resurrect);
		return;
	}

	Corpse* mtcp = MyTarget::AsCorpse_ByPlayer(c);
	if (!mtcp) {
		c->Message(15, "You must <target> a player's corpse to use this command");
		return;
	}

	bool cast_ae = false;
	if (!strcasecmp(sep->arg[1], "ae"))
		cast_ae = true;

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STBaseEntry* local_entry = *iter_list;
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Corpse:
			if (!mtcp)
				continue;
			if (cast_ae)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtcp;
			break;
		case BCEnum::TT_AECaster:
			if (!mtcp)
				continue;
			if (!cast_ae)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtcp;
			if (b) {
				b->GMMove(t->GetX(), t->GetY(), t->GetZ());
				b->FaceTarget(c);
			}
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/*X*/
void bot_command_rune(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Rune];
	if (spell_list_fail(c, local_list, BCEnum::ST_Rune) || command_alias_fail(c, "bot_command_rune", sep->arg[0], "rune"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Rune);
		return;
	}

	Client* mtsp = MyTarget::AsSingle_ByPlayer(c);
	Client* mtgmp = MyTarget::AsGroupMember_ByPlayer(c);
	if (!mtsp && !mtgmp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_ByGroupedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STBaseEntry* local_entry = *iter_list;
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
		case BCEnum::TT_GroupV2:
			if (!mtsp)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtsp;
			break;
		case BCEnum::TT_GroupV1:
			if (!mtgmp)
				continue;
			b = MyBot::AsGroupMember_ByMinLevelAndClass(c, mtgmp, local_entry->spell_level, local_entry->caster_class);
			t = mtgmp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/*xxxxx*/
void bot_command_send_home(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_SendHome];
	if (spell_list_fail(c, local_list, BCEnum::ST_SendHome) || command_alias_fail(c, "bot_command_send_home", sep->arg[0], "sendhome"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s ([group])", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_SendHome);
		return;
	}

	//
	return;
	//

	// Ascending order (by group_flag), Descending order (by 'tier' - implied), Ascending order (by spell_level)
	//const LocalData local_data[local_count] = {
	/*	{ [caster_class], [spell_level], [spell_id], [spell_name] }	*/
	//	{ WIZARD, 52, 1334, "Translocate: Group", false },
	//	{ WIZARD, 50, 1422, "Translocate", true }
	//};

	

	Client* t = nullptr;// player_as_single_target(c);
	if (!t) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	bool group_flag = !strcasecmp(sep->arg[1], "group") ? true : false;

	Bot* b = nullptr;

	int i = 0;
	for (; i < 0 /*local_count*/; ++i) {
		//if (local_data[i].group_flag != group_flag)
		//	continue;

		b = nullptr;// spawned_bot_by_minlevel_class(c, local_data[i].spell_level, local_data[i].caster_class);
		if (b) {
			b->InterruptSpell();
			//if (is_bot_in_client_group(t, b)) {
			//	Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", local_data[i].spell_name.c_str(), t->GetCleanName());
			//	if (!is_player_in_client_group(c, t))
			//		c->Message(MT_Tell, "%s tells you, Attempting to cast '%s' on %s", b->GetCleanName(), local_data[i].spell_name.c_str(), t->GetCleanName());
			//}
			//else {
			//	b->Say(0, "Attempting to cast '%s' on %s", local_data[i].spell_name.c_str(), t->GetCleanName());
			//}
			//b->CastSpell(local_data[i].spell_id, t->GetID(), 1, -1, -1);

			break;
		}
	}

	if (!b) {
		c->Message(15, "No spawned bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/*X*/
void bot_command_shrink(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Shrink];
	if (spell_list_fail(c, local_list, BCEnum::ST_Shrink) || command_alias_fail(c, "bot_command_shrink", sep->arg[0], "shrink"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Shrink);
		return;
	}

	// { GroupV2, SHAMAN, 64, 3393, "Tiny Terror" },

	Client* mtsp = MyTarget::AsSingle_ByPlayer(c);
	if (!mtsp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STBaseEntry* local_entry = *iter_list;
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
			if (!mtsp)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtsp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/*xxxxx*/
void bot_command_summon_corpse(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_SummonCorpse];
	

	// Ascending order (by max_target_level, 'tier' - implied), Descending order (by spell_level, caster_class)
	//const LocalData local_data[local_count] = {
	/*	{ [caster_class], [spell_level], [spell_id], [spell_name] }	*/
	//	{ 35, NECROMANCER, 12, 2213, "Lesser Summon Corpse" },
	//	{ 35, SHADOWKNIGHT, 12, 2213, "Lesser Summon Corpse" },
	//	{ 70, NECROMANCER, 57, 1773, "Conjure Corpse" },
	//	{ 70, SHADOWKNIGHT, 57, 1773, "Conjure Corpse" },
	//	{ 70, NECROMANCER, 35, 3, "Summon Corpse" },
	//	{ 70, SHADOWKNIGHT, 35, 3, "Summon Corpse" },
	//	{ 75, NECROMANCER, 71, 10042, "Exhumer's Call" },
	//	{ 75, SHADOWKNIGHT, 71, 10042, "Exhumer's Call" },
	//	{ 80, NECROMANCER, 76, 14823, "Procure Corpse" },
	//	{ 80, SHADOWKNIGHT, 76, 14823, "Procure Corpse" },
	//	{ 85, NECROMANCER, 81, 18928, "Reaper's Call" },
	//	{ 85, SHADOWKNIGHT, 81, 18928, "Reaper's Call" },
	//	{ 90, NECROMANCER, 86, 25555, "Reaper's Beckon" },
	//	{ 90, SHADOWKNIGHT, 86, 25555, "Reaper's Beckon" },
	//	{ 95, NECROMANCER, 91, 28632, "Reaper's Decree" },
	//	{ 95, SHADOWKNIGHT, 91, 28632, "Reaper's Decree" },
	//	{ 100, NECROMANCER, 96, 34662, "Reaper's Proclamation" },
	//	{ 100, SHADOWKNIGHT, 96, 34662, "Reaper's Proclamation" }
	/*	{ 105, NECROMANCER, 101, ?, "Stormreaper's Proclamation" },	*/
	/*	{ 105, SHADOWKNIGHT, 101, ?, "Stormreaper's Proclamation" }	*/
	//};
	
	if (spell_list_fail(c, local_list, BCEnum::ST_SummonCorpse) || command_alias_fail(c, "bot_command_summon_corpse", sep->arg[0], "summoncorpse"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_SummonCorpse);
		return;
	}

	//
	return;
	//

	

	Client* t = nullptr;// player_as_single_target(c);
	if (!t) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	Bot* b = nullptr;
	/*
	int i = 0;
	for (; i < local_count; ++i) {
		if (t->GetLevel() > local_data[i].max_target_level)
			continue;

		b = spawned_bot_by_minlevel_class(c, local_data[i].spell_level, local_data[i].caster_class);
		if (b) {
			b->InterruptSpell();
			
			Mob* target_proxy = b->GetTarget();
			b->SetTarget(t);

			if (is_bot_in_client_group(t, b)) {
				Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", local_data[i].spell_name.c_str(), t->GetCleanName());
				if (!is_player_in_client_group(c, t))
					c->Message(MT_Tell, "%s tells you, Attempting to cast '%s' on %s", b->GetCleanName(), local_data[i].spell_name.c_str(), t->GetCleanName());
			}
			else {
				b->Say(0, "Attempting to cast '%s' on %s", local_data[i].spell_name.c_str(), t->GetCleanName());
			}
			b->CastSpell(local_data[i].spell_id, t->GetID(), 1, -1, -1);

			b->SetTarget(target_proxy);
			target_proxy = nullptr;

			break;
		}
	}
	*/
	if (!b) {
		c->Message(15, "No spawned bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/*xxxxx*/
void bot_command_taunt(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_command_taunt", sep->arg[0], "taunt"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s ([on | off]) ([name])", sep->arg[0]);
		c->Message(0, "requires one of the following bot classes:");
		//c->Message(1, "Ranger(1), Druid(20) or Bard(35)");
		return;
	}

	//
	return;
	//


	bool taunt_flag = false;
	bool toggle_taunt = true;
	if (!strcasecmp(sep->arg[1], "on") || !strcasecmp(sep->arg[2], "on")) {
		taunt_flag = true;
		toggle_taunt = false;
	}
	else if (!strcasecmp(sep->arg[1], "off") || !strcasecmp(sep->arg[2], "off")) {
		toggle_taunt = false;
	}

	Mob* t = c->GetTarget();
	if ((!strcasecmp(sep->arg[1], "on") || !strcasecmp(sep->arg[1], "off")) && (!t || !t->IsBot() || t->CastToBot()->GetBotOwner() != c)) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}

	if (strcasecmp(sep->arg[1], "on") && strcasecmp(sep->arg[1], "off") && sep->arg[1][0] != '\0') {
		t = entity_list.GetBotByBotName(sep->arg[1]);
		if (!t) {
			c->Message(15, "You must [name] a bot that you own to use this command");
			return;
		}
	}

	Bot* b = t->CastToBot();

	if (toggle_taunt)
		b->SetTaunting(!b->IsTaunting());
	else
		b->SetTaunting(taunt_flag);

	//if (is_bot_in_client_group(c, b))
	//	Bot::BotGroupSay(b, "I am %s taunting", b->IsTaunting() ? "now" : "no longer");
	//else
	//	b->Say(0, "I am %s taunting", b->IsTaunting() ? "now" : "no longer");

	// TODO: Finalization
}

/*X*/
void bot_command_track(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_command_track", sep->arg[0], "track"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s (Ranger: [all | rare | local])", sep->arg[0]);
		c->Message(0, "requires one of the following bot classes:");
		c->Message(1, "Ranger(1), Druid(20) or Bard(35)");
		return;
	}

	std::string tracking_scope = sep->arg[1];

	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	Bot* b = MyBot::AsSpawned_ByClass(c, sbl, RANGER);
	if (!b && tracking_scope.empty())
		b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, 20, DRUID);
	if (!b && tracking_scope.empty())
		b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, 35, BARD);
	if (!b) {
		c->Message(15, "No spawned bots are capable of performing this action");
		return;
	}

	int base_distance = 0;
	bool track_named = false;
	std::string tracking_msg;
	switch (b->GetClass()) {
	case RANGER:
		if (!tracking_scope.compare("local")) {
			base_distance = 30;
			tracking_msg = "Tracking...";
		}
		else if (!tracking_scope.compare("rare")) {
			base_distance = 80;
			bool track_named = true;
			tracking_msg = "Far tracking by name...";
		}
		else { // default to 'all'
			base_distance = 80;
			b->BotGroupSay(b, "Far tracking...");
		}
		break;
	case DRUID:
		base_distance = 30;
		tracking_msg = "Tracking...";
		break;
	case BARD:
		base_distance = 20;
		tracking_msg = "Near tracking...";
		break;
	default:
		return;
	}
	if (!base_distance)
		return;

	b->InterruptSpell();
	b->BotGroupSay(b, tracking_msg.c_str());
	entity_list.ShowSpawnWindow(c, (c->GetLevel() * base_distance), track_named);

	// TODO: Finalization
}

/*X*/
void bot_command_water_breathing(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_WaterBreathing];
	if (spell_list_fail(c, local_list, BCEnum::ST_WaterBreathing) || command_alias_fail(c, "bot_command_water_breathing", sep->arg[0], "waterbreathing"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_WaterBreathing);
		return;
	}

	Client* mtsp = MyTarget::AsSingle_ByPlayer(c);
	if (!mtsp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	std::list<Bot*> sbl;
	MyBot::Populate_BySpawnedBots(c, sbl);
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STBaseEntry* local_entry = *iter_list;
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_Single:
		case BCEnum::TT_GroupV2:
			if (!mtsp)
				continue;
			b = MyBot::AsSpawned_ByMinLevelAndClass(c, sbl, local_entry->spell_level, local_entry->caster_class);
			t = mtsp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots are capable of performing this action");
		return;
	}

	// TODO: Finalization
}


/*
 * bot subcommands go below here
 */
/* OK */
void bot_subcommand_bot_appearance(Client *c, const Seperator *sep)
{
	// Functionality is probably ok with all of the subcommands...
	// Behavior is odd due to actual values..and maybe the use of 'Illusion' packets
	
	const std::list<std::string> subcommand_list = {
		"botbeardcolor", "botbeardstyle", "botdetails", "boteyes", "botface",
		"bothaircolor", "bothairstyle", "botheritage", "bottattoo"
	};
	
	if (command_alias_fail(c, "bot_subcommand_bot_appearance", sep->arg[0], "botappearance"))
		return;

	c->Message(0, "Available bot appearance subcommands:");
	
	send_available_subcommands(c, subcommand_list);
}

/* PROBABLY OK */
void bot_subcommand_bot_beard_color(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_beard_color", sep->arg[0], "botbeardcolor"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s [value:0-?] (Dwarves or male bots only)", sep->arg[0]);
		return;
	}

	Bot* b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}

	if (!sep->IsNumber(1)) {
		c->Message(15, "A numeric [value] is required to use this command");
		return;
	}

	uint8 uvalue = atoi(sep->arg[1]);

	BCEnum::AFType fail_type = BCEnum::AFT_None;
	if (b->GetGender() != MALE && b->GetRace() != DWARF)
		fail_type = BCEnum::AFT_GenderRace;
	else if (uvalue > (uint8)~0) // set proper value
		fail_type = BCEnum::AFT_Value;
	else
		b->SetBeardColor(uvalue);

	if (helper_bot_appearance_fail(c, b, fail_type, "beard color"))
		return;

	helper_bot_appearance_final(c, b);

	// TODO: Finalization
}

/* PROBABLY OK */
void bot_subcommand_bot_beard_style(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_beard_style", sep->arg[0], "botbeardstyle"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s [value:0-?] (Dwarves or male bots only)", sep->arg[0]);
		return;
	}

	Bot* b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}

	if (!sep->IsNumber(1)) {
		c->Message(15, "A numeric [value] is required to use this command");
		return;
	}

	uint8 uvalue = atoi(sep->arg[1]);

	BCEnum::AFType fail_type = BCEnum::AFT_None;
	if (b->GetGender() != MALE && b->GetRace() != DWARF)
		fail_type = BCEnum::AFT_GenderRace;
	else if (uvalue > (uint8)~0) // set proper value
		fail_type = BCEnum::AFT_Value;
	else
		b->SetBeard(uvalue);

	if (helper_bot_appearance_fail(c, b, fail_type, "beard style"))
		return;

	helper_bot_appearance_final(c, b);

	// TODO: Finalization
}

/* OK */
void bot_subcommand_bot_camp(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_camp", sep->arg[0], "botcamp"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s ([ownergroup | targetgroup | spawned/all])", sep->arg[0]);
		return;
	}

	Bot* b = nullptr;
	SelectActionableBots::SABType multitarget = SelectActionableBots::Single;
	std::list<Bot*> sbl;
	if (!SelectActionableBots::SetActionableBots(c, sep->arg[1], multitarget, b, sbl))
		return;

	for (std::list<Bot*>::iterator iter = sbl.begin(); iter != sbl.end(); ++iter)
		(*iter)->Camp();

	//sbl.clear();
}

/* OK */
void bot_subcommand_bot_clone(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_clone", sep->arg[0], "botclone"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s [name]", sep->arg[0]);
		return;
	}

	Bot* b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}
	if (!b->GetBotID()) {
		c->Message(15, "An unknown error has occured - BotName: %s, BotID: %u", b->GetCleanName(), b->GetBotID());
		Log.Out(Logs::General, Logs::Commands, "bot_command_clone(): - Error: Active bot reported invalid ID (BotName: %s, BotID: %u, OwnerName: %s, OwnerID: %u, AcctName: %s, AcctID: %u)",
			b->GetCleanName(), b->GetBotID(), c->GetCleanName(), c->CharacterID(), c->AccountName(), c->AccountID());
		return;
	}

	if (sep->arg[1][0] == '\0' || sep->IsNumber(1)) {
		c->Message(15, "You must [name] your bot clone");
		return;
	}
	std::string bot_name = sep->arg[1];

	if (!Bot::IsValidName(bot_name)) {
		c->Message(15, "'%s' is an invalid name. You may only use characters 'A-Z', 'a-z' and '_'", bot_name.c_str());
		return;
	}

	std::string TempErrorMessage;

	if (!Bot::IsBotNameAvailable(bot_name.c_str(), &TempErrorMessage)) {
		if (!TempErrorMessage.empty())
			c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		c->Message(15, "The name %s is already being used. Please choose a different name", bot_name.c_str());
		return;
	}

	uint32 mbc = RuleI(Bots, CreationLimit);
	if (Bot::CreatedBotCount(c->CharacterID(), &TempErrorMessage) >= mbc) {
		if (!TempErrorMessage.empty())
			c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		c->Message(15, "You have reached the maximum limit of %i bots", mbc);
		return;
	}

	std::string data_query = StringFormat(
		"INSERT INTO `bot_data`"
		" ("
		"`owner_id`,"
		" `spells_id`,"
		" `name`,"
		" `last_name`,"
		" `title`,"
		" `suffix`,"
		" `zone_id`,"
		" `gender`,"
		" `race`,"
		" `class`,"
		" `level`,"
		" `deity`,"
		" `creation_day`,"
		" `last_spawn`,"
		" `time_spawned`,"
		" `size`,"
		" `face`,"
		" `hair_color`,"
		" `hair_style`,"
		" `beard`,"
		" `beard_color`,"
		" `eye_color_1`,"
		" `eye_color_2`,"
		" `drakkin_heritage`,"
		" `drakkin_tattoo`,"
		" `drakkin_details`,"
		" `ac`,"
		" `atk`,"
		" `hp`,"
		" `mana`,"
		" `str`,"
		" `sta`,"
		" `cha`,"
		" `dex`,"
		" `int`,"
		" `agi`,"
		" `wis`,"
		" `fire`,"
		" `cold`,"
		" `magic`,"
		" `poison`,"
		" `disease`,"
		" `corruption`,"
		" `show_helm`,"
		" `follow_distance`"
		")"
		" SELECT"
		" bd.`owner_id`,"
		" bd.`spells_id`,"
		" '%s',"
		" '',"
		" bd.`title`,"
		" bd.`suffix`,"
		" bd.`zone_id`,"
		" bd.`gender`,"
		" bd.`race`,"
		" bd.`class`,"
		" bd.`level`,"
		" bd.`deity`,"
		" UNIX_TIMESTAMP(),"
		" UNIX_TIMESTAMP(),"
		" '0',"
		" bd.`size`,"
		" bd.`face`,"
		" bd.`hair_color`,"
		" bd.`hair_style`,"
		" bd.`beard`,"
		" bd.`beard_color`,"
		" bd.`eye_color_1`,"
		" bd.`eye_color_2`,"
		" bd.`drakkin_heritage`,"
		" bd.`drakkin_tattoo`,"
		" bd.`drakkin_details`,"
		" bd.`ac`,"
		" bd.`atk`,"
		" bd.`hp`,"
		" bd.`mana`,"
		" bd.`str`,"
		" bd.`sta`,"
		" bd.`cha`,"
		" bd.`dex`,"
		" bd.`int`,"
		" bd.`agi`,"
		" bd.`wis`,"
		" bd.`fire`,"
		" bd.`cold`,"
		" bd.`magic`,"
		" bd.`poison`,"
		" bd.`disease`,"
		" bd.`corruption`,"
		" bd.`show_helm`,"
		" bd.`follow_distance`"
		" FROM `bot_data` bd"
		" WHERE"
		" bd.`bot_id` = '%u'",
		bot_name.c_str(),
		b->GetBotID()
	);
	auto data_results = database.QueryDatabase(data_query);
	if (!data_results.Success()) {
		c->Message(15, "Clone creation of bot '%s' failed...", b->GetCleanName());
		return;
	}

	uint32 bot_id = data_results.LastInsertedID();
	if (!bot_id) {
		c->Message(15, "Bot clone '%s' returned an id of '0' from the database...", bot_name.c_str());
		return;
	}

	std::string inv_query = StringFormat(
		"INSERT INTO `bot_inventories`"
		" ("
		"bot_id,"
		" `slot_id`,"
		" `item_id`,"
		" `inst_charges`,"
		" `inst_color`,"
		" `inst_no_drop`,"
		" `inst_custom_data`,"
		" `ornament_icon`,"
		" `ornament_id_file`,"
		" `ornament_hero_model`,"
		" `augment_1`,"
		" `augment_2`,"
		" `augment_3`,"
		" `augment_4`,"
		" `augment_5`,"
		" `augment_6`"
		")"
		" SELECT"
		" '%u' bot_id,"
		" bi.`slot_id`,"
		" bi.`item_id`,"
		" bi.`inst_charges`,"
		" bi.`inst_color`,"
		" bi.`inst_no_drop`,"
		" bi.`inst_custom_data`,"
		" bi.`ornament_icon`,"
		" bi.`ornament_id_file`,"
		" bi.`ornament_hero_model`,"
		" bi.`augment_1`,"
		" bi.`augment_2`,"
		" bi.`augment_3`,"
		" bi.`augment_4`,"
		" bi.`augment_5`,"
		" bi.`augment_6`"
		" FROM `bot_inventories` bi"
		" WHERE"
		" bi.`bot_id` = '%u'",
		bot_id,
		b->GetBotID()
	);
	auto inv_results = database.QueryDatabase(inv_query);
	if (!inv_results.Success()) {
		c->Message(15, "Inventory import for bot clone '%s' failed...", bot_name.c_str());
		return;
	}

	c->Message(0, "Bot '%s' was successfully cloned to bot '%s'", b->GetCleanName(), bot_name.c_str());
}

/* OK */
void bot_subcommand_bot_create(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_create", sep->arg[0], "botcreate"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s [name] [class] [race] [gender]", sep->arg[0]);
		c->Message(0, "class: %u(WAR), %u(CLR), %u(PAL), %u(RNG), %u(SHD), %u(DRU), %u(MNK), %u(BRD), %u(ROG), %u(SHM), %u(NEC), %u(WIZ), %u(MAG), %u(ENC), %u(BST), %u(BER)",
			WARRIOR, CLERIC, PALADIN, RANGER, SHADOWKNIGHT, DRUID, MONK, BARD, ROGUE, SHAMAN, NECROMANCER, WIZARD, MAGICIAN, ENCHANTER, BEASTLORD, BERSERKER);
		c->Message(0, "race: %u(HUM), %u(BAR), %u(ERU), %u(ELF), %u(HIE), %u(DEF), %u(HEF), %u(DWF), %u(TRL), %u(OGR), %u(HFL), %u(GNM), %u(IKS), %u(VAH), %u(FRG), %u(DRK)",
			HUMAN, BARBARIAN, ERUDITE, WOOD_ELF, HIGH_ELF, DARK_ELF, HALF_ELF, DWARF, TROLL, OGRE, HALFLING, GNOME, IKSAR, VAHSHIR, FROGLOK, DRAKKIN);
		c->Message(0, "gender: %u(M), %u(F)", MALE, FEMALE);
		return;
	}

	if (sep->arg[1][0] == '\0' || sep->IsNumber(1)) {
		c->Message(15, "You must [name] your bot");
		return;
	}
	std::string bot_name = sep->arg[1];

	if (sep->arg[2][0] == '\0' || !sep->IsNumber(2)) {
		c->Message(15, "class: %u(WAR), %u(CLR), %u(PAL), %u(RNG), %u(SHD), %u(DRU), %u(MNK), %u(BRD), %u(ROG), %u(SHM), %u(NEC), %u(WIZ), %u(MAG), %u(ENC), %u(BST), %u(BER)",
			WARRIOR, CLERIC, PALADIN, RANGER, SHADOWKNIGHT, DRUID, MONK, BARD, ROGUE, SHAMAN, NECROMANCER, WIZARD, MAGICIAN, ENCHANTER, BEASTLORD, BERSERKER);
		return;
	}
	uint8 bot_class = atoi(sep->arg[2]);

	if (sep->arg[3][0] == '\0' || !sep->IsNumber(3)) {
		c->Message(15, "race: %u(HUM), %u(BAR), %u(ERU), %u(ELF), %u(HIE), %u(DEF), %u(HEF), %u(DWF), %u(TRL), %u(OGR), %u(HFL), %u(GNM), %u(IKS), %u(VAH), %u(FRG), %u(DRK)",
			HUMAN, BARBARIAN, ERUDITE, WOOD_ELF, HIGH_ELF, DARK_ELF, HALF_ELF, DWARF, TROLL, OGRE, HALFLING, GNOME, IKSAR, VAHSHIR, FROGLOK, DRAKKIN);
		return;
	}
	uint16 bot_race = atoi(sep->arg[3]);

	if (sep->arg[4][0] == '\0') {
		c->Message(15, "gender: %u(M), %u(F)", MALE, FEMALE);
		return;
	}
	uint8 bot_gender = atoi(sep->arg[4]);

	helper_bot_create(c, bot_name, bot_class, bot_race, bot_gender);
}

/* OK */
void bot_subcommand_bot_delete(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_delete", sep->arg[0], "botdelete"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s", sep->arg[0]);
		return;
	}

	Bot* b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}

	std::string TempErrorMessage;

	b->DeleteBot(&TempErrorMessage);
	if (!TempErrorMessage.empty()) {
		c->Message(13, "Failed to delete '%s' due to database error: %s", b->GetCleanName(), TempErrorMessage.c_str());
		return;
	}

	uint32 bid = b->GetBotID();
	std::string bot_name = b->GetCleanName();

	b->Camp(false);

	c->Message(15, "Successfully deleted bot '%s' (id: %i)", bot_name.c_str(), bid);
}

/* PROBABLY OK */
void bot_subcommand_bot_details(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_details", sep->arg[0], "botdetails"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s [value:0-7] (Drakkin bots only)", sep->arg[0]);
		return;
	}

	Bot* b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}

	if (!sep->IsNumber(1)) {
		c->Message(15, "A numeric [value] is required to use this command");
		return;
	}

	uint32 uvalue = atoi(sep->arg[1]);

	BCEnum::AFType fail_type = BCEnum::AFT_None;
	if (b->GetRace() != DRAKKIN)
		fail_type = BCEnum::AFT_Race;
	else if (uvalue > (uint32)~0) // set proper value
		fail_type = BCEnum::AFT_Value;
	else
		b->SetDrakkinDetails(uvalue);

	if (helper_bot_appearance_fail(c, b, fail_type, "details"))
		return;

	helper_bot_appearance_final(c, b);

	// TODO: Finalization
}

/*X*/
void bot_subcommand_bot_dye_armor(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_dye_armor", sep->arg[0], "botdyearmor"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s [mat_slot] [red: 0-255] [green: 0-255] [blue: 0-255] ([ownergroup | targetgroup | spawned | all])", sep->arg[0]);
		c->Message(0, "mat_slot: %c(All), %i(Head), %i(Chest), %i(Arms), %i(Wrists), %i(Hands), %i(Legs), %i(Feet)",
			'*', MaterialHead, MaterialChest, MaterialArms, MaterialWrist, MaterialHands, MaterialLegs, MaterialFeet);
		return;
	}

	Bot* b = nullptr;
	SelectActionableBots::SABType multitarget = SelectActionableBots::Single;
	std::list<Bot*> sbl;
	if (!SelectActionableBots::SetActionableBots(c, sep->arg[5], multitarget, b, sbl))
		return;

	uint8 material_slot = _MaterialInvalid;
	int16 slot_id = INVALID_INDEX;

	bool dye_all = (sep->arg[1][0] == '*');
	if (!dye_all) {
		material_slot = atoi(sep->arg[1]);
		slot_id = Inventory::CalcSlotFromMaterial(material_slot);

		if (!sep->IsNumber(1) || slot_id == INVALID_INDEX || material_slot > MaterialFeet) {
			c->Message(15, "Valid [mat_slot]s for this command are:");
			c->Message(15, "mat_slot: %c(All), %i(Head), %i(Chest), %i(Arms), %i(Wrists), %i(Hands), %i(Legs), %i(Feet)",
				'*', MaterialHead, MaterialChest, MaterialArms, MaterialWrist, MaterialHands, MaterialLegs, MaterialFeet);
			return;
		}
	}

	uint32 red_value = atoi(sep->arg[2]);
	if (!sep->IsNumber(2) || red_value > 255) {
		c->Message(15, "Valid [red] values for this command are [0-255]");
		return;
	}

	uint32 green_value = atoi(sep->arg[3]);
	if (!sep->IsNumber(3) || green_value > 255) {
		c->Message(15, "Valid [green] values for this command are [0-255]");
		return;
	}

	uint32 blue_value = atoi(sep->arg[4]);
	if (!sep->IsNumber(4) || blue_value > 255) {
		c->Message(15, "Valid [blue] values for this command are [0-255]");
		return;
	}

	uint32 rgb_value = (red_value << 16) | (green_value << 8) | (blue_value);

	for (std::list<Bot*>::iterator iter = sbl.begin(); iter != sbl.end(); ++iter) {
		if (!(*iter))
			continue;

		if (!(*iter)->DyeArmor(slot_id, rgb_value, dye_all, (multitarget != SelectActionableBots::All))) {
			c->Message(15, "Failed to change armor color for '%s' due to unknown cause", (*iter)->GetCleanName());
			return;
		}
	}

	if (multitarget == SelectActionableBots::All) {
		std::string where_clause;
		if (dye_all)
			where_clause = StringFormat(" WHERE bi.`slot_id` IN (%u, %u, %u, %u, %u, %u, %u, %u)",
			MainHead, MainChest, MainArms, MainWrist1, MainWrist2, MainHands, MainLegs, MainFeet);
		else
			where_clause = StringFormat(" WHERE bi.`slot_id` = '%u'", slot_id);

		std::string query = StringFormat(
			"UPDATE `bot_inventories` bi"
			" INNER JOIN `bot_data` bd"
			" ON bd.`owner_id` = '%u'"
			" SET bi.`inst_color` = '%u'"
			" %s"
			" AND bi.`bot_id` = bd.`bot_id`",
			c->CharacterID(),
			rgb_value,
			where_clause.c_str()
			);

		auto results = database.QueryDatabase(query);
		if (!results.Success()) {
			c->Message(15, "Failed to save dye armor changes for your bots due to unknown cause");
			return;
		}
	}

	// TODO: Finalization
}

/* PROBABLY OK */
void bot_subcommand_bot_eyes(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_eyes", sep->arg[0], "boteyes"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s [value:0-11]", sep->arg[0]);
		return;
	}

	Bot* b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}

	if (!sep->IsNumber(1)) {
		c->Message(15, "A numeric [value] is required to use this command");
		return;
	}

	uint8 uvalue = atoi(sep->arg[1]);

	BCEnum::AFType fail_type = BCEnum::AFT_None;
	if (uvalue > (uint8)~0) { // set proper value
		fail_type = BCEnum::AFT_Value;
	}
	else {
		b->SetEyeColor1(uvalue);
		b->SetEyeColor2(uvalue);
	}

	if (helper_bot_appearance_fail(c, b, fail_type, "eyes"))
		return;

	helper_bot_appearance_final(c, b);

	// TODO: Finalization
}

/* PROBABLY OK */
void bot_subcommand_bot_face(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_face", sep->arg[0], "botface"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s [value:0-?]", sep->arg[0]);
		return;
	}

	Bot* b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}

	if (!sep->IsNumber(1)) {
		c->Message(15, "A numeric [value] is required to use this command");
		return;
	}

	uint8 uvalue = atoi(sep->arg[1]);

	BCEnum::AFType fail_type = BCEnum::AFT_None;
	if (uvalue > (uint8)~0) // set proper value
		fail_type = BCEnum::AFT_Value;
	else
		b->SetLuclinFace(uvalue);

	if (helper_bot_appearance_fail(c, b, fail_type, "face"))
		return;

	helper_bot_appearance_final(c, b);

	// TODO: Finalization
}

/* PROBABLY OK */
void bot_subcommand_bot_hair_color(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_hair_color", sep->arg[0], "bothaircolor"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s [value:0-?]", sep->arg[0]);
		return;
	}

	Bot* b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}

	if (!sep->IsNumber(1)) {
		c->Message(15, "A numeric [value] is required to use this command");
		return;
	}

	uint8 uvalue = atoi(sep->arg[1]);

	BCEnum::AFType fail_type = BCEnum::AFT_None;
	if (uvalue > (uint8)~0) // set proper value
		fail_type = BCEnum::AFT_Value;
	else
		b->SetHairColor(uvalue);

	if (helper_bot_appearance_fail(c, b, fail_type, "hair color"))
		return;

	helper_bot_appearance_final(c, b);

	// TODO: Finalization
}

/* PROBABLY OK */
void bot_subcommand_bot_hairstyle(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_hairstyle", sep->arg[0], "bothairstyle"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s [value:0-?]", sep->arg[0]);
		return;
	}

	Bot* b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}

	if (!sep->IsNumber(1)) {
		c->Message(15, "A numeric [value] is required to use this command");
		return;
	}

	uint8 uvalue = atoi(sep->arg[1]);

	BCEnum::AFType fail_type = BCEnum::AFT_None;
	if (uvalue > (uint8)~0) // set proper value
		fail_type = BCEnum::AFT_Value;
	else
		b->SetHairStyle(uvalue);

	if (helper_bot_appearance_fail(c, b, fail_type, "hair style"))
		return;

	helper_bot_appearance_final(c, b);

	// TODO: Finalization
}

/* PROBABLY OK */
void bot_subcommand_bot_heritage(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_heritage", sep->arg[0], "botheritage"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s [value:0-6] (Drakkin bots only)", sep->arg[0]);
		return;
	}

	Bot* b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}

	if (!sep->IsNumber(1)) {
		c->Message(15, "A numeric [value] is required to use this command");
		return;
	}

	uint32 uvalue = atoi(sep->arg[1]);

	BCEnum::AFType fail_type = BCEnum::AFT_None;
	if (b->GetRace() != DRAKKIN)
		fail_type = BCEnum::AFT_Race;
	else if (uvalue > (uint32)~0) // set proper value
		fail_type = BCEnum::AFT_Value;
	else
		b->SetDrakkinHeritage(uvalue);

	if (helper_bot_appearance_fail(c, b, fail_type, "heritage"))
		return;

	helper_bot_appearance_final(c, b);

	// TODO: Finalization
}

/* OK */
void bot_subcommand_bot_inspect_message(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_inspect_message", sep->arg[0], "botinspectmessage"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s [set | clear] ([ownergroup | targetgroup | spawned | all])", sep->arg[0]);
		c->Message(13, "Notes:");
		if (c->GetClientVersion() >= ClientVersion::SoF) {
			c->Message(1, "- Self-inspect and type your bot's inspect message");
			c->Message(1, "- Close the self-inspect window to update the server");
			c->Message(1, "- Type '%s set' to set the bot's message", sep->arg[0]);
		}
		else {
			c->Message(1, "- Self-inspect and type your bot's inspect message");
			c->Message(1, "- Close the self-inspect window");
			c->Message(1, "- Self-inspect again to update the server");
			c->Message(1, "- Type '%s set' to set the bot's message", sep->arg[0]);
		}
		return;
	}

	bool set_flag = false;
	if (!strcasecmp(sep->arg[1], "set")) {
		set_flag = true;
	}
	else if (strcasecmp(sep->arg[1], "clear")) {
		c->Message(15, "This command requires a [set | clear] argument");
		return;
	}

	Bot* b = nullptr;
	SelectActionableBots::SABType multitarget = SelectActionableBots::Single;
	std::list<Bot*> sbl;
	if (!SelectActionableBots::SetActionableBots(c, sep->arg[2], multitarget, b, sbl))
		return;

	const InspectMessage_Struct& cms = c->GetInspectMessage();

	int bot_count = 0;
	for (std::list<Bot*>::iterator iter = sbl.begin(); iter != sbl.end(); ++iter) {
		if (!(*iter))
			continue;

		InspectMessage_Struct& bms = (*iter)->GetInspectMessage();
		memset(&bms, 0, sizeof(InspectMessage_Struct));
		if (set_flag)
			memcpy(&bms, &cms, sizeof(InspectMessage_Struct));

		if (multitarget != SelectActionableBots::All)
			database.SetBotInspectMessage((*iter)->GetBotID(), &bms);

		++bot_count;
	}

	if (multitarget == SelectActionableBots::All) {
		InspectMessage_Struct bms;
		memset(&bms, 0, sizeof(InspectMessage_Struct));
		if (set_flag)
			memcpy(&bms, &cms, sizeof(InspectMessage_Struct));

		std::string query = StringFormat(
			"UPDATE `bot_inspect_messages` bim"
			" INNER JOIN `bot_data` bd"
			" ON bd.`owner_id` = '%u'"
			" SET bim.`inspect_message` = '%s'"
			" WHERE bim.`bot_id` = bd.`bot_id`",
			c->CharacterID(),
			&bms
			);
		auto results = database.QueryDatabase(query);
		if (!results.Success()) {
			c->Message(15, "Failed to save inspect message changes for your bots due to unknown cause");
			return;
		}

		c->Message(0, "%s all of your bot inspect messages", set_flag ? "Set" : "Cleared");
	}
	else {
		c->Message(0, "%s %i of your spawned bot inspect messages", set_flag ? "Set" : "Cleared", bot_count);
	}

	// TODO: Finalization
}

/* OK */
void bot_subcommand_bot_list(Client *c, const Seperator *sep)
{
	enum { FilterClass, FilterRace, FilterName, FilterCount, MaskClass = (1 << FilterClass), MaskRace = (1 << FilterRace), MaskName = (1 << FilterName) };

	if (command_alias_fail(c, "bot_subcommand_bot_list", sep->arg[0], "botlist"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s (class [value]) (race [value]) (name [name: partial-full])", sep->arg[0]);
		c->Message(0, "Note: filter criteria is an orderless, either/or option");
		return;
	}

	// Seperator arg count is defaulted to 10..so, the limit is 10, unless the command processor is changed to pass a larger value (4 filter options)
	for (int i = 1; i < (FilterCount * 2); i += 2) {
		if (sep->arg[i][0] != '\0' && strcasecmp(sep->arg[i], "name") && !sep->IsNumber(i + 1)) {
			c->Message(15, "A numeric value is required to use the filter proprty of this command (f: '%s', v: '%s')", sep->arg[i], sep->arg[i + 1]);
			return;
		}
	}

	uint32 filter_value[FilterCount];
	int name_criteria_arg = 0;
	memset(&filter_value, 0, sizeof(uint32) * FilterCount);

	int filter_mask = 0;
	for (int i = 1; i < (FilterCount * 2); i += 2) {
		if (!strcasecmp(sep->arg[i], "class")) {
			filter_mask |= MaskClass;
			filter_value[FilterClass] = atoi(sep->arg[i + 1]);
			continue;
		}
		if (!strcasecmp(sep->arg[i], "race")) {
			filter_mask |= MaskRace;
			filter_value[FilterRace] = atoi(sep->arg[i + 1]);
			continue;
		}
		if (!strcasecmp(sep->arg[i], "name")) {
			filter_mask |= MaskName;
			name_criteria_arg = (i + 1);
			continue;
		}
	}

	std::string TempErrorMessage;
	std::list<BotsAvailableList> dbbl = Bot::GetBotList(c->CharacterID(), &TempErrorMessage);

	if (!TempErrorMessage.empty()) {
		c->Message(15, "Failed to load 'BotsAvailableList' due to unknown cause");
		return;
	}
	if (dbbl.empty()) {
		c->Message(15, "You have no bots");
		return;
	}

	int bot_count = 0;
	for (std::list<BotsAvailableList>::iterator iter = dbbl.begin(); iter != dbbl.end(); ++iter) {
		if (filter_mask) {
			if ((filter_mask & MaskClass) && filter_value[FilterClass] != iter->BotClass)
				continue;
			if ((filter_mask & MaskRace) && filter_value[FilterRace] != iter->BotRace)
				continue;
			if (filter_mask & MaskName) {
				std::string name_criteria = sep->arg[name_criteria_arg];
				std::transform(name_criteria.begin(), name_criteria.end(), name_criteria.begin(), ::tolower);
				std::string name_check = iter->BotName;
				std::transform(name_check.begin(), name_check.end(), name_check.begin(), ::tolower);
				if (name_check.find(name_criteria) == std::string::npos)
					continue;
			}
		}

		c->Message(1, "%s is a level %u %s %s",
			iter->BotName, iter->BotLevel, Bot::RaceIdToString(iter->BotRace).c_str(), Bot::ClassIdToString(iter->BotClass).c_str());

		++bot_count;
	}

	if (!bot_count) {
		c->Message(15, "You have no bots meeting this criteria");
		return;
	}
}

/**/
void bot_subcommand_bot_out_of_combat(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_out_of_combat", sep->arg[0], "botoutofcombat"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s", sep->arg[0]);
		return;
	}

	//
	return;
	//


	if (!strcasecmp(sep->arg[1], "usage")) {
		if (!strcasecmp(sep->arg[0], "bardoutofcombat"))
			c->Message(0, "usage: %cbardoutofcombat", BOT_COMMAND_CHAR);
		else if (!strcasecmp(sep->arg[0], "outofcombat"))
			c->Message(0, "usage: %coutofcombat", BOT_COMMAND_CHAR);
		else
			c->Message(15, "Undefined linker usage in bot_command_out_of_combat (%c%s)", BOT_COMMAND_CHAR, sep->arg[0]);

		// c->Message(0, "requires one of the following bot classes in your group:");
		// c->Message(1, "Cleric(10), Druid(12), Necromancer(12), Wizard(12), Magician(12), Enchanter(12) or Shaman(14)");

		return;
	}




	if (!strcasecmp(sep->arg[1], "bardoutofcombat")) {
		bool useOutOfCombatSongs = false;
		if (sep->arg[2] && sep->arg[3]){
			if (!strcasecmp(sep->arg[2], "on"))
				useOutOfCombatSongs = true;
			else if (!strcasecmp(sep->arg[2], "off"))
				useOutOfCombatSongs = false;
			else {
				c->Message(0, "Usage #bot bardoutofcombat [on|off]");
				return;
			}

			Mob *target = c->GetTarget();
			if (target && target->IsBot() && (c == target->GetOwner()->CastToClient())) {
				Bot* bardBot = target->CastToBot();
				if (bardBot) {
					bardBot->SetBardUseOutOfCombatSongs(useOutOfCombatSongs);
					c->Message(0, "Bard use of out of combat songs updated.");
				}
			}
			else
				c->Message(0, "Your target must be a bot that you own.");
		}
		else
			c->Message(0, "Usage #bot bardoutofcombat [on|off]");
		return;
	}

	// TODO: Logging
}

/* OK */
void bot_subcommand_bot_report(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_report", sep->arg[0], "botreport"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s ([spawned])", sep->arg[0]);
		return;
	}

	std::list<Bot*> sbl;
	if (!strcasecmp(sep->arg[1], "spawned")) {
		MyBot::Populate_BySpawnedBots(c, sbl);
	}
	else {
		Bot* b = MyBot::AsTarget_ByBot(c);
		if (b)
			sbl.push_front(b);
	}
	if (sbl.empty()) {
		c->Message(15, "You currently have no spawned bots");
		return;
	}

	for (std::list<Bot*>::iterator iter = sbl.begin(); iter != sbl.end(); ++iter) {
		Bot* b = *iter;
		if (!b)
			continue;

		bool fighter_flag = (b->GetClass() == WARRIOR || b->GetClass() == MONK || b->GetClass() == BARD || b->GetClass() == BERSERKER || b->GetClass() == ROGUE);

		std::string report_msg = StringFormat("%s %s reports", Bot::ClassIdToString(b->GetClass()).c_str(), b->GetCleanName());
		report_msg.append(StringFormat(": %3.1f%% health", b->GetHPRatio()));
		if (!fighter_flag)
			report_msg.append(StringFormat(": %3.1f%% mana", b->GetManaRatio()));

		c->Message(1, "%s", report_msg.c_str());
	}
}

/* ALMOST OK */
void bot_subcommand_bot_spawn(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_spawn", sep->arg[0], "botspawn"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s [name]", sep->arg[0]);
		return;
	}

	int rule_level = RuleI(Bots, MinCharacterLevel);
	if (c->GetLevel() < rule_level) {
		c->Message(15, "You must be level %i to use bots", rule_level);
		return;
	}

	if (c->GetFeigned()) {
		c->Message(15, "You can not spawn a bot while feigned");
		return;
	}

	std::string TempErrorMessage;

	int sbc = Bot::SpawnedBotCount(c->CharacterID(), &TempErrorMessage);
	if (!TempErrorMessage.empty()) {
		c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		return;
	}

	int rule_limit = RuleI(Bots, SpawnLimit);
	if (sbc >= rule_limit && !c->GetGM()) {
		c->Message(15, "You can not have more than %i spawned bots", rule_limit);
		return;
	}

	if (RuleB(Bots, QuestableSpawnLimit) && !c->GetGM()) {
		int abc = Bot::AllowedBotSpawns(c->CharacterID(), &TempErrorMessage);
		if (!TempErrorMessage.empty()) {
			c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
			return;
		}
		if (!abc) {
			c->Message(15, "You are not currently allowed any spawned bots");
			return;
		}
		if (sbc >= abc) {
			c->Message(15, "You have reached your current limit of %i spawned bots", abc);
			return;
		}
	}

	if (sep->arg[1][0] == '\0' || sep->IsNumber(1)) {
		c->Message(15, "You must specify a [name] to use this command");
		return;
	}
	std::string bot_name = sep->arg[1];

	uint32 bid = Bot::GetBotIDByBotName(bot_name);
	if (Bot::GetBotOwnerCharacterID(bid, &TempErrorMessage) != c->CharacterID()) {
		if (!TempErrorMessage.empty())
			c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		c->Message(15, "You don't own a bot named '%s'", bot_name.c_str());
		return;
	}

	if (entity_list.GetMobByBotID(bid)) {
		c->Message(15, "'%s' is already spawned in zone", bot_name.c_str());
		return;
	}

	/////////////

	// original 'final' criteria
	/*
	if (c->IsGrouped()) {
	Group *g = entity_list.GetGroupByClient(c);
	for (int i = 0; i < MAX_GROUP_MEMBERS; i++) {
	if (g && g->members[i] && !g->members[i]->qglobal && (g->members[i]->GetAppearance() != eaDead)
	&& (g->members[i]->IsEngaged() || (g->members[i]->IsClient() && g->members[i]->CastToClient()->GetAggroCount()))) {
	c->Message(15, "You can't summon bots while you are engaged.");
	return;
	}

	if (g && g->members[i] && g->members[i]->qglobal)
	return;
	}
	}
	else {
	if (c->GetAggroCount() > 0) {
	c->Message(15, "You can't spawn bots while you are engaged.");
	return;
	}
	}
	*/

	if (c->IsEngaged()) {
		c->Message(15, "You can't spawn bots while you are engaged.");
		return;
	}

	////////////

	Bot* b = Bot::LoadBot(bid, &TempErrorMessage);
	if (!TempErrorMessage.empty()) {
		c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		safe_delete(b);
		return;
	}
	if (!b) {
		c->Message(15, "No valid bot '%s' (id: %i) exists", bot_name.c_str(), bid);
		return;
	}

	b->Spawn(c, &TempErrorMessage); // 'TempErrorMessage' not used...

	static std::string bot_spawn_message[17] = {
		"I am ready for battle!", // Generic
		"A solid weapon is my ally!", // WARRIOR
		"The pious shall never die!", // CLERIC
		"I am the symbol of Light!", // PALADIN
		"There are enemies near!", // RANGER
		"Out of the shadows, I step!", // SHADOWKNIGHT
		"Nature's fury shall be wrought!", // DRUID
		"Your punishment will be my fist!", // MONK
		"Music is the overture of battle! ", // BARD
		"Daggers into the backs of my enemies!", // ROGUE
		"More bones to grind!", // SHAMAN
		"Death is only the beginning!", // NECROMANCER
		"I am the harbinger of demise!", // WIZARD
		"The elements are at my command!", // MAGICIAN
		"No being can resist my charm!", // ENCHANTER
		"Battles are won by hand and paw!", // BEASTLORD
		"My bloodthirst shall not be quenched!" // BERSERKER
	};

	Bot::BotGroupSay(b, "%s", bot_spawn_message[(b->GetClass() >= WARRIOR && b->GetClass() <= BERSERKER ? b->GetClass() : 0)].c_str());

	// TODO: Finalization
}

/*xxxxx*/
void bot_subcommand_bot_stance(Client *c, const Seperator *sep)
{
	const int local_count = 7;

	// Ascending order (by stance_id)
	const std::string local_data[local_count + 1] = {
		{ "Passive" },
		{ "Balanced" },
		{ "Efficient" },
		{ "Reactive" },
		{ "Aggressive" },
		{ "Burn" },
		{ "BurnAE" },
		{ "Unknown" }
	};

	if (command_alias_fail(c, "bot_subcommand_bot_stance", sep->arg[0], "botstance"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s", sep->arg[0]);
		c->Message(0, "requires one of the following bot classes:");
		c->Message(1, required_bots_map[BCEnum::ST_BindAffinity].c_str());
		return;
	}



	if (!strcasecmp(sep->arg[1], "usage")) {
		if (!strcasecmp(sep->arg[0], "stance"))
			c->Message(0, "usage: (<target>) %cstance [current | value: 0-6] ([name])", BOT_COMMAND_CHAR);
		else
			c->Message(15, "Undefined linker usage in bot_command_stance (%c%s)", BOT_COMMAND_CHAR, sep->arg[0]);

		c->Message(
			0,
			"value: %u(%s), %u(%s), %u(%s), %u(%s), %u(%s), %u(%s), %u(%s)",
			BotStancePassive, local_data[BotStancePassive].c_str(),
			BotStanceBalanced, local_data[BotStanceBalanced].c_str(),
			BotStanceEfficient, local_data[BotStanceEfficient].c_str(),
			BotStanceReactive, local_data[BotStanceReactive].c_str(),
			BotStanceAggressive, local_data[BotStanceAggressive].c_str(),
			BotStanceBurn, local_data[BotStanceBurn].c_str(),
			BotStanceBurnAE, local_data[BotStanceBurnAE].c_str()
			);

		return;
	}

	if (strcasecmp(sep->arg[1], "current") && !sep->IsNumber(1)) {
		c->Message(15, "A [current] argument or numeric [value] is required to use this command");
		return;
	}

	Mob* t = c->GetTarget();
	if (sep->arg[2][0] == '\0' && (!t || !t->IsBot() || t->CastToBot()->GetBotOwner() != c)) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}

	if (sep->arg[2][0] != '\0') {
		t = entity_list.GetBotByBotName(sep->arg[1]);
		if (!t) {
			c->Message(15, "You must [name] a bot that you own to use this command");
			return;
		}
	}

	Bot* b = t->CastToBot();

	if (!strcasecmp(sep->arg[1], "current")) {
		/*if (is_bot_in_client_group(c, b))
		Bot::BotGroupSay(
		b,
		"My current stance is '%s' (%u)",
		local_data[((b->GetBotStance() >= BotStancePassive && b->GetBotStance() <= BotStanceBurnAE) ? b->GetBotStance() : local_count)].c_str(),
		b->GetBotStance()
		);
		else
		c->Message(
		MT_Tell,
		"%s tells you, My current stance is '%s' (%u)",
		b->GetCleanName(),
		local_data[((b->GetBotStance() >= BotStancePassive && b->GetBotStance() <= BotStanceBurnAE) ? b->GetBotStance() : local_count)].c_str(),
		b->GetBotStance()
		);*/

		return;
	}

	int bsv = atoi(sep->arg[1]);
	if (bsv < BotStancePassive || bsv > BotStanceBurnAE) {
		c->Message(
			15,
			"value: %u(%s), %u(%s), %u(%s), %u(%s), %u(%s), %u(%s), %u(%s)",
			BotStancePassive, local_data[BotStancePassive].c_str(),
			BotStanceBalanced, local_data[BotStanceBalanced].c_str(),
			BotStanceEfficient, local_data[BotStanceEfficient].c_str(),
			BotStanceReactive, local_data[BotStanceReactive].c_str(),
			BotStanceAggressive, local_data[BotStanceAggressive].c_str(),
			BotStanceBurn, local_data[BotStanceBurn].c_str(),
			BotStanceBurnAE, local_data[BotStanceBurnAE].c_str()
			);
		return;
	}

	b->SetBotStance((BotStanceType)bsv);
	b->CalcChanceToCast();
	b->Save();

	/*if (is_bot_in_client_group(c, b))
	Bot::BotGroupSay(
	b,
	"My stance is now '%s' (%u)",
	local_data[((b->GetBotStance() >= BotStancePassive && b->GetBotStance() <= BotStanceBurnAE) ? b->GetBotStance() : local_count)].c_str(),
	b->GetBotStance()
	);
	else
	c->Message(
	MT_Tell,
	"%s tells you, My stance is now '%s' (%u)",
	b->GetCleanName(),
	local_data[((b->GetBotStance() >= BotStancePassive && b->GetBotStance() <= BotStanceBurnAE) ? b->GetBotStance() : local_count)].c_str(),
	b->GetBotStance()
	);*/

	// TODO: Finalization
}

/*X*/
void bot_subcommand_bot_summon(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_summon", sep->arg[0], "botsummon"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s ([name])", sep->arg[0]);
		return;
	}

	Bot* b = nullptr;
	if (sep->arg[1][0] != '\0' && !sep->IsNumber(1))
		b = MyBot::AsNamed_ByBot(c, sep->arg[1]);
	else
		b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> or [name] a bot that you own to use this command");
		return;
	}

	c->Message(0, "Summoning %s to you", b->GetCleanName());
	b->BotGroupSay(b, "Whee!");

	b->Warp(glm::vec3(c->GetPosition()));

	// TODO: Finalization
}

/* PROBABLY OK */
void bot_subcommand_bot_tattoo(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_tattoo", sep->arg[0], "bottattoo"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: <target> %s [value:0-7] (Drakkin bots only)", sep->arg[0]);
		return;
	}

	Bot* b = MyBot::AsTarget_ByBot(c);
	if (!b) {
		c->Message(15, "You must <target> a bot that you own to use this command");
		return;
	}

	if (!sep->IsNumber(1)) {
		c->Message(15, "A numeric [value] is required to use this command");
		return;
	}

	uint32 uvalue = atoi(sep->arg[1]);

	BCEnum::AFType fail_type = BCEnum::AFT_None;
	if (b->GetRace() != DRAKKIN)
		fail_type = BCEnum::AFT_Race;
	else if (uvalue > (uint32)~0) // set proper value
		fail_type = BCEnum::AFT_Value;
	else
		b->SetDrakkinTattoo(uvalue);

	if (helper_bot_appearance_fail(c, b, fail_type, "tattoo"))
		return;

	helper_bot_appearance_final(c, b);

	// TODO: Finalization
}

/*xxxxx*/
void bot_subcommand_bot_toggle_archer(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_toggle_archer", sep->arg[0], "bottogglearcher"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s ([name])", sep->arg[0]);
		return;
	}

	Bot* b = MyBot::AsTarget_ByArcherBot(c);
	if (!b) {
		c->Message(15, "You must <target> or [name] an archer bot that you own to use this command");
		return;
	}

	b->SetBotArcher(!b->IsBotArcher());
	b->ChangeBotArcherWeapons(b->IsBotArcher());

	if (b->GetClass() == RANGER && b->GetLevel() >= 61)
		b->SetRangerAutoWeaponSelect(b->IsBotArcher());

	// TODO: Finalization
}

/*X*/
void bot_subcommand_bot_toggle_helm(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_toggle_helm", sep->arg[0], "bottogglehelm"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s ([on | off]) ([ownergroup | targetgroup | spawned | all])", sep->arg[0]);
		return;
	}

	std::string arg1 = sep->arg[1];

	bool helm_state = false;
	bool toggle_helm = true;
	int bat_arg = 1;
	if (!arg1.compare("on")) {
		helm_state = true;
		toggle_helm = false;
		bat_arg = 2;
	}
	else if (!arg1.compare("off")) {
		toggle_helm = false;
		bat_arg = 2;
	}

	Bot* b = nullptr;
	SelectActionableBots::SABType multitarget = SelectActionableBots::Single;
	std::list<Bot*> sbl;
	if (!SelectActionableBots::SetActionableBots(c, sep->arg[bat_arg], multitarget, b, sbl))
		return;

	int bot_count = 0;
	for (std::list<Bot*>::iterator iter = sbl.begin(); iter != sbl.end(); ++iter) {
		Bot* b = *iter;
		if (!b)
			continue;

		if (toggle_helm)
			b->SetShowHelm(!b->GetShowHelm());
		else
			b->SetShowHelm(helm_state);

		if (multitarget != SelectActionableBots::All) {
			std::string query = StringFormat("UPDATE `bot_data` SET `show_helm` = '%u' WHERE `bot_id` = '%u'", (b->GetShowHelm() ? 1 : 0), b->GetBotID());
			auto results = database.QueryDatabase(query);
			if (!results.Success()) {
				c->Message(15, "Failed to save helm changes for your bots due to unknown cause");
				return;
			}

			helper_bot_appearance_send_current(b);
		}
		++bot_count;
	}

	if (multitarget == SelectActionableBots::All) {
		std::string query;
		if (toggle_helm)
			query = StringFormat("UPDATE `bot_data` SET `show_helm` = (`show_helm` XOR '1') WHERE `owner_id` = '%u'", c->CharacterID());
		else
			query = StringFormat("UPDATE `bot_data` SET `show_helm` = '%u' WHERE `owner_id` = '%u'", (helm_state ? 1 : 0), c->CharacterID());
		auto results = database.QueryDatabase(query);
		if (!results.Success()) {
			c->Message(15, "Failed to save helm changes for your bots due to unknown cause");
			return;
		}

		c->Message(0, "%s all of your bot show helm flags", toggle_helm ? "Toggled" : (helm_state ? "Set" : "Cleared"));
	}
	else {
		c->Message(0, "%s %i of your spawned bot show helm flags", toggle_helm ? "Toggled" : (helm_state ? "Set" : "Cleared"), bot_count);
	}

	// TODO: Finalization

	// Notes:
	/*
	[CLIENT OPCODE TEST]
	[10-16-2015 :: 14:57:56] [Packet :: Client -> Server (Dump)] [OP_SpawnAppearance - 0x01d1] [Size: 10]
	0: A4 02 [2B 00] 01 00 00 00 - showhelm = true (client)
	[10-16-2015 :: 14:57:56] [Packet :: Server -> Client (Dump)] [OP_SpawnAppearance - 0x01d1] [Size: 10]
	0: A4 02 [2B 00] 01 00 00 00 - showhelm = true (client)

	[10-16-2015 :: 14:58:02] [Packet :: Client -> Server (Dump)] [OP_SpawnAppearance - 0x01d1] [Size: 10]
	0: A4 02 [2B 00] 00 00 00 00 - showhelm = false (client)
	[10-16-2015 :: 14:58:02] [Packet :: Server -> Client (Dump)] [OP_SpawnAppearance - 0x01d1] [Size: 10]
	0: A4 02 [2B 00] 00 00 00 00 - showhelm = false (client)

	[BOT OPCODE TEST]
	[10-16-2015 :: 22:15:34] [Packet :: Client -> Server (Dump)] [OP_ChannelMessage - 0x0045] [Size: 167]
	0: 43 6C 65 72 69 63 62 6F - 74 00 00 00 00 00 00 00  | Clericbot.......
	16: 00 00 00 00 00 00 00 00 - 00 00 00 00 00 00 00 00  | ................
	32: 00 00 00 00 00 00 00 00 - 00 00 00 00 00 00 00 00  | ................
	48: 00 00 00 00 00 00 00 00 - 00 00 00 00 00 00 00 00  | ................
	64: 43 6C 65 72 69 63 62 6F - 74 00 00 00 00 00 00 00  | Clericbot.......
	80: 00 00 00 00 00 00 00 00 - 00 00 00 00 00 00 00 00  | ................
	96: 00 00 00 00 00 00 00 00 - 00 00 00 00 00 00 00 00  | ................
	112: 00 00 00 00 00 00 00 00 - 00 00 00 00 00 00 00 00  | ................
	128: 00 00 00 00 08 00 00 00 - CD CD CD CD CD CD CD CD  | ................
	144: 64 00 00 00 23 62 6F 74 - 20 73 68 6F 77 68 65 6C  | d...#bot showhel
	160: 6D 20 6F 6E 00                                     | m on.

	[10-16-2015 :: 22:15:34] [Packet :: Server -> Client (Dump)] [OP_SpawnAppearance - 0x01d1] [Size: 10]
	0: A2 02 2B 00 01 00 00 00 - showhelm = true

	[10-16-2015 :: 22:15:40] [Packet :: Client -> Server (Dump)] [OP_ChannelMessage - 0x0045] [Size: 168]
	0: 43 6C 65 72 69 63 62 6F - 74 00 00 00 00 00 00 00  | Clericbot.......
	16: 00 00 00 00 00 00 00 00 - 00 00 00 00 00 00 00 00  | ................
	32: 00 00 00 00 00 00 00 00 - 00 00 00 00 00 00 00 00  | ................
	48: 00 00 00 00 00 00 00 00 - 00 00 00 00 00 00 00 00  | ................
	64: 43 6C 65 72 69 63 62 6F - 74 00 00 00 00 00 00 00  | Clericbot.......
	80: 00 00 00 00 00 00 00 00 - 00 00 00 00 00 00 00 00  | ................
	96: 00 00 00 00 00 00 00 00 - 00 00 00 00 00 00 00 00  | ................
	112: 00 00 00 00 00 00 00 00 - 00 00 00 00 00 00 00 00  | ................
	128: 00 00 00 00 08 00 00 00 - CD CD CD CD CD CD CD CD  | ................
	144: 64 00 00 00 23 62 6F 74 - 20 73 68 6F 77 68 65 6C  | d...#bot showhel
	160: 6D 20 6F 66 66 00                                  | m off.

	[10-16-2015 :: 22:15:40] [Packet :: Server -> Client (Dump)] [OP_SpawnAppearance - 0x01d1] [Size: 10]
	0: A2 02 2B 00 00 00 00 00 - showhelm = false

	*** Bot did not update using the OP_SpawnAppearance packet with AT_ShowHelm appearance type ***
	*/
}

/*X*/
void bot_subcommand_bot_update(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_bot_update", sep->arg[0], "botupdate"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: (<target>) %s ([spawned])", sep->arg[0]);
		return;
	}

	if (!strcasecmp(sep->arg[1], "spawned")) {
		std::list<Bot*> sbl;
		MyBot::Populate_BySpawnedBots(c, sbl);
		if (sbl.empty()) {
			c->Message(15, "You currently have no spawned bots");
			return;
		}

		int bc = 0;
		for (std::list<Bot*>::iterator iter = sbl.begin(); iter != sbl.end(); ++iter) {
			Bot* b = *iter;
			if (!b || b->IsEngaged() || b->GetLevel() == c->GetLevel())
				continue;

			b->SetPetChooser(false);
			b->CalcBotStats();
			++bc;
		}

		c->Message(0, "Updated %i of your %i spawned bots", bc, sbl.size());
	}
	else {
		Bot* b = MyBot::AsTarget_ByBot(c);
		if (!b) {
			c->Message(15, "You must <target> a bot that you own to use this command");
			return;
		}
		if (b->IsEngaged()) {
			c->Message(15, "You can not update a bot that is in combat");
			return;
		}
		if (b->GetLevel() == c->GetLevel()) {
			c->Message(15, "Your bot is already up-to-date");
			return;
		}

		b->SetPetChooser(false);
		b->CalcBotStats();
	}

	// TODO: Finalization
}

/**/
void bot_subcommand_botgroup_add(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_botgroup_add", sep->arg[0], "botgroupadd"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "(<target>) usage: %s [bot-group member name] ([bot-group leader name])", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot botgroup add <bot group member name to add> <bot group leader name or target>");

	int argCount = 0;
	argCount = sep->argnum;
	std::string botGroupLeaderName;
	std::string botGroupMemberName;
	if (argCount >= 3)
		botGroupMemberName = std::string(sep->arg[3]);

	Bot* botGroupMember = entity_list.GetBotByBotName(botGroupMemberName);
	if (!botGroupMember) {
		if (botGroupMemberName.empty())
			c->Message(13, "You must target a bot in this zone. Please try again.");
		else
			c->Message(13, "%s is not a bot in this zone. Please try again.", botGroupMemberName.c_str());

		return;
	}

	Bot* botGroupLeader = 0;
	if (argCount == 4) {
		botGroupLeaderName = std::string(sep->arg[4]);
		botGroupLeader = entity_list.GetBotByBotName(botGroupLeaderName);
	}
	else if (c->GetTarget() && c->GetTarget()->IsBot())
		botGroupLeader = c->GetTarget()->CastToBot();

	if (!botGroupLeader) {
		if (botGroupLeaderName.empty())
			c->Message(13, "You must target a bot in this zone. Please try again.");
		else
			c->Message(13, "%s is not a bot in this zone. Please try again.", botGroupLeaderName.c_str());

		return;
	}

	if (botGroupLeader->HasGroup()) {
		Group* g = botGroupLeader->GetGroup();

		if (g) {
			if (g->IsLeader(botGroupLeader)) {
				if (g->GroupCount() < MAX_GROUP_MEMBERS) {
					if (!botGroupMemberName.empty() && botGroupMember) {
						botGroupMember = entity_list.GetBotByBotName(botGroupMemberName);
					}

					if (botGroupMember) {
						if (!botGroupMember->HasGroup()) {
							if (Bot::AddBotToGroup(botGroupMember, g)) {
								database.SetGroupID(botGroupMember->GetName(), g->GetID(), botGroupMember->GetBotID());
								botGroupMember->BotGroupSay(botGroupMember, "I have joined %s\'s group.", botGroupLeader->GetName());
							}
							else
								botGroupMember->BotGroupSay(botGroupMember, "I can not join %s\'s group.", botGroupLeader->GetName());
						}
						else {
							Group* tempGroup = botGroupMember->GetGroup();
							if (tempGroup)
								botGroupMember->BotGroupSay(botGroupMember, "I can not join %s\'s group. I am already a member in %s\'s group.", botGroupLeader->GetName(), tempGroup->GetLeaderName());
						}
					}
					else
						c->Message(13, "You must target a spawned bot first.");
				}
				else
					botGroupLeader->BotGroupSay(botGroupMember, "I have no more openings in my group, %s.", c->GetName());
			}
			else {
				Group* tempGroup = botGroupLeader->GetGroup();
				if (tempGroup)
					botGroupLeader->BotGroupSay(botGroupLeader, "I can not lead anyone because I am a member in %s\'s group.", tempGroup->GetLeaderName());
			}
		}
	}
}

/**/
void bot_subcommand_botgroup_attack(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_botgroup_attack", sep->arg[0], "botgroupattack"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "<target> usage: %s [bot-group leader name]", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot botgroup attack <bot group leader name> <mob name to attack or target>");

	Mob* targetMob = c->GetTarget();
	Bot* botGroupLeader = 0;
	std::string botGroupLeaderName = std::string(sep->arg[3]);
	std::string targetName = std::string(sep->arg[4]);
	if (!botGroupLeaderName.empty()) {
		botGroupLeader = entity_list.GetBotByBotName(botGroupLeaderName);
		if (botGroupLeader) {
			if (!targetName.empty())
				targetMob = entity_list.GetMob(targetName.c_str());

			if (targetMob) {
				if (c->IsAttackAllowed(targetMob)) {
					if (botGroupLeader->HasGroup()) {
						Group* g = botGroupLeader->GetGroup();
						if (g) {
							if (g->IsLeader(botGroupLeader))
								Bot::BotGroupOrderAttack(g, targetMob, c);
						}
					}
					else if (c->HasGroup())
						Bot::BotGroupOrderAttack(c->GetGroup(), targetMob, c);
				}
				else
					c->Message(13, "You must target a monster.");
			}
			else
				c->Message(13, "You must target a monster.");
		}
		else
			c->Message(13, "You must target a spawned bot group leader first.");
	}
}

/**/
void bot_subcommand_botgroup_create(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_botgroup_create", sep->arg[0], "botgroupcreate"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "(<target>) usage: %s ([bot-group leader name])", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot botgroup create <bot group leader name or target>. This will designate a bot to be a bot group leader.");

	Mob* targetMob = c->GetTarget();
	std::string targetName = std::string(sep->arg[3]);
	Bot* botGroupLeader = 0;
	if (!targetName.empty())
		botGroupLeader = entity_list.GetBotByBotName(targetName);
	else if (targetMob) {
		if (targetMob->IsBot())
			botGroupLeader = targetMob->CastToBot();
	}

	if (botGroupLeader) {
		if (Bot::BotGroupCreate(botGroupLeader))
			botGroupLeader->BotGroupSay(botGroupLeader, "I am prepared to lead.");
		else
			botGroupLeader->BotGroupSay(botGroupLeader, "I cannot lead.");
	}
	else
		c->Message(13, "You must target a spawned bot first.");
}

/**/
void bot_subcommand_botgroup_delete(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_botgroup_delete", sep->arg[0], "botgroupdelete"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s [bot-group name]", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot botgroup delete <bot group name>");

	std::string TempErrorMessage;

	std::string botGroupName = std::string(sep->arg[3]);
	if (!botGroupName.empty()) {
		uint32 botGroupId = Bot::CanLoadBotGroup(c->CharacterID(), botGroupName, &TempErrorMessage);
		if (!TempErrorMessage.empty()) {
			c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
			return;
		}

		if (botGroupId > 0) {
			Bot::DeleteBotGroup(botGroupName, &TempErrorMessage);
			if (!TempErrorMessage.empty()) {
				c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
				return;
			}
		}
	}
}

/**/
void bot_subcommand_botgroup_disband(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_botgroup_disband", sep->arg[0], "botgroupdisband"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "(<target>) usage: %s ([bot-group leader name])", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot botgroup disband <bot group leader name or target>. Disbands the designated bot group leader's bot group.");

	Mob* targetMob = c->GetTarget();
	std::string targetName = std::string(sep->arg[3]);
	Bot* botGroupLeader = 0;
	if (!targetName.empty())
		botGroupLeader = entity_list.GetBotByBotName(targetName);
	else if (targetMob) {
		if (targetMob->IsBot())
			botGroupLeader = targetMob->CastToBot();
	}

	if (botGroupLeader) {
		if (botGroupLeader->HasGroup()) {
			Group* g = botGroupLeader->GetGroup();
			if (g->IsLeader(botGroupLeader)) {
				if (Bot::RemoveBotFromGroup(botGroupLeader, g))
					botGroupLeader->BotGroupSay(botGroupLeader, "I have disbanded my group, %s.", c->GetName());
				else
					botGroupLeader->BotGroupSay(botGroupLeader, "I was not able to disband my group, %s.", c->GetName());
			}
			else
				botGroupLeader->BotGroupSay(botGroupLeader, "I can not disband my group, %s, because I am not the leader. %s is the leader of my group.", c->GetName(), g->GetLeaderName());
		}
		else
			botGroupLeader->BotGroupSay(botGroupLeader, "I am not a group leader, %s.", c->GetName());
	}
	else
		c->Message(13, "You must target a spawned bot group leader first.");
}

/**/
void bot_subcommand_botgroup_follow(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_botgroup_follow", sep->arg[0], "botgroupfollow"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "(<target>) usage: %s ([bot-group leader name])", sep->arg[0]);
		return;
	}

	//
	return;;
	//

	c->Message(0, "#bot botgroup follow <bot group leader name or target>");

	Mob* targetMob = c->GetTarget();
	std::string targetName = std::string(sep->arg[3]);
	Bot* botGroupLeader = 0;
	if (!targetName.empty())
		botGroupLeader = entity_list.GetBotByBotName(targetName);
	else if (targetMob) {
		if (targetMob->IsBot())
			botGroupLeader = targetMob->CastToBot();
	}

	if (botGroupLeader) {
		if (botGroupLeader->HasGroup()) {
			Group* g = botGroupLeader->GetGroup();
			if (g->IsLeader(botGroupLeader))
				Bot::BotGroupOrderFollow(g, c);
		}
	}
	else if (c->HasGroup())
		Bot::BotGroupOrderFollow(c->GetGroup(), c);
}

/**/
void bot_subcommand_botgroup_guard(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_botgroup_guard", sep->arg[0], "botgroupguard"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "(<target>) usage: %s ([bot-group leader name])", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot botgroup guard <bot group leader name or target>");

	Mob* targetMob = c->GetTarget();
	std::string targetName = std::string(sep->arg[3]);
	Bot* botGroupLeader = 0;
	if (!targetName.empty())
		botGroupLeader = entity_list.GetBotByBotName(targetName);
	else if (targetMob) {
		if (targetMob->IsBot())
			botGroupLeader = targetMob->CastToBot();
	}

	if (botGroupLeader) {
		if (botGroupLeader->HasGroup()) {
			Group* g = botGroupLeader->GetGroup();
			if (g->IsLeader(botGroupLeader))
				Bot::BotGroupOrderGuard(g, c);
		}
	}
	else if (c->HasGroup())
		Bot::BotGroupOrderGuard(c->GetGroup(), c);
}

/**/
void bot_subcommand_botgroup_list(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_botgroup_list", sep->arg[0], "botgrouplist"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot botgroup list");

	std::string TempErrorMessage;

	std::list<BotGroupList> botGroupList = Bot::GetBotGroupListByBotOwnerCharacterId(c->CharacterID(), &TempErrorMessage);
	if (!TempErrorMessage.empty()) {
		c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		return;
	}

	if (!botGroupList.empty()) {
		for (std::list<BotGroupList>::iterator botGroupListItr = botGroupList.begin(); botGroupListItr != botGroupList.end(); ++botGroupListItr)
			c->Message(0, "Bot Group Name: %s -- Bot Group Leader: %s", botGroupListItr->BotGroupName.c_str(), botGroupListItr->BotGroupLeaderName.c_str());
	}
	else
		c->Message(0, "You have no bot groups created. Use the #bot botgroup save command to save bot groups.");
}

/**/
void bot_subcommand_botgroup_load(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_botgroup_load", sep->arg[0], "botgroupload"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s [bot-group name]", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot botgroup load <bot group name>");

	std::string TempErrorMessage;

	Group *g = c->GetGroup();
	if (g) {
		for (int i = 0; i < MAX_GROUP_MEMBERS; i++) {
			if (!g->members[i])
				continue;

			if ((g->members[i]->IsClient() && g->members[i]->CastToClient()->GetAggroCount()) || g->members[i]->IsEngaged()) {
				c->Message(0, "You can't spawn bots while your group is engaged.");
				return;
			}
		}
	}
	else {
		if (c->GetAggroCount() > 0) {
			c->Message(0, "You can't spawn bots while you are engaged.");
			return;
		}
	}

	std::string botGroupName = std::string(sep->arg[3]);
	if (botGroupName.empty()) {
		c->Message(13, "Invalid botgroup name supplied.");
		return;
	}

	uint32 botGroupID = Bot::CanLoadBotGroup(c->CharacterID(), botGroupName, &TempErrorMessage);
	if (!TempErrorMessage.empty()) {
		c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		return;
	}
	if (botGroupID <= 0) {
		c->Message(13, "Invalid botgroup id found.");
		return;
	}

	std::list<BotGroup> botGroup = Bot::LoadBotGroup(botGroupName, &TempErrorMessage);
	if (!TempErrorMessage.empty()) {
		c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		return;
	}

	int spawnedBots = Bot::SpawnedBotCount(c->CharacterID(), &TempErrorMessage);
	if (!TempErrorMessage.empty()) {
		c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		return;
	}

	if (RuleB(Bots, QuestableSpawnLimit)) {
		const int allowedBotsBQ = Bot::AllowedBotSpawns(c->CharacterID(), &TempErrorMessage);
		if (!TempErrorMessage.empty()) {
			c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
			return;
		}

		if (allowedBotsBQ == 0) {
			c->Message(0, "You can't spawn any bots.");
			return;
		}

		if (spawnedBots >= allowedBotsBQ || spawnedBots + (int)botGroup.size() > allowedBotsBQ) {
			c->Message(0, "You can't spawn more than %i bot(s).", allowedBotsBQ);
			return;
		}
	}

	const int allowedBotsSBC = RuleI(Bots, SpawnLimit);
	if (spawnedBots >= allowedBotsSBC || spawnedBots + (int)botGroup.size() > allowedBotsSBC) {
		c->Message(0, "You can't spawn more than %i bots.", allowedBotsSBC);
		return;
	}

	uint32 botGroupLeaderBotID = Bot::GetBotGroupLeaderIdByBotGroupName(botGroupName);
	Bot *botGroupLeader = Bot::LoadBot(botGroupLeaderBotID, &TempErrorMessage);
	if (!TempErrorMessage.empty()) {
		c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		safe_delete(botGroupLeader);
		return;
	}
	if (!botGroupLeader) {
		c->Message(13, "Failed to load botgroup leader.");
		safe_delete(botGroupLeader);
		return;
	}

	botGroupLeader->Spawn(c, &TempErrorMessage);
	if (!TempErrorMessage.empty()) {
		c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		safe_delete(botGroupLeader);
		return;
	}

	if (!Bot::BotGroupCreate(botGroupLeader)) {
		c->Message(13, "Unable to create botgroup.");
		return;
	}

	Group *newBotGroup = botGroupLeader->GetGroup();
	if (!newBotGroup) {
		c->Message(13, "Unable to find valid botgroup");
		return;
	}

	for (auto botGroupItr = botGroup.begin(); botGroupItr != botGroup.end(); ++botGroupItr) {
		if (botGroupItr->BotID == botGroupLeader->GetBotID())
			continue;

		Bot *botGroupMember = Bot::LoadBot(botGroupItr->BotID, &TempErrorMessage);
		if (!TempErrorMessage.empty()) {
			c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
			safe_delete(botGroupMember);
			return;
		}

		if (!botGroupMember) {
			safe_delete(botGroupMember);
			continue;
		}

		botGroupMember->Spawn(c, &TempErrorMessage);
		if (!TempErrorMessage.empty()) {
			c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
			safe_delete(botGroupMember);
			return;
		}

		Bot::AddBotToGroup(botGroupMember, newBotGroup);
	}
}

/**/
void bot_subcommand_botgroup_remove(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_botgroup_remove", sep->arg[0], "botgroupremove"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "(<target>) usage: %s ([bot-group member name])", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot botgroup remove <bot group member name to remove or target>");

	Mob* targetMob = c->GetTarget();
	std::string targetName = std::string(sep->arg[3]);
	Bot* botGroupMember = 0;

	if (!targetName.empty())
		botGroupMember = entity_list.GetBotByBotName(targetName);
	else if (targetMob) {
		if (targetMob->IsBot())
			botGroupMember = targetMob->CastToBot();
	}

	if (botGroupMember) {
		if (botGroupMember->HasGroup()) {
			Group* g = botGroupMember->GetGroup();
			if (Bot::RemoveBotFromGroup(botGroupMember, g))
				botGroupMember->BotGroupSay(botGroupMember, "I am no longer in a group.");
			else
				botGroupMember->BotGroupSay(botGroupMember, "I can not leave %s\'s group.", g->GetLeaderName());
		}
		else
			botGroupMember->BotGroupSay(botGroupMember, "I am not in a group.");
	}
	else
		c->Message(13, "You must target a spawned bot first.");
}

/**/
void bot_subcommand_botgroup_save(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_botgroup_save", sep->arg[0], "botgroupsave"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "(<target>) usage: %s [bot-group name] ([bot-group leader name])", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot botgroup save <bot group name> <bot group leader name or target>");

	std::string TempErrorMessage;

	std::string botGroupName = std::string(sep->arg[3]);
	if (!botGroupName.empty()) {
		if (!Bot::DoesBotGroupNameExist(botGroupName)) {
			Bot* groupLeader = 0;
			if (c->GetTarget() && c->GetTarget()->IsBot())
				groupLeader = c->GetTarget()->CastToBot();
			else
				groupLeader = entity_list.GetBotByBotName(std::string(sep->arg[4]));

			if (groupLeader) {
				if (groupLeader->HasGroup() && groupLeader->GetGroup()->IsLeader(groupLeader)) {
					Bot::SaveBotGroup(groupLeader->GetGroup(), botGroupName, &TempErrorMessage);
					if (!TempErrorMessage.empty())
						c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
					else
						c->Message(0, "%s's bot group has been saved as %s.", groupLeader->GetName(), botGroupName.c_str());
				}
				else
					c->Message(0, "You must target a bot group leader only.");
			}
			else
				c->Message(0, "You must target a bot that is in the same zone as you.");
		}
		else
			c->Message(0, "The bot group name already exists. Please choose another name to save your bot group as.");
	}
}

/**/
void bot_subcommand_botgroup_summon(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_botgroup_summon", sep->arg[0], "botgroupsummon"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "(<target>) usage: %s ([bot-group leader name])", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot botgroup summon <bot group leader name or target>. Summons the bot group to your location.");

	Mob* targetMob = c->GetTarget();
	std::string targetName = std::string(sep->arg[3]);
	Bot* botGroupLeader = 0;

	if (!targetName.empty())
		botGroupLeader = entity_list.GetBotByBotName(targetName);
	else if (targetMob) {
		if (targetMob->IsBot())
			botGroupLeader = targetMob->CastToBot();
	}

	if (botGroupLeader) {
		if (botGroupLeader->HasGroup()) {
			Group* g = botGroupLeader->GetGroup();
			if (g->IsLeader(botGroupLeader))
				Bot::BotGroupSummon(g, c);
		}
	}
	else if (c->HasGroup())
		Bot::BotGroupSummon(c->GetGroup(), c);
}

/*X*/
void bot_subcommand_circle(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Depart];
	if (spell_list_fail(c, local_list, BCEnum::ST_Depart) || command_alias_fail(c, "bot_subcommand_circle", sep->arg[0], "circle"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s [list | destination]", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Depart, DRUID);
		return;
	}

	if (!strcasecmp(sep->arg[1], "list")) {
		c->Message(0, "The following destinations are available:");

		Bot* b = MyBot::AsGroupMember_ByClass(c, c, DRUID);
		std::string msg;
		std::string text_link;

		int dc = 0;
		for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
			STDepartEntry* local_entry = (*iter_list)->SafeCastToDepart();
			if (!local_entry)
				continue;

			if (b && b->GetClass() == local_entry->caster_class && b->GetLevel() >= local_entry->spell_level) {
				msg = StringFormat("%ccircle %s", BOT_COMMAND_CHAR, spells[local_entry->spell_id].teleport_zone);
				text_link = b->CreateSayLink(c, msg.c_str(), local_entry->long_name.c_str());
				Bot::BotGroupSay(b, "%s %s", spells[local_entry->spell_id].teleport_zone, text_link.c_str());
				++dc;
				continue;
			}
		}
		if (!dc)
			c->Message(15, "None");

		return;
	}

	Client* mtgmp = MyTarget::AsGroupMember_ByPlayer(c);
	if (!mtgmp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STDepartEntry* local_entry = (*iter_list)->SafeCastToDepart();
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_GroupV1:
			if (!mtgmp)
				continue;
			b = MyBot::AsGroupMember_ByMinLevelAndClass(c, mtgmp, local_entry->spell_level, local_entry->caster_class);
			t = mtgmp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots in your group are capable of performing this action");
		return;
	}

	// TODO: Finalization
}

/**/
void bot_subcommand_group_attack(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_group_attack", sep->arg[0], "groupattack"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "<target> usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot group attack <bot group leader name> <mob name to attack or target>");

	if (c->IsGrouped() && (c->GetTarget() != nullptr) && c->IsAttackAllowed(c->GetTarget()))
		Bot::BotGroupOrderAttack(c->GetGroup(), c->GetTarget(), c);
	else
		c->Message(15, "You must target a monster.");
}

/**/
void bot_subcommand_group_follow(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_group_follow", sep->arg[0], "groupfollow"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot group follow <bot group leader name or target>");

	if (c->IsGrouped())
		Bot::BotGroupOrderFollow(c->GetGroup(), c);
}

/**/
void bot_subcommand_group_guard(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_group_guard", sep->arg[0], "groupguard"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot group guard <bot group leader name or target>");

	if (c->IsGrouped())
		Bot::BotGroupOrderGuard(c->GetGroup(), c);
}

/**/
void bot_subcommand_group_summon(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_group_summon", sep->arg[0], "groupsummon"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot group summon <bot group leader name or target>. Summons the bot group to your location.");

	if (c->IsGrouped())
		Bot::BotGroupSummon(c->GetGroup(), c);
}

/**/
void bot_subcommand_heal_rotation_add_member(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_heal_rotation_add_member", sep->arg[0], "healrotationaddmember"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot healrotation addmember <bot healrotation leader name> <bot healrotation member name to add> ");

	if (sep->argnum == 4) {
		Bot* leaderBot;
		std::string botName = std::string(sep->arg[3]);
		if (!botName.empty())
			leaderBot = entity_list.GetBotByBotName(botName);
		else {
			c->Message(13, "You must name a valid bot.");
			return;
		}

		if (leaderBot) {
			Bot* healer;
			std::string healerName = std::string(sep->arg[4]);
			if (leaderBot->GetBotOwner() != c) {
				c->Message(13, "You must target a bot that you own.");
				return;
			}

			if (!healerName.empty())
				healer = entity_list.GetBotByBotName(healerName);
			else {
				c->Message(13, "You must name a valid bot.");
				return;
			}

			if (healer) {
				if (healer->GetBotOwner() != c) {
					c->Message(13, "You must target a bot that you own.");
					return;
				}

				if (!(healer->IsBotCaster() && healer->CanHeal())) {
					c->Message(13, "Heal rotation members must be able to heal.");
					return;
				}

				if (leaderBot->AddHealRotationMember(healer))
					c->Message(0, "Bot heal rotation member added successfully.");
				else
					c->Message(13, "Unable to add bot to rotation.");
			}
		}
		else {
			c->Message(13, "You must name a valid bot.");
			return;
		}
	}
	else {
		c->Message(0, "#bot healrotation addmember <bot healrotation leader name> <bot healrotation member name to add> ");
		return;
	}
}

/**/
void bot_subcommand_heal_rotation_add_target(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_heal_rotation_add_target", sep->arg[0], "healrotationaddtarget"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot healrotation addtarget <bot healrotation leader name> [bot healrotation target name to add] ");

	if (sep->argnum == 3 || sep->argnum == 4) {
		Bot* leaderBot;
		std::string botName = std::string(sep->arg[3]);
		if (!botName.empty())
			leaderBot = entity_list.GetBotByBotName(botName);
		else {
			c->Message(13, "You must name a valid heal rotation leader.");
			return;
		}

		if (leaderBot) {
			if (leaderBot->GetBotOwner() != c) {
				c->Message(13, "You must target a bot that you own.");
				return;
			}

			Mob* target = nullptr;
			std::string targetName = std::string(sep->arg[4]);
			if (!targetName.empty())
				target = entity_list.GetMob(targetName.c_str());
			else {
				if (c->GetTarget() != nullptr)
					target = c->GetTarget();
			}

			if (target) {
				if (leaderBot->AddHealRotationTarget(target))
					c->Message(0, "Bot heal rotation target added successfully.");
				else
					c->Message(13, "Unable to add rotation target.");
			}
			else {
				c->Message(13, "Invalid target.");
				return;
			}
		}
		else {
			c->Message(13, "You must name a valid bot.");
			return;
		}
	}
	else {
		c->Message(0, "#bot healrotation addtarget <bot healrotation leader name> [bot healrotation target name to add] ");
		return;
	}
}

/**/
void bot_subcommand_heal_rotation_clear_targets(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_heal_rotation_clear_targets", sep->arg[0], "healrotationcleartargets"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot healrotation cleartargets <bot healrotation leader name>");

	if (sep->argnum == 3) {
		Bot* leaderBot;
		std::string botName = std::string(sep->arg[3]);
		if (!botName.empty())
			leaderBot = entity_list.GetBotByBotName(botName);
		else {
			c->Message(13, "You must name a valid heal rotation leader.");
			return;
		}

		if (leaderBot) {
			std::list<Bot*> botList;
			if (leaderBot->GetBotOwner() != c) {
				c->Message(13, "You must target a bot that you own.");
				return;
			}

			botList = Bot::GetBotsInHealRotation(leaderBot);
			for (std::list<Bot*>::iterator botListItr = botList.begin(); botListItr != botList.end(); ++botListItr) {
				Bot* tempBot = *botListItr;
				if (tempBot && tempBot->GetBotOwnerCharacterID() == c->CharacterID())
					tempBot->ClearHealRotationTargets();
			}
		}
		else {
			c->Message(13, "You must name a valid bot.");
			return;
		}
	}
	else {
		c->Message(0, "#bot healrotation cleartargets <bot healrotation leader name>");
		return;
	}
}

/**/
void bot_subcommand_heal_rotation_create(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_heal_rotation_create", sep->arg[0], "healrotationcreate"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot healrotation create <bot healrotation leader name> <timer> <fasthealson | fasthealsoff> [target]. This will create a heal rotation with the designated leader.");

	if (sep->argnum == 5 || sep->argnum == 6) {
		Bot* leaderBot;
		std::string botName = std::string(sep->arg[3]);
		if (!botName.empty())
			leaderBot = entity_list.GetBotByBotName(botName);
		else {
			c->Message(13, "You must name a valid heal rotation leader.");
			return;
		}

		if (leaderBot) {
			Mob* target = nullptr;
			uint32 timer;
			bool fastHeals = false;
			if (!sep->IsNumber(4)) {
				c->Message(0, "Usage #bot healrotation create <bot healrotation leader name> <timer> <fasthealson | fasthealsoff> [target].");
				return;
			}

			timer = (uint32)(atof(sep->arg[4]) * 1000);
			if (leaderBot->GetBotOwner() != c) {
				c->Message(13, "You must target a bot that you own.");
				return;
			}

			if (!(leaderBot->IsBotCaster() && leaderBot->CanHeal())) {
				c->Message(13, "Heal rotation members must be able to heal.");
				return;
			}

			if (!strcasecmp(sep->arg[5], "fasthealson"))
				fastHeals = true;
			else if (strcasecmp(sep->arg[5], "fasthealsoff")) {
				c->Message(0, "Usage #bot healrotation create <bot healrotation leader name> <timer> <fasthealson | fasthealsoff> [target].");
				return;
			}

			if (!leaderBot->GetInHealRotation()) {
				if (sep->argnum == 6) {
					std::string targetName = std::string(sep->arg[6]);
					if (!targetName.empty())
						target = entity_list.GetMob(targetName.c_str());
					else {
						c->Message(13, "You must name a valid target.");
						return;
					}

					if (!target) {
						c->Message(13, "You must name a valid target.");
						return;
					}
				}
				leaderBot->CreateHealRotation(target, timer);
				leaderBot->SetHealRotationUseFastHeals(fastHeals);
				c->Message(0, "Bot heal rotation created successfully.");
			}
			else {
				c->Message(13, "That bot is already in a heal rotation.");
				return;
			}
		}
		else {
			c->Message(13, "You must name a valid bot.");
			return;
		}
	}
	else {
		c->Message(0, "Usage #bot healrotation create <bot healrotation leader name> <timer> <fasthealson | fasthealsoff> [target].");
		return;
	}
}

/**/
void bot_subcommand_heal_rotation_fast_heals(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_heal_rotation_fast_heals", sep->arg[0], "healrotationfastheals"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot healrotation fastheals <bot healrotation leader name> <on | off>");

	if (sep->argnum == 3) {
		Bot* leaderBot;
		std::string botName = std::string(sep->arg[3]);
		if (!botName.empty())
			leaderBot = entity_list.GetBotByBotName(botName);
		else {
			c->Message(13, "You must name a valid heal rotation leader.");
			return;
		}

		if (leaderBot) {
			bool fastHeals = false;
			std::list<Bot*> botList;
			if (leaderBot->GetBotOwner() != c) {
				c->Message(13, "You must target a bot that you own.");
				return;
			}

			if (!strcasecmp(sep->arg[4], "on"))
				fastHeals = true;
			else if (strcasecmp(sep->arg[4], "off")) {
				c->Message(0, "Usage #bot healrotation fastheals <bot healrotation leader name> <on | off>.");
				return;
			}

			botList = Bot::GetBotsInHealRotation(leaderBot);
			for (std::list<Bot*>::iterator botListItr = botList.begin(); botListItr != botList.end(); ++botListItr) {
				Bot* tempBot = *botListItr;
				if (tempBot && tempBot->GetBotOwnerCharacterID() == c->CharacterID())
					tempBot->SetHealRotationUseFastHeals(fastHeals);
			}
		}
		else {
			c->Message(13, "You must name a valid bot.");
			return;
		}
	}
	else {
		c->Message(0, "#bot healrotation fastheals <bot healrotation leader name> <on | off>");
		return;
	}
}

/**/
void bot_subcommand_heal_rotation_list(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_heal_rotation_list", sep->arg[0], "healrotationlist"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot healrotation list <bot healrotation leader name | all>");

	if (sep->argnum == 3) {
		bool showAll = false;
		Bot* leaderBot;
		std::string botName = std::string(sep->arg[3]);
		if (!strcasecmp(sep->arg[3], "all")) {
			std::list<Bot*> BotList = entity_list.GetBotsByBotOwnerCharacterID(c->CharacterID());
			for (std::list<Bot*>::iterator botListItr = BotList.begin(); botListItr != BotList.end(); ++botListItr) {
				Bot* tempBot = *botListItr;
				if (tempBot->GetInHealRotation() && tempBot->GetHealRotationLeader() == tempBot)
					c->Message(0, "Bot Heal Rotation- Leader: %s, Number of Members: %i, Timer: %1.1f", tempBot->GetCleanName(), tempBot->GetNumHealRotationMembers(), (float)(tempBot->GetHealRotationTimer() / 1000));
			}
		}
		else {
			std::string botName = std::string(sep->arg[3]);
			if (!botName.empty())
				leaderBot = entity_list.GetBotByBotName(botName);
			else {
				c->Message(13, "You must name a valid heal rotation leader.");
				return;
			}

			if (leaderBot) {
				std::list<Bot*> botList;
				if (leaderBot->GetBotOwner() != c) {
					c->Message(13, "You must target a bot that you own.");
					return;
				}

				botList = Bot::GetBotsInHealRotation(leaderBot);
				c->Message(0, "Bot Heal Rotation- Leader: %s", leaderBot->GetCleanName());
				c->Message(0, "Bot Heal Rotation- Timer: %1.1f", ((float)leaderBot->GetHealRotationTimer() / 1000.0f));
				for (std::list<Bot*>::iterator botListItr = botList.begin(); botListItr != botList.end(); ++botListItr) {
					Bot* tempBot = *botListItr;
					if (tempBot && tempBot->GetBotOwnerCharacterID() == c->CharacterID())
						c->Message(0, "Bot Heal Rotation- Member: %s", tempBot->GetCleanName());
				}

				for (int i = 0; i < MaxHealRotationTargets; i++) {
					if (leaderBot->GetHealRotationTarget(i)) {
						Mob* tempTarget = leaderBot->GetHealRotationTarget(i);
						if (tempTarget) {
							std::string targetInfo = "";
							targetInfo += tempTarget->GetHPRatio() < 0 ? "(dead) " : "";
							targetInfo += tempTarget->GetZoneID() != leaderBot->GetZoneID() ? "(not in zone) " : "";
							c->Message(0, "Bot Heal Rotation- Target: %s %s", tempTarget->GetCleanName(), targetInfo.c_str());
						}
					}
				}
			}
			else {
				c->Message(13, "You must name a valid bot.");
				return;
			}
		}
	}
	else {
		c->Message(0, "#bot healrotation list <bot healrotation leader name | all>");
		return;
	}
}

/**/
void bot_subcommand_heal_rotation_remove_member(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_heal_rotation_remove_member", sep->arg[0], "healrotationremovemember"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot healrotation removemember <bot healrotation leader name> <bot healrotation member name to remove>");

	if (sep->argnum == 4) {
		Bot* leaderBot;
		std::string botName = std::string(sep->arg[3]);

		if (!botName.empty())
			leaderBot = entity_list.GetBotByBotName(botName);
		else {
			c->Message(13, "You must name a valid bot.");
			return;
		}

		if (leaderBot) {
			if (leaderBot->GetBotOwner() != c) {
				c->Message(13, "You must target a bot that you own.");
				return;
			}

			Bot* healer;
			std::string healerName = std::string(sep->arg[4]);
			if (!healerName.empty())
				healer = entity_list.GetBotByBotName(healerName);
			else {
				c->Message(13, "You must name a valid bot.");
				return;
			}

			if (healer) {
				if (healer->GetBotOwner() != c) {
					c->Message(13, "You must target a bot that you own.");
					return;
				}

				if (leaderBot->RemoveHealRotationMember(healer))
					c->Message(0, "Bot heal rotation member removed successfully.");
				else
					c->Message(13, "Unable to remove bot from rotation.");
			}
			else {
				c->Message(13, "You must name a valid bot.");
				return;
			}
		}
		else {
			c->Message(13, "You must name a valid bot.");
			return;
		}
	}
	else {
		c->Message(0, "#bot healrotation removemember <bot healrotation leader name> <bot healrotation member name to remove>");
		return;
	}
}

/**/
void bot_subcommand_heal_rotation_remove_target(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_heal_rotation_remove_target", sep->arg[0], "healrotationremovetarget"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot healrotation removetarget <bot healrotation leader name> <bot healrotation target name to remove>");

	if (sep->argnum == 4) {
		Bot* leaderBot;
		std::string botName = std::string(sep->arg[3]);
		if (!botName.empty())
			leaderBot = entity_list.GetBotByBotName(botName);
		else {
			c->Message(13, "You must name a valid heal rotation leader.");
			return;
		}

		if (leaderBot) {
			if (leaderBot->GetBotOwner() != c) {
				c->Message(13, "You must target a bot that you own.");
				return;
			}

			Mob* target;
			std::string targetName = std::string(sep->arg[4]);
			if (!targetName.empty())
				target = entity_list.GetMob(targetName.c_str());
			else {
				c->Message(13, "You must name a valid target.");
				return;
			}

			if (target) {
				if (leaderBot->RemoveHealRotationTarget(target))
					c->Message(0, "Bot heal rotation target removed successfully.");
				else
					c->Message(13, "Unable to remove rotation target.");
			}
		}
		else {
			c->Message(13, "You must name a valid bot.");
			return;
		}
	}
	else {
		c->Message(0, "#bot healrotation removetarget <bot healrotation leader name> <bot healrotation target name to remove>");
		return;
	}
}

/**/
void bot_subcommand_heal_rotation_start(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_heal_rotation_start", sep->arg[0], "healrotationstart"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot healrotation start <bot healrotation leader name | all>");

	if (sep->argnum == 3) {
		if (!strcasecmp(sep->arg[3], "all")) {
			std::list<Bot*> BotList = entity_list.GetBotsByBotOwnerCharacterID(c->CharacterID());
			for (std::list<Bot*>::iterator botListItr = BotList.begin(); botListItr != BotList.end(); ++botListItr) {
				Bot* leaderBot = *botListItr;
				if (leaderBot->GetInHealRotation() && leaderBot->GetHealRotationLeader() == leaderBot) {
					std::list<Bot*> rotationMemberList;
					int index = 0;
					rotationMemberList = Bot::GetBotsInHealRotation(leaderBot);
					for (std::list<Bot*>::iterator rotationMemberItr = rotationMemberList.begin(); rotationMemberItr != rotationMemberList.end(); ++rotationMemberItr) {
						Bot* tempBot = *rotationMemberItr;
						if (tempBot) {
							tempBot->SetHealRotationActive(true);
							tempBot->SetHealRotationNextHealTime(Timer::GetCurrentTime() + index * leaderBot->GetHealRotationTimer() * 1000);
							tempBot->SetHasHealedThisCycle(false);
						}
						index++;
					}
					c->Message(0, "Bot heal rotation started successfully.");
				}
			}
		}
		else {
			Bot* leaderBot;
			std::string botName = std::string(sep->arg[3]);
			if (!botName.empty())
				leaderBot = entity_list.GetBotByBotName(botName);
			else {
				c->Message(13, "You must name a valid heal rotation leader.");
				return;
			}

			if (leaderBot) {
				std::list<Bot*> botList;
				int index = 0;
				if (leaderBot->GetBotOwner() != c) {
					c->Message(13, "You must target a bot that you own.");
					return;
				}

				botList = Bot::GetBotsInHealRotation(leaderBot);
				for (std::list<Bot*>::iterator botListItr = botList.begin(); botListItr != botList.end(); ++botListItr) {
					Bot* tempBot = *botListItr;
					if (tempBot) {
						tempBot->SetHealRotationActive(true);
						tempBot->SetHealRotationNextHealTime(Timer::GetCurrentTime() + index * leaderBot->GetHealRotationTimer() * 1000);
						tempBot->SetHasHealedThisCycle(false);
					}
					index++;
				}
				c->Message(0, "Bot heal rotation started successfully.");
			}
			else {
				c->Message(13, "You must name a valid bot.");
				return;
			}
		}
	}
	else {
		c->Message(0, "#bot healrotation start <bot healrotation leader name | all>");
		return;
	}
}

/**/
void bot_subcommand_heal_rotation_stop(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_heal_rotation_stop", sep->arg[0], "healrotationstop"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	c->Message(0, "#bot healrotation stop <bot healrotation leader name | all>");

	if (sep->argnum == 3) {
		if (!strcasecmp(sep->arg[3], "all")) {
			std::list<Bot*> BotList = entity_list.GetBotsByBotOwnerCharacterID(c->CharacterID());
			for (std::list<Bot*>::iterator botListItr = BotList.begin(); botListItr != BotList.end(); ++botListItr) {
				Bot* leaderBot = *botListItr;
				if (leaderBot->GetInHealRotation() && leaderBot->GetHealRotationLeader() == leaderBot) {
					std::list<Bot*> rotationMemberList;
					rotationMemberList = Bot::GetBotsInHealRotation(leaderBot);
					for (std::list<Bot*>::iterator rotationMemberItr = rotationMemberList.begin(); rotationMemberItr != rotationMemberList.end(); ++rotationMemberItr) {
						Bot* tempBot = *rotationMemberItr;
						if (tempBot) {
							tempBot->SetHealRotationActive(false);
							tempBot->SetHasHealedThisCycle(false);
						}
					}
					c->Message(0, "Bot heal rotation started successfully.");
				}
			}
		}
		else {
			Bot* leaderBot;
			std::string botName = std::string(sep->arg[3]);
			if (!botName.empty())
				leaderBot = entity_list.GetBotByBotName(botName);
			else {
				c->Message(13, "You must name a valid heal rotation leader.");
				return;
			}

			if (leaderBot) {
				std::list<Bot*> botList;
				if (leaderBot->GetBotOwner() != c) {
					c->Message(13, "You must target a bot that you own.");
					return;
				}

				botList = Bot::GetBotsInHealRotation(leaderBot);
				for (std::list<Bot*>::iterator botListItr = botList.begin(); botListItr != botList.end(); ++botListItr) {
					Bot* tempBot = *botListItr;
					if (tempBot && tempBot->GetBotOwnerCharacterID() == c->CharacterID()) {
						tempBot->SetHealRotationActive(false);
						tempBot->SetHasHealedThisCycle(false);
					}
				}

				c->Message(0, "Bot heal rotation stopped successfully.");
			}
			else {
				c->Message(13, "You must name a valid bot.");
				return;
			}
		}
	}
	else {
		c->Message(0, "#bot healrotation stop <bot healrotation leader name | all>");
		return;
	}
}

/**/
void bot_subcommand_inventory_list(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_inventory_list", sep->arg[0], "inventorylist"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "(<target>) usage: %s ([name])", sep->arg[0]);
		return;
	}

	//
	return;
	//

	std::string TempErrorMessage;

	if (!strcasecmp(sep->arg[1], "inventory") && !strcasecmp(sep->arg[2], "list")) {
		if (c->GetTarget() != nullptr) {
			if (c->GetTarget()->IsBot() && c->GetTarget()->CastToBot()->GetBotOwnerCharacterID() == c->CharacterID()) {
				Mob* b = c->GetTarget();
				int x = c->GetTarget()->CastToBot()->GetBotItemsCount(&TempErrorMessage);
				if (!TempErrorMessage.empty()) {
					c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
					return;
				}

				const char* equipped[EmuConstants::EQUIPMENT_SIZE + 1] = { "Charm", "Left Ear", "Head", "Face", "Right Ear", "Neck", "Shoulders", "Arms", "Back",
					"Left Wrist", "Right Wrist", "Range", "Hands", "Primary Hand", "Secondary Hand",
					"Left Finger", "Right Finger", "Chest", "Legs", "Feet", "Waist", "Ammo", "Powersource" };

				const ItemInst* inst = nullptr;
				const Item_Struct* item = nullptr;
				bool is2Hweapon = false;

				std::string item_link;
				Client::TextLink linker;
				linker.SetLinkType(linker.linkItemInst);

				for (int i = EmuConstants::EQUIPMENT_BEGIN; i <= (EmuConstants::EQUIPMENT_END + 1); ++i) {
					if ((i == MainSecondary) && is2Hweapon)
						continue;

					inst = b->CastToBot()->GetBotItem(i == 22 ? 9999 : i);
					if (inst)
						item = inst->GetItem();
					else
						item = nullptr;

					if (!TempErrorMessage.empty()) {
						c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
						return;
					}

					if (item == nullptr) {
						c->Message(15, "I need something for my %s (Item %i)", equipped[i], (i == 22 ? 9999 : i));
						continue;
					}

					if ((i == MainPrimary) && ((item->ItemType == ItemType2HSlash) || (item->ItemType == ItemType2HBlunt) || (item->ItemType == ItemType2HPiercing))) {
						is2Hweapon = true;
					}

					linker.SetItemInst(inst);
					item_link = linker.GenerateLink();
					c->Message(15, "Using %s in my %s (Item %i)", item_link.c_str(), equipped[i], (i == 22 ? 9999 : i));
				}
			}
			else
				c->Message(15, "You must group your bot first.");
		}
		else
			c->Message(15, "You must target a bot first.");

		return;
	}
}

/**/
void bot_subcommand_inventory_remove(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_inventory_remove", sep->arg[0], "inventoryremove"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "(<target>) usage: %s [slotid: 0-22] ([name])", sep->arg[0]);
		return;
	}

	//
	return;
	//

	std::string TempErrorMessage;

	if (!strcasecmp(sep->arg[1], "inventory") && !strcasecmp(sep->arg[2], "remove")) {
		if ((c->GetTarget() == nullptr) || (sep->arg[3][0] == '\0') || !c->GetTarget()->IsBot()) {
			c->Message(15, "Usage: #bot inventory remove [slotid] (You must have a bot targeted) ");
			return;
		}
		else if (c->GetTarget()->IsBot() && c->GetTarget()->CastToBot()->GetBotOwnerCharacterID() == c->CharacterID()) {
			if (c->GetTradeskillObject() || (c->trade->state == Trading))
				return;

			int slotId = atoi(sep->arg[3]);
			if ((slotId > EmuConstants::EQUIPMENT_END || slotId < EmuConstants::EQUIPMENT_BEGIN) && slotId != 9999) {
				c->Message(15, "A bot has 22 slots in its inventory, please choose a slot between 0 and 21 or 9999.");
				return;
			}

			const char* equipped[EmuConstants::EQUIPMENT_SIZE + 1] = { "Charm", "Left Ear", "Head", "Face", "Right Ear", "Neck", "Shoulders", "Arms", "Back",
				"Left Wrist", "Right Wrist", "Range", "Hands", "Primary Hand", "Secondary Hand",
				"Left Finger", "Right Finger", "Chest", "Legs", "Feet", "Waist", "Ammo", "Powersource" };

			const Item_Struct* itm = nullptr;
			const ItemInst* itminst = c->GetTarget()->CastToBot()->GetBotItem(slotId);
			if (itminst)
				itm = itminst->GetItem();

			if (!TempErrorMessage.empty()) {
				c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
				return;
			}

			// Don't allow the player to remove a lore item they already possess and cause a crash
			bool failedLoreCheck = false;
			if (itminst) {
				for (int m = AUG_BEGIN; m < EmuConstants::ITEM_COMMON_SIZE; ++m) {
					ItemInst *itma = itminst->GetAugment(m);
					if (itma) {
						if (c->CheckLoreConflict(itma->GetItem()))
							failedLoreCheck = true;
					}
				}

				if (c->CheckLoreConflict(itm))
					failedLoreCheck = true;
			}
			if (!failedLoreCheck) {
				if (itm) {
					c->PushItemOnCursor(*itminst, true);
					Bot *gearbot = c->GetTarget()->CastToBot();
					if ((slotId == MainRange) || (slotId == MainAmmo) || (slotId == MainPrimary) || (slotId == MainSecondary))
						gearbot->SetBotArcher(false);

					gearbot->RemoveBotItemBySlot(slotId, &TempErrorMessage);

					if (!TempErrorMessage.empty()) {
						c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
						return;
					}

					gearbot->BotRemoveEquipItem(slotId);
					gearbot->CalcBotStats();
					switch (slotId) {
					case MainCharm:
					case MainEar1:
					case MainHead:
					case MainFace:
					case MainEar2:
					case MainNeck:
					case MainBack:
					case MainWrist1:
					case MainWrist2:
					case MainRange:
					case MainPrimary:
					case MainSecondary:
					case MainFinger1:
					case MainFinger2:
					case MainChest:
					case MainWaist:
					case MainPowerSource:
					case MainAmmo:
						gearbot->BotGroupSay(gearbot, "My %s is now unequipped.", equipped[slotId]);
						break;
					case MainShoulders:
					case MainArms:
					case MainHands:
					case MainLegs:
					case MainFeet:
						gearbot->BotGroupSay(gearbot, "My %s are now unequipped.", equipped[slotId]);
						break;
					default:
						break;
					}
				}
				else {
					switch (slotId) {
					case MainCharm:
					case MainEar1:
					case MainHead:
					case MainFace:
					case MainEar2:
					case MainNeck:
					case MainBack:
					case MainWrist1:
					case MainWrist2:
					case MainRange:
					case MainPrimary:
					case MainSecondary:
					case MainFinger1:
					case MainFinger2:
					case MainChest:
					case MainWaist:
					case MainPowerSource:
					case MainAmmo:
						c->GetTarget()->CastToBot()->BotGroupSay(c->GetTarget()->CastToBot(), "My %s is already unequipped.", equipped[slotId]);
						break;
					case MainShoulders:
					case MainArms:
					case MainHands:
					case MainLegs:
					case MainFeet:
						c->GetTarget()->CastToBot()->BotGroupSay(c->GetTarget()->CastToBot(), "My %s are already unequipped.", equipped[slotId]);
						break;
					default:
						break;
					}
				}
			}
			else {
				c->Message_StringID(0, PICK_LORE);
			}
		}
		return;
	}
}

/**/
void bot_subcommand_item_augment(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_item_augment", sep->arg[0], "itemaugment"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "<target> usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	Mob* t = c->GetTarget();
	if (!t || !t->IsBot() || t->CastToBot()->GetBotOwner() != c) {
		c->Message(15, "You must target a bot that you own to use this command");
		return;
	}

	t->CastToBot()->FinishTrade(c, Bot::BotTradeClientNoDropNoTrade);

	if (!strcasecmp(sep->arg[1], "augmentitem") || !strcasecmp(sep->arg[1], "ai")) {
		AugmentItem_Struct* in_augment = new AugmentItem_Struct[sizeof(AugmentItem_Struct)];
		in_augment->container_slot = 1000; // <watch>
		in_augment->augment_slot = -1;
		Object::HandleAugmentation(c, in_augment, c->GetTradeskillObject());
		return;
	}
}

/**/
void bot_subcommand_item_give(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_item_give", sep->arg[0], "itemgive"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "(<target>) usage: %s ([name])", sep->arg[0]);
		return;
	}

	//
	return;
	//

	if (!strcasecmp(sep->arg[1], "usage")) {
		if (!strcasecmp(sep->arg[0], "gi"))
			c->Message(0, "usage: %cgi <target>", BOT_COMMAND_CHAR);
		else if (!strcasecmp(sep->arg[0], "giveitem"))
			c->Message(0, "usage: %cgiveitem <target>", BOT_COMMAND_CHAR);
		else
			c->Message(15, "Undefined linker usage in bot_command_give_item (%c%s)", BOT_COMMAND_CHAR, sep->arg[0]);

		return;
	}

	Mob* t = c->GetTarget();
	if (!t || !t->IsBot() || t->CastToBot()->GetBotOwner() != c) {
		c->Message(15, "You must target a bot that you own to use this command");
		return;
	}

	t->CastToBot()->FinishTrade(c, Bot::BotTradeClientNoDropNoTrade);
}

/**/
void bot_subcommand_pet_remove(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_pet_remove", sep->arg[0], "petremove"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "(<target>) usage: %s ([name])", sep->arg[0]);
		return;
	}

	//
	return;
	//

	if (c->GetTarget() != nullptr) {
		if (c->IsGrouped() && c->GetTarget()->IsBot() && (c->GetTarget()->CastToBot()->GetBotOwner() == c) &&
			((c->GetTarget()->GetClass() == NECROMANCER) || (c->GetTarget()->GetClass() == ENCHANTER) || (c->GetTarget()->GetClass() == DRUID))) {
			if (c->GetTarget()->CastToBot()->IsBotCharmer()) {
				c->GetTarget()->CastToBot()->SetBotCharmer(false);
				c->GetTarget()->CastToBot()->BotGroupSay(c->GetTarget()->CastToBot(), "Using a summoned pet.");
			}
			else {
				if (c->GetTarget()->GetPet()) {
					c->GetTarget()->GetPet()->Say_StringID(PET_GETLOST_STRING);
					c->GetTarget()->GetPet()->Depop(false);
					c->GetTarget()->SetPetID(0);
				}
				c->GetTarget()->CastToBot()->SetBotCharmer(true);
				c->GetTarget()->CastToBot()->BotGroupSay(c->GetTarget()->CastToBot(), "Available for Dire Charm command.");
			}
		}
		else
			c->Message(15, "You must target your Enchanter, Necromancer, or Druid bot.");
	}
	else
		c->Message(15, "You must target an Enchanter, Necromancer, or Druid bot.");
}

/**/
void bot_subcommand_pet_set_type(Client *c, const Seperator *sep)
{
	if (command_alias_fail(c, "bot_subcommand_pet_set_type", sep->arg[0], "petsettype"))
		return;

	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s", sep->arg[0]);
		return;
	}

	//
	return;
	//

	if (c->GetTarget() && c->GetTarget()->IsBot() && (c->GetTarget()->GetClass() == MAGICIAN)) {
		if (c->GetTarget()->CastToBot()->GetBotOwnerCharacterID() == c->CharacterID()) {
			int botlevel = c->GetTarget()->GetLevel();
			c->GetTarget()->CastToBot()->SetPetChooser(true);
			if (botlevel == 1) {
				c->GetTarget()->CastToBot()->BotGroupSay(c->GetTarget()->CastToBot(), "I don't have any pets yet.");
				return;
			}

			if (!strcasecmp(sep->arg[2], "water")) {
				c->GetTarget()->CastToBot()->SetPetChooserID(0);
			}
			else if (!strcasecmp(sep->arg[2], "fire")) {
				if (botlevel < 3) {
					c->GetTarget()->CastToBot()->BotGroupSay(c->GetTarget()->CastToBot(), "I don't have that pet yet.");
					return;
				}
				else
					c->GetTarget()->CastToBot()->SetPetChooserID(1);
			}
			else if (!strcasecmp(sep->arg[2], "air")) {
				if (botlevel < 4) {
					c->GetTarget()->CastToBot()->BotGroupSay(c->GetTarget()->CastToBot(), "I don't have that pet yet.");
					return;
				}
				else
					c->GetTarget()->CastToBot()->SetPetChooserID(2);
			}
			else if (!strcasecmp(sep->arg[2], "earth")) {
				if (botlevel < 5) {
					c->GetTarget()->CastToBot()->BotGroupSay(c->GetTarget()->CastToBot(), "I don't have that pet yet.");
					return;
				}
				else
					c->GetTarget()->CastToBot()->SetPetChooserID(3);
			}
			else if (!strcasecmp(sep->arg[2], "monster")) {
				if (botlevel < 30) {
					c->GetTarget()->CastToBot()->BotGroupSay(c->GetTarget()->CastToBot(), "I don't have that pet yet.");
					return;
				}
				else
					c->GetTarget()->CastToBot()->SetPetChooserID(4);
			}

			if (c->GetTarget()->GetPet()) {
				uint16 id = c->GetTarget()->GetPetID();
				c->GetTarget()->SetPetID(0);
				c->GetTarget()->CastSpell(331, id);
			}
		}
	}
	else
		c->Message(15, "You must target your Magician bot!");
}

/*X*/
void bot_subcommand_portal(Client *c, const Seperator *sep)
{
	bcst_list* local_list = &bot_command_spells[BCEnum::ST_Depart];
	if (spell_list_fail(c, local_list, BCEnum::ST_Depart) || command_alias_fail(c, "bot_subcommand_portal", sep->arg[0], "portal"))
		return;
	
	if (is_help_or_usage(sep->arg[1])) {
		c->Message(0, "usage: %s [list | destination]", sep->arg[0]);
		send_usage_required_bots(c, BCEnum::ST_Depart, WIZARD);
		return;
	}

	if (!strcasecmp(sep->arg[1], "list")) {
		c->Message(0, "The following destinations are available:");

		Bot* b = MyBot::AsGroupMember_ByClass(c, c, WIZARD);
		std::string msg;
		std::string text_link;

		int dc = 0;
		for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
			STDepartEntry* local_entry = (*iter_list)->SafeCastToDepart();
			if (!local_entry)
				continue;

			if (b && b->GetClass() == local_entry->caster_class && b->GetLevel() >= local_entry->spell_level) {
				msg = StringFormat("%cportal %s", BOT_COMMAND_CHAR, spells[local_entry->spell_id].teleport_zone);
				text_link = b->CreateSayLink(c, msg.c_str(), local_entry->long_name.c_str());
				Bot::BotGroupSay(b, "%s %s", spells[local_entry->spell_id].teleport_zone, text_link.c_str());
				++dc;
				continue;
			}
		}
		if (!dc)
			c->Message(15, "None");

		return;
	}

	Client* mtgmp = MyTarget::AsGroupMember_ByPlayer(c);
	if (!mtgmp) {
		c->Message(15, "You must <target> a player to use this command");
		return;
	}

	Bot* b = nullptr;
	Mob* t = nullptr;
	for (bcst_list::iterator iter_list = local_list->begin(); iter_list != local_list->end(); ++iter_list) {
		STDepartEntry* local_entry = (*iter_list)->SafeCastToDepart();
		if (!local_entry)
			continue;

		if (spells[local_entry->spell_id].zonetype && zone->GetZoneType() && !(spells[local_entry->spell_id].zonetype & zone->GetZoneType())) // needs verification (i.e., sp: 255, zn: 0)
			continue;

		switch (local_entry->target_type) {
		case BCEnum::TT_GroupV1:
			if (!mtgmp)
				continue;
			b = MyBot::AsGroupMember_ByMinLevelAndClass(c, mtgmp, local_entry->spell_level, local_entry->caster_class);
			t = mtgmp;
			break;
		default:
			continue;
		}
		if (!b)
			continue;

		b->InterruptSpell();
		Bot::BotGroupSay(b, "Attempting to cast '%s' on %s", spells[local_entry->spell_id].name, t->GetCleanName());
		b->CastSpell(local_entry->spell_id, t->GetID(), 1, -1, -1);

		break;
	}
	if (!b) {
		c->Message(15, "No bots in your group are capable of performing this action");
		return;
	}

	// TODO: Finalization
}


/*
 * bot command helpers go below here
 */
/*X*/
bool helper_bot_appearance_fail(Client *c, Bot *b, BCEnum::AFType fail_type, const char* type_desc)
{
	switch (fail_type) {
	case BCEnum::AFT_Value:
		c->Message(15, "Failed to change '%s' for %s due to invalid value for this command", type_desc, b->GetCleanName());
		return true;
	case BCEnum::AFT_GenderRace:
		c->Message(15, "Failed to change '%s' for %s due to invalid bot gender and/or race for this command", type_desc, b->GetCleanName());
		return true;
	case BCEnum::AFT_Race:
		c->Message(15, "Failed to change '%s' for %s due to invalid bot race for this command", type_desc, b->GetCleanName());
		return true;
	default:
		return false;
	}
}

/*X*/
void helper_bot_appearance_final(Client *c, Bot *b)
{
	if (!c || !MyBot::IsMyBot(c, b))
		return;

	if (!b->Save()) {
		c->Message(15, "Failed to save appearance change for %s due to unknown cause...", b->GetCleanName());
		return;
	}

	helper_bot_appearance_send_current(b);

	c->Message(0, "Successfully changed appearance for %s!", b->GetCleanName());

	// TODO: Finalization
}

/*X*/
void helper_bot_appearance_send_current(Bot *b)
{
	if (!b)
		return;

	b->SendIllusionPacket(
		b->GetRace(),
		b->GetGender(),
		0xFF,	//b->GetTexture(),		// 0xFF - change back if issues arise
		0xFF,	//b->GetHelmTexture(),	// 0xFF - change back if issues arise
		b->GetHairColor(),
		b->GetBeardColor(),
		b->GetEyeColor1(),
		b->GetEyeColor2(),
		b->GetHairStyle(),
		b->GetLuclinFace(),
		b->GetBeard(),
		0xFF,					// aa_title (0xFF)
		b->GetDrakkinHeritage(),
		b->GetDrakkinTattoo(),
		b->GetDrakkinDetails(),
		b->GetSize()
	);
}

/*X*/
uint32 helper_bot_create(Client *c, std::string bot_name, uint8 bot_class, uint16 bot_race, uint8 bot_gender)
{
	uint32 bot_id = 0;
	if (!c)
		return bot_id;

	if (!Bot::IsValidName(bot_name)) {
		c->Message(15, "'%s' is an invalid name. You may only use characters 'A-Z', 'a-z' and '_'", bot_name.c_str());
		return bot_id;
	}

	std::string TempErrorMessage;

	if (!Bot::IsBotNameAvailable(bot_name.c_str(), &TempErrorMessage)) {
		if (!TempErrorMessage.empty())
			c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		c->Message(15, "The name %s is already being used. Please choose a different name", bot_name.c_str());
		return bot_id;
	}

	if (!Bot::IsValidRaceClassCombo(bot_race, bot_class)) {
		c->Message(15, "'%s'(%u):'%s'(%u) is an invalid race-class combination",
			Bot::RaceIdToString(bot_race).c_str(), bot_race, Bot::ClassIdToString(bot_class).c_str(), bot_class);
		return bot_id;
	}

	if (bot_gender > FEMALE) {
		c->Message(15, "gender: %(M), %u(F)", MALE, FEMALE);
		return bot_id;
	}

	uint32 mbc = RuleI(Bots, CreationLimit);
	if (Bot::CreatedBotCount(c->CharacterID(), &TempErrorMessage) >= mbc) {
		if (!TempErrorMessage.empty())
			c->Message(13, "Database Error: %s", TempErrorMessage.c_str());
		c->Message(15, "You have reached the maximum limit of %i bots", mbc);
		return bot_id;
	}

	NPCType DefaultNPCTypeStruct = Bot::CreateDefaultNPCTypeStructForBot(
		bot_name.c_str(),
		"",
		c->GetLevel(),
		bot_race,
		bot_class,
		bot_gender
	);

	Bot* b = new Bot(DefaultNPCTypeStruct, c);

	if (!b->Save()) {
		c->Message(15, "Failed to create '%s' due to unknown cause", b->GetCleanName());
		return bot_id;
	}
	
	c->Message(0, "Successfully created '%s' (id: %u)", b->GetCleanName(), b->GetBotID());

	bot_id = b->GetBotID();
	safe_delete(b);

	return bot_id;

	// TODO: Finalization
}
