#include "db.h"

class titleformat_sql_char_filter : public titleformat_text_filter {
public:
	void write(const GUID & p_inputtype, pfc::string_receiver & p_out,
		const char * p_data, t_size p_data_length) {
		t_size i, j = 0;
		for (i = 0; i < p_data_length && p_data[i] != 0; i++) {
			if (p_data[i] == '\'') {
				p_out.add_string(p_data + j, i - j);
				p_out.add_char('\'');
				j = i;
			}
		}
		p_out.add_string(p_data + j, p_data_length - j);
	}
};

//! SPECIAL WARNING: to allow multi-CPU optimizations to parse relative track paths, 
// this API works in threads other than the main app thread. 
// Main thread MUST be blocked while working in such scenarios, 
// it's NOT safe to call from worker threads while the Media Library content/configuration might be getting altered.
class titleformat_relative_path_hook : public titleformat_hook {
	metadb_handle_ptr &m_item;
	static_api_ptr_t<library_manager> &m_lib;
public:
	titleformat_relative_path_hook(metadb_handle_ptr &item, static_api_ptr_t<library_manager> &lib) : m_item(item), m_lib(lib) { }
	bool process_field(titleformat_text_out * p_out, const char * p_name, t_size p_name_length, bool & p_found_flag) {
		if (!strcmp(p_name, "relative_path")) {
			pfc::string8 path, rpath, pos;
			try {
				m_lib->get_relative_path(m_item, rpath);
				path = m_item->get_path();
				t_size sz = path.find_last('\\', path.get_length() - rpath.get_length() - 2) + 1;
				if (sz > path.get_length()) sz = 0;
				p_out->write(titleformat_inputtypes::unknown, path.get_ptr() + sz, path.get_length() - sz);
				return true;
			}
			catch (...) {
			}
			return true;
		}
		if (!strcmp(p_name, "path_index")) {
			pfc::string8 path, rpath, pos;
			try {
				m_lib->get_relative_path(m_item, rpath);
				path = m_item->get_path();
				if (strncmp(path.get_ptr(), "file://", 7)) {
					return false;
				}
				else {
					path = pfc::string8(path + 7);
				}
			}
			catch (...) {
				return false;
			}
			t_size p = path.find_last('\\', path.get_length() - rpath.get_length() - 2) + 1;
			pos << pfc::format_int(p > path.length() ? 0 : p, 3).toString();
			while ((p = path.find_first('\\', p + 1)) != ~0) {
				pos << pfc::format_int(strlen_utf8(path, p), 3).toString();
			}
			p_out->write(titleformat_inputtypes::unknown, pos.get_ptr());
			return true;
		}
		return false;
	}
	bool process_function(titleformat_text_out * p_out, const char * p_name,
			t_size p_name_length, titleformat_hook_function_params * p_params, bool & p_found_flag) {
		return false;
	}
};

db::db(const char *file) {
	if (sqlite3_open(file, &_db) != SQLITE_OK) {
		console::printf("foo_moo: open database file '%s' failed!", file);
	}
	else if (sqlite3_exec(_db, "SELECT * from `" DB_TRACK_TABLE "` LIMIT 0,1", 0, NULL, NULL) != SQLITE_OK) {
		console::printf("foo_moo: initializing database file '%s'...", file);
		init_db();
	}
}

void db::init_db() {
	char *err;
	auto ret = sqlite3_exec(_db, "CREATE TABLE `" DB_PATH_TABLE "` ("
			"id INTEGER PRIMARY KEY, "
			"directory_path VARCHAR(512), "
			"relative_path VARCHAR(512) UNIQUE, "
			"path_index VARCHAR(512), "
			"add_date DATETIME"
		"); "
		"CREATE TABLE `" DB_TRACK_TABLE "` ("
			"id INTEGER PRIMARY KEY AUTOINCREMENT, "
			"title VARCHAR(256), "
			"artist VARCHAR(256), "
			"album_artist VARCHAR(256), "
			"album VARCHAR(256), "
			"date VARCHAR(256), "
			"genre VARCHAR(256), "
			"tracknumber INTEGER, "
			"codec VARCHAR(256), "
			"filename_ext VARCHAR(256), "
			"pid INTEGER, "
			"subsong INTEGER, "
			"length VARCHAR(256), "
			"length_seconds INTEGER, "
			"last_modified DATETIME, "
			"UNIQUE (pid, filename_ext, subsong) ON CONFLICT REPLACE"
		");", 0, NULL, &err);
	if (ret != SQLITE_OK) {
		console::printf("foo_moo: init database failed! (err %d: %s)", ret, err ? err : "unknown");
		sqlite3_free(err);
	}
	else {
		static_api_ptr_t<library_manager> lib;
		pfc::list_t<metadb_handle_ptr> list;
		lib->get_all_items(list);
		add_items(list);
	}
}

void db::exec_sql_each(const char* action, const char* sql_tpl,
		const pfc::list_base_const_t<metadb_handle_ptr> &list) {
	// Prepare formatter
	static_api_ptr_t<titleformat_compiler> tfc;
	service_ptr_t<titleformat_object> sqlfmt;
	tfc->compile_force(sqlfmt, sql_tpl);

	auto len = list.get_count();
	if (_db != NULL && len > 0) {
		pfc::string8_fastalloc sql;
		auto flt = titleformat_sql_char_filter();
		static_api_ptr_t<library_manager> lib;
		char *err;
		// use transaction to speed up
		sqlite3_exec(_db, "begin transaction;", NULL, NULL, &err);
		for (t_size i = 0; i < len; i++) {
			auto item = list.get_item(i);
			auto hook = titleformat_relative_path_hook(item, lib);
			item->format_title(&hook, sql, sqlfmt, &flt);
			// execute sql
			auto ret = sqlite3_exec(_db, sql.get_ptr(), 0, NULL, &err);
			if (ret != SQLITE_OK) {
				console::printf("foo_moo: %s item(s) failed! (err %d: %s)", action, ret, err ? err : "unknown");
				sqlite3_free(err);
			}
		}
		auto ret = sqlite3_exec(_db, "commit transaction;", NULL, NULL, &err);
		if (ret != SQLITE_OK) {
			console::printf("foo_moo: update media database failed! (err %d: %s)", action, ret, err ? err : "unknown");
			sqlite3_free(err);
		}
		else {
			console::printf("foo_moo: %s %d item(s) done", action, len);
		}
	}
}

void db::add_items(const pfc::list_base_const_t<metadb_handle_ptr> &list) {
	const auto tpl = "INSERT OR IGNORE INTO `" DB_PATH_TABLE "` '('"
			"directory_path, "
			"relative_path, "
			"path_index, "
			"add_date"
		"')' VALUES '('"
			"''$directory_path(%path%)\\'', "
			"''$directory_path(%relative_path%)\\'', "
			"''%path_index%'', "
			"DATETIME'('''now'', ''localtime''')'"
		"')';"
		"INSERT OR REPLACE INTO `" DB_TRACK_TABLE "` '('"
			"title, "
			"artist, "
			"album_artist, "
			"album, "
			"date, "
			"genre, "
			"tracknumber, "
			"codec, "
			"filename_ext, "
			"pid, "
			"subsong, "
			"length, "
			"length_seconds, "
			"last_modified"
		"')' VALUES '('"
			"''%title%'', "
			"''%artist%'', "
			"''%album artist%'', "
			"''%album%'', "
			"''%date%'', "
			"''%genre%'', "
			"''%tracknumber%'', "
			"''%codec%'', "
			"''%filename_ext%'', "
			"'('SELECT id from `" DB_PATH_TABLE "` WHERE directory_path=''$directory_path(%path%)\\'' LIMIT 0,1')', "
			"%subsong%, "
			"''%length%'', "
			"%length_seconds%, "
			"''%last_modified%''"
		"')'; ";
	exec_sql_each("add", tpl, list);
}

void db::remove_items(const pfc::list_base_const_t<metadb_handle_ptr> &list) {
	const auto tpl = "DELETE FROM `" DB_TRACK_TABLE "` "
		"WHERE pid='('SELECT id FROM `" DB_PATH_TABLE "` WHERE directory_path=''$directory_path(%path%)\\'' LIMIT 0,1')' "
			"AND filename_ext=''%filename_ext%'' "
			"AND subsong=%subsong%; "
		"DELETE FROM `" DB_PATH_TABLE "` WHERE directory_path=''$directory_path(%path%)\\'' "
			"AND NOT EXISTS '('SELECT  `" DB_TRACK_TABLE "`.id FROM `" DB_TRACK_TABLE "` LEFT JOIN `" DB_PATH_TABLE "` "
			"ON pid=`" DB_PATH_TABLE "`.id WHERE directory_path=''$directory_path(%path%)\\'' LIMIT 0,1')';";
	exec_sql_each("remove", tpl, list);
}

void db::update_items(const pfc::list_base_const_t<metadb_handle_ptr> &list) {
	const auto tpl = "UPDATE `" DB_TRACK_TABLE "` SET "
			"`title`=''%title%'', "
			"`artist`=''%artist%'', "
			"`album_artist`=''%album artist%'', "
			"`album`=''%album%'', "
			"`date`=''%date%'', "
			"`genre`=''%genre%'', "
			"`tracknumber`=''%tracknumber%'', "
			"`codec`=''%codec%'', "
			"`length`=''%length%'', "
			"`length_seconds`=%length_seconds%, "
			"`last_modified`=''%last_modified%'' "
		"WHERE `pid`='('SELECT id FROM `" DB_PATH_TABLE "` WHERE directory_path=''$directory_path(%path%)\\'' LIMIT 0,1')' "
			"AND filename_ext=''%filename_ext%'' "
			"AND subsong=%subsong%;";
	exec_sql_each("update", tpl, list);
}

json db::query_track_from_id(std::vector<int> ids) {
	auto list = json::array();

	char ids_join[4096] = { 0 };
	char case_join[4096] = { 0 };
	for (auto i = 0, n = 0, m = 0; i < ids.size() && n < sizeof(ids_join); i++) {
		n += sprintf(ids_join + n, n == 0 ? "%d" : ", %d", ids[i]);
		m += sprintf(case_join + m, m == 0 ? "WHEN %d THEN %d" : " WHEN %d THEN %d", ids[i], i);
	}

	char sql[8192] = { 0 };
	sprintf(sql, "SELECT `" DB_TRACK_TABLE "`.`id`, `directory_path` || `filename_ext`, `subsong` FROM `" DB_TRACK_TABLE "` "
		"LEFT JOIN `" DB_PATH_TABLE "` ON `" DB_TRACK_TABLE "`.`pid`=`" DB_PATH_TABLE "`.`id` "
		"WHERE `" DB_TRACK_TABLE "`.`id` in (%s) "
		"ORDER BY CASE `" DB_TRACK_TABLE "`.`id` %s END", ids_join, case_join);

	sqlite3_stmt *stmt;
	if (sqlite3_prepare_v2(_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) != SQLITE_DONE) {
			list.push_back({
				{ "id", sqlite3_column_int(stmt, 0) },
				{ "path", (const char *) sqlite3_column_text(stmt, 1) },
				{ "subsong", sqlite3_column_int(stmt, 2) },
			});
		}
	}
	sqlite3_finalize(stmt);

	return list;
}

json db::query_track_from_id(int id) {
	std::vector<int> ids = { id };
	auto list = query_track_from_id(ids);
	return list[0];
}

static void strrep(char *str, char find, char replace) {
	for (auto p = str; *p; p++) {
		if (*p == find) {
			*p = replace;
		}
	}
}

struct folder_id_path {
	int id;
	std::string path;
};

json db::browse_items(const char *p, int begin, int end) {
	auto list = json::array();
	int total = 0;

	char path[4096];
	strcpy(path, p);
	strrep(path, '/', '\\');

	auto slashes_count = 0;
	for (auto p = path; *p; p++) {
		slashes_count += *p == '\\';
	}

	sqlite3_stmt *stmt;
	char sql[4096];
	std::vector<folder_id_path> dirs;

	// get folders
	sprintf(sql, "SELECT i, d, b, SUBSTR(b, 1, e - r) AS p "
		"FROM(SELECT *, id AS i, relative_path AS b, directory_path AS d, "
			"CAST(SUBSTR(path_index, 1, 3) AS INTEGER) AS r, "
			"MAX(CAST(SUBSTR(path_index, %d, 3) AS INTEGER), CAST(SUBSTR(path_index, %d, 3) AS INTEGER)) + 1 AS e "
			"FROM " DB_PATH_TABLE " WHERE SUBSTR(relative_path, 1, %d) = ?) "
		"GROUP BY p",
		slashes_count * 3 + 1, slashes_count * 3 + 4, pfc::strlen_utf8(path));
	if (sqlite3_prepare_v2(_db, sql, -1, &stmt, NULL) == SQLITE_OK &&
		sqlite3_bind_text(stmt, 1, path, strlen(path), SQLITE_STATIC) == SQLITE_OK) {
		while (sqlite3_step(stmt) != SQLITE_DONE) {
			auto id = sqlite3_column_int(stmt, 0);
			auto real_path = (const char *)sqlite3_column_text(stmt, 1);
			auto dir = (const char *)sqlite3_column_text(stmt, 2);
			if (strcmp(path, dir) == 0) {
				folder_id_path ip = { id, real_path };
				dirs.push_back(ip);
			}
			else {
				if (total >= begin && (end < 0 || total < end)) {
					char path[4096];
					sprintf(path, "/%s", sqlite3_column_text(stmt, 3));
					strrep(path, '\\', '/');
					list.push_back(json({
						{ "type", "folder" },
						{ "id", id },
						{ "path", path },
					}));
				}
				total++;
			}
		}
	}
	sqlite3_finalize(stmt);

	// get tracks
	for (auto i = dirs.begin(); i != dirs.end(); ++i) {
		strcpy(sql, "SELECT id, filename_ext, subsong, title, tracknumber, artist, album, album_artist, length, length_seconds, codec "
			"FROM " DB_TRACK_TABLE " WHERE pid=? ORDER BY album, tracknumber");
		if (sqlite3_prepare_v2(_db, sql, -1, &stmt, NULL) == SQLITE_OK &&
			sqlite3_bind_int(stmt, 1, i->id) == SQLITE_OK) {
			while (sqlite3_step(stmt) != SQLITE_DONE) {
				if (total >= begin && (end < 0 || total < end)) {
					list.push_back(json({
						{ "type", "track" },
						{ "id", sqlite3_column_int(stmt, 0) },
						{ "title", (const char *)sqlite3_column_text(stmt, 3) },
						{ "tracknumber", sqlite3_column_int(stmt, 4) },
						{ "artist", (const char *)sqlite3_column_text(stmt, 5) },
						{ "album", (const char *)sqlite3_column_text(stmt, 6) },
						{ "album_artist", (const char *)sqlite3_column_text(stmt, 7) },
						{ "length", (const char *)sqlite3_column_text(stmt, 8) },
						{ "length_seconds", sqlite3_column_int(stmt, 9) },
						{ "codec", (const char *)sqlite3_column_text(stmt, 10) },
					}));
				}
				total++;
			}
		}
		sqlite3_finalize(stmt);
	}

	return json({
		{ "total", total },
		{ "list", list }
	});
}

