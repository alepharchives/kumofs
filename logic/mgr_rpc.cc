#include "logic/mgr_impl.h"

#define EACH_ACTIVE_NEW_COMERS_BEGIN(NODE) \
	for(new_servers_t::iterator _it_(m_new_servers.begin()), it_end(m_new_servers.end()); \
			_it_ != it_end; ++_it_) { \
		shared_node NODE(_it_->lock()); \
		if(SESSION_IS_ACTIVE(NODE)) {
			// FIXME m_new_servers.erase(it) ?

#define EACH_ACTIVE_NEW_COMERS_END \
		} \
	}

namespace kumo {


CLUSTER_FUNC(WHashSpaceRequest, from, response, z, param)
try {
	pthread_scoped_lock hslk(m_hs_mutex);
	HashSpace::Seed* seed = z->allocate<HashSpace::Seed>(m_whs);
	hslk.unlock();
	response.result(*seed, z);
}
RPC_CATCH(WHashSpaceRequest, response)

CLUSTER_FUNC(RHashSpaceRequest, from, response, z, param)
try {
	pthread_scoped_lock hslk(m_hs_mutex);
	HashSpace::Seed* seed = z->allocate<HashSpace::Seed>(m_rhs);
	hslk.unlock();
	response.result(*seed, z);
}
RPC_CATCH(RHashSpaceRequest, response)


RPC_FUNC(HashSpaceRequest, from, response, z, param)
try {
	pthread_scoped_lock hslk(m_hs_mutex);
	HashSpace::Seed* seed = z->allocate<HashSpace::Seed>(m_whs);
	hslk.unlock();
	response.result(*seed, z);
}
RPC_CATCH(HashSpaceRequest, response)



namespace {
	struct each_client_push {
		each_client_push(HashSpace::Seed* hs, rpc::callback_t cb, shared_zone& l) :
			life(l),
			arg(*hs),
			callback(cb) { }

		void operator() (rpc::shared_peer p)
		{
			LOG_WARN("push hash space to ",(void*)p.get());
			p->call(protocol::HashSpacePush, arg, life, callback, 10);
		}

	private:
		rpc::shared_zone& life;
		protocol::type::HashSpacePush arg;
		rpc::callback_t callback;
	};
}  // noname namespace

void Manager::push_hash_space_clients(REQUIRE_HSLK)
{
	LOG_WARN("push hash space ...");
	shared_zone life(new msgpack::zone());
	HashSpace::Seed* seed = life->allocate<HashSpace::Seed>(m_whs);
	rpc::callback_t callback( BIND_RESPONSE(ResHashSpacePush) );
	subsystem().for_each_peer( each_client_push(seed, callback, life) );
}

RPC_REPLY(ResHashSpacePush, from, res, err, life)
{
	// FIXME retry
}


void Manager::sync_hash_space_servers(REQUIRE_HSLK)
{
	shared_zone life(new msgpack::zone());
	HashSpace::Seed* wseed = life->allocate<HashSpace::Seed>(m_whs);
	HashSpace::Seed* rseed = life->allocate<HashSpace::Seed>(m_rhs);

	protocol::type::HashSpaceSync arg(*wseed, *rseed, m_clock.get_incr());

	rpc::callback_t callback( BIND_RESPONSE(ResHashSpaceSync) );

	pthread_scoped_lock slk(m_servers_mutex);
	EACH_ACTIVE_SERVERS_BEGIN(node)
		node->call(protocol::HashSpaceSync, arg, life, callback, 10);
	EACH_ACTIVE_SERVERS_END
}


void Manager::sync_hash_space_partner(REQUIRE_HSLK)
{
	if(!m_partner.connectable()) { return; }

	shared_zone life(new msgpack::zone());
	HashSpace::Seed* wseed = life->allocate<HashSpace::Seed>(m_whs);
	HashSpace::Seed* rseed = life->allocate<HashSpace::Seed>(m_rhs);

	protocol::type::HashSpaceSync arg(*wseed, *rseed, m_clock.get_incr());
	get_node(m_partner)->call(
			protocol::HashSpaceSync, arg, life,
			BIND_RESPONSE(ResHashSpaceSync), 10);
}

RPC_REPLY(ResHashSpaceSync, from, res, err, life)
{
	// FIXME retry
}

CLUSTER_FUNC(HashSpaceSync, from, response, z, param)
try {
	if(from->addr() != m_partner) {
		throw std::runtime_error("unknown partner node");
	}

	m_clock.update(param.clock());

	bool ret = false;

	pthread_scoped_lock hslk(m_hs_mutex);
	if(!param.wseed().empty() && (m_whs.empty() ||
			m_whs.clocktime() <= ClockTime(param.wseed().clocktime()))) {
		m_whs = HashSpace(param.wseed());
		ret = true;
	}

	if(!param.rseed().empty() && (m_rhs.empty() ||
			m_rhs.clocktime() <= ClockTime(param.rseed().clocktime()))) {
		m_rhs = HashSpace(param.rseed());
		ret = true;
	}
	hslk.unlock();

	if(ret) {
		response.result(true);
	} else {
		response.null();
	}
}
RPC_CATCH(HashSpaceSync, response)


void Manager::keep_alive()
{
	LOG_TRACE("keep alive ...");
	shared_zone nullz;
	protocol::type::KeepAlive arg(m_clock.get_incr());

	using namespace mp::placeholders;
	rpc::callback_t callback( BIND_RESPONSE(ResKeepAlive) );

	pthread_scoped_lock slk(m_servers_mutex);
	EACH_ACTIVE_SERVERS_BEGIN(node)
		// FIXME exception
		node->call(protocol::KeepAlive, arg, nullz, callback, 10);
	EACH_ACTIVE_SERVERS_END
	slk.unlock();

	pthread_scoped_lock nslk(m_new_servers_mutex);
	EACH_ACTIVE_NEW_COMERS_BEGIN(node)
		// FIXME exception
		node->call(protocol::KeepAlive, arg,
				nullz, callback, 10);
	EACH_ACTIVE_NEW_COMERS_END
	nslk.unlock();

	if(m_partner.connectable()) {
		// FIXME cache result of get_node(m_partner)
		get_node(m_partner)->call(
				protocol::KeepAlive, arg, nullz, callback, 10);
	}
}

RPC_REPLY(ResKeepAlive, from, res, err, life)
{
	if(err.is_nil()) {
		LOG_TRACE("KeepAlive succeeded");
	} else {
		LOG_DEBUG("KeepAlive failed: ",err);
	}
}


CLUSTER_FUNC(KeepAlive, from, response, z, param)
try {
	response.null();
}
RPC_CATCH(KeepAlive, response)


}  // namespace kumo

