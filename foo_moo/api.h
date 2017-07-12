#pragma once

#include "mg.h"
#include "db.h"

// its wired but we have to include foobar2000 sdk before mongoose header
#include <winsock2.h>

#include "../foobar2000/SDK/foobar2000.h"
#include "../mongoose-6.8/mongoose.h"

class api_playback_control : public mg_route_handler {
private:
	db *_db;
public:
	api_playback_control(db *d) : _db(d) { }
	static json get_playback_meta();
	virtual void handle(mg_conn *conn, http_message *hm, mg_str prefix);
};

class api_browse_library : public mg_route_handler {
private:
	db *_db;
public:
	api_browse_library(db *d) : _db(d) { }
	virtual void handle(mg_conn *conn, http_message *hm, mg_str prefix);
};
