#include "nfs_client.h"
#include <glib.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>

#include "log.h"
#include "call.h"
#include "media_socket.h"
#include "helpers.h"

// Simple packet header for socket communication - must match recording-daemon
struct nfs_packet_header {
	uint32_t stream_idx;
	uint32_t data_len;
	char call_id[256];
	// followed by IP/UDP/RTP packet data
};

char *nfs_socket_path = NULL;

static int socket_fd = -1;
static pthread_mutex_t socket_lock = PTHREAD_MUTEX_INITIALIZER;
static int reconnect_attempts = 0;
#define MAX_RECONNECT_ATTEMPTS 5
#define RECONNECT_DELAY_US 100000  // 100ms

#define MAX_PACKET_HEADER_LEN 128  // IP + UDP headers


static int nfs_client_connect(void) {
	if (socket_fd >= 0)
		return 0;

	if (!nfs_socket_path)
		return -1;

	socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (socket_fd < 0) {
		ilog(LOG_ERR, "NFS client: failed to create socket: %s", strerror(errno));
		return -1;
	}

	// Set non-blocking for connect attempt, then switch to blocking for sends
	int flags = fcntl(socket_fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, nfs_socket_path, sizeof(addr.sun_path) - 1);

	int ret = connect(socket_fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0 && errno != EINPROGRESS) {
		ilog(LOG_ERR, "NFS client: failed to connect to %s: %s", 
			nfs_socket_path, strerror(errno));
		close(socket_fd);
		socket_fd = -1;
		return -1;
	}

	// Wait for connection with timeout
	if (ret < 0) {
		fd_set wfds;
		struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
		FD_ZERO(&wfds);
		FD_SET(socket_fd, &wfds);
		
		ret = select(socket_fd + 1, NULL, &wfds, NULL, &tv);
		if (ret <= 0) {
			ilog(LOG_ERR, "NFS client: connection timeout to %s", nfs_socket_path);
			close(socket_fd);
			socket_fd = -1;
			return -1;
		}

		int error = 0;
		socklen_t len = sizeof(error);
		getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &len);
		if (error) {
			ilog(LOG_ERR, "NFS client: connection error to %s: %s", 
				nfs_socket_path, strerror(error));
			close(socket_fd);
			socket_fd = -1;
			return -1;
		}
	}

	// Switch back to blocking mode for sends
	if (flags >= 0)
		fcntl(socket_fd, F_SETFL, flags);

	ilog(LOG_INFO, "NFS client: connected to %s", nfs_socket_path);
	reconnect_attempts = 0;
	return 0;
}


bool nfs_client_init(void) {
	if (!nfs_socket_path) {
		ilog(LOG_DEBUG, "NFS client: no socket path configured");
		return false;
	}

	pthread_mutex_lock(&socket_lock);
	int ret = nfs_client_connect();
	pthread_mutex_unlock(&socket_lock);

	return ret == 0;
}


void nfs_client_cleanup(void) {
	pthread_mutex_lock(&socket_lock);
	if (socket_fd >= 0) {
		close(socket_fd);
		socket_fd = -1;
	}
	pthread_mutex_unlock(&socket_lock);
}


bool nfs_client_available(void) {
	return nfs_socket_path != NULL;
}


// Build fake IP/UDP headers for the packet (same format as kernel interface)
static unsigned int build_packet_headers(unsigned char *out, struct media_packet *mp, const str *payload) {
	endpoint_t *src_endpoint = &mp->fsin;
	endpoint_t *dst_endpoint = &mp->sfd->socket.local;

	unsigned int hdr_len = endpoint_packet_header(out, src_endpoint, dst_endpoint, payload->len);

	// Copy payload
	memcpy(out + hdr_len, payload->s, payload->len);

	return hdr_len + payload->len;
}


int nfs_client_send_packet(const char *call_id, unsigned int stream_idx,
                           struct media_packet *mp, const str *packet_data)
{
	if (!nfs_socket_path)
		return -1;

	pthread_mutex_lock(&socket_lock);

	// Try to reconnect if needed
	if (socket_fd < 0) {
		if (reconnect_attempts >= MAX_RECONNECT_ATTEMPTS) {
			pthread_mutex_unlock(&socket_lock);
			return -1;
		}
		reconnect_attempts++;
		if (nfs_client_connect() < 0) {
			pthread_mutex_unlock(&socket_lock);
			return -1;
		}
	}

	// Build the packet with headers
	size_t max_pkt_size = sizeof(struct nfs_packet_header) + MAX_PACKET_HEADER_LEN + packet_data->len;
	unsigned char *buf = alloca(max_pkt_size);

	struct nfs_packet_header *hdr = (void *)buf;
	memset(hdr, 0, sizeof(*hdr));
	hdr->stream_idx = stream_idx;
	strncpy(hdr->call_id, call_id, sizeof(hdr->call_id) - 1);

	// Build the packet data with IP/UDP headers
	unsigned char *pkt_data = buf + sizeof(*hdr);
	hdr->data_len = build_packet_headers(pkt_data, mp, packet_data);

	size_t total_len = sizeof(*hdr) + hdr->data_len;

	// Send the packet
	ssize_t sent = send(socket_fd, buf, total_len, MSG_NOSIGNAL);
	if (sent < 0) {
		ilog(LOG_WARN, "NFS client: send failed: %s", strerror(errno));
		close(socket_fd);
		socket_fd = -1;
		pthread_mutex_unlock(&socket_lock);
		return -1;
	}

	if ((size_t)sent != total_len) {
		ilog(LOG_WARN, "NFS client: partial send: %zd/%zu", sent, total_len);
		pthread_mutex_unlock(&socket_lock);
		return -1;
	}

	pthread_mutex_unlock(&socket_lock);
	return 0;
}
