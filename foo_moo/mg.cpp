#include "mg.h"

#define MG_CT_CMD_WRITE 0xabcdefab
#define MG_CT_CMD_FLAGS (0xabcdefab + 1)
#define MAX_WRITE_SIZE 4096

struct mg_send_proxy_header {
	mg_connection *nc;
	unsigned int cmd;
	size_t size;
};

static void send_handler(mg_connection *nc, int ev, void *p) {
	auto pp = (mg_send_proxy_header *)p;
	if (!pp || pp->nc != nc) {
		return;
	}
	else if (pp->cmd == MG_CT_CMD_WRITE) {
		mg_send(nc, pp + 1, pp->size);
	}
	else if (pp->cmd == MG_CT_CMD_FLAGS) {
		auto flags = (unsigned long *)(pp + 1);
		nc->flags |= *flags;
	}
}

static void event_handler(mg_connection *nc, int ev, void *p) {
	auto app = (mg *)nc->mgr->user_data;
	if (ev == MG_EV_ACCEPT) {
		app->get_conn(nc);
	}
	else if (ev == MG_EV_HTTP_REQUEST) {
		app->handle_route(nc, ev, (http_message *)p);
	}
	else if (ev == MG_EV_SEND) {
		app->get_conn(nc)->is_buffered = false;
	}
	else if (ev == MG_EV_CLOSE) {
		app->remove_conn(nc);
	}
}

mg_conn::mg_conn(mg_mgr *m, mg_connection *c) : mgr(m), nc(c) {
	tid = GetCurrentThreadId();
}

void mg_conn::response_json(int code, json *data) {
	char buf[1024];
	auto temp = data->dump();
	auto size = sprintf(buf, "HTTP/1.1 %d OK\r\n"
		"Content-Type: application/json; charset=utf-8\r\n"
		"Content-Length: %d\r\n"
		"\r\n", code, temp.length());
	send(buf, size);
	send(temp.c_str(), temp.length());
	set_flags(MG_F_SEND_AND_CLOSE);
}

void mg_conn::send(const void *p, size_t size) {
	if (!is_connected) {
		return;
	}

	is_buffered = true;

	if (GetCurrentThreadId() == tid) {
		mg_send(nc, p, size);
		return;
	}

	char temp[sizeof(mg_send_proxy_header) + MAX_WRITE_SIZE];
	auto header = (mg_send_proxy_header *)temp;
	header->nc = nc;
	header->cmd = MG_CT_CMD_WRITE;

	auto data = (char *)p;
	auto sent = 0;
	while (sent < size) {
		auto to_send = size - sent > MAX_WRITE_SIZE ? MAX_WRITE_SIZE : size - sent;

		header->size = to_send;
		memcpy(header + 1, data + sent, to_send);
		mg_broadcast(mgr, send_handler, header, sizeof(mg_send_proxy_header) + to_send);

		sent += to_send;
	}
}

void mg_conn::set_flags(unsigned long flags_to_set) {
	if (!is_connected) {
		return;
	}

	if (GetCurrentThreadId() == tid) {
		nc->flags |= flags_to_set;
		return;
	}

	char temp[sizeof(mg_send_proxy_header) + sizeof(unsigned long)];
	auto header = (mg_send_proxy_header *)temp;
	header->nc = nc;
	header->cmd = MG_CT_CMD_FLAGS;

	auto flags = (unsigned long *)(header + 1);
	*flags = flags_to_set;

	mg_broadcast(mgr, send_handler, header, sizeof(temp));
}

void mg::handle_route(mg_connection *nc, int ev, http_message *hm) {
	auto it = routes.begin();
	for (; it != routes.end(); it++) {
		if (mg_strncmp(it->prefix, hm->uri, it->prefix.len) == 0) {
			auto conn = get_conn(nc);
			it->handler->handle(conn, hm, it->prefix);
			break;
		}
	}

	if (it == routes.end()) {
		mg_serve_http(nc, hm, *opts);
	}
}

void mg::add_route(const char* pre, mg_route_handler *handler) {
	auto prefix = mg_mk_str(pre);
	mg_route_rule route = { prefix, handler };
	routes.insert(routes.begin(), route);
}

void mg::run_forever(const char* addr, mg_serve_http_opts *http_opts, bool *is_running) {
	opts = http_opts;
	mgr = new mg_mgr();
	mg_mgr_init(mgr, this);
	console::print("foo_moo: starting server...");

	auto nc = mg_bind(mgr, addr, event_handler);
	if (nc == NULL) {
		console::printf("foo_moo: bind to %s failed!", addr);
	}
	else {
		console::printf("foo_moo: server started ar port %s, doc root '%s'", addr, opts->document_root);
		mg_set_protocol_http_websocket(nc);
		while (is_running == NULL || *is_running) {
			mg_mgr_poll(mgr, 1000);
		}
	}

	mg_mgr_free(mgr);
}

void mg::broadcast_via_ws(const char *buf, int size) {
	for (auto c = mg_next(mgr, NULL); c != NULL; c = mg_next(mgr, c)) {
		if (c->flags & MG_F_IS_WEBSOCKET) {
			mg_send_websocket_frame(c, WEBSOCKET_OP_TEXT, buf, size >= 0 ? size : strlen(buf));
		}
	}
}
