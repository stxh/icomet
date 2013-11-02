#ifndef ICOMET_SERVER_CONFIG_H
#define ICOMET_SERVER_CONFIG_H

#include "util/config.h"

class ServerConfig{
public:
	static int max_channels;
	static int max_subscribers_per_channel;
	static int polling_timeout;
	static int polling_idles; // max idle count to reconnect
	static int channel_buffer_size;
	static int channel_timeout;
	// rename max_channel_idles
	static int channel_idles; // max idle count to offline
};

#endif
