/**
 * DO NOT MODIFY THIS FILE
 *
 * This repository was automatically generated and is NOT to be modified directly.
 * Any repository modifications are meant to be made to the repository extending the base.
 * Any modifications to base repositories are to be made by the generator only
 *
 * @generator ./utils/scripts/generators/repository-generator.pl
 * @docs https://docs.eqemu.io/developer/repositories
 */

#ifndef EQEMU_BASE_BOT_SETTINGS_REPOSITORY_H
#define EQEMU_BASE_BOT_SETTINGS_REPOSITORY_H

#include "../../database.h"
#include "../../strings.h"
#include <ctime>

class BaseBotSettingsRepository {
public:
	struct BotSettings {
		uint32_t    character_id;
		uint32_t    bot_id;
		uint8_t     stance;
		uint16_t    setting_id;
		uint8_t     setting_type;
		int32_t     value;
		std::string category_name;
		std::string setting_name;
	};

	static std::string PrimaryKey()
	{
		return std::string("character_id");
	}

	static std::vector<std::string> Columns()
	{
		return {
			"character_id",
			"bot_id",
			"stance",
			"setting_id",
			"setting_type",
			"value",
			"category_name",
			"setting_name",
		};
	}

	static std::vector<std::string> SelectColumns()
	{
		return {
			"character_id",
			"bot_id",
			"stance",
			"setting_id",
			"setting_type",
			"value",
			"category_name",
			"setting_name",
		};
	}

	static std::string ColumnsRaw()
	{
		return std::string(Strings::Implode(", ", Columns()));
	}

	static std::string SelectColumnsRaw()
	{
		return std::string(Strings::Implode(", ", SelectColumns()));
	}

	static std::string TableName()
	{
		return std::string("bot_settings");
	}

	static std::string BaseSelect()
	{
		return fmt::format(
			"SELECT {} FROM {}",
			SelectColumnsRaw(),
			TableName()
		);
	}

	static std::string BaseInsert()
	{
		return fmt::format(
			"INSERT INTO {} ({}) ",
			TableName(),
			ColumnsRaw()
		);
	}

	static BotSettings NewEntity()
	{
		BotSettings e{};

		e.character_id	= 0;
		e.bot_id        = 0;
		e.stance        = 0;
		e.setting_id    = 0;
		e.setting_type  = 0;
		e.value         = 0;
		e.category_name = "";
		e.setting_name  = "";

		return e;
	}

	static BotSettings GetBotSettings(
		const std::vector<BotSettings> &bot_settingss,
		int bot_settings_id
	)
	{
		for (auto &bot_settings : bot_settingss) {
			if (bot_settings.character_id == bot_settings_id) {
				return bot_settings;
			}
		}

		return NewEntity();
	}

	static BotSettings FindOne(
		Database& db,
		int bot_settings_id
	)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"{} WHERE {} = {} LIMIT 1",
				BaseSelect(),
				PrimaryKey(),
				bot_settings_id
			)
		);

		auto row = results.begin();
		if (results.RowCount() == 1) {
			BotSettings e{};

			e.character_id	= row[0] ? static_cast<uint32_t>(strtoul(row[0], nullptr, 10)) : 0;
			e.bot_id        = row[1] ? static_cast<uint32_t>(strtoul(row[1], nullptr, 10)) : 0;
			e.stance        = row[2] ? static_cast<uint8_t>(strtoul(row[2], nullptr, 10)) : 0;
			e.setting_id    = row[3] ? static_cast<uint16_t>(strtoul(row[3], nullptr, 10)) : 0;
			e.setting_type  = row[4] ? static_cast<uint8_t>(strtoul(row[4], nullptr, 10)) : 0;
			e.value         = row[5] ? static_cast<int32_t>(atoi(row[5])) : 0;
			e.category_name = row[6] ? row[6] : "";
			e.setting_name  = row[7] ? row[7] : "";

			return e;
		}

		return NewEntity();
	}

	static int DeleteOne(
		Database& db,
		int bot_settings_id
	)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"DELETE FROM {} WHERE {} = {}",
				TableName(),
				PrimaryKey(),
				bot_settings_id
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static int UpdateOne(
		Database& db,
		const BotSettings &e
	)
	{
		std::vector<std::string> v;

		auto columns = Columns();

		v.push_back(columns[0] + " = " + std::to_string(e.character_id));
		v.push_back(columns[1] + " = " + std::to_string(e.bot_id));
		v.push_back(columns[2] + " = " + std::to_string(e.stance));
		v.push_back(columns[3] + " = " + std::to_string(e.setting_id));
		v.push_back(columns[4] + " = " + std::to_string(e.setting_type));
		v.push_back(columns[5] + " = " + std::to_string(e.value));
		v.push_back(columns[6] + " = '" + Strings::Escape(e.category_name) + "'");
		v.push_back(columns[7] + " = '" + Strings::Escape(e.setting_name) + "'");

		auto results = db.QueryDatabase(
			fmt::format(
				"UPDATE {} SET {} WHERE {} = {}",
				TableName(),
				Strings::Implode(", ", v),
				PrimaryKey(),
				e.character_id
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static BotSettings InsertOne(
		Database& db,
		BotSettings e
	)
	{
		std::vector<std::string> v;

		v.push_back(std::to_string(e.character_id));
		v.push_back(std::to_string(e.bot_id));
		v.push_back(std::to_string(e.stance));
		v.push_back(std::to_string(e.setting_id));
		v.push_back(std::to_string(e.setting_type));
		v.push_back(std::to_string(e.value));
		v.push_back("'" + Strings::Escape(e.category_name) + "'");
		v.push_back("'" + Strings::Escape(e.setting_name) + "'");

		auto results = db.QueryDatabase(
			fmt::format(
				"{} VALUES ({})",
				BaseInsert(),
				Strings::Implode(",", v)
			)
		);

		if (results.Success()) {
			e.character_id = results.LastInsertedID();
			return e;
		}

		e = NewEntity();

		return e;
	}

	static int InsertMany(
		Database& db,
		const std::vector<BotSettings> &entries
	)
	{
		std::vector<std::string> insert_chunks;

		for (auto &e: entries) {
			std::vector<std::string> v;

			v.push_back(std::to_string(e.character_id));
			v.push_back(std::to_string(e.bot_id));
			v.push_back(std::to_string(e.stance));
			v.push_back(std::to_string(e.setting_id));
			v.push_back(std::to_string(e.setting_type));
			v.push_back(std::to_string(e.value));
			v.push_back("'" + Strings::Escape(e.category_name) + "'");
			v.push_back("'" + Strings::Escape(e.setting_name) + "'");

			insert_chunks.push_back("(" + Strings::Implode(",", v) + ")");
		}

		std::vector<std::string> v;

		auto results = db.QueryDatabase(
			fmt::format(
				"{} VALUES {}",
				BaseInsert(),
				Strings::Implode(",", insert_chunks)
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static std::vector<BotSettings> All(Database& db)
	{
		std::vector<BotSettings> all_entries;

		auto results = db.QueryDatabase(
			fmt::format(
				"{}",
				BaseSelect()
			)
		);

		all_entries.reserve(results.RowCount());

		for (auto row = results.begin(); row != results.end(); ++row) {
			BotSettings e{};

			e.character_id	= row[0] ? static_cast<uint32_t>(strtoul(row[0], nullptr, 10)) : 0;
			e.bot_id        = row[1] ? static_cast<uint32_t>(strtoul(row[1], nullptr, 10)) : 0;
			e.stance        = row[2] ? static_cast<uint8_t>(strtoul(row[2], nullptr, 10)) : 0;
			e.setting_id    = row[3] ? static_cast<uint16_t>(strtoul(row[3], nullptr, 10)) : 0;
			e.setting_type  = row[4] ? static_cast<uint8_t>(strtoul(row[4], nullptr, 10)) : 0;
			e.value         = row[5] ? static_cast<int32_t>(atoi(row[5])) : 0;
			e.category_name = row[6] ? row[6] : "";
			e.setting_name  = row[7] ? row[7] : "";

			all_entries.push_back(e);
		}

		return all_entries;
	}

	static std::vector<BotSettings> GetWhere(Database& db, const std::string &where_filter)
	{
		std::vector<BotSettings> all_entries;

		auto results = db.QueryDatabase(
			fmt::format(
				"{} WHERE {}",
				BaseSelect(),
				where_filter
			)
		);

		all_entries.reserve(results.RowCount());

		for (auto row = results.begin(); row != results.end(); ++row) {
			BotSettings e{};

			e.character_id	= row[0] ? static_cast<uint32_t>(strtoul(row[0], nullptr, 10)) : 0;
			e.bot_id        = row[1] ? static_cast<uint32_t>(strtoul(row[1], nullptr, 10)) : 0;
			e.stance        = row[2] ? static_cast<uint8_t>(strtoul(row[2], nullptr, 10)) : 0;
			e.setting_id    = row[3] ? static_cast<uint16_t>(strtoul(row[3], nullptr, 10)) : 0;
			e.setting_type  = row[4] ? static_cast<uint8_t>(strtoul(row[4], nullptr, 10)) : 0;
			e.value         = row[5] ? static_cast<int32_t>(atoi(row[5])) : 0;
			e.category_name = row[6] ? row[6] : "";
			e.setting_name  = row[7] ? row[7] : "";

			all_entries.push_back(e);
		}

		return all_entries;
	}

	static int DeleteWhere(Database& db, const std::string &where_filter)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"DELETE FROM {} WHERE {}",
				TableName(),
				where_filter
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static int Truncate(Database& db)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"TRUNCATE TABLE {}",
				TableName()
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static int64 GetMaxId(Database& db)
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"SELECT COALESCE(MAX({}), 0) FROM {}",
				PrimaryKey(),
				TableName()
			)
		);

		return (results.Success() && results.begin()[0] ? strtoll(results.begin()[0], nullptr, 10) : 0);
	}

	static int64 Count(Database& db, const std::string &where_filter = "")
	{
		auto results = db.QueryDatabase(
			fmt::format(
				"SELECT COUNT(*) FROM {} {}",
				TableName(),
				(where_filter.empty() ? "" : "WHERE " + where_filter)
			)
		);

		return (results.Success() && results.begin()[0] ? strtoll(results.begin()[0], nullptr, 10) : 0);
	}

	static std::string BaseReplace()
	{
		return fmt::format(
			"REPLACE INTO {} ({}) ",
			TableName(),
			ColumnsRaw()
		);
	}

	static int ReplaceOne(
		Database& db,
		const BotSettings &e
	)
	{
		std::vector<std::string> v;

		v.push_back(std::to_string(e.character_id));
		v.push_back(std::to_string(e.bot_id));
		v.push_back(std::to_string(e.stance));
		v.push_back(std::to_string(e.setting_id));
		v.push_back(std::to_string(e.setting_type));
		v.push_back(std::to_string(e.value));
		v.push_back("'" + Strings::Escape(e.category_name) + "'");
		v.push_back("'" + Strings::Escape(e.setting_name) + "'");

		auto results = db.QueryDatabase(
			fmt::format(
				"{} VALUES ({})",
				BaseReplace(),
				Strings::Implode(",", v)
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}

	static int ReplaceMany(
		Database& db,
		const std::vector<BotSettings> &entries
	)
	{
		std::vector<std::string> insert_chunks;

		for (auto &e: entries) {
			std::vector<std::string> v;

			v.push_back(std::to_string(e.character_id));
			v.push_back(std::to_string(e.bot_id));
			v.push_back(std::to_string(e.stance));
			v.push_back(std::to_string(e.setting_id));
			v.push_back(std::to_string(e.setting_type));
			v.push_back(std::to_string(e.value));
			v.push_back("'" + Strings::Escape(e.category_name) + "'");
			v.push_back("'" + Strings::Escape(e.setting_name) + "'");

			insert_chunks.push_back("(" + Strings::Implode(",", v) + ")");
		}

		std::vector<std::string> v;

		auto results = db.QueryDatabase(
			fmt::format(
				"{} VALUES {}",
				BaseReplace(),
				Strings::Implode(",", insert_chunks)
			)
		);

		return (results.Success() ? results.RowsAffected() : 0);
	}
};

#endif //EQEMU_BASE_BOT_SETTINGS_REPOSITORY_H
