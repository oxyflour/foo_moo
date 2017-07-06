#include "api.h"

class main_thread_play_control : public main_thread_callback {
public:
	char cmd[1024] = { 0 };
	mg_conn *conn;
	db *_db;
	virtual void callback_run() {
		console::printf("foo_moo: play control '%s'", cmd);

		static_api_ptr_t<playback_control> pc;
		if (!strcmp(cmd, "meta")) {
			metadb_handle_ptr p_track;
			auto is_playing = pc->get_now_playing(p_track);
			conn->response_json(200, {
				{ "src", is_playing ? p_track->get_path() : "" },
				{ "duration", is_playing ? p_track->get_length() : 0 },
				{ "time", pc->playback_get_position() },
				{ "volume", pc->get_volume() },
				{ "paused", pc->is_paused() },
				{ "stopped", !pc->is_playing() },
			});
		}
		else if (strncmp(cmd, "start/", strlen("start/")) == 0) {
			auto id = atoi(cmd + strlen("start/"));
			if (id > 0) {
				auto ret = _db->query_track_from_id(id);
				if (ret["subsong"].get<int>() >= 0) {
					// TODO
					conn->response_json(200, ret);
				}
				else {
					conn->response_json(404, {});
				}
			}
			else {
				conn->response_json(404, {});
			}
		}
		else {
			if (!strcmp(cmd, "start")) {
				pc->play_start();
			}
			else if (!strcmp(cmd, "pause")) {
				pc->pause(true);
			}
			else if (!strcmp(cmd, "stop")) {
				pc->play_stop();
			}
			else if (!strcmp(cmd, "play-pause")) {
				pc->play_or_pause();
			}
			conn->response_json(200, {
				{ "result", "ok" }
			});
		}
	}
};

void api_playback_control::handle(mg_conn *conn, http_message *hm, mg_str prefix) {
	auto cb = new service_impl_t<main_thread_play_control>();
	strncpy(cb->cmd, hm->uri.p + prefix.len, hm->uri.len - prefix.len);
	cb->conn = conn;
	cb->_db = _db;
	static_api_ptr_t<main_thread_callback_manager> cbm;
	cbm->add_callback(cb);
}

void api_browse_library::handle(mg_conn *conn, http_message *hm, mg_str prefix) {
	char path[2048];
	mg_url_decode(hm->uri.p + prefix.len, hm->uri.len - prefix.len, path, sizeof(path), 0);

	char num[32];
	mg_get_http_var(&hm->query_string, "begin", num, sizeof(num));
	int begin = strlen(num) ? atoi(num) : 0;
	mg_get_http_var(&hm->query_string, "end", num, sizeof(num));
	int end = strlen(num) ? atoi(num) : -1;

	auto ret = _db->browse_items(path, begin, end);
	conn->response_json(200, ret);
}
