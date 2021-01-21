/*
 * Copyright 2003-2021 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "Database.hxx"
#include "Sticker.hxx"
#include "lib/sqlite/Util.hxx"
#include "fs/Path.hxx"
#include "Idle.hxx"
#include "util/StringCompare.hxx"
#include "util/ScopeExit.hxx"

#include <cassert>
#include <iterator>

using namespace Sqlite;

enum sticker_sql {
	STICKER_SQL_GET,
	STICKER_SQL_LIST,
	STICKER_SQL_UPDATE,
	STICKER_SQL_INSERT,
	STICKER_SQL_DELETE,
	STICKER_SQL_DELETE_VALUE,
	STICKER_SQL_FIND,
	STICKER_SQL_FIND_VALUE,
	STICKER_SQL_FIND_LT,
	STICKER_SQL_FIND_GT,
	STICKER_SQL_COUNT
};

static const char *const sticker_sql[] = {
	//[STICKER_SQL_GET] =
	"SELECT value FROM sticker WHERE type=? AND uri=? AND name=?",
	//[STICKER_SQL_LIST] =
	"SELECT name,value FROM sticker WHERE type=? AND uri=?",
	//[STICKER_SQL_UPDATE] =
	"UPDATE sticker SET value=? WHERE type=? AND uri=? AND name=?",
	//[STICKER_SQL_INSERT] =
	"INSERT INTO sticker(type,uri,name,value) VALUES(?, ?, ?, ?)",
	//[STICKER_SQL_DELETE] =
	"DELETE FROM sticker WHERE type=? AND uri=?",
	//[STICKER_SQL_DELETE_VALUE] =
	"DELETE FROM sticker WHERE type=? AND uri=? AND name=?",
	//[STICKER_SQL_FIND] =
	"SELECT uri,value FROM sticker WHERE type=? AND uri LIKE (? || '%') AND name=?",

	//[STICKER_SQL_FIND_VALUE] =
	"SELECT uri,value FROM sticker WHERE type=? AND uri LIKE (? || '%') AND name=? AND value=?",

	//[STICKER_SQL_FIND_LT] =
	"SELECT uri,value FROM sticker WHERE type=? AND uri LIKE (? || '%') AND name=? AND value<?",

	//[STICKER_SQL_FIND_GT] =
	"SELECT uri,value FROM sticker WHERE type=? AND uri LIKE (? || '%') AND name=? AND value>?",
};

static const char sticker_sql_create[] =
	"CREATE TABLE IF NOT EXISTS sticker("
	"  type VARCHAR NOT NULL, "
	"  uri VARCHAR NOT NULL, "
	"  name VARCHAR NOT NULL, "
	"  value VARCHAR NOT NULL"
	");"
	"CREATE UNIQUE INDEX IF NOT EXISTS"
	" sticker_value ON sticker(type, uri, name);"
	"";

StickerDatabase::StickerDatabase(Path path)
	:db(path.c_str())
{
	assert(!path.IsNull());

	int ret;

	/* create the table and index */

	ret = sqlite3_exec(db, sticker_sql_create,
			   nullptr, nullptr, nullptr);
	if (ret != SQLITE_OK)
		throw SqliteError(db, ret,
				  "Failed to create sticker table");

	/* prepare the statements we're going to use */

	for (unsigned i = 0; i < std::size(sticker_sql); ++i) {
		assert(sticker_sql[i] != nullptr);

		stmt[i] = Prepare(db, sticker_sql[i]);
	}
}

StickerDatabase::~StickerDatabase() noexcept
{
	assert(db != nullptr);

	for (unsigned i = 0; i < std::size(stmt); ++i) {
		assert(stmt[i] != nullptr);

		sqlite3_finalize(stmt[i]);
	}
}

std::string
StickerDatabase::LoadValue(const char *type, const char *uri, const char *name)
{
	sqlite3_stmt *const s = stmt[STICKER_SQL_GET];

	assert(type != nullptr);
	assert(uri != nullptr);
	assert(name != nullptr);

	if (StringIsEmpty(name))
		return std::string();

	BindAll(s, type, uri, name);

	AtScopeExit(s) {
		sqlite3_reset(s);
		sqlite3_clear_bindings(s);
	};

	std::string value;
	if (ExecuteRow(s))
		value = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));

	return value;
}

void
StickerDatabase::ListValues(std::map<std::string, std::string> &table,
			    const char *type, const char *uri)
{
	sqlite3_stmt *const s = stmt[STICKER_SQL_LIST];

	assert(type != nullptr);
	assert(uri != nullptr);

	BindAll(s, type, uri);

	AtScopeExit(s) {
		sqlite3_reset(s);
		sqlite3_clear_bindings(s);
	};

	ExecuteForEach(s, [s, &table](){
		const char *name = reinterpret_cast<const char *>(sqlite3_column_text(s, 0));
		const char *value = reinterpret_cast<const char *>(sqlite3_column_text(s, 1));
		table.insert(std::make_pair(name, value));
	});
}

bool
StickerDatabase::UpdateValue(const char *type, const char *uri,
			     const char *name, const char *value)
{
	sqlite3_stmt *const s = stmt[STICKER_SQL_UPDATE];

	assert(type != nullptr);
	assert(uri != nullptr);
	assert(name != nullptr);
	assert(*name != 0);
	assert(value != nullptr);

	BindAll(s, value, type, uri, name);

	AtScopeExit(s) {
		sqlite3_reset(s);
		sqlite3_clear_bindings(s);
	};

	bool modified = ExecuteModified(s);

	if (modified)
		idle_add(IDLE_STICKER);
	return modified;
}

void
StickerDatabase::InsertValue(const char *type, const char *uri,
			     const char *name, const char *value)
{
	sqlite3_stmt *const s = stmt[STICKER_SQL_INSERT];

	assert(type != nullptr);
	assert(uri != nullptr);
	assert(name != nullptr);
	assert(*name != 0);
	assert(value != nullptr);

	BindAll(s, type, uri, name, value);

	AtScopeExit(s) {
		sqlite3_reset(s);
		sqlite3_clear_bindings(s);
	};

	ExecuteCommand(s);
	idle_add(IDLE_STICKER);
}

void
StickerDatabase::StoreValue(const char *type, const char *uri,
			    const char *name, const char *value)
{
	assert(type != nullptr);
	assert(uri != nullptr);
	assert(name != nullptr);
	assert(value != nullptr);

	if (StringIsEmpty(name))
		return;

	if (!UpdateValue(type, uri, name, value))
		InsertValue(type, uri, name, value);
}

bool
StickerDatabase::Delete(const char *type, const char *uri)
{
	sqlite3_stmt *const s = stmt[STICKER_SQL_DELETE];

	assert(type != nullptr);
	assert(uri != nullptr);

	BindAll(s, type, uri);

	AtScopeExit(s) {
		sqlite3_reset(s);
		sqlite3_clear_bindings(s);
	};

	bool modified = ExecuteModified(s);
	if (modified)
		idle_add(IDLE_STICKER);
	return modified;
}

bool
StickerDatabase::DeleteValue(const char *type, const char *uri,
			     const char *name)
{
	sqlite3_stmt *const s = stmt[STICKER_SQL_DELETE_VALUE];

	assert(type != nullptr);
	assert(uri != nullptr);

	BindAll(s, type, uri, name);

	AtScopeExit(s) {
		sqlite3_reset(s);
		sqlite3_clear_bindings(s);
	};

	bool modified = ExecuteModified(s);
	if (modified)
		idle_add(IDLE_STICKER);
	return modified;
}

Sticker
StickerDatabase::Load(const char *type, const char *uri)
{
	Sticker s;

	ListValues(s.table, type, uri);

	return s;
}

sqlite3_stmt *
StickerDatabase::BindFind(const char *type, const char *base_uri,
			  const char *name,
			  StickerOperator op, const char *value)
{
	assert(type != nullptr);
	assert(name != nullptr);

	if (base_uri == nullptr)
		base_uri = "";

	switch (op) {
	case StickerOperator::EXISTS:
		BindAll(stmt[STICKER_SQL_FIND], type, base_uri, name);
		return stmt[STICKER_SQL_FIND];

	case StickerOperator::EQUALS:
		BindAll(stmt[STICKER_SQL_FIND_VALUE],
			type, base_uri, name, value);
		return stmt[STICKER_SQL_FIND_VALUE];

	case StickerOperator::LESS_THAN:
		BindAll(stmt[STICKER_SQL_FIND_LT],
			type, base_uri, name, value);
		return stmt[STICKER_SQL_FIND_LT];

	case StickerOperator::GREATER_THAN:
		BindAll(stmt[STICKER_SQL_FIND_GT],
			type, base_uri, name, value);
		return stmt[STICKER_SQL_FIND_GT];
	}

	assert(false);
	gcc_unreachable();
}

void
StickerDatabase::Find(const char *type, const char *base_uri, const char *name,
		      StickerOperator op, const char *value,
		      void (*func)(const char *uri, const char *value,
				   void *user_data),
		      void *user_data)
{
	assert(func != nullptr);

	sqlite3_stmt *const s = BindFind(type, base_uri, name, op, value);
	assert(s != nullptr);

	AtScopeExit(s) {
		sqlite3_reset(s);
		sqlite3_clear_bindings(s);
	};

	ExecuteForEach(s, [s, func, user_data](){
			func(reinterpret_cast<const char*>(sqlite3_column_text(s, 0)),
			     reinterpret_cast<const char*>(sqlite3_column_text(s, 1)),
			     user_data);
		});
}
