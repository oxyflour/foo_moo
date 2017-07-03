#pragma once

// its wired but we have to include foobar2000 sdk before mongoose header
#include <winsock2.h>

#include "../foobar2000/SDK/foobar2000.h"
#include "../mongoose-6.8/mongoose.h"

#include "../json-2.1.1/json.hpp"
using json = nlohmann::json;

#include <vector>
#include <map>

class mg_conn {
private:
	mg_mgr *mgr;
	mg_connection *nc;
	DWORD tid;
public:
	bool is_connected = true;
	bool is_buffered = false;
	mg_conn(mg_mgr *m, mg_connection *c);
	void send(const void *p, size_t size);
	void response_json(int code, json *data);
	void set_flags(unsigned long flags);
};

class mg_route_handler {
public:
	virtual void handle(mg_conn *conn, http_message *hm, mg_str prefix) = 0;
};

struct mg_route_rule {
	mg_str prefix;
	mg_route_handler *handler;
};

class mg {
private:
	mg_mgr *mgr;
	mg_serve_http_opts *opts;

	std::vector<mg_route_rule> routes;
	std::map<mg_connection *, mg_conn *> conns;
public:
	void handle_route(mg_connection *nc, int ev, http_message *hm);

	void add_route(const char* path, mg_route_handler *handler);
	void run_forever(const char* addr, mg_serve_http_opts *opts, bool *is_running);

	void broadcast_via_ws(const char *buf, int size = -1);

	mg_conn *get_conn(mg_connection *nc) {
		if (conns[nc] == NULL) {
			conns[nc] = new mg_conn(mgr, nc);
		}
		conns[nc]->is_connected = true;
		return conns[nc];
	}
	void remove_conn(mg_connection *nc) {
		// TODO: clean up
		if (conns[nc] != NULL) {
			conns[nc]->is_connected = false;
		}
	}
};

