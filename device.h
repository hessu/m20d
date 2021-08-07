
#ifndef DEVICE_H
#define DEVICE_H

#define DEF_DEVICE "/dev/gsm"
#define IBLEN 16384

/* Open a device, returning the fd or -1 on criticalerror, -2 on temporary
 * error. Device may be a serial device file name, or a host:port pair
 * for an inet socket.
 */
extern int open_device(char *dev);

/* Write a string to fd. */
extern int hwrite(int f, char *s);

/* printf to hwrite */
extern int fdprintf(int f, const char *fmt, ...);

/* Read and discard bytes from fd for sec seconds */
extern int empty_read_buffer(int f, int sec);

/* check if one of the listed strings is found in the buffer
 * return a pointer to the location, or NULL
 */
extern char *string_in(char *buffer, char **strings);

/*
 *	Read from fd to buffer until one of the defined strings is received.
 *
 *	returns:
 *	 0 on timeout
 *	 -1 on error (read/write/EOF)
 *	 otherwise the number of bytes read
 */

extern int readuntil(int f, char *buf, int buflen, char **expect_ok, char **expect_error, int timeout);

extern char *device;
extern char *host;
extern int port;
int serial_speed;		/* in bits per second */
extern int trace_connection;	/* print module traffic to stdout */

#endif

