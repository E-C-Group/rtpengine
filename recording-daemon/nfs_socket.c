#define _GNU_SOURCE
#include "nfs_socket.h"
#include <glib.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "log.h"
#include "main.h"
#include "epoll.h"
#include "metafile.h"
#include "packet.h"
#include "stream.h"
#include "types.h"

// Simple packet header for socket communication
// This matches the format used by the main daemon
struct nfs_packet_header {
	uint32_t stream_idx;
	uint32_t data_len;
	char call_id[256];
	// followed by IP/UDP/RTP packet data
};

static int listen_fd = -1;
static handler_t listen_handler;
static pthread_mutex_t streams_lock = PTHREAD_MUTEX_INITIALIZER;

// Map of call_id:stream_idx -> stream_t
static GHashTable *stream_map = NULL;

// Client connection state
struct nfs_client {
	int fd;
	handler_t handler;
	unsigned char *buf;
	size_t buf_size;
	size_t buf_used;
};

#define MAX_PACKET_SIZE 65536
#define RECV_BUF_SIZE (sizeof(struct nfs_packet_header) + MAX_PACKET_SIZE)


static char *make_stream_key(const char *call_parent, unsigned int stream_idx) {
	return g_strdup_printf("%s:%u", call_parent, stream_idx);
}

void nfs_socket_register_stream(const char *call_parent, unsigned int stream_idx, const char *stream_name) {
	if (!nfs_socket_path || !call_parent)
		return;

	char *key = make_stream_key(call_parent, stream_idx);
	
	pthread_mutex_lock(&streams_lock);
	if (!stream_map)
		stream_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	
	// We just store the mapping - the stream_t will be looked up when packets arrive
	g_hash_table_insert(stream_map, key, GUINT_TO_POINTER(1));
	pthread_mutex_unlock(&streams_lock);
	
	ilog(LOG_INFO, "Registered NFS socket stream: %s -> %s", key, stream_name);
}

void nfs_socket_unregister_stream(const char *call_parent, unsigned int stream_idx) {
	if (!nfs_socket_path || !call_parent || !stream_map)
		return;

	char *key = make_stream_key(call_parent, stream_idx);
	
	pthread_mutex_lock(&streams_lock);
	g_hash_table_remove(stream_map, key);
	pthread_mutex_unlock(&streams_lock);
	
	g_free(key);
}

static void nfs_client_free(struct nfs_client *client) {
	if (!client)
		return;
	if (client->fd >= 0) {
		epoll_del(client->fd);
		close(client->fd);
	}
	g_free(client->buf);
	g_free(client);
}

// Forward declaration
static void nfs_client_handler(handler_t *handler);

// Process a complete packet from the client
static void nfs_process_packet(struct nfs_client *client, struct nfs_packet_header *hdr, 
		unsigned char *data, size_t data_len) {
	
	dbg("NFS socket received packet: call_id=%s stream_idx=%u len=%zu",
		hdr->call_id, hdr->stream_idx, data_len);

	// Look up the metafile by call_id (which is actually the meta_prefix/parent)
	// Metafiles are stored with ".meta" suffix, so we need to append it
	char *meta_name = g_strdup_printf("%s.meta", hdr->call_id);
	
	// metafile_lookup returns with mf->lock held
	metafile_t *mf = metafile_lookup(meta_name);
	g_free(meta_name);
	
	if (!mf) {
		// This can happen if packets arrive before inotify has processed the metadata file
		// Just drop the packet silently - this is a transient race condition
		static int drop_count = 0;
		if (++drop_count <= 5 || (drop_count % 100) == 0)
			ilog(LOG_DEBUG, "NFS socket: no metafile yet for call_id %s (dropped %d packets)", 
				hdr->call_id, drop_count);
		return;
	}

	// Get the stream from the metafile (lock is already held from metafile_lookup)
	stream_t *stream = NULL;
	if (hdr->stream_idx < mf->streams->len)
		stream = g_ptr_array_index(mf->streams, hdr->stream_idx);

	if (!stream) {
		ilog(LOG_DEBUG, "NFS socket: no stream %u yet for call_id %s", hdr->stream_idx, hdr->call_id);
		metafile_release(mf);
		return;
	}

	// Release the lock before processing - packet_process may take time
	metafile_release(mf);

	// Copy the packet data - packet_process takes ownership
	unsigned char *pkt_copy = malloc(data_len);
	if (!pkt_copy) {
		ilog(LOG_ERR, "NFS socket: failed to allocate packet buffer");
		return;
	}
	memcpy(pkt_copy, data, data_len);

	// Process the packet through the normal pipeline
	if (decoding_enabled)
		packet_process(stream, pkt_copy, data_len);
	else
		free(pkt_copy);
}

static void nfs_client_handler(handler_t *handler) {
	struct nfs_client *client = handler->ptr;

	while (1) {
		// Try to read more data
		size_t space = client->buf_size - client->buf_used;
		if (space == 0) {
			ilog(LOG_ERR, "NFS socket: client buffer full");
			nfs_client_free(client);
			return;
		}

		ssize_t ret = read(client->fd, client->buf + client->buf_used, space);
		if (ret == 0) {
			ilog(LOG_INFO, "NFS socket: client disconnected");
			nfs_client_free(client);
			return;
		}
		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				break;
			ilog(LOG_ERR, "NFS socket: read error: %s", strerror(errno));
			nfs_client_free(client);
			return;
		}

		client->buf_used += ret;

		// Process complete packets
		while (client->buf_used >= sizeof(struct nfs_packet_header)) {
			struct nfs_packet_header *hdr = (void *)client->buf;
			
			size_t total_len = sizeof(*hdr) + hdr->data_len;
			if (client->buf_used < total_len)
				break; // Need more data

			if (hdr->data_len > MAX_PACKET_SIZE) {
				ilog(LOG_ERR, "NFS socket: packet too large: %u", hdr->data_len);
				nfs_client_free(client);
				return;
			}

			// Process the packet
			nfs_process_packet(client, hdr, client->buf + sizeof(*hdr), hdr->data_len);

			// Remove processed data from buffer
			size_t remaining = client->buf_used - total_len;
			if (remaining > 0)
				memmove(client->buf, client->buf + total_len, remaining);
			client->buf_used = remaining;
		}
	}
}

static void nfs_accept_handler(handler_t *handler) {
	while (1) {
		int client_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (client_fd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				break;
			ilog(LOG_ERR, "NFS socket accept failed: %s", strerror(errno));
			break;
		}

		ilog(LOG_INFO, "NFS socket: new client connection");

		struct nfs_client *client = g_new0(struct nfs_client, 1);
		client->fd = client_fd;
		client->buf_size = RECV_BUF_SIZE;
		client->buf = g_malloc(client->buf_size);
		client->buf_used = 0;
		client->handler.func = nfs_client_handler;
		client->handler.ptr = client;

		if (epoll_add(client_fd, EPOLLIN, &client->handler) < 0) {
			ilog(LOG_ERR, "NFS socket: failed to add client to epoll");
			nfs_client_free(client);
		}
	}
}

void nfs_socket_setup(void) {
	if (!nfs_socket_path)
		return;

	// Remove any existing socket file
	unlink(nfs_socket_path);

	listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd < 0)
		die_errno("NFS socket: failed to create socket");

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, nfs_socket_path, sizeof(addr.sun_path) - 1);

	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die_errno("NFS socket: failed to bind to %s", nfs_socket_path);

	// Make socket writable by others so main daemon can connect
	chmod(nfs_socket_path, 0666);

	if (listen(listen_fd, 16) < 0)
		die_errno("NFS socket: failed to listen");

	listen_handler.func = nfs_accept_handler;
	listen_handler.ptr = NULL;

	if (epoll_add(listen_fd, EPOLLIN, &listen_handler) < 0)
		die_errno("NFS socket: failed to add to epoll");

	ilog(LOG_INFO, "NFS socket listening on %s", nfs_socket_path);
}

void nfs_socket_cleanup(void) {
	if (listen_fd >= 0) {
		epoll_del(listen_fd);
		close(listen_fd);
		listen_fd = -1;
	}

	if (nfs_socket_path) {
		unlink(nfs_socket_path);
	}

	pthread_mutex_lock(&streams_lock);
	g_clear_pointer(&stream_map, g_hash_table_destroy);
	pthread_mutex_unlock(&streams_lock);
}
