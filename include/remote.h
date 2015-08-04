#ifndef CROS_YAVTA_REMOTE_H_
#define CROS_YAVTA_REMOTE_H_

#include <stdbool.h>
#include <stdint.h>

// TODO add command line option to change port
#define REMOTE_SERVER_PORT 8888

// This should be big enough for v4l2_format (208 bytes) and small enough to
// fit in one network packet.
#define REMOTE_COMMAND_BLOCK_SIZE 256

typedef uint64_t remote_addr_t;

enum remote_command {
	/* Simple system calls */
	REMOTE_CMD_OPEN,
	REMOTE_CMD_CLOSE,
	REMOTE_CMD_IOCTL,
	REMOTE_CMD_MMAP,
	REMOTE_CMD_MUNMAP,
	/* Read memory from the given address and length */
	REMOTE_CMD_READMEM,
	/* Wrapping for select(). See detail in comment of handle_select_fd() */
	REMOTE_CMD_SELECT_FD,
	/* Transfer dummy data back and forth to benchmark */
	REMOTE_CMD_BENCH,
};

extern bool use_remote;
extern char remote_server_name[64];

/* Write given length. Will continue to write if it is interrupted */
int writelen(int fd, int size, const void *buf);
/* Read given length. Will continue to read if it is interrupted */
int readlen(int fd, int size, void *buf);

int connect_to_remote(int port);
int remote_open(const char *path, int mode);
int remote_close(int fd);
int remote_ioctl(int fd, int request, void *data);
void *remote_mmap(void *addr, size_t length, int prot, int flags,
		int fd, off_t offset);
int remote_munmap(void *addr, size_t length);
int remote_select_fd(int sock, int fd, int on);
int remote_readmem(void *src, void *dst, int size);
int benchmark_remote_overhead(void);

// Serialize the payload of ioctl by removing paddings in the structures.
// Returns the size after paddings removed.
int serialize_ioctl_payload(int request, const char *inbuf, char *outbuf);
// Deserialize the payload of ioctl by inserting paddings into the structures.
// len is the size of inbuf. *req is "size fixed" ioctl request number.
// Returns -1 if len is incorrect.
int deserialize_ioctl_payload(int request, const char *inbuf, char *outbuf, int len, int *req);
#endif  // CROS_YAVTA_REMOTE_H_
