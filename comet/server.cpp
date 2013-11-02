#include "../config.h"
#include <http-internal.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>
#include "server.h"
#include "server_config.h"
#include "util/log.h"
#include "util/list.h"


static void set_response_no_cache(struct evhttp_request *req){
	evhttp_add_header(req->output_headers, "Content-Type", "text/javascript; charset=utf-8");
	evhttp_add_header(req->output_headers, "Connection", "keep-alive");
	evhttp_add_header(req->output_headers, "Cache-Control", "no-cache");
	evhttp_add_header(req->output_headers, "Expires", "0");
}

class HttpQuery{
private:
	struct evkeyvalq params;
public:
	HttpQuery(struct evhttp_request *req){
		evhttp_parse_query(evhttp_request_get_uri(req), &params);
	}
	int get_int(const char *name, int def){
		const char *val = evhttp_find_header(&params, name);
		return val? atoi(val) : def;
	}
	const char* get_str(const char *name, const char *def){
		const char *val = evhttp_find_header(&params, name);
		return val? val : def;
	}
};

Server::Server(){
	this->auth = AUTH_NONE;
	subscribers = 0;
	sub_pool.pre_alloc(1024);

	channel_slots.resize(ServerConfig::max_channels);
	for(int i=0; i<channel_slots.size(); i++){
		Channel *channel = &channel_slots[i];
		channel->id = i;
		free_channels.push_back(channel);
	}
}

Server::~Server(){
}

Channel* Server::get_channel(int cid){
	if(cid < 0 || cid >= channel_slots.size()){
		return NULL;
	}
	return &channel_slots[cid];
}

Channel* Server::get_channel_by_name(const std::string &cname){
	std::map<std::string, Channel *>::iterator it;
	it = cname_channels.find(cname);
	if(it == cname_channels.end()){
		return NULL;
	}
	return it->second;
}

Channel* Server::new_channel(const std::string &cname){
	if(free_channels.empty()){
		return NULL;
	}
	Channel *channel = free_channels.head;
	assert(channel->subs.size == 0);
	// first remove, then push_back, do not mistake the order
	free_channels.remove(channel);
	used_channels.push_back(channel);

	channel->name = cname;
	cname_channels[channel->name] = channel;
	log_debug("new channel: %d, name: %s", channel->id, channel->name.c_str());
	
	add_presence(PresenceOnline, channel->name);
	
	return channel;
}

void Server::free_channel(Channel *channel){
	assert(channel->subs.size == 0);
	log_debug("free channel: %d, name: %s", channel->id, channel->name.c_str());
	add_presence(PresenceOffline, channel->name);

	// first remove, then push_back, do not mistake the order
	used_channels.remove(channel);
	free_channels.push_back(channel);

	cname_channels.erase(channel->name);
	channel->reset();
}

int Server::check_timeout(){
	//log_debug("<");
	struct evbuffer *buf = evbuffer_new();
	LinkedList<Channel *>::Iterator it = used_channels.iterator();
	while(Channel *channel = it.next()){
		if(channel->subs.size == 0){
			if(--channel->idle < 0){
				this->free_channel(channel);
			}
			continue;
		}
		if(channel->idle < ServerConfig::channel_idles){
			channel->idle = ServerConfig::channel_idles;
		}

		LinkedList<Subscriber *>::Iterator it2 = channel->subs.iterator();
		while(Subscriber *sub = it2.next()){
			if(++sub->idle <= ServerConfig::polling_idles){
				continue;
			}
			evbuffer_add_printf(buf,
				"%s({type: \"noop\", cname: \"%s\", seq: \"%d\"});\n",
				sub->callback.c_str(),
				channel->name.c_str(),
				sub->noop_seq
				);
			evhttp_send_reply_chunk(sub->req, buf);
			evhttp_send_reply_end(sub->req);
			//
			evhttp_connection_set_closecb(sub->req->evcon, NULL, NULL);
			this->sub_end(sub);
		}
	}
	evbuffer_free(buf);
	//log_debug(">");
	return 0;
}

void Server::add_presence(PresenceType type, const std::string &cname){
	if(psubs.empty()){
		return;
	}
	struct evbuffer *buf = evbuffer_new();
	evbuffer_add_printf(buf, "%d %s\n", type, cname.c_str());

	LinkedList<PresenceSubscriber *>::Iterator it = psubs.iterator();
	while(PresenceSubscriber *psub = it.next()){
		evhttp_send_reply_chunk(psub->req, buf);

		//struct evbuffer *output = bufferevent_get_output(req->evcon->bufev);
		//if(evbuffer_get_length(output) > MAX_OUTPUT_BUFFER){
		//  close_presence_subscriber();
		//}
	}
	
	evbuffer_free(buf);
}

static void on_psub_disconnect(struct evhttp_connection *evcon, void *arg){
	log_trace("presence subscriber disconnected");
	PresenceSubscriber *psub = (PresenceSubscriber *)arg;
	Server *serv = psub->serv;
	serv->psub_end(psub);
}

int Server::psub(struct evhttp_request *req){
	bufferevent_enable(req->evcon->bufev, EV_READ);

	PresenceSubscriber *psub = new PresenceSubscriber();
	psub->req = req;
	psub->serv = this;
	psubs.push_back(psub);
	log_debug("%s:%d psub, psubs: %d", req->remote_host, req->remote_port, psubs.size);

	set_response_no_cache(req);
	evhttp_send_reply_start(req, HTTP_OK, "OK");
	evhttp_connection_set_closecb(req->evcon, on_psub_disconnect, psub);
	return 0;
}

int Server::psub_end(PresenceSubscriber *psub){
	struct evhttp_request *req = psub->req;
	psubs.remove(psub);
	log_debug("%s:%d psub_end, psubs: %d", req->remote_host, req->remote_port, psubs.size);
	return 0;
}

static void on_sub_disconnect(struct evhttp_connection *evcon, void *arg){
	log_trace("subscriber disconnected");
	Subscriber *sub = (Subscriber *)arg;
	Server *serv = sub->serv;
	serv->sub_end(sub);
}

int Server::sub(struct evhttp_request *req){
	if(evhttp_request_get_command(req) != EVHTTP_REQ_GET){
		evhttp_send_reply(req, 405, "Method Not Allowed", NULL);
		return 0;
	}
	bufferevent_enable(req->evcon->bufev, EV_READ);

	HttpQuery query(req);
	int seq = query.get_int("seq", 0);
	int noop = query.get_int("noop", 0);
	const char *cb = query.get_str("cb", DEFAULT_JSONP_CALLBACK);
	const char *token = query.get_str("token", "");
	std::string cname = query.get_str("cname", "");

	Channel *channel = this->get_channel_by_name(cname);
	if(!channel && this->auth == AUTH_NONE){
		channel = this->new_channel(cname);
		if(!channel){
			struct evbuffer *buf = evbuffer_new();
			evbuffer_add_printf(buf, "too many channels\n");
			evhttp_send_reply(req, 404, "Not Found", buf);
			evbuffer_free(buf);
			return 0;
		}
	}
	if(!channel || (this->auth == AUTH_TOKEN && channel->token != token)){
		log_debug("%s:%d, Token Error, cname: %s, token: %s",
			req->remote_host,
			req->remote_port,
			cname.c_str(),
			token
			);
		struct evbuffer *buf = evbuffer_new();
		evbuffer_add_printf(buf,
			"%s({type: \"401\", cname: \"%s\", seq: \"0\", content: \"Token Error\"});\n",
			cb,
			cname.c_str()
			);
		evhttp_send_reply(req, HTTP_OK, "OK", buf);
		evbuffer_free(buf);
		return 0;
	}
	if(channel->subs.size >= ServerConfig::max_subscribers_per_channel){
		log_debug("%s:%d, Too Many Requests, cname: %s",
			req->remote_host,
			req->remote_port,
			cname.c_str()
			);
		struct evbuffer *buf = evbuffer_new();
		evbuffer_add_printf(buf,
			"%s({type: \"429\", cname: \"%s\", seq: \"0\", content: \"Too Many Requests\"});\n",
			cb,
			cname.c_str()
			);
		evhttp_send_reply(req, HTTP_OK, "OK", buf);
		evbuffer_free(buf);
		return 0;
	}
	channel->idle = ServerConfig::channel_idles;

	set_response_no_cache(req);
	
	// send buffered messages
	if(!channel->msg_list.empty() && channel->seq_next != seq){
		std::vector<std::string>::iterator it = channel->msg_list.end();
		int msg_seq_min = channel->seq_next - channel->msg_list.size();
		if(Channel::SEQ_GT(seq, channel->seq_next) || Channel::SEQ_GT(msg_seq_min, seq)){
			seq = msg_seq_min;
		}
		log_debug("send old msg: [%d, %d]", seq, channel->seq_next - 1);
		it -= (channel->seq_next - seq);

		struct evbuffer *buf = evbuffer_new();
		evbuffer_add_printf(buf, "%s([", cb);
		for(/**/; it != channel->msg_list.end(); it++, seq++){
			std::string &msg = *it;
			evbuffer_add_printf(buf,
				"{type: \"data\", cname: \"%s\", seq: \"%d\", content: \"%s\"}",
				cname.c_str(),
				seq,
				msg.c_str());
			if(seq != channel->seq_next - 1){
				evbuffer_add(buf, ",", 1);
			}
		}
		evbuffer_add_printf(buf, "]);\n");
		evhttp_send_reply(req, HTTP_OK, "OK", buf);
		evbuffer_free(buf);
		return 0;
	}
	
	Subscriber *sub = sub_pool.alloc();
	sub->req = req;
	sub->serv = this;
	sub->idle = 0;
	sub->noop_seq = noop;
	sub->callback = cb;
	
	channel->add_subscriber(sub);
	subscribers ++;
	log_debug("%s:%d sub %s, subs: %d, channels: %d",
		req->remote_host, req->remote_port,
		channel->name.c_str(), channel->subs.size,
		used_channels.size);

	evhttp_send_reply_start(req, HTTP_OK, "OK");
	evhttp_connection_set_closecb(req->evcon, on_sub_disconnect, sub);
	return 0;
}

int Server::sub_end(Subscriber *sub){
	struct evhttp_request *req = sub->req;
	Channel *channel = sub->channel;
	channel->del_subscriber(sub);
	subscribers --;
	log_debug("%s:%d sub_end %s, subs: %d, channels: %d",
		req->remote_host, req->remote_port,
		channel->name.c_str(), channel->subs.size,
		used_channels.size);
	sub_pool.free(sub);
	return 0;
}

int Server::ping(struct evhttp_request *req){
	HttpQuery query(req);
	const char *cb = query.get_str("cb", DEFAULT_JSONP_CALLBACK);

	set_response_no_cache(req);
	struct evbuffer *buf = evbuffer_new();
	evbuffer_add_printf(buf,
		"%s({type: \"ping\", sub_timeout: %d});\n",
		cb,
		ServerConfig::polling_timeout);
	evhttp_send_reply(req, HTTP_OK, "OK", buf);
	evbuffer_free(buf);
	return 0;
}

int Server::pub(struct evhttp_request *req){
	if(evhttp_request_get_command(req) != EVHTTP_REQ_GET){
		evhttp_send_reply(req, 405, "Invalid Method", NULL);
		return 0;
	}
	
	HttpQuery query(req);
	const char *cb = query.get_str("cb", NULL);
	std::string cname = query.get_str("cname", "");
	const char *content = query.get_str("content", "");
	
	Channel *channel = NULL;
	channel = this->get_channel_by_name(cname);
	if(!channel || channel->idle == -1){
		struct evbuffer *buf = evbuffer_new();
		log_trace("cname[%s] not connected, not pub content: %s", cname.c_str(), content);
		evbuffer_add_printf(buf, "cname[%s] not connected\n", cname.c_str());
		evhttp_send_reply(req, 404, "Not Found", buf);
		evbuffer_free(buf);
		return 0;
	}
	log_debug("channel: %s, subs: %d, pub content: %s", channel->name.c_str(), channel->subs.size, content);
		
	// response to publisher
	evhttp_add_header(req->output_headers, "Content-Type", "text/javascript; charset=utf-8");
	struct evbuffer *buf = evbuffer_new();
	if(cb){
		evbuffer_add_printf(buf, "%s(", cb);
	}
	evbuffer_add_printf(buf, "{type: \"ok\"}");
	if(cb){
		evbuffer_add(buf, ");\n", 3);
	}else{
		evbuffer_add(buf, "\n", 1);
	}
	evhttp_send_reply(req, 200, "OK", buf);
	evbuffer_free(buf);

	// push to subscribers
	channel->send("data", content);
	return 0;
}


int Server::sign(struct evhttp_request *req){
	HttpQuery query(req);
	int expires = query.get_int("expires", -1);
	const char *cb = query.get_str("cb", NULL);
	std::string cname = query.get_str("cname", "");

	if(expires <= 0){
		expires = ServerConfig::channel_timeout;
	}
	
	Channel *channel = this->get_channel_by_name(cname);
	if(!channel){
		channel = this->new_channel(cname);
	}	
	if(!channel){
		struct evbuffer *buf = evbuffer_new();
		evbuffer_add_printf(buf, "too many channels\n");
		evhttp_send_reply(req, 404, "Not Found", buf);
		evbuffer_free(buf);
		return 0;
	}

	if(channel->token.empty()){
		channel->create_token();
	}
	if(channel->idle == -1){
		log_debug("%s:%d sign cname:%s, cid:%d, t:%s, expires:%d",
			req->remote_host, req->remote_port,
			cname.c_str(), channel->id, channel->token.c_str(), expires);
	}else{
		log_debug("%s:%d re-sign cname:%s, cid:%d, t:%s, expires:%d",
			req->remote_host, req->remote_port,
			cname.c_str(), channel->id, channel->token.c_str(), expires);
	}
	channel->idle = expires/CHANNEL_CHECK_INTERVAL;

	evhttp_add_header(req->output_headers, "Content-Type", "text/html; charset=utf-8");
	struct evbuffer *buf = evbuffer_new();
	if(cb){
		evbuffer_add_printf(buf, "%s(", cb);
	}
	evbuffer_add_printf(buf,
		"{type: \"sign\", cname: \"%s\", seq: %d, token: \"%s\", expires: %d, sub_timeout: %d}",
		channel->name.c_str(),
		channel->msg_seq_min(),
		channel->token.c_str(),
		expires,
		ServerConfig::channel_timeout);
	if(cb){
		evbuffer_add(buf, ");\n", 3);
	}else{
		evbuffer_add(buf, "\n", 1);
	}
	evhttp_send_reply(req, 200, "OK", buf);
	evbuffer_free(buf);

	return 0;
}

int Server::close(struct evhttp_request *req){
	HttpQuery query(req);
	std::string cname = query.get_str("cname", "");

	Channel *channel = this->get_channel_by_name(cname);
	if(!channel){
		log_warn("channel %s not found", cname.c_str());
		struct evbuffer *buf = evbuffer_new();
		evbuffer_add_printf(buf, "channel[%s] not connected\n", cname.c_str());
		evhttp_send_reply(req, 404, "Not Found", buf);
		evbuffer_free(buf);
		return 0;
	}
	log_debug("close channel: %s, subs: %d", cname.c_str(), channel->subs.size);
		
	// response to publisher
	evhttp_add_header(req->output_headers, "Content-Type", "text/html; charset=utf-8");
	struct evbuffer *buf = evbuffer_new();
	evbuffer_add_printf(buf, "ok %d\n", channel->seq_next);
	evhttp_send_reply(req, 200, "OK", buf);
	evbuffer_free(buf);

	// push to subscribers
	if(channel->idle != -1){
		channel->send("close", "");
		this->free_channel(channel);
	}

	return 0;
}

int Server::info(struct evhttp_request *req){
	HttpQuery query(req);
	std::string cname = query.get_str("cname", "");

	evhttp_add_header(req->output_headers, "Content-Type", "text/html; charset=utf-8");
	struct evbuffer *buf = evbuffer_new();
	if(!cname.empty()){
		Channel *channel = this->get_channel_by_name(cname);
		// TODO: if(!channel) 404
		int onlines = channel? channel->subs.size : 0;
		evbuffer_add_printf(buf,
			"{cname: \"%s\", subscribers: %d}\n",
			cname.c_str(),
			onlines);
	}else{
		evbuffer_add_printf(buf,
			"{channels: %d, subscribers: %d}\n",
			used_channels.size,
			subscribers);
	}
	evhttp_send_reply(req, 200, "OK", buf);
	evbuffer_free(buf);

	return 0;
}

int Server::check(struct evhttp_request *req){
	HttpQuery query(req);
	std::string cname = query.get_str("cname", "");

	evhttp_add_header(req->output_headers, "Content-Type", "text/html; charset=utf-8");
	struct evbuffer *buf = evbuffer_new();
	Channel *channel = this->get_channel_by_name(cname);
	if(channel && channel->idle != -1){
		evbuffer_add_printf(buf, "{\"%s\": 1}\n", cname.c_str());
	}else{
		evbuffer_add_printf(buf, "{}\n");
	}
	evhttp_send_reply(req, 200, "OK", buf);
	evbuffer_free(buf);

	return 0;
}
