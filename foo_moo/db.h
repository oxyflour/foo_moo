#pragma once

#include "../foobar2000/SDK/foobar2000.h"
#include "../sqlite-amalgamation-3190300/sqlite3.h"

#include "../json-2.1.1/json.hpp"
using json = nlohmann::json;

#define DB_PATH_TABLE "lib_path"
#define DB_TRACK_TABLE "lib_track"

class db {
private:
	sqlite3 *_db;
	void init_db();
	void exec_sql_each(const char* action, const char* sql_tpl, const pfc::list_base_const_t<metadb_handle_ptr> &list);
public:
	db(const char *file);

	void add_items(const pfc::list_base_const_t<metadb_handle_ptr> &list);
	void remove_items(const pfc::list_base_const_t<metadb_handle_ptr> &list);
	void update_items(const pfc::list_base_const_t<metadb_handle_ptr> &list);

	json query_track_from_id(std::vector<int> ids);
	json query_track_from_id(int id);
	json browse_items(const char *path, int begin = 0, int end = -1);
};
