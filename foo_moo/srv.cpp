#include "srv.h"
#include "db.h"

#include "../foobar2000/helpers/input_helpers.h"
#include "../lame-3.99.5/include/lame.h"

struct WAVE_FORMAT_HEADER {
	// RIFF WAVE CHUNK
	char szRiffID[4];  // 'R','I','F','F'
	DWORD dwRiffSize; // file size - 8
	char szRiffFormat[4]; // 'W','A','V','E'

	// FORMAT CHUNK
	char szFmtID[4]; // 'f', 'm', 't', ' '
	DWORD dwFormatSize; // 16 or 18 (for wExtra)
	WORD wFormatTag; // 0x001
	WORD wChannels; // 1 or 2
	DWORD dwSamplesPerSec;
	DWORD dwAvgBytesPerSec;
	WORD wBlockAlign;
	WORD wBitsPerSample;
	//	WORD wExtra;

	// DATA CHUNK
	char szDataID[4]; // 'd','a','t','a'
	DWORD dwDataSize; // size of data
};

static WAVE_FORMAT_HEADER create_wave_header(WORD channels, DWORD srate, DWORD dataSize, WORD wavbit) {
	WAVE_FORMAT_HEADER wf;

	memcpy_s(wf.szRiffID, 4, "RIFF", 4);
	wf.dwRiffSize = dataSize + sizeof(WAVE_FORMAT_HEADER) - 8;
	memcpy_s(wf.szRiffFormat, 4, "WAVE", 4);

	memcpy_s(wf.szFmtID, 4, "fmt ", 4);
	wf.dwFormatSize = 16;
	wf.wFormatTag = 0x001;
	wf.wChannels = channels;
	wf.dwSamplesPerSec = srate;
	wf.dwAvgBytesPerSec = wf.dwSamplesPerSec * wavbit / 8 * wf.wChannels;
	wf.wBlockAlign = wavbit / 8 * wf.wChannels;
	wf.wBitsPerSample = wavbit;
	//	wf.wExtra = 0;

	memcpy_s(wf.szDataID, 4, "data", 4);
	wf.dwDataSize = dataSize;
	return wf;
}

// This function returns the opened input_helper and try to decode the first chunk
// to guess the track data length in bytes & content length in bytes
static int guess_content_length(const char *fpath, int subsong, double seek, int wavbit,
		input_helper &ih, audio_chunk &chunk) {
	file_info_impl fi;
	abort_callback_dummy cb;

	// open file and get information
	ih.open_path(NULL, fpath, cb, false, false);
	ih.open_decoding(subsong, 0, cb);
	ih.get_info(subsong, fi, cb);
	if (seek > 0 && ih.can_seek()) {
		ih.seek(seek, cb);
	}
	else {
		seek = 0;
	}

	// get first chunk
	ih.run(chunk, cb);

	// guess track total bytes;
	if (wavbit > 0) {
		auto body_length = (int)((fi.get_length() - seek) *
			chunk.get_sample_rate() * chunk.get_channel_count() * wavbit / 8);
		return sizeof(WAVE_FORMAT_HEADER) + body_length;
	}
	else {
		return 0;
	}
}

static pfc::string8 get_gmt_date(const time_t current) {
	tm timer;
	char dateStr[128];
	gmtime_s(&timer, &current);
	strftime(dateStr, sizeof(dateStr), "%a, %d %b %Y %H:%M:%S GMT", &timer);
	return pfc::string8(dateStr);
}

class stream_service {
private:
	mg_conn *conn;

	int range_current = 0;
	void send_content_in_range(void *buf, size_t size) {
		if (range_begin < 0) {
			conn->send(buf, size);
			return;
		}

		int tosend = size, offset = 0;
		if (range_current + size > range_begin) {
			if (range_current < range_begin) {
				offset = range_begin - range_current;
				tosend = range_current + size - range_begin;
			}
			if (range_end > 0 && range_current + tosend > range_end + 1) {
				tosend = range_end + 1 - range_current;
			}
			if (tosend > 0) {
				conn->send((char *)buf + offset, tosend);
			}
		}

		range_current += size;
	}
public:
	char path[1024] = { 0 };
	int subsong = 0;
	double seek = 0;

	char fmt[32] = { 0 };
	int wavbit = 16;

	int range_begin = -1;
	int range_end = -1;

	stream_service(mg_conn *c) : conn(c) { }
	void stream_track() {
		input_helper ih;
		audio_chunk_impl_temporary chunk;

		// guess content length
		int content_length = 0;
		try {
			content_length = guess_content_length(path, subsong, seek, wavbit, ih, chunk);
		}
		catch (std::exception &e) {
			console::printf("foo_moo: stream open error: %s, file: %s", e.what(), path);
			ih.close();
			return;
		}

		auto use_lame = stricmp(fmt, "mp3") == 0;
		if (use_lame) {
			// we can not estimate the content length when encoding mp3
			content_length = 0;
		}

		if (range_begin >= 0 && range_end < 0) {
			range_end = content_length - 1;
		}
		console::printf("foo_moo: decoding file: %s, subsong %d, %s (%d-%d)", path, subsong, fmt, range_begin, range_end);

		auto mime = use_lame ? "audio/mpeg" : "audio/wav";
		char temp[1024];
		size_t size = 0;
		if (content_length > 0 && range_begin >= 0 && range_end > 0) {
			size = sprintf(temp, "HTTP/1.0 206 Partial Content\r\n"
				"Content-length: %d\r\n"
				"Accept-ranges: bytes\r\n"
				"Content-type: %s\r\n"
				"Content-range: bytes %d-%d/%d\r\n"
				"\r\n", range_end - range_begin + 1, mime, range_begin, range_end, content_length);
		}
		else if (content_length > 0) {
			size = sprintf(temp, "HTTP/1.0 200 OK\r\n"
				"Content-length: %d\r\n"
				"Accept-ranges: bytes\r\n"
				"Content-type: %s\r\n"
				"\r\n", content_length, mime);
		}
		else {
			size = sprintf(temp, "HTTP/1.0 200 OK\r\n"
				"Content-type: %s\r\n"
				"\r\n", mime);
		}
		conn->send(temp, size);

		lame_t lame = NULL;
		if (use_lame) {
			lame = lame_init();
		}

		range_current = 0;
		try {
			// prepare data
			if (lame) {
				lame_set_num_channels(lame, chunk.get_channel_count());
				lame_set_in_samplerate(lame, chunk.get_sample_rate());
				if (lame_init_params(lame) < 0) {
					throw std::exception("lame init failed");
				}
			}
			else if (wavbit == 8 || wavbit == 16 || wavbit == 24) {
				auto body_length = content_length - sizeof(WAVE_FORMAT_HEADER);
				auto wave_header = create_wave_header(chunk.get_channel_count(), chunk.get_sample_rate(), body_length, wavbit);
				send_content_in_range((char *)&wave_header, sizeof(WAVE_FORMAT_HEADER));
			}
			else {
				throw std::exception("unsupported encoding");
			}

			// do send
			mem_block_container_impl_t<> buf;
			static_api_ptr_t<audio_postprocessor> proc;
			abort_callback_dummy cb;
			do {
				// FIXME: dirty hack
				while (conn->is_connected && conn->is_buffered) {
					Sleep(50);
				}

				// convert to mp3
				if (lame) {
					buf.set_size((t_size)1.25 * chunk.get_sample_count() + 7200);
					int size = lame_encode_buffer_interleaved_ieee_float(lame,
						chunk.get_data(), chunk.get_sample_count(), (unsigned char *)buf.get_ptr(), buf.get_size());
					if (size < 0) {
						throw std::exception(pfc::string8("lame encode error: ") << size);
					}
					else {
						buf.set_size(size);
					}
				}
				// convert to wav
				else {
					proc->run(chunk, buf, wavbit, wavbit, false, 1.0);
				}

				// send data
				if (buf.get_size() > 0) {
					send_content_in_range(buf.get_ptr(), buf.get_size());
				}
			} while (ih.run(chunk, cb) && (range_end < 0 || range_current < range_end) && conn->is_connected);

			// flush lame buffer for mp3
			if (lame && buf.get_size() > 0) {
				if (buf.get_size() < 7200) { // !! MUST check buffer length
					buf.set_size(7200);
				}
				int size = lame_encode_flush(lame, (unsigned char *)buf.get_ptr(), buf.get_size());
				if (size > 0) {
					send_content_in_range(buf.get_ptr(), size);
				}
			}

			conn->set_flags(MG_F_SEND_AND_CLOSE);
			console::printf("foo_moo: stream finished");
		}
		catch (std::exception &e) {
			console::printf("foo_moo: stream failed (%s)", e.what());
		}

		if (lame) {
			lame_close(lame);
		}

		ih.close();
	}
};

static void *start_streaming(void *param) {
	auto sp = (stream_service *)param;
	sp->stream_track();
	delete sp;
	return NULL;
}

srv_stream_music::srv_stream_music(db* d) : _db(d) {
}

void srv_stream_music::handle(mg_conn *conn, http_message *hm, mg_str prefix) {
	auto sp = new stream_service(conn);

	char request_range[128] = { 0 };
	for (auto i = 0; hm->header_names[i].len > 0; i++) {
		auto name = hm->header_names[i];
		auto value = hm->header_values[i];
		if (!mg_strcmp(name, mg_mk_str("Range"))) {
			strncpy(request_range, value.p, value.len);
			sscanf_s(request_range, "bytes=%d-%d", &sp->range_begin, &sp->range_end);
		}
	}

	char uri[1024] = { 0 };
	strncpy(uri, hm->uri.p + prefix.len, hm->uri.len - prefix.len);

	auto ext = strrchr(uri, '.');
	strcpy(sp->fmt, ext ? ext + 1 : "wav");

	auto ret = _db->query_track_from_id(atoi(uri));
	sp->subsong = ret["subsong"].get<int>();

	auto path = ret["path"].get<std::string>();
	strcpy(sp->path, path.c_str());

	if (sp->subsong >= 0) {
		mg_start_thread(start_streaming, sp);
	}
	else {
		json ret({ });
		conn->response_json(404, &ret);
		delete sp;
	}
}
