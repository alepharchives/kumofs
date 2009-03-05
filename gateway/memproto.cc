#include "gateway/memproto.h"
#include "memproto/memproto.h"
#include <mp/object_callback.h>
#include <mp/stream_buffer.h>
#include <stdexcept>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <memory>
#include <deque>

namespace kumo {


static const size_t MEMPROTO_INITIAL_ALLOCATION_SIZE = 2048;
static const size_t MEMPROTO_RESERVE_SIZE = 1024;


Memproto::Memproto(int lsock) :
	m_lsock(lsock) { }

Memproto::~Memproto() {}


void Memproto::accepted(Gateway* gw, int fd, int err)
{
	if(fd < 0) {
		LOG_FATAL("accept failed: ",strerror(err));
		gw->signal_end();
		return;
	}
	LOG_DEBUG("accept memproto text user fd=",fd);
	wavy::add<Connection>(fd, gw);
}

void Memproto::listen(Gateway* gw)
{
	using namespace mp::placeholders;
	wavy::listen(m_lsock,
			mp::bind(&Memproto::accepted, gw, _1, _2));
}



class Memproto::Connection : public wavy::handler {
public:
	Connection(int fd, Gateway* gw);
	~Connection();

public:
	void read_event();

private:
	// get, getq, getk, getkq
	inline void request_getx(memproto_header* h,
			const char* key, uint16_t keylen);

	// set
	inline void request_set(memproto_header* h,
			const char* key, uint16_t keylen,
			const char* val, uint32_t vallen,
			uint32_t flags, uint32_t expiration);

	// delete
	inline void request_delete(memproto_header* h,
			const char* key, uint16_t keylen,
			uint32_t expiration);

	// noop
	inline void request_noop(memproto_header* h);

	inline void request_flush(memproto_header* h,
			uint32_t expiration);

private:
	memproto_parser m_memproto;
	mp::stream_buffer m_buffer;

	Gateway* m_gw;

	typedef Gateway::get_request gw_get_request;
	typedef Gateway::set_request gw_set_request;
	typedef Gateway::delete_request gw_delete_request;

	typedef Gateway::get_response gw_get_response;
	typedef Gateway::set_response gw_set_response;
	typedef Gateway::delete_response gw_delete_response;

	typedef rpc::shared_zone shared_zone;

	shared_zone m_zone;


	struct entry;


	class response_queue {
	public:
		response_queue(int fd);
		~response_queue();

		void push_entry(entry* e, shared_zone& life);
		void reached_try_send(entry* e, struct iovec* vec, size_t veclen);

		int is_valid() const;
		void invalidate();

	private:
		bool m_valid;
		int m_fd;

		struct element_t {
			entry* e;
			shared_zone life;
			struct iovec* vec;
			size_t veclen;
		};

		mp::pthread_mutex m_queue_mutex;

		typedef std::deque<element_t> queue_t;
		queue_t m_queue;

		struct find_entry_compare;

	private:
		response_queue();
		response_queue(const response_queue&);
	};

	typedef mp::shared_ptr<response_queue> shared_entry_queue;
	shared_entry_queue m_queue;


	struct entry {
		shared_entry_queue queue;
		memproto_header header;
	};


	// get, getq, getk, getkq
	struct get_entry : entry {
		bool flag_key;
		bool flag_quiet;
	};
	static void response_getx(void* user, gw_get_response& res);


	// set
	struct set_entry : entry {
	};
	static void response_set(void* user, gw_set_response& res);


	// delete
	struct delete_entry : entry {
	};
	static void response_delete(void* user, gw_delete_response& res);


	static void send_response_nosend(entry* e);

	static void send_response_nodata(entry* e, msgpack::zone& life,
			uint8_t status);

	static void send_response(entry* e, msgpack::zone& life,
			uint8_t status,
			const char* key, uint16_t keylen,
			const void* val, uint16_t vallen,
			const char* extra, uint16_t extralen);

	static inline void pack_header(
			char* hbuf, uint16_t status, uint8_t op,
			uint16_t keylen, uint32_t vallen, uint8_t extralen,
			uint32_t opaque, uint64_t cas);

private:
	Connection();
	Connection(const Connection&);
};


Memproto::Connection::Connection(int fd, Gateway* gw) :
	wavy::handler(fd),
	m_buffer(MEMPROTO_INITIAL_ALLOCATION_SIZE),
	m_gw(gw),
	m_zone(new msgpack::zone()),
	m_queue(new response_queue(fd))
{
	void (*cmd_getx)(void*, memproto_header*,
			const char*, uint16_t) = &mp::object_callback<void (memproto_header*,
				const char*, uint16_t)>
				::mem_fun<Connection, &Connection::request_getx>;

	void (*cmd_set)(void*, memproto_header*,
			const char*, uint16_t,
			const char*, uint32_t,
			uint32_t, uint32_t) = &mp::object_callback<void (memproto_header*,
				const char*, uint16_t,
				const char*, uint32_t,
				uint32_t, uint32_t)>
				::mem_fun<Connection, &Connection::request_set>;

	void (*cmd_delete)(void*, memproto_header*,
			const char*, uint16_t,
			uint32_t) = &mp::object_callback<void (memproto_header*,
				const char*, uint16_t,
				uint32_t)>
				::mem_fun<Connection, &Connection::request_delete>;

	void (*cmd_noop)(void*, memproto_header*) =
			&mp::object_callback<void (memproto_header*)>
				::mem_fun<Connection, &Connection::request_noop>;

	void (*cmd_flush)(void*, memproto_header*,
			uint32_t) = &mp::object_callback<void (memproto_header*,
				uint32_t expiration)>
				::mem_fun<Connection, &Connection::request_flush>;

	memproto_callback cb = {
		cmd_getx,    // get
		cmd_set,     // set
		NULL,        // add
		NULL,        // replace
		cmd_delete,  // delete
		NULL,        // increment
		NULL,        // decrement
		NULL,        // quit
		cmd_flush,   // flush
		cmd_getx,    // getq
		cmd_noop,    // noop
		NULL,        // version
		cmd_getx,    // getk
		cmd_getx,    // getkq
		NULL,        // append
		NULL,        // prepend
	};

	memproto_parser_init(&m_memproto, &cb, this);
}

Memproto::Connection::~Connection()
{
	m_queue->invalidate();
}


struct Memproto::Connection::response_queue::find_entry_compare {
	find_entry_compare(entry* key) : m_key(key) { }

	bool operator() (const element_t& elem)
	{
		return elem.e == m_key;
	}

	entry* m_key;
};

Memproto::Connection::response_queue::response_queue(int fd) :
	m_valid(true), m_fd(fd) { }

Memproto::Connection::response_queue::~response_queue() { }

inline void Memproto::Connection::response_queue::push_entry(
		entry* e, shared_zone& life)
{
	element_t m = {e, life};

	mp::pthread_scoped_lock mqlk(m_queue_mutex);
	m_queue.push_back(m);
}

inline void Memproto::Connection::response_queue::reached_try_send(
		entry* e, struct iovec* vec, size_t veclen)
{
	mp::pthread_scoped_lock mqlk(m_queue_mutex);

	queue_t::iterator found = std::find_if(m_queue.begin(), m_queue.end(),
			find_entry_compare(e));

	if(found == m_queue.end()) {
		return;
	}

	found->e = NULL;
	found->vec = vec;
	found->veclen = veclen;

	if(found != m_queue.begin()) {
		return;
	}

	for(; found != m_queue.end(); found = m_queue.begin()) {
		element_t& elem(*found);

		if(elem.e) {
			break;
		}

		if(elem.veclen > 0) {
			wavy::request req(&mp::object_delete<shared_zone>, new shared_zone(elem.life));
			wavy::writev(m_fd, elem.vec, elem.veclen, req);
		}

		m_queue.pop_front();
	}
}

inline int Memproto::Connection::response_queue::is_valid() const
{
	return m_valid;
}

inline void Memproto::Connection::response_queue::invalidate()
{
	m_valid = false;
}


void Memproto::Connection::read_event()
try {
	m_buffer.reserve_buffer(MEMPROTO_RESERVE_SIZE);

	size_t rl = ::read(fd(), m_buffer.buffer(), m_buffer.buffer_capacity());
	if(rl < 0) {
		if(errno == EAGAIN || errno == EINTR) {
			return;
		} else {
			throw std::runtime_error("read error");
		}
	} else if(rl == 0) {
		LOG_DEBUG("connection closed: ",strerror(errno));
		throw std::runtime_error("connection closed");
	}

	m_buffer.buffer_consumed(rl);

	do {
		size_t off = 0;
		int ret = memproto_parser_execute(&m_memproto,
				(char*)m_buffer.data(), m_buffer.data_size(), &off);

		if(ret == 0) {
			break;
		}

		if(ret < 0) {
			//std::cout << "parse error " << ret << std::endl;
			throw std::runtime_error("parse error");
		}

		m_buffer.data_used(off);

		m_zone->push_finalizer(
				&mp::object_delete<mp::stream_buffer::reference>,
				m_buffer.release());

		ret = memproto_dispatch(&m_memproto);
		if(ret <= 0) {
			LOG_DEBUG("unknown command ",(uint16_t)-ret);
			throw std::runtime_error("unknown command");
		}

		m_zone.reset(new msgpack::zone());

	} while(m_buffer.data_size() > 0);

} catch (std::exception& e) {
	LOG_DEBUG("memcached binary protocol error: ",e.what());
	throw;
} catch (...) {
	LOG_DEBUG("memcached binary protocol error: unknown error");
	throw;
}

void Memproto::Connection::request_getx(memproto_header* h,
		const char* key, uint16_t keylen)
{
	LOG_TRACE("getx");

	get_entry* e = m_zone->allocate<get_entry>();
	e->queue      = m_queue;
	e->header     = *h;
	e->flag_key   = (h->opcode == MEMPROTO_CMD_GETK || h->opcode == MEMPROTO_CMD_GETKQ);
	e->flag_quiet = (h->opcode == MEMPROTO_CMD_GETQ || h->opcode == MEMPROTO_CMD_GETKQ);

	gw_get_request req;
	req.keylen   = keylen;
	req.key      = key;
	req.hash     = Gateway::stdhash(req.key, req.keylen);
	req.life     = m_zone;
	req.user     = reinterpret_cast<void*>(e);
	req.callback = &Connection::response_getx;

	m_queue->push_entry(e, m_zone);

	m_gw->submit(req);
}

void Memproto::Connection::request_set(memproto_header* h,
		const char* key, uint16_t keylen,
		const char* val, uint32_t vallen,
		uint32_t flags, uint32_t expiration)
{
	LOG_TRACE("set");

	if(h->cas || flags || expiration) {
		// FIXME error response
		throw std::runtime_error("memcached binary protocol: invalid argument");
	}

	get_entry* e = m_zone->allocate<get_entry>();
	e->queue      = m_queue;
	e->header     = *h;

	gw_set_request req;
	req.keylen   = keylen;
	req.key      = key;
	req.vallen   = vallen;
	req.hash     = Gateway::stdhash(req.key, req.keylen);
	req.val      = val;
	req.life     = m_zone;
	req.user     = reinterpret_cast<void*>(e);
	req.callback = &Connection::response_set;

	m_queue->push_entry(e, m_zone);

	m_gw->submit(req);
}

void Memproto::Connection::request_delete(memproto_header* h,
		const char* key, uint16_t keylen,
		uint32_t expiration)
{
	LOG_TRACE("delete");

	if(expiration) {
		// FIXME error response
		throw std::runtime_error("memcached binary protocol: invalid argument");
	}

	get_entry* e = m_zone->allocate<get_entry>();
	e->queue      = m_queue;
	e->header     = *h;

	gw_delete_request req;
	req.key      = key;
	req.keylen   = keylen;
	req.hash     = Gateway::stdhash(req.key, req.keylen);
	req.life     = m_zone;
	req.user     = reinterpret_cast<void*>(e);
	req.callback = &Connection::response_delete;

	m_queue->push_entry(e, m_zone);

	m_gw->submit(req);
}

void Memproto::Connection::request_noop(memproto_header* h)
{
	LOG_TRACE("noop");

	entry* e = m_zone->allocate<entry>();
	e->queue      = m_queue;
	e->header     = *h;

	m_queue->push_entry(e, m_zone);
	send_response_nodata(e, *m_zone, MEMPROTO_RES_NO_ERROR);
}

void Memproto::Connection::request_flush(memproto_header* h,
		uint32_t expiration)
{
	LOG_TRACE("flush");

	if(expiration) {
		// FIXME error response
		throw std::runtime_error("memcached binary protocol: invalid argument");
	}

	entry* e = m_zone->allocate<entry>();
	e->queue      = m_queue;
	e->header     = *h;

	m_queue->push_entry(e, m_zone);
	send_response_nodata(e, *m_zone, MEMPROTO_RES_NO_ERROR);
}

namespace {
	static const uint32_t ZERO_FLAG = 0;
}  // noname namespace

void Memproto::Connection::response_getx(void* user, gw_get_response& res)
{
	get_entry* e = reinterpret_cast<get_entry*>(user);
	if(!e->queue->is_valid()) { return; }

	LOG_TRACE("get response");

	if(res.error) {
		// error
		if(e->flag_quiet) {
			send_response_nosend(e);
			return;
		}
		LOG_TRACE("getx res err");
		send_response_nodata(e, *res.life, MEMPROTO_RES_INVALID_ARGUMENTS);
		return;
	}

	if(!res.val) {
		// not found
		if(e->flag_quiet) {
			send_response_nosend(e);
			return;
		}
		send_response_nodata(e, *res.life, MEMPROTO_RES_KEY_NOT_FOUND);
		return;
	}

	// found
	send_response(e, *res.life, MEMPROTO_RES_NO_ERROR,
			res.key, (e->flag_key ? res.keylen : 0),
			res.val, res.vallen,
			(char*)&ZERO_FLAG, 4);
}

void Memproto::Connection::response_set(void* user, gw_set_response& res)
{
	set_entry* e = reinterpret_cast<set_entry*>(user);
	if(!e->queue->is_valid()) { return; }

	LOG_TRACE("set response");

	if(res.error) {
		// error
		send_response_nodata(e, *res.life, MEMPROTO_RES_OUT_OF_MEMORY);
		return;
	}

	// stored
	send_response_nodata(e, *res.life, MEMPROTO_RES_NO_ERROR);
}

void Memproto::Connection::response_delete(void* user, gw_delete_response& res)
{
	delete_entry* e = reinterpret_cast<delete_entry*>(user);
	if(!e->queue->is_valid()) { return; }

	LOG_TRACE("delete response");

	if(res.error) {
		// error
		send_response_nodata(e, *res.life, MEMPROTO_RES_INVALID_ARGUMENTS);
		return;
	}

	if(res.deleted) {
		send_response_nodata(e, *res.life, MEMPROTO_RES_NO_ERROR);
	} else {
		send_response_nodata(e, *res.life, MEMPROTO_RES_OUT_OF_MEMORY);
	}
}


void Memproto::Connection::pack_header(
		char* hbuf, uint16_t status, uint8_t op,
		uint16_t keylen, uint32_t vallen, uint8_t extralen,
		uint32_t opaque, uint64_t cas)
{
	hbuf[0] = 0x81;
	hbuf[1] = op;
	*(uint16_t*)&hbuf[2] = htons(keylen);
	hbuf[4] = extralen;
	hbuf[5] = 0x00;
	*(uint16_t*)&hbuf[6] = htons(status);
	*(uint32_t*)&hbuf[8] = htonl(vallen + keylen + extralen);
	*(uint32_t*)&hbuf[12] = htonl(opaque);
	*(uint32_t*)&hbuf[16] = htonl((uint32_t)(cas>>32));
	*(uint32_t*)&hbuf[20] = htonl((uint32_t)(cas&0xffffffff));
}

void Memproto::Connection::send_response_nosend(entry* e)
{
	e->queue->reached_try_send(e, NULL, 0);
}

void Memproto::Connection::send_response_nodata(
		entry* e, msgpack::zone& life,
		uint8_t status)
{
	char* header = (char*)life.malloc(MEMPROTO_HEADER_SIZE);

	pack_header(header, status, e->header.opcode,
			0, 0, 0,
			e->header.opaque, 0);  // cas = 0

	struct iovec* vec = (struct iovec*)life.malloc(
			sizeof(struct iovec) * 1);
	vec[0].iov_base = header;
	vec[0].iov_len  = MEMPROTO_HEADER_SIZE;

	e->queue->reached_try_send(e, vec, 1);
}

inline void Memproto::Connection::send_response(
		entry* e, msgpack::zone& life,
		uint8_t status,
		const char* key, uint16_t keylen,
		const void* val, uint16_t vallen,
		const char* extra, uint16_t extralen)
{
	char* header = (char*)life.malloc(MEMPROTO_HEADER_SIZE);
	pack_header(header, status, e->header.opcode,
			keylen, vallen, extralen,
			e->header.opaque, 0);  // cas = 0

	struct iovec* vec = (struct iovec*)life.malloc(
			sizeof(struct iovec) * 4);

	vec[0].iov_base = header;
	vec[0].iov_len  = MEMPROTO_HEADER_SIZE;
	size_t cnt = 1;

	if(extralen > 0) {
		vec[cnt].iov_base = const_cast<char*>(extra);
		vec[cnt].iov_len  = extralen;
		++cnt;
	}

	if(keylen > 0) {
		vec[cnt].iov_base = const_cast<char*>(key);
		vec[cnt].iov_len  = keylen;
		++cnt;
	}

	if(vallen > 0) {
		vec[cnt].iov_base = const_cast<void*>(val);
		vec[cnt].iov_len  = vallen;
		++cnt;
	}

	e->queue->reached_try_send(e, vec, cnt);
}


}  // namespace kumo

