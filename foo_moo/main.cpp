#include "mg.h"
#include "db.h"
#include "api.h"
#include "srv.h"

#include <fstream>

#include "../json-2.1.1/json.hpp"
using json = nlohmann::json;

#define COMPONENT_NAME "foo_moo"
#define DB_FILE_NAME "database.sqlite3"
#define WWW_DIR_NAME "www"
#define CONFIG_FILE_NAME (COMPONENT_NAME ".json")
#define CONFIG_DOC_URL "https://docs.cesanta.com/mongoose/master"

DECLARE_COMPONENT_VERSION("Foobar2000 Media OO Plugin","0.0.1","about message goes here");
VALIDATE_COMPONENT_FILENAME(COMPONENT_NAME ".dll");

static auto load_config_as_json(const char *path) {
	json config({ });

	std::ifstream config_file(path);
	if (config_file.fail()) {
		console::printf("foo_moo: %s is not found. create it with foobar2000.exe if you want to change http serve settings. see " CONFIG_DOC_URL " for details", path);
	}
	else {
		try {
			config_file >> config;
		}
		catch (...) {
			console::printf("foo_moo: parse %s failed.", path);
		}
	}

	return config;
}

static const char *string_or_null(const json &data, const char* key, const char *val = NULL) {
	try {
		const auto it = data.find(key);
		if (it != data.end()) {
			return strdup(it->get<std::string>().c_str());
		}
	}
	catch (...) {
		console::printf("foo_moo: option %s ignored due to parsing error.", key);
	}
	return val;
}

class lib_listener : public library_callback_dynamic_impl_base {
private:
	mg *_mg;
	db *_db;
public:
	lib_listener(mg *m, db *d) : _mg(m), _db(d) { }
	void on_items_added(const pfc::list_base_const_t<metadb_handle_ptr> &list) {
		_db->add_items(list);
		_mg->broadcast_json({
			{ "type", "lib:add" }
		});
	}
	void on_items_removed(const pfc::list_base_const_t<metadb_handle_ptr> &list) {
		_db->remove_items(list);
		_mg->broadcast_json({
			{ "type", "lib:remove" }
		});
	}
	void on_items_modified(const pfc::list_base_const_t<metadb_handle_ptr> &list) {
		_db->update_items(list);
		_mg->broadcast_json({
			{ "type", "lib:update" }
		});
	}
};

class play_listener : public play_callback_impl_base {
private:
	mg *_mg;
public:
	play_listener(mg *p) : _mg(p) { }
	void on_playback_starting(play_control::t_track_command p_command, bool p_paused) {
		_mg->broadcast_json({
			{ "type", "play:start" },
			{ "is_paused", p_paused },
		});
	}
	void on_playback_new_track(metadb_handle_ptr p_track) {
		_mg->broadcast_json({
			{ "type", "play:track" },
		});
	}
	void on_playback_stop(play_control::t_stop_reason p_reason) {
		_mg->broadcast_json({
			{ "type", "play:stop" }
		});
	}
	void on_playback_pause(bool p_state) {
		_mg->broadcast_json({
			{ "type", "play:pause" },
			{ "is_paused", p_state },
		});
	}
	void on_volume_change(float p_new_val) {
		_mg->broadcast_json({
			{ "type", "play:volume" },
			{ "volume", p_new_val },
		});
	}
	void on_playback_seek(double p_time) {
		_mg->broadcast_json({
			{ "type", "play:seek" },
			{ "time", p_time },
		});
	}
};

static void *start_server(void* param);

class app_initquit : public initquit {
private:
	bool is_running;

	pfc::string8 profile_path;
	pfc::string8 component_path;
	pfc::string8 www_path;

	db *_db;
	mg *_mg;

	play_listener *pl_cb;
	lib_listener *lib_cb;
public:
	void run_forever() {
		auto config = load_config_as_json(CONFIG_FILE_NAME);
		mg_serve_http_opts opts;
		opts.auth_domain              = string_or_null(config, "auth_domain");
		opts.cgi_file_pattern         = string_or_null(config, "cgi_file_pattern");
		opts.cgi_interpreter          = string_or_null(config, "cgi_interpreter");
		opts.custom_mime_types        = string_or_null(config, "custom_mime_types");
		opts.dav_auth_file            = string_or_null(config, "dav_auth_file");
		opts.dav_document_root        = string_or_null(config, "dav_document_root");
		opts.document_root            = string_or_null(config, "document_root", www_path);
		opts.enable_directory_listing = string_or_null(config, "enable_directory_listing", "yes");
		opts.extra_headers            = string_or_null(config, "extra_headers");
		opts.global_auth_file         = string_or_null(config, "global_auth_file");
		opts.hidden_file_pattern      = string_or_null(config, "hidden_file_pattern");
		opts.index_files              = string_or_null(config, "index_files");
		opts.ip_acl                   = string_or_null(config, "ip_acl");
		opts.per_directory_auth_file  = string_or_null(config, "per_directory_auth_file");
		opts.ssi_pattern              = string_or_null(config, "ssi_pattern");
		opts.url_rewrites             = string_or_null(config, "url_rewrites");
		auto addr                     = string_or_null(config, "bind_address", "8080");

		_mg->add_route("/srv/stream/", new srv_stream_music(_db));
		_mg->add_route("/api/playback/", new api_playback_control());
		_mg->add_route("/api/browse/", new api_browse_library(_db));

		_mg->run_forever(addr, &opts, &is_running);
	}
	void on_init() {
		profile_path = pfc::string8() << core_api::get_profile_path() + strlen("file://");
		component_path = pfc::string8() << profile_path << "\\user-components\\" COMPONENT_NAME;
		www_path = pfc::string8() << component_path << "\\" WWW_DIR_NAME;

		_db = new db(pfc::string8() << component_path << "\\" DB_FILE_NAME);
		_mg = new mg();

		pl_cb = new play_listener(_mg);
		lib_cb = new lib_listener(_mg, _db);

		is_running = true;
		mg_start_thread(start_server, this);
	}
	void on_quit() {
		is_running = false;

		delete _db;
		delete _mg;

		delete pl_cb;
		delete lib_cb;
	}
};

static void *start_server(void* param) {
	auto app = (app_initquit *)param;
	app->run_forever();
	return NULL;
}

static initquit_factory_t<app_initquit> this_app;

