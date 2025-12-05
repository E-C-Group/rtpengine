#ifndef _NFS_SOCKET_H_
#define _NFS_SOCKET_H_

#include <stdbool.h>

extern char *nfs_socket_path;

void nfs_socket_setup(void);
void nfs_socket_cleanup(void);

// Called when we get a STREAM interface section from metadata
// to register the stream for socket-based packet delivery
void nfs_socket_register_stream(const char *call_parent, unsigned int stream_idx, const char *stream_name);
void nfs_socket_unregister_stream(const char *call_parent, unsigned int stream_idx);

#endif
