
/*
 *	device.c
 *
 *	m20d - driver for Siemens M20 GSM modules
 *	by Heikki Hannikainen
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>

#include "device.h"
#include "log.h"
#include "hmalloc.h"

char *device = DEF_DEVICE;
char *host = NULL;
int port = 0;
int serial_speed = 38400;	/* in bits per second */
int trace_connection = 0;	/* print module traffic to stdout */

/*
 *	Open a serial device and configure it, returning the fd
 */

int open_serial_device(char *d)
{
	int f;
	int speed;
	struct termios tio;
	
	if ((f = open(d, O_RDWR)) == -1) {
		hlog(LOG_CRIT, "Could not open %s for read/write: %s",
			d, strerror(errno));
		return -1;
	}
	
	if (tcgetattr(f, &tio)) {
		hlog(LOG_CRIT, "tcgetattr %s failed: %s", d, strerror(errno));
		close(f);
		return -1;
	}
	
	switch (serial_speed) {
	case 300: speed = B300; break;
	case 600: speed = B600; break;
	case 1200: speed = B1200; break;
	case 2400: speed = B2400; break;
	case 4800: speed = B4800; break;
	case 9600: speed = B9600; break;
	case 19200: speed = B19200; break;
	case 38400: speed = B38400; break;
#ifdef B57600
	case 57600: speed = B57600; break;
#endif
#ifdef B76800
	case 76800: speed = B76800; break;
#endif
#ifdef B115200
	case 115200: speed = B115200; break;
#endif
	default:
		hlog(LOG_CRIT, "Unsupported serial port speed: %d", serial_speed);
		close(f);
		return -1;
	}
	
	tio.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
	tio.c_oflag &= ~OPOST;
	tio.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	tio.c_cflag = CS8|CREAD|HUPCL|CLOCAL;
	cfsetispeed(&tio, speed);
	cfsetospeed(&tio, speed);
	
	if (tcsetattr(f, TCSANOW, &tio)) {
		hlog(LOG_CRIT, "tcsetattr on %s failed: %s", d, strerror(errno));
		close(f);
		return -1;
	}
	
	return f;
}

/*
 *	Open a socket device
 */

int open_socket_device(char *h, int p)
{
	int f;
	struct sockaddr_in sa;
	struct hostent *hp;
	
	hlog(LOG_DEBUG, "Looking up host %s ...", h);
	
	if (!(hp = gethostbyname(h))) {
		hlog(LOG_ERR, "Could not resolve hostname: %s", h);
		return -2;
	}
	
	memset(&sa, 0, sizeof(struct sockaddr_in));sa.sin_family = AF_INET;
	memcpy(&sa.sin_addr, hp->h_addr_list[0], hp->h_length);
	sa.sin_port = htons((u_short) port);
	
	hlog(LOG_DEBUG, "Connecting to %s:%d ...", inet_ntoa(sa.sin_addr), port);
	
	if ((f = socket(sa.sin_family, SOCK_STREAM, 0)) < 0) {  /* get socket */
		hlog(LOG_CRIT, "Could not get a socket: %s", strerror(errno));
		return -1;
	}
	
	if (connect(f, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		hlog(LOG_ERR, "Could not connect to %s:%d: %s", inet_ntoa(sa.sin_addr), port, strerror(errno));
		close(f);
		return -2;
	}
	
	return f;
}

/*
 *	Figure out if the device is a host:port inet address or a device file
 */

int open_device(char *dev)
{
	char *p, *d;
	int i;
	
	d = hstrdup(dev);
	
	hlog(LOG_INFO, "Connecting to module at %s ...", dev);
	
	if ((p = strrchr(d, ':'))) {
		*p++ = 0;
		i = atoi(p);
		if (i <  1 || i > 65535) {
			hlog(LOG_CRIT, "Specified TCP port %d is out of bounds (1-65535).", i);
			hfree(d);
			return -1;
		}
		if (host)
			hfree(host);
		host = hstrdup(d);
		port = i;
		
		hfree(d);
		
		return open_socket_device(host, port);
	} else {
		i = open_serial_device(d);
		hfree(d);
		return i;
	}
}

/*
 *	Write a string to fd
 */

int hwrite(int f, char *s)
{
	int w;
	int l = strlen(s);
	if ((w = write(f, s, l)) != l) {
		hlog(LOG_CRIT, "hwrite: Write to fd %d only wrote %d instead of %d: %s",
			f, w, l, strerror(errno));
	}
	if (trace_connection) {
		printf("hwrite: %s", s);
		fflush(stdout);
	}
	return w;
}

/*
 *	printf a string to fd
 */

int fdprintf(int f, const char *fmt, ...)
{
	va_list args;
	char s[IBLEN];
	
	va_start(args, fmt);

#ifdef NO_SNPRINTF
	vsprintf(s, fmt, args);
#else
	vsnprintf(s, IBLEN, fmt, args);
#endif
        va_end(args);
        s[IBLEN] = '\0';
	return hwrite(f, s);
}

/*
 *	Read and discard bytes from fd for sec seconds
 */

int empty_read_buffer(int f, int sec)
{
	fd_set read_fds;
	struct timeval tv;
	char c;
	int i, n;
	
	FD_ZERO(&read_fds);
	FD_SET(f, &read_fds);
	tv.tv_sec = sec;
	tv.tv_usec = 0;
	
	if (trace_connection)
		printf("empty_read_buffer: ");
	
	n = 0;
	while ((i = select(f+1, &read_fds, NULL, NULL, &tv))) {
		if (read(f, &c, 1) != 1) {
 			hlog(LOG_ERR, "Could not read a character from %s: %s", device, strerror(errno));
			return -2;
		}
		n++;
		if ((trace_connection) && (c != '\r'))
			printf("%c", c);
		FD_ZERO(&read_fds);
		FD_SET(f, &read_fds);
	}
	if (trace_connection) {
		printf("\n");
		fflush(stdout);
	}
	if (i == -1) {
		hlog(LOG_ERR, "select on %s failed: %s", device, strerror(errno));
		return -2;
	}
	return n;
}

/*
 *	check if one of the listed strings is found in the buffer
 *	return a pointer to the location
 */

char *string_in(char *buffer, char **strings)
{
	char **s, *p;
	
	for (s = strings; (*s); s++)
		if ((p = strstr(buffer, *s)))
			return p;
	
	return NULL;
}

/*
 *	Read from fd to buffer until one of the defined strings is received.
 *
 *	returns:
 *	 0 on timeout
 *	 -1 on error (read/write/EOF)
 *	 otherwise the number of bytes read
 */

int readuntil(int f, char *buf, int buflen, char **expect_ok, char **expect_error, int timeout)
{
	fd_set read_fds;
	struct timeval tv;
	char c;
	int i = 0;
	int l, r;
	
	FD_ZERO(&read_fds);
	FD_SET(f, &read_fds);
	
	tv.tv_sec = timeout / 1000;
	tv.tv_usec = (timeout % 1000) * 1000;
	
	l = 0;
	bzero(buf, buflen);
	
	if (trace_connection)
		printf("readuntil: ");
	
	while ((l < buflen) && (!string_in(buf, expect_ok)) && (!string_in(buf, expect_error))
	   && ((i = select(f+1, &read_fds, NULL, NULL, &tv) > 0))) {
		r = read(f, &c, 1);
		if (r == 0) {
			hlog(LOG_ERR, "End of file from %s", device);
			return -1;
		}
		if (r < 0) {
			hlog(LOG_ERR, "Could not read a character from %s: %s", device, strerror(errno));
			return -1;
		}
		
		if (c != '\r') {
			buf[l] = c;
			l++;
			if (trace_connection)
				printf("%c", c);
		}
		
		FD_ZERO(&read_fds);
		FD_SET(f, &read_fds);
	}
	
	if (trace_connection) {
		printf("\n");
		fflush(stdout);
	}
	
	buf[buflen-1] = '\0';
	
	if (i == 0) {
		//hlog(LOG_DEBUG, "select() timed out");
		return 0;
	}
	
	if (i == -1) {
		hlog(LOG_CRIT, "select on %s failed: %s", device, strerror(errno));
		if (errno == EINTR)
			return -1;
		else
			exit(2);
	}
	
	return l;
}

