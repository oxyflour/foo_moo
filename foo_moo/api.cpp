#include "api.h"

static int playing_track_id;

json api_playback_control::get_playback_meta() {
	static_api_ptr_t<playback_control> pc;
	metadb_handle_ptr p_track;
	auto is_playing = pc->get_now_playing(p_track);
	return json({
		{ "srcId", playing_track_id },
		{ "duration", is_playing ? p_track->get_length() : 0 },
		{ "currentTime", pc->playback_get_position() },
		{ "volume", pc->get_volume() },
		{ "paused", pc->is_paused() },
		{ "stopped", !pc->is_playing() },
	});
}

class main_thread_play_control : public main_thread_callback {
public:
	char uri[1024] = { 0 };
	mg_conn *conn;
	db *_db;
	virtual void callback_run() {
		console::printf("foo_moo: play control '%s'", uri);

		static_api_ptr_t<playback_control> pc;
		if (!strcmp(uri, "meta")) {
			conn->response_json(200, {
				{ "meta", api_playback_control::get_playback_meta() },
			});
		}
		else if (!strncmp(uri, "src/", strlen("src/"))) {
			playing_track_id = atoi(strchr(uri, '/') + 1);

			auto ret = _db->query_track_from_id(playing_track_id);
			auto id = ret["id"].get<int>();
			if (playing_track_id > 0 && id == playing_track_id) {
				auto path = ret["path"].get<std::string>();
				auto subsong = ret["subsong"].get<int>();

				pfc::list_t<metadb_handle_ptr> temp;
				static_api_ptr_t<playlist_incoming_item_filter> pliif;
				pliif->process_location(path.c_str(), temp, false, NULL, NULL, core_api::get_main_window());

				static_api_ptr_t<playlist_manager> plm;
				plm->queue_flush();

				auto len = temp.get_count();
				for (auto i = 0; i < len; i++) {
					auto item = temp.get_item(i);
					if (item->get_subsong_index() == subsong) {
						plm->queue_add_item(item);
					}
				}

				pc->set_stop_after_current(true);
				conn->response_json(200, {
					{ "result", "ok" },
					{ "path", path },
					{ "subsong", subsong },
				});
			}
			else {
				conn->response_json(404, {
					{ "error", "track not found" },
				});
			}
		}
		else {
			if (!strcmp(uri, "start")) {
				pc->play_start();
			}
			else if (!strcmp(uri, "play")) {
				pc->play_or_unpause();
			}
			else if (!strcmp(uri, "pause")) {
				pc->pause(true);
			}
			else if (!strcmp(uri, "stop")) {
				pc->play_stop();
			}
			else if (!strcmp(uri, "play-pause")) {
				pc->play_or_pause();
			}
			conn->response_json(200, {
				{ "result", "ok" },
			});
		}
	}
};

void api_playback_control::handle(mg_conn *conn, http_message *hm, mg_str prefix) {
	auto cb = new service_impl_t<main_thread_play_control>();
	strncpy(cb->uri, hm->uri.p + prefix.len, hm->uri.len - prefix.len);
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
