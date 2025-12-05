#ifndef _NFS_CLIENT_H_
#define _NFS_CLIENT_H_

#include <stdbool.h>
#include "str.h"

struct media_packet;

extern char *nfs_socket_path;

// Initialize the NFS socket client (connect to recording daemon)
bool nfs_client_init(void);

// Cleanup the NFS socket client
void nfs_client_cleanup(void);

// Check if NFS client is connected and available
bool nfs_client_available(void);

// Send a packet to the recording daemon
// Returns 0 on success, -1 on error
int nfs_client_send_packet(const char *call_id, unsigned int stream_idx,
                           struct media_packet *mp, const str *packet_data);

#endif
