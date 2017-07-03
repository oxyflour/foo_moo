#pragma once
#include "mg.h"
#include "db.h"

// its wired but we have to include foobar2000 sdk before mongoose header
#include <winsock2.h>

#include "../foobar2000/SDK/foobar2000.h"
#include "../mongoose-6.8/mongoose.h"

class srv_stream_music : public mg_route_handler {
private:
	db* _db;
public:
	srv_stream_music(db *d);
	virtual void handle(mg_conn *conn, http_message *hm, mg_str prefix);
};
