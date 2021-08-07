
/*
 *	m20d - driver for Siemens M20 GSM modules
 *	by Heikki Hannikainen
 *
 *	PDU handling code is a cleaned up version from
 *	SMS Server Tools, Copyright (C) 2000-2002 Stefan Frings
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


#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#ifndef __sun__
#include <getopt.h>
#endif

#include "log.h"
#include "hmalloc.h"
#include "charset.h"
#include "message.h"
#include "device.h"

/* Default settings */

#define PROGNAME "m20d"
#define VERSION "2.1.6"

#define BASEDIR "/opt/m20d"
#define DEF_SPOOLDIR BASEDIR "/spool"
#define DEF_SPOOL_Q DEF_SPOOLDIR "/queue"
#define DEF_HANDLER BASEDIR "/bin/handler.pl"
#define DEF_DEVICE "/dev/gsm"
#define DEF_PIN "0000"

#define VERSTR PROGNAME " " VERSION " by Heikki Hannikainen\n"

/*
 * ********************
 * Configurable items
 * ********************
 */

char *pin = DEF_PIN;
char *logname = PROGNAME;

int spool_scantime = 200;	/* MO spool directory scan interval, milliseconds ! */
int cmd_timeout = 3000;		/* Any other command, ms ! */
int transmit_timeout = 60000;	/* MO transmit timeout, ms ! */
int register_timeout = 60000;	/* Network registration timeout (from AT+CPIN to OK), ms ! */
int retry_sleep = 10;		/* module reconnection delay time: seconds */
int poll_time = 30;		/* module poll time: seconds */
int mo_queue_max_tries = 4;	/* mo max tries */
int mo_queue_init_retryt = 10;	/* mo initial retry time: seconds */
float mo_queue_retry_mult = 3;	/* retry time multiplicator at each retry */
int mo_queue_max_retryt = 300;	/* mo max retry time: seconds */
int fork_a_daemon = 0;		/* fork a daemon */

char *spool_dir = DEF_SPOOLDIR;
char *outhandler = DEF_HANDLER;
char *pidfile = NULL;
char *statefile = NULL;

/*
 * ********************
 * Variables and stats
 * ********************
 */

int shutting_down = 0;		/* a shutdown is queued */

long stats_mt = 0;		/* received MT messages */
long stats_mt_ok = 0;		/* MT: successfully handled messages */
long stats_mt_fail = 0;		/* MT: unsuccessfully handled messages */
long stats_mt_fail_parse = 0;	/* MT: PDU parsing failures */
long stats_mt_fail_handle = 0;	/* MT: Handler failures */
long stats_mo = 0;		/* MO: messages taken for delivery */
long stats_mo_ok = 0;		/* MO: successfully delivered */
long stats_mo_tries = 0;	/* MO: delivery attempts made */
long stats_mo_try_fail = 0;	/* MO: delivery attempts failed */
long stats_mo_dropped = 0;	/* MO: messages dropped */

/*
 *	running state
 */

#define STATE_UNDEFINED		0
#define STATE_DOWN_INIT		1
#define STATE_DOWN_CONNECTING	2
#define STATE_DOWN_HANDSHAKING	3
#define STATE_DOWN_NONETWORK	4
#define STATE_DOWN_RETRYSLEEP	5
#define STATE_DOWN_SHUTTINGDOWN	6
#define STATE_DOWN_SHUTDOWN	7
#define STATE_DOWN_FAILQUIT	8
#define STATE_UP		9
#define STATE_UP_SLEEPING	9
#define STATE_UP_SENDING_MO	10
#define STATE_UP_POLLING	11
#define STATE_MAX		12

static char *state_strings[] = {
	"DOWN/UNDEFINED",
	"DOWN/INIT",
	"DOWN/CONNECTING",
	"DOWN/HANDSHAKING",
	"DOWN/NONETWORK",
	"DOWN/RETRYSLEEP",
	"DOWN/SHUTTINGDOWN",
	"DOWN/SHUTDOWN",
	"DOWN/FAILQUIT",
	"UP/SLEEPING",
	"UP/SENDING_MO",
	"UP/POLLING",
	NULL
};

int running_state = STATE_UNDEFINED;
char *last_message = NULL;
char *net_status = NULL;

/*
 *	response sets to expect
 */

char *expect_errors[] = { "ERROR", NULL };
char *expect_ok[] = { "OK", NULL };
char *expect_ok_or_mt[] = { "OK", "+CMT:", "+CDS:", "+CBM:", NULL };
char *expect_mt[] = { "+CMT:", "+CDS:", "+CBM:", NULL };
char *expect_mo_transmit[] = { ">", "+CMT:", "+CDS:", "+CBM:", NULL };
char *expect_linefeed[] = { "\n", NULL };

/*
 *	Log statistics counters
 */

void log_stats(void)
{
	hlog(LOG_NOTICE, "STATS mt=%ld mt_ok=%ld mt_fail=%ld mt_fail_parse=%ld mt_fail_handle=%ld"
		" mo=%ld mo_ok=%ld mo_dropped=%ld mo_tries=%ld mo_try_fails=%ld mo_queued=%ld mo_queue_len=%ld",
		stats_mt, stats_mt_ok, stats_mt_fail, stats_mt_fail_parse, stats_mt_fail_handle,
		stats_mo, stats_mo_ok, stats_mo_dropped, stats_mo_tries, stats_mo_try_fail, stats_mo_queued, stats_mo_queue_len);
}

/*
 *	Translate state to string
 */

char *statestring(int state)
{
	if (state < 0 || state >= STATE_MAX) {
		state = STATE_UNDEFINED;
	}
	return state_strings[state];
}

/*
 *	Write state file
 */

int write_statefile(char *fname)
{
	char *tmpf;
	int fd;
	FILE *f;
	time_t t;
	struct tm *rt;
	
	tmpf = hmalloc(strlen(fname) + 4 + 1);
	sprintf(tmpf, "%s.tmp", fname);
	
	fd = open(tmpf, O_CREAT|O_EXCL|O_WRONLY, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
	if (f < 0) {
		hlog(LOG_ERR, "Could not create temporary state file %s: %s", tmpf, strerror(errno));
		hfree(tmpf);
		return -1;
	}
	
	if (!(f = fdopen(fd, "w"))) {
		hlog(LOG_ERR, "Could not fdopen temporary state file %s: %s", tmpf, strerror(errno));
		close(fd);
		if (unlink(tmpf))
			hlog(LOG_ERR, "Could not unlink temporary state file %s: %s", tmpf, strerror(errno));
		hfree(tmpf);
		return -1;
	}
	
	time(&t);
	rt = gmtime(&t);
	
	fprintf(f, "State: %s\n", statestring(running_state));
	fprintf(f, "Message: %s\n", (last_message) ? last_message : "No message");
	if (net_status)
		fprintf(f, "Network: %s\n", net_status);
	fprintf(f, "Updated: %02d/%02d/%02d %d:%02d:%02d UTC %ld\n",
		rt->tm_year % 100, rt->tm_mon + 1, rt->tm_mday,
		rt->tm_hour, rt->tm_min, rt->tm_sec,
		(long)t);
	
	if (fclose(f)) {
		hlog(LOG_ERR, "Could not close temporary state file %s after writing: %s", tmpf, strerror(errno));
		if (unlink(tmpf))
			hlog(LOG_ERR, "Could not unlink temporary state file %s: %s", tmpf, strerror(errno));
		hfree(tmpf);
		return -1;
	}
	
	if (rename(tmpf, fname)) {
		hlog(LOG_ERR, "Could not rename state file %s to %s: %s", tmpf, fname, strerror(errno));
		if (unlink(tmpf))
			hlog(LOG_ERR, "Could not unlink temporary state file %s: %s", tmpf, strerror(errno));
		hfree(tmpf);
		return -1;
	}
	
	return 0;
}

/*
 *	State change
 */

void state_change(int new_state, const char *fmt, ...)
{
	va_list args;
	char s[LOG_LEN];
	
	if (fmt) {
		va_start(args, fmt);
		vsnprintf(s, LOG_LEN, fmt, args);
		va_end(args);
		
		if (last_message)
			hfree(last_message);
		last_message = hstrdup(s);
	}
	
	if (running_state != new_state) {
		hlog(LOG_DEBUG, "Changing state from %s to %s", statestring(running_state), statestring(new_state));
	}
	
	running_state = new_state;
	
	write_statefile(statefile);
}

/*
 *      Signal handler
 */

void sig_handler(int sig)
{
	int s = sig;
	
	switch (s) {
	case SIGUSR1:
		signal(SIGUSR1, &sig_handler);
		log_stats();
		return;
	case SIGINT:
		hlog(LOG_NOTICE, "Received SIGINT, shutting down ...");
		shutting_down = 1;
		return;
	case SIGTERM:
		hlog(LOG_NOTICE, "Received SIGTERM, shutting down ...");
		shutting_down = 1;
		return;
	default:
		log_stats();
		hlog(LOG_CRIT, "Received signal %d, quitting NOW.", s);
		if (stats_mo_queue_len)
			hlog(LOG_ERR, "Lost %d queued messages!", stats_mo_queue_len);
		exit(0);
	}
}

/*
 *	Print help
 */

void print_version(void)
{
	fputs(VERSTR, stderr);
}

void print_help(void)
{
	fprintf(stderr, "usage: " PROGNAME " [-n <logname>] [-d <device>] [-b <speed>] [-p <pin>]\n" \
		"\t[-s <spooldir>] [-a <handler>] [-x <pidfile>]\n" \
		"\t[-t <cmd timeout>] [-l <reconnect delay>] [-i <poll interval>]\n" \
		"\t[-e <loglevel>] [-o <logdest>] [-f (fork)] [-r (trace)]\n" \
		"\t[-1 <initial retry time>] [-2 <retry time multiplicator>]\n" \
		"\t[-3 <max retry count>]\n" \
		"defaults: device " DEF_DEVICE " pin " DEF_PIN "\n" \
		"\tspool " DEF_SPOOLDIR " handler " DEF_HANDLER "\n" \
		"log levels: " LOG_LEVELS "\n" \
		"log destinations: " LOG_DESTS "\n");
}

/*
 *	Parse arguments
 */

void parse_cmdline(int argc, char *argv[])
{
	int s;
	int i;
	
	while ((s = getopt(argc, argv, "d:b:p:n:x:t:i:l:s:a:e:o:1:2:3:fr?h")) != -1) {
	switch (s) {
		case 'd':
			device = hstrdup(optarg);
			break;
		case 'b':
			if ((serial_speed = atoi(optarg)) <= 0) {
				fprintf(stderr, "Bad serial speed: \"%s\": minimum 1.\n", optarg);
				print_help();
				exit(1);
			}
			break;
		case 'p':
			pin = hstrdup(optarg);
			break;
		case 'n':
			logname = hstrdup(optarg);
			break;
		case 'x':
			pidfile = hstrdup(optarg);
			break;
		case 't':
			cmd_timeout = atoi(optarg);
			break;
		case 'l':
			if ((retry_sleep = atoi(optarg)) <= 1) {
				fprintf(stderr, "Bad retry sleep \"%s\": minimum 1.\n", optarg);
				print_help();
				exit(1);
			}
			break;
		case 'i':
			if ((poll_time = atoi(optarg)) <= 1) {
				fprintf(stderr, "Bad poll time \"%s\": minimum 1.\n", optarg);
				print_help();
				exit(1);
			}
			break;
		case 's':
			spool_dir = hstrdup(optarg);
			break;
		case 'a':
			outhandler = hstrdup(optarg);
			break;
		case 'e':
			i = pick_loglevel(optarg, log_levelnames);
			if (i > -1)
				log_level = i;
			else {
				fprintf(stderr, "Log level unknown: \"%s\"\n", optarg);
				print_help();
				exit(1);
			}
			break;
		case 'o':
			i = pick_loglevel(optarg, log_destnames);
			if (i > -1)
				log_dest = i;
			else {
				fprintf(stderr, "Log destination unknown: \"%s\"\n", optarg);
				print_help();
				exit(1);
			}
			break;
		case '1':
			mo_queue_init_retryt = atoi(optarg);
			break;
		case '2':
			mo_queue_retry_mult = atof(optarg);
			break;
		case '3':
			mo_queue_max_tries = atoi(optarg);
			break;
		case 'f':
			fork_a_daemon = 1;
			break;
		case 'r':
			trace_connection = 1;
			break;
		default:
			print_version();
			print_help();
			exit(0);
	}
	}
	
	if (!statefile) {
		statefile = hmalloc(strlen(spool_dir) + 1 + strlen(logname) + 7);
		sprintf(statefile, "%s/state.%s", spool_dir, logname);
	}
}

/*
 *	Issue a command to the module, wait for normal OK or error or timeout
 *	Does not handle incoming messages if such appear
 *
 * returns:
 *	0 on OK
 *	-1 on I/O error
 *	-2 on error message received
 *	-3 on timeut
 */

int issue_cmd_nomt(int f, char *cmd, char *msgid)
{
 	char buf[IBLEN];
	int i;
	
	buf[0] = 0;
	
	fdprintf(f, "%s\r\n", cmd);
	i = readuntil(f, buf, IBLEN, expect_ok, expect_errors, cmd_timeout);
	if (i > 0) {
		if (string_in(buf, expect_errors)) {
			hlog(LOG_ERR, "[%s] Error response to %s !", msgid, cmd);
			i =  -2;
		}
	} else if (i < 1) {
		hlog(LOG_ERR, "[%s] No OK response to %s: I/O error!", msgid, cmd);
		i = -1;
	} else if (i == 0) {
		hlog(LOG_ERR, "[%s] Timeout for %s !", msgid, cmd);
		i = -3;
	}
	
	return i;
}

/*
 *	Fork a handler program for MT message
 */
 
int fork_handler(struct message *m)
{
	char buf[IBLEN];
	pid_t p;
	int i;
	char *tmpf;
	char *spoolf;
	int fd;
	FILE *f;
	struct tm *rt;
	
	spoolf = hmalloc(strlen(spool_dir) + 1 + strlen(m->msgid) + 3 + 1);
	sprintf(spoolf, "%s/%s.mt", spool_dir, m->msgid);
	tmpf = hmalloc(strlen(spoolf) + 4 + 1);
	sprintf(tmpf, "%s.tmp", spoolf);
	
	hlog(LOG_DEBUG, "[%s] Writing temporary spool file: %s", m->msgid, tmpf);
	fd = open(tmpf, O_CREAT|O_EXCL|O_WRONLY, S_IRUSR|S_IWUSR|S_IRGRP);
	if (f < 0) {
		hlog(LOG_ERR, "[%s] Could not create spool file %s: %s", m->msgid, tmpf, strerror(errno));
		hfree(spoolf);
		hfree(tmpf);
		return -1;
	}
	
	if (!(f = fdopen(fd, "w"))) {
		hlog(LOG_ERR, "[%s] Could not fdopen spool file %s: %s", m->msgid, tmpf, strerror(errno));
		close(fd);
		if (unlink(tmpf))
			hlog(LOG_ERR, "[%s] Could not unlink spool file %s: %s", m->msgid, tmpf, strerror(errno));
		hfree(spoolf);
		hfree(tmpf);
		return -1;
	}
	
	rt = gmtime(&m->received);
	
	fprintf(f, "From: %s\n", m->src);
	fprintf(f, "Message-id: %s\n", m->msgid);
	fprintf(f, "Sent: %s %s\n", m->date, m->time);
	fprintf(f, "Received: %02d/%02d/%02d %d:%02d:%02d UTC %ld\n",
		rt->tm_year % 100, rt->tm_mon + 1, rt->tm_mday,
		rt->tm_hour, rt->tm_min, rt->tm_sec,
		(long)m->received);
	fprintf(f, "TP-PID: %d\n", m->pid);
	fprintf(f, "TP-DCS: %d\n", m->dcs);
	if (m->has_udh)
		fprintf(f, "Has-UDH: %d\n", m->has_udh);
	if (m->is_binary) {
		fprintf(f, "Is-binary: %d\n", m->is_binary);
		fprintf(f, "Length: %d\n", m->len);
	}
	fprintf(f, "\n");
	
	if (m->len > 0) {
		if (m->is_binary) {
			bin2hexstring(m->content, m->len, buf);
			i = fwrite(buf, strlen(buf), 1, f);
		} else
			i = fwrite(m->content, m->len, 1, f);
			
		if (i != 1) {
			hlog(LOG_ERR, "[%s] Could not write to spool file %s: %s", m->msgid, tmpf, strerror(errno));
			if (fclose(f))
				hlog(LOG_ERR, "[%s] Could not close spool file %s after failed write: %s", m->msgid, tmpf, strerror(errno));
			if (unlink(tmpf))
				hlog(LOG_ERR, "[%s] Could not unlink spool file %s: %s", m->msgid, tmpf, strerror(errno));
			hfree(spoolf);
			hfree(tmpf);
			return -1;
		}
	}
	
	if (fclose(f)) {
		hlog(LOG_ERR, "[%s] Could not close spool file %s after writing: %s", m->msgid, tmpf, strerror(errno));
		if (unlink(tmpf))
			hlog(LOG_ERR, "[%s] Could not unlink spool file %s: %s", m->msgid, tmpf, strerror(errno));
		hfree(spoolf);
		hfree(tmpf);
		return -1;
	}
	
	if (rename(tmpf, spoolf)) {
		hlog(LOG_ERR, "[%s] Could not rename spool file %s to %s: %s", m->msgid, tmpf, spoolf, strerror(errno));
		if (unlink(tmpf))
			hlog(LOG_ERR, "[%s] Could not unlink spool file %s: %s", m->msgid, tmpf, strerror(errno));
		hfree(spoolf);
		hfree(tmpf);
		return -1;
	}
	
	if ((p = fork()) == 0) {
		/* child */
		for (i = 3; i < 128; i++)
			close(i);
		close(0);
		hlog(LOG_DEBUG, "[%s] Executing handler with pid %d spoolfile %s", m->msgid, (int)getpid(), spoolf);
		execl(outhandler, outhandler, m->msgid, m->src, spoolf, NULL);
		hlog(LOG_ERR, "[%s] Could not execute handler %s: %s", m->msgid, outhandler, strerror(errno));
		if (unlink(spoolf))
			hlog(LOG_ERR, "[%s] Could not unlink spool file %s: %s", m->msgid, spoolf, strerror(errno));
		exit(0);
	}
	
	/* parent */
	if (p < 0) {
		hlog(LOG_ERR, "[%s] Fork failed for handler: %s", m->msgid, strerror(errno));
		if (unlink(spoolf))
			hlog(LOG_ERR, "[%s] Could not unlink spool file %s: %s", m->msgid, spoolf, strerror(errno));
		hfree(spoolf);
		hfree(tmpf);
		return -1;
	}
	
	hfree(spoolf);
	hfree(tmpf);
	
	return 0;
}

/*
 *	Parse an ASCII PDU
 */

#define MAX_PDU_BIN_LEN 500

int mt_parse_pdu_ascii(struct message *m, char *pdu)
{
	char bin[MAX_PDU_BIN_LEN];
	char ascii[MAX_PDU_BIN_LEN];
	int binlen;
	
	/* length of data */
	binlen = octet2bin(pdu);
	if (binlen > MAX_PDU_BIN_LEN - 100) {
		hlog(LOG_ERR, "[%s] Ouch, received message claims to have a %d bytes of content, discarding message!", m->msgid, binlen);
		return -1;
	}
	
	/* convert from hex to binary */
	hexstring2bin(pdu + 2, binlen, bin, MAX_PDU_BIN_LEN);
	
	/* from binary to ascii */
	binary2ascii(bin, binlen, ascii, MAX_PDU_BIN_LEN, 0);
	
	m->content = hstrdup(ascii);
	m->len = binlen;
	
	ascii2escaped(ascii, strlen(ascii), bin, MAX_PDU_BIN_LEN);
	hlog(LOG_DEBUG, "[%s] Text %d bytes: \"%s\"", m->msgid, binlen, bin);
	
	return 0;
}

int mt_parse_pdu_binary(struct message *m, char *pdu)
{
	unsigned char bin[MAX_PDU_BIN_LEN];
	int binlen;
	int l;
	
	binlen = octet2bin(pdu);
	if (binlen > MAX_PDU_BIN_LEN - 100) {
		hlog(LOG_ERR, "[%s] Ouch, received message claims to have a %d bytes of content, discarding message!", m->msgid, binlen);
		return -1;
	}
	for (l  = 0; l < binlen; l++)
		bin[l] = octet2bin(pdu + (l<<1) + 2);
	bin[l] = 0;
	
	hlog(LOG_DEBUG, "[%s] Binary %d bytes", m->msgid, binlen);
	
	m->content = hmalloc(binlen);
	memcpy(m->content, bin, binlen);
	m->len = binlen;
	
	return 0;
}

/*
 *	Split a DELIVER type PDU
 */

int mt_split_pdu_deliver(struct message *m, char *pdu)
{
	int l, pad;
	unsigned char tonnpi, ton, npi;
	unsigned char pid, dcs;
	char binsender[60];
	char sender[60];
	char *senderp;
	char date[10];
	char time[10];
	
	/* length of sender address */
	l = octet2bin(pdu);
	pad = l % 2;
	
	if (l > 50) {
		hlog(LOG_ERR, "[%s] Ouch, received PDU claims to have a %d-character sender address, discarding message!", m->msgid, l);
		return -1;
	}
	
	/* TON/NPI (Type of number, numbering plan indicator */
	pdu += 2;
	tonnpi = octet2bin(pdu);
	npi = tonnpi & 15;
	ton = (tonnpi >> 4) & 7;
	if (((tonnpi >> 7) & 1) != 1)
		hlog(LOG_WARNING, "[%s] Very strange, bit 7 of Type-of-Address is not 1!", m->msgid);
	
	hlog(LOG_DEBUG, "[%s] Sender address TON %d (%s) NPI %d (%s) length %d%s", m->msgid, ton, tons[ton], npi, npis(npi), l, (pad) ? " (pad)" : "");
	
	/* grab sender address */
	pdu += 2;
	if (ton == TON_ALPHANUMERIC) {
		/* Alphanumeric sender */
			
		/* convert from hex to binary */
		hexstring2bin(pdu, l / 2 +1, binsender, sizeof(binsender));
		
		/* from binary to ascii */
		binary2ascii(binsender, l / 2 +1, sender, sizeof(sender), 1);
		
	} else {
		/* Non-alphanumeric, so should be numeric */
		if (ton == TON_INTERNATIONAL) {
			/* international format number, prepend ASCII formatted number with + */
			strcpy(sender, "+");
			senderp = &sender[1];
		} else {
			/* anything else */
			senderp = &sender[0];
		}
		
		strncpy(senderp, pdu, l + pad);
		swapchars(senderp);
		senderp[l] = 0;
		
		if (senderp[l-1] == 'F')
			senderp[l-1] = 0;
	}
	
	pdu += l + pad;
	
	/* Protocol Identifier */
	m->pid = pid = octet2bin(pdu);
	
	/* Data Coding Scheme */
	pdu += 2;
	m->dcs = dcs = octet2bin(pdu);
	
	hlog(LOG_DEBUG, "[%s] pid %d dcs %d", m->msgid, pid, dcs);
	
	/* binary? */
	if (dcs >> 6 == 0) {
		/* General Data Coding */
		hlog(LOG_DEBUG, "[%s] DCS: General data coding", m->msgid);
		if (((dcs >> 5) & 1) == 1) {
			hlog(LOG_ERR, "[%s] DCS: Text is compressed! Cannot parse.", m->msgid);
			return -1;
		} else
			hlog(LOG_DEBUG, "[%s] DCS: Text is uncompressed", m->msgid);
		if (((dcs >> 4) & 1) == 1) {
			hlog(LOG_DEBUG, "[%s] DCS: Message class: %d: %s", m->msgid, dcs & 3, messageclasses[dcs & 3]);
		} else {
			hlog(LOG_DEBUG, "[%s] DCS: No message class (%d)", m->msgid, dcs & 3);
		}
		switch (dcs >> 2 & 3) {
		case 0:
			hlog(LOG_DEBUG, "[%s] DCS: Alphabet: Default", m->msgid);
			break;
		case 1:
		case 2:
		case 3:
			hlog(LOG_DEBUG, "[%s] DCS: Alphabet %s (%d)! Assuming binary.",
				m->msgid, alphabets[dcs >> 2 & 3], dcs >> 2 & 3);
			m->is_binary = 1;
			break;
		}
	} else if (dcs >> 6 == 1 || dcs >> 6 == 2) {
		/* Reserved coding groups */
		hlog(LOG_DEBUG, "[%s] DCS: Reserved data coding group! Assuming binary.", m->msgid);
		hlog(LOG_DEBUG, "[%s] DCS: Alphabet: %d: %s", m->msgid, dcs >> 2 & 3, alphabets[dcs >> 2 & 3]);
		m->is_binary = 1;
	} else if (dcs >> 6 == 3) {
		switch (dcs >> 4 & 3) {
		case 0:
			hlog(LOG_DEBUG, "[%s] DCS: Message waiting indication: Discard Message (Default Alphabet).", m->msgid);
			hlog(LOG_DEBUG, "[%s] DCS: Set Indication %s: %s message waiting",
				m->msgid, (dcs >> 3 & 1) ? "Active" : "Inactive", messagewaitclasses[dcs & 3]);
			break;
		case 1:
			hlog(LOG_DEBUG, "[%s] DCS: Message waiting indication: Store Message (Default Alphabet).", m->msgid);
			hlog(LOG_DEBUG, "[%s] DCS: Set Indication %s: %s message waiting",
				m->msgid, (dcs >> 3 & 1) ? "Active" : "Inactive", messagewaitclasses[dcs & 3]);
			break;
		case 2:
			hlog(LOG_DEBUG, "[%s] DCS: Message waiting indication: Store Message (UCS). Interpreting as binary.", m->msgid);
			hlog(LOG_DEBUG, "[%s] DCS: Set Indication %s: %s message waiting",
				m->msgid, (dcs >> 3 & 1) ? "Active" : "Inactive", messagewaitclasses[dcs & 3]);
			m->is_binary = 1;
			break;
		case 3:
			hlog(LOG_DEBUG, "[%s] DCS: Data coding/message class specified: %s / %s",
				m->msgid, ((dcs >> 2) & 1) ? "8-bit data" : "Default alphabet",
				messageclasses[dcs & 3]);
			if (dcs >> 2 & 1)
				m->is_binary = 1;
			break;
		}
	}
	
	/* date/time */
	pdu += 2;
	
	sprintf(date, "%c%c/%c%c/%c%c", pdu[1], pdu[0], pdu[3], pdu[2], pdu[5], pdu[4]);
	pdu += 6;
	sprintf(time, "%c%c:%c%c:%c%c", pdu[1], pdu[0], pdu[3], pdu[2], pdu[5], pdu[4]);
	pdu += 8;
	
	/* fill structure */
	m->src = hstrdup(sender);
	m->date = hstrdup(date);
	m->time = hstrdup(time);
	
	/* feed to parser */
	if (m->is_binary)
		return mt_parse_pdu_binary(m, pdu);
	else
		return mt_parse_pdu_ascii(m, pdu);
	
	return 0;
}

/*
 *	Parse a received PDU
 */

int mt_parse_pdu(struct message *m, char *pdu)
{
	int l;
	char *p;
	char buf[60];
	unsigned char pdutype;
	
	/* get sender's SMSC */
	l = octet2bin(pdu) * 2 - 2;
	if (l > 50) {
		hlog(LOG_ERR, "[%s] Ouch, received PDU claims to have a %d-character SMSC address, discarding message!", m->msgid, l);
		return -1;
	}
	if (l > 0) {
		p = pdu + 4;
		strncpy(buf, p, l);
		swapchars(buf);
		if (buf[l-1] == 'F')
			buf[l-1] = 0;
		buf[l] = 0;
		m->smsc = hstrdup(buf);
	}
	
	p = pdu + l + 4;
	
	pdutype = octet2bin(p);
	
	buf[0] = 0;
	if (pdutype & 1 << 2)
		strcat(buf, " MMS");
	
	if (pdutype & 1 << 5)
		strcat(buf, " SRI");
	
	if (pdutype & 1 << 6) {
		strcat(buf, " UDH");
		m->has_udh = 1;
	}
	
	if (pdutype & 1 << 7)
		strcat(buf, " RP");
	
	m->type = pdutype & 3;
	p += 2;
	
	hlog(LOG_DEBUG, "[%s] SMSC %s, type %d, flags:%s", m->msgid, m->smsc, m->type, buf);
	
	if (m->type == 0)	/* sms deliver */
		return mt_split_pdu_deliver(m, p);
	else {
		hlog(LOG_WARNING, "Unsupported message type %d received", m->type);
		return -1;
	}
	
	return 0;
}

/*
 *	Handle a received PDU
 */

char *mt_handle_pdu(char *p, int f)
{
	char *s, *c, *e;
	struct message *m;
	int must_ack = 0;
	char buf[IBLEN];
	char cmd[24];
	
	stats_mt++;
	
	if ((s = strstr(p, "CMGL:"))) {
		if ((c = strstr(s, ": "))) {
			c += 2;
			snprintf(cmd, 20, "AT+CMGD=%s", c);
			if ((e = strchr(cmd, ','))) {
				*e = 0;
				hlog(LOG_DEBUG, "Deleting message %s from SIM", cmd+8);
				issue_cmd_nomt(f, cmd, "delmt");
			} else {
				hlog(LOG_ERR, "Ouch! Received CMGL without a comma after message index! Could not delete!");
			}
		}
	} else if ((s = strstr(p, "CDS:"))) {
	} else if ((s = strstr(p, "CBM:"))) {
		must_ack = 1;
	} else if ((s = strstr(p, "CMT:"))) {
		must_ack = 1;
	} else {
		hlog(LOG_ERR, "Ouch! In mt_handle_pdu and cannot find CMT/CMGL/CBM/CMT in buffer!");
		stats_mt_fail++;
		return p+3;
	}
	
	if ((s = strchr(s, '\n'))) {
		s++;
	} else {
		hlog(LOG_ERR, "Ouch! Received CMT or CMGL without newline after command!");
		stats_mt_fail++;
		return p+3;
	}
	
	if ((e = strchr(s, '\n'))) {
	} else {
		hlog(LOG_ERR, "Ouch! Received CMT or CMGL without newline after PDU!");
		stats_mt_fail++;
		return p+3;
	}
	
	m = alloc_message();
	m->msgid = hstrdup(genmsgid("mt"));
	m->received = time(NULL);
	
	if (must_ack) {
		hlog(LOG_DEBUG, "[%s] Acknowledging received message to terminal", m->msgid);
		issue_cmd_nomt(f, "AT+CNMA", m->msgid);
	}
	
	if (mt_parse_pdu(m, s)) {
		hlog(LOG_ERR, "[%s] MESSAGE MT RESULT:FAILED Failed to parse a MT PDU, message discarded.", m->msgid);
		free_message(m);
		stats_mt_fail_parse++;
		stats_mt_fail++;
		return e+1;
	}
	
	if (m->is_binary) {
		bin2hexstring(m->content, m->len, buf);
		hlog(LOG_NOTICE, "[%s] MESSAGE MT RESULT:OK from %s sent-at %s %s type binary length %d content %s",
			m->msgid, m->src, m->date, m->time, m->len, buf);
	} else {
		ascii2escaped(m->content, m->len, buf, IBLEN);
		hlog(LOG_NOTICE, "[%s] MESSAGE MT RESULT:OK from %s sent-at %s %s type text length %d content \"%s\"",
			m->msgid, m->src, m->date, m->time, m->len, buf);
	}
	
	stats_mt_ok++;
	
	if (fork_handler(m))
		stats_mt_fail_handle++;
		
	free_message(m);
	
	return e + 1;
}

/*
 *	Check if the module responds to AT
 */
 
int ping_module(int f)
{
	char buf[IBLEN];
	int i;
	
	hlog(LOG_DEBUG, "Checking if the module is responding ...");
	if (hwrite(f, "\r\n") < 2)
		return -2;
	if (empty_read_buffer(f, 1))
		return -2;
	if (hwrite(f, "ATE0\r\n") < 6)
		return -2;
	i = readuntil(f, buf, IBLEN, expect_ok, expect_errors, cmd_timeout);
	if (i == -1) {
		hlog(LOG_ERR, "Connection closed after sending ATE0");
		return -2;
	} else if (i == 0) {
		hlog(LOG_ERR, "Module did not respond to ATE0 in %d ms", cmd_timeout);
		return -2;
	}
	if (string_in(buf, expect_ok))
		return 0;
		
	hlog(LOG_ERR, "Module did not respond to ATE0 with an OK");
	return -2;
}

/*
 *	Check if the module needs a PIN code, send it if needed
 */

int send_pin(int f, int reset_if_ready)
{
	char buf[IBLEN];
	int i;
	
	hlog(LOG_DEBUG, "Checking if the module has the PIN code");
	if (hwrite(f, "AT+CPIN?\r\n") < 0)
		return -1;
	readuntil(f, buf, IBLEN, expect_ok, expect_errors, cmd_timeout);
	if (string_in(buf, expect_errors)) {
		hlog(LOG_CRIT, "Module said \"ERROR\" for AT+CPIN?, no SIM?", pin);
		return 1;
	}
	if (!(string_in(buf, expect_ok))) {
		hlog(LOG_CRIT, "Module did not respond with an OK to AT+CPIN?");
		return -2;
	}
	if (strstr(buf, "CPIN: SIM PIN")) {
		hlog(LOG_DEBUG, "Sending SIM PIN");
		fdprintf(f, "AT+CPIN=%s\r\n", pin);
		i = readuntil(f, buf, IBLEN, expect_ok, expect_errors, register_timeout);
		if (string_in(buf, expect_errors)) {
			hlog(LOG_CRIT, "Module said \"ERROR\" for AT+CPIN=%s, wrong PIN?", pin);
			return 1;
		} else if (string_in(buf, expect_ok)) {
			hlog(LOG_INFO, "SIM PIN code inserted");
			return 0;
		} else if (i == -1) {
			hlog(LOG_CRIT, "Module timed out after AT+CPIN=%s, possibly network registration is taking a long time or fails?", pin);
			return -1;
		} else {
			hlog(LOG_CRIT, "Unknown response for AT+CPIN=%s, bug or bad PIN?", pin);
			return 1;
		}
	} else if (strstr(buf, "CPIN: READY")) {
		hlog(LOG_INFO, "Module has the required PIN codes");
		if (reset_if_ready) {
			hlog(LOG_INFO, "Trying to enable network registration with AT+COPS=2, AT+COPS=0");
			if ((i = issue_cmd_nomt(f, "AT+COPS=2", "send_pin")) < 0)
				return i;
			sleep(10);
			if ((i = issue_cmd_nomt(f, "AT+COPS=0", "send_pin")) < 0)
				return i;
			sleep(5);
			hlog(LOG_INFO, "Attempt to enable network registration has been made.");
			return -1;
		}
	} else {
		hlog(LOG_CRIT, "Module is not READY and does not want SIM PIN, maybe wants PUK?");
		return 1;
	}
	return 0;
}	

/*
 *	Issue a command to the module, wait for normal OK or error or timeout
 *	Also handle incoming messages if such appear
 *
 * returns:
 *	0 on OK
 *	-1 on I/O error
 *	-2 on error message received
 *	-3 on timeut
 */

int issue_cmd(int f, char *cmd, char *msgid)
{
 	char buf[IBLEN];
	int i;
	char *p;
	
	fdprintf(f, "%s\r\n", cmd);

rewait:	
	buf[0] = 0;
	i = readuntil(f, buf, IBLEN, expect_ok_or_mt, expect_errors, cmd_timeout);
	if (i > 0) {
		if (string_in(buf, expect_errors)) {
			hlog(LOG_ERR, "[%s] Error response to %s !", msgid, cmd);
			i =  -2;
		} else if (string_in(buf, expect_mt)) {
			hlog(LOG_DEBUG, "[%s] Received MT PDU in response to command %s. Reading the rest and handling.", msgid, cmd);
			p = buf + i;
			i = readuntil(f, p, buf + IBLEN - p, expect_linefeed, expect_errors, cmd_timeout);
			if (i < 0) {
				hlog(LOG_ERR, "I/O error on module while reading MT");
				return -1;
			}
			p += i;
			i = readuntil(f, p, buf + IBLEN - p, expect_linefeed, expect_errors, cmd_timeout);
			if (i < 0) {
				hlog(LOG_ERR, "I/O error on module while reading MT");
				return -1;
			}
			p = buf;
			while ((p = strstr(p, "CMT:")))
				p = mt_handle_pdu(p, f);
			p = buf;
			while ((p = strstr(p, "CBM:")))
				p = mt_handle_pdu(p, f);
			p = buf;
			while ((p = strstr(p, "CDS:")))
				p = mt_handle_pdu(p, f);
			goto rewait;
		}
	} else if (i < 1) {
		hlog(LOG_ERR, "[%s] No OK response to %s: I/O error!", msgid, cmd);
		i = -1;
	} else if (i == 0) {
		hlog(LOG_ERR, "[%s] Timeout for %s !", msgid, cmd);
		i = -3;
	}
	
	return i;
	
}

/*
 *	Wait for network registration
 */

int wait_registration(int f)
{
	char buf[IBLEN];
	int i;
	
	hlog(LOG_DEBUG, "Enabling extended error reporting");
	if ((i = issue_cmd(f, "AT+CMEE=2", "init")) < 0)
		return i;
	
	hlog(LOG_DEBUG, "Enabling unsolicited registration status messages");
	if ((i = issue_cmd(f, "AT+CREG=1", "init")) < 0)
		return i;
	
	hlog(LOG_DEBUG, "Selecting SMS message format: PDU");
	if ((i = issue_cmd(f, "AT+CMGF=0", "init")) < 0)
		return i;
	
	hlog(LOG_DEBUG, "Enabling GSM 07.05 Phase 2+ mode");
	issue_cmd(f, "AT+CSMS=1", "init");
	/*
	if ((i = issue_cmd(f, "AT+CSMS=1", "init")) < 0)
		return i;
	*/
	
	hlog(LOG_DEBUG, "Enabling unsolicited SMS message indications");
	if ((i = issue_cmd(f, "AT+CNMI=1,2,0,0", "init")) < 0)
		return i;
	
	while (1) {
		hlog(LOG_DEBUG, "Checking if module is registered to a network");
		if (hwrite(f, "AT+CREG?\r\n") < 0)
			return -1;
		if (readuntil(f, buf, IBLEN, expect_ok, expect_errors, cmd_timeout) < 1) {
			hlog(LOG_ERR, "No response to AT+CREG? !");
			return -2;
		}
		if (!(string_in(buf, expect_ok))) {
			hlog(LOG_ERR, "No OK response to AT+CREG? !");
			return -2;
		}
		
		if (strstr(buf, "CREG: 1,0")) {
			hlog(LOG_INFO, "Module not trying to register, checking if PIN is needed");
			if (send_pin(f, 1) > 0)
				return 4;
		} else if (strstr(buf, "CREG: 1,2")) {
			hlog(LOG_INFO, "Module is searching for a network to register on");
			state_change(STATE_DOWN_NONETWORK, "Module is searching for a network to register on");
		} else if (strstr(buf, "CREG: 1,1")) {
			hlog(LOG_INFO, "Module registered, home network");
			return 0;
		} else if (strstr(buf, "CREG: 1,5")) {
			hlog(LOG_INFO, "Module registered, roaming");
			return 0;
		} else {
			hlog(LOG_INFO, "Not registered, waiting");
			state_change(STATE_DOWN_NONETWORK, "Module is not registered to a network, waiting");
		}
		
		sleep(5);
	}
}

/*
 *	Encode
 */

int mo_encode_ascii(char *ascii, char *pdu)
{
	char tmp[500];
	char octet[10];
	int pdubitposition;
	int pdubyteposition;
	int asciiLength;
	int character;
	int bit;
	int pdubitnr;
	char converted;
	int ext_characters = 0;
	int counted_characters = 0;
	
	asciiLength = strlen(ascii);
	
	bzero(tmp, sizeof(tmp));
	
	for (character = 0; ((character + ext_characters) < 160) && (character < asciiLength); character++)  {
		if (convert_charset) {
			// Is the character an extended character?
			converted = ext_convert(ascii[character], CS_ISO, CS_SMS);
			if (converted != ' ') {
				counted_characters++;
				// It is an extended character. Insert ESC first.
				for (bit=0; bit < 7; bit++) {
					pdubitnr = 7* (character + ext_characters) + bit;
					pdubyteposition = pdubitnr / 8;
					pdubitposition = pdubitnr % 8;
					if (0x1B & (1 << bit))
						tmp[pdubyteposition] = tmp[pdubyteposition] | (1 << pdubitposition);
					else
						tmp[pdubyteposition] = tmp[pdubyteposition] & ~(1 << pdubitposition);
				}
				ext_characters++;
			} else // Is is a regular character
				converted = convert(ascii[character], CS_ISO, CS_SMS);
		} else
			converted = ascii[character];
		
		counted_characters++;
		for (bit=0; bit < 7; bit++) {
			pdubitnr = 7* (character + ext_characters) + bit;
			pdubyteposition = pdubitnr / 8;
			pdubitposition = pdubitnr % 8;
			if (converted & (1 << bit))
				tmp[pdubyteposition] = tmp[pdubyteposition] | (1 << pdubitposition);
			else
				tmp[pdubyteposition] = tmp[pdubyteposition] & ~(1 << pdubitposition);
		}
		
	}
	
	tmp[pdubyteposition + 1] = 0;
	pdu[0] = 0;
	
	for (character = 0; character <= pdubyteposition; character++) {
		sprintf(octet, "%02X", (unsigned char)tmp[character]);
		strcat(pdu, octet);
	}
	
	return counted_characters;
}

/*
 *	Set up a PDU
 */
int mo_create_pdu(struct message *m, char *pdu)
{
	int coding = 0;
	int len;
	int flags = 1;
	int ton, npi, toa;
	char tmp[53];
	char tmp2[500];
	char *dstp;
	
	hlog(LOG_DEBUG, "[%s] Setting up a PDU", m->msgid);
	
	/* Select TON & NPI (in a not very elegant way) */
	npi = NPI_ISDN;
	dstp = m->dst;
	if (*dstp == '+') {
		/* international format */
		dstp++;
		ton = TON_INTERNATIONAL;
	} else {
		/* non-international format */
		ton = TON_UNKNOWN;
	}
	toa = (1 << 7) | ((ton & 7) << 4) | (npi & 15);
	hlog(LOG_DEBUG, "[%s] Recipient address TON %d (%s) NPI %d (%s) TOA 0x%02X", m->msgid, ton, tons[ton], npi, npis(npi), toa);
	
	strncpy(tmp, dstp, 50);
	tmp[50] = 0;
	
	/* If the length is odd, terminate number with F */
	if (strlen(tmp) % 2)
		strcat(tmp, "F");
	
	swapchars(tmp);
	
	if (m->is_flash) {
		/* FIXME */
	} else {
		if (m->is_binary) {
			coding |=  1 << 2; /* 8-bit */
			if (m->has_udh)
				flags |= 1 << 6; /* user data header */
		}
	}
	
	if (m->request_report)
		flags |= 1 << 5;
	
	flags |= 1 << 4; /* relative format Validity Period present */
	
	if (m->dcs) /* overriding what we set above */
		coding = m->dcs;
	
	if (m->is_binary) {
		bin2hexstring(m->content, m->len, tmp2);
		len = m->len;
	} else
		len = mo_encode_ascii(m->content, tmp2);
	
	hlog(LOG_DEBUG, "[%s] pid %d dcs %d%s", m->msgid, m->pid, coding, (m->has_udh) ? " UDH" : "");
	
	sprintf(pdu, "00%02X00%02X%02X%s%02X%02XAA%02X", flags, (unsigned int)strlen(dstp), toa, tmp, m->pid, coding, (unsigned int)len);
	strcat(pdu, tmp2);
	
	return 0;
}

/*
 *	Send a MO message
 */

int mo_transmit(int f, struct message *m)
{
	char pdu[IBLEN];
	char buf[IBLEN];
	char *p;
	int i;
	int retval;
	
	stats_mo_tries++;
	m->tries++;
	
	if (m->is_binary) {
		bin2hexstring(m->content, m->len, pdu);
		hlog(LOG_NOTICE, "[%s] MESSAGE MO to %s try %d type binary length %d content %s",
			m->msgid, m->dst, m->tries, m->len, pdu);
	} else {
		ascii2escaped(m->content, m->len, pdu, IBLEN);
		hlog(LOG_NOTICE, "[%s] MESSAGE MO to %s try %d type text length %d content \"%s\"",
			m->msgid, m->dst, m->tries, m->len, pdu);
	}
	
	mo_create_pdu(m, pdu);
	
	hlog(LOG_DEBUG, "[%s] Sending PDU length to module", m->msgid);
	fdprintf(f, "AT+CMGS=%d\r\n", strlen(pdu) / 2 - 1);
	
rewait_mo_recnum:
	
	i = readuntil(f, buf, IBLEN, expect_mo_transmit, expect_errors, cmd_timeout);
	if (i > 0) {
		if (string_in(buf, expect_mt)) {
			hlog(LOG_INFO, "[%s] Got MT message in response to AT+CMGS! Reading the rest and handling.", m->msgid);
			p = buf + i;
			i = readuntil(f, p, buf + IBLEN - p, expect_linefeed, expect_errors, cmd_timeout);
			if (i < 0) {
				hlog(LOG_ERR, "I/O error on module while reading MT");
				retval = -1;
				goto ret;
			}
			p += i;
			i = readuntil(f, p, buf + IBLEN - p, expect_linefeed, expect_errors, cmd_timeout);
			if (i < 0) {
				hlog(LOG_ERR, "I/O error on module while reading MT");
				retval = -1;
				goto ret;
			}
			
			
			p = buf;
			while ((p = strstr(p, "CMT:")))
				p = mt_handle_pdu(p, f);
			p = buf;
			while ((p = strstr(p, "CBM:")))
				p = mt_handle_pdu(p, f);
			p = buf;
			while ((p = strstr(p, "CDS:")))
				p = mt_handle_pdu(p, f);
				
			goto rewait_mo_recnum;
		}
		if (string_in(buf, expect_errors)) {
			hlog(LOG_ERR, "[%s] Error response to AT+CMGS!", m->msgid);
			retval = -2;
			goto ret;
		}
	} else if (i < 1) {
		hlog(LOG_ERR, "[%s] No OK response to AT+CMGS: I/O error!", m->msgid);
		retval = -1;
		goto ret;
	} else if (i == 0) {
		hlog(LOG_ERR, "[%s] Timeout for AT+CMGS !", m->msgid);
		retval = -3;
		goto ret;
	}
	
	hlog(LOG_DEBUG, "[%s] Sending PDU to module", m->msgid);
	fdprintf(f, "%s\x1A", pdu);
	
rewait_mo_pdu:

	i = readuntil(f, buf, IBLEN, expect_ok_or_mt, expect_errors, transmit_timeout);
	if (i > 0) {
		if (string_in(buf, expect_mt)) {
			hlog(LOG_INFO, "[%s] Got MT message in response to MO PDU! Reading the rest and handling.", m->msgid);
			p = buf + i;
			i = readuntil(f, p, buf + IBLEN - p, expect_linefeed, expect_errors, cmd_timeout);
			if (i < 0) {
				hlog(LOG_ERR, "I/O error on module while reading MT");
				retval = -1;
				goto ret;
			}
			p += i;
			i = readuntil(f, p, buf + IBLEN - p, expect_linefeed, expect_errors, cmd_timeout);
			if (i < 0) {
				hlog(LOG_ERR, "I/O error on module while reading MT");
				retval = -1;
				goto ret;
			}
			
			p = buf;
			while ((p = strstr(p, "CMT:")))
				p = mt_handle_pdu(p, f);
			p = buf;
			while ((p = strstr(p, "CBM:")))
				p = mt_handle_pdu(p, f);
			p = buf;
			while ((p = strstr(p, "CDS:")))
				p = mt_handle_pdu(p, f);
				
			goto rewait_mo_pdu;
		} else if (string_in(buf, expect_errors)) {
			hlog(LOG_ERR, "[%s] MESSAGE MO RESULT:FAILED time:%d try:%d Error response to AT+CMGS !", m->msgid, time(NULL) - m->received, m->tries);
			retval = -2;
			goto ret;
		} else if (string_in(buf, expect_ok)) {
			hlog(LOG_NOTICE, "[%s] MESSAGE MO RESULT:OK time:%d try:%d", m->msgid, time(NULL) - m->received, m->tries);
			retval = 0;
			goto ret;
		}
		hlog(LOG_ERR, "[%s] MESSAGE MO RESULT:FAILED time:%d try:%d Unknown response to AT+CMGS !", m->msgid, time(NULL) - m->received, m->tries);
		retval = -2;
	} else if (i < 1) {
		hlog(LOG_ERR, "[%s] MESSAGE MO RESULT:FAILED time:%d try:%d No OK response to AT+CMGS: I/O error!", m->msgid, time(NULL) - m->received, m->tries);
		retval = -1;
	} else if (i == 0) {
		hlog(LOG_ERR, "[%s] MESSAGE MO RESULT:FAILED time:%d try:%d Timeout for AT+CMGS !", m->msgid, time(NULL) - m->received, m->tries);
		retval = -3;
	}
	
ret:
	if (retval)
		stats_mo_try_fail++;
	else
		stats_mo_ok++;
	
	return retval;
}

/*
 *	Check for queued messages, send the ones that need to be sent,
 *	return number of messages attempted
 */

int send_retries(int f, struct message *q)
{
	struct message *next;
	int c = 0;
	time_t now;
	
	time(&now);
	while (q) {
		next = q->next; /* store the next item in case we free this one */
		
		if (q->next_try <= now) {
			c++;
			/* attempt delivery */
			state_change(STATE_UP_SENDING_MO, "Sending MO retry for [%s]", q->msgid);
			if (mo_transmit(f, q)) {
				/* failed */
				if (q->tries >= mo_queue_max_tries) {
					/* too many times, drop! */
					hlog(LOG_ERR, "[%s] MESSAGE MO RESULT:DROPPED time:%d try:%d Retry count exceeded!", q->msgid, time(NULL) - q->received, q->tries);
					unqueue_message(q);
					free_message(q);
					stats_mo_dropped++;
				} else {
					/* calculate next retry time */
					q->retry_time *= mo_queue_retry_mult;
					if (q->retry_time > mo_queue_max_retryt)
						q->retry_time = mo_queue_max_retryt;
					q->next_try = time(NULL) + q->retry_time;
					hlog(LOG_DEBUG, "[%s] QUEUE: Retry failed, queuing message for %d seconds", q->msgid, q->retry_time);
				}
			} else {
				/* retry succeed, free it */
				hlog(LOG_DEBUG, "[%s] QUEUE: Retry succeeded, removing from queue", q->msgid);
				unqueue_message(q);
				free_message(q);
			}
		}
		
		q = next;
	}
	
	if (c)
		hlog(LOG_DEBUG, "QUEUE: Attempted delivery of %ld messages in the queue, %ld left.", c, stats_mo_queue_len);
	
	return c;
}

/*
 *	Handle an SMS spool file
 */
 
int handle_spoolfile(int f, char *fn)
{
	FILE *sf;
	struct message *m;
	char s[IBLEN];
	int l, i;
	char bin[IBLEN], *p;
	
	state_change(STATE_UP_SENDING_MO, "Sending MO from %s", fn);
	
	if (!(sf = fopen(fn, "r"))) {
		hlog(LOG_ERR, "Could not open %s for reading: %s", fn, strerror(errno));
		if (unlink(fn))
			hlog(LOG_ERR, "Could not unlink %s: %s", fn, strerror(errno));
		return -1;
	}
	
	m = alloc_message();
	m->msgid = hstrdup(genmsgid("mo"));
	m->received = time(NULL);
	m->spoolfile = hstrdup(fn);
	hlog(LOG_DEBUG, "[%s] Reading MO spool file %s", m->msgid, fn);
	
	while (fgets(s, IBLEN, sf)) {
		while ((p = strchr(s, '\n'))) *p = '\0';
		while ((p = strchr(s, '\r'))) *p = '\0';
		
		if (s[0] == 0)
			break;	/* content starts here */
			
		if (!(p = strchr(s, ':'))) {
			hlog(LOG_ERR, "[%s] %s: Bad header: \"%s\"", m->msgid, fn, s);
			continue;
		}
		*p++ = 0;
		while ((*p) && (*p == ' ' || *p == '\t'))
			p++;
			
		if (!strcasecmp(s, "To")) {
			m->dst = hstrdup(p);
		} else if (!strcasecmp(s, "Is-binary")) {
			m->is_binary = atoi(p);
		} else if (!strcasecmp(s, "Has-UDH")) {
			m->has_udh = atoi(p);
		} else if (!strcasecmp(s, "TP-PID")) {
			m->pid = atoi(p);
		} else if (!strcasecmp(s, "TP-DCS")) {
			m->dcs = atoi(p);
		} else if (!strcasecmp(s, "Message-id")) {
			hlog(LOG_DEBUG, "[%s] New message-id: [%s]", m->msgid, p);
			hfree(m->msgid);
			m->msgid = hstrdup(p);
		} else {
			hlog(LOG_WARNING, "[%s] %s: Ignoring unsupported header: \"%s\"", m->msgid, fn, s);
		}
	}
	l = fread(s, 1, IBLEN-1, sf);
	if (l >= 0)
		s[l] = 0;
	
	if (m->is_binary) {
		while ((p = strchr(s, '\n'))) *p = '\0';
		while ((p = strchr(s, '\r'))) *p = '\0';
		l = strlen(s);
		if (l % 2)
			hlog(LOG_ERR, "[%s] %s: Hex-encoded binary content length is odd! Losing one nybble.", m->msgid, fn);
		m->len = l / 2;
		/* convert from hex to binary */
		for (i  = 0; i < m->len; i++)
			bin[i] = octet2bin(&s[i*2]);
		m->content = hmalloc(m->len);
		memcpy(m->content, bin, m->len);
	} else {
		m->content = strdup(s);
		m->len = strlen(m->content);
	}
	
	if (fclose(sf))
		hlog(LOG_ERR, "[%s] Could not close %s after reading: %s", m->msgid, fn, strerror(errno));
	
	stats_mo++;
	
	if (mo_transmit(f, m)) {
		m->retry_time = mo_queue_init_retryt;
		m->next_try = time(NULL) + m->retry_time;
		hlog(LOG_DEBUG, "[%s] QUEUE: First try failed, queuing message for %d seconds", m->msgid, m->retry_time);
		queue_message(m);
	} else
		free_message(m);
	
	if (unlink(fn))
		hlog(LOG_ERR, "[%s] Could not unlink %s: %s", m->msgid, fn, strerror(errno));
	
	return 0;
}

/*
 *	Match .sms files for scandir()
 */

int select_spoolf(char *fn)
{
	int l = strlen(fn);
	
	if ((l >= 4) && (!strcmp(fn + (l-4), ".sms")))
		return 1;
		
	return 0;
}

/*
 *	Check input SMS spool
 */

int check_spool(int f)
{
	int c = 0;
	char *s;
	DIR *d;
	struct dirent *de;
	struct stat sb;
#ifdef DISABLE_UNSOL_WHILE_SENDING_MO /* this didn't help - this made things worse */
	int polling = 1;
#endif
	
	if (!(d = opendir(spool_dir))) {
		hlog(LOG_ERR, "Could not open directory %s: %s", spool_dir, strerror(errno));
		return -1;
	}
	
	errno = 0;
	while ((de = readdir(d)))
		if (select_spoolf(de->d_name)) {
			c++;
			hlog(LOG_INFO, "Found SMS spool file: %s", de->d_name);
			s = hmalloc(strlen(spool_dir) + 2 + strlen(de->d_name));
			sprintf(s, "%s/%s", spool_dir, de->d_name);
			if (stat(s, &sb)) {
				hlog(LOG_ERR, "Could not stat %s: %s - Deleting!", s, strerror(errno));
				if (unlink(s))
					hlog(LOG_ERR, "Could not unlink spool file %s: %s", s, strerror(errno));
			} else {
				if (S_ISREG(sb.st_mode)) {
#ifdef DISABLE_UNSOL_WHILE_SENDING_MO
					if (polling) {
						polling = 0;
						hlog(LOG_DEBUG, "Disabling unsolicited SMS message indications");
						issue_cmd(f, "AT+CNMI=0,0,0,0", "mofeed");
					}
#endif
					handle_spoolfile(f, s);
					hfree(s);
					break;
				} else {
					hlog(LOG_ERR, "Spool file %s: Is not a regular file! Deleting!", s, strerror(errno));
					if (unlink(s))
						hlog(LOG_ERR, "Could not unlink spool file %s: %s", s, strerror(errno));
				}
			}
			hfree(s);
		}
	
#ifdef DISABLE_UNSOL_WHILE_SENDING_MO
	if (!polling) {
		hlog(LOG_DEBUG, "Enabling unsolicited SMS message indications");
		issue_cmd(f, "AT+CNMI=1,2,0,0", "mofeed");
	}
#endif
	
	if (errno)
		hlog(LOG_ERR, "Error while reading directory %s: %s", spool_dir, strerror(errno));
		
	if (closedir(d))
		hlog(LOG_ERR, "Could not close directory %s: %s", spool_dir, strerror(errno));
	
	return c;
}

/*
 *	return (a hmalloc'd copy of) the next non-whitespace string
 */

char *next_key(char **where)
{
	int len;
	char *start, *new;
	
	while (**where && (**where == ' ' || **where == '\t'))
		*where = *where + 1;
	if (!**where)
		return NULL;
	start = *where;
	while (**where && **where != ' ' && **where != '\t')
		*where = *where + 1;
	len = *where - start;
	new = hmalloc(len+1);
	strncpy(new, start, len);
	new[len] = 0;
	return new;
}

/*
 *	Poll signal strength from module
 */

int poll_signal(int f)
{
	char buf[IBLEN];
	char *p, *e;
	char *chan, *rs, *dbm, *plmn, *lac, *cell, *rxlev;
	
#ifndef DISABLED_FOR_SOME_REASON
	if (net_status) {
		hfree(net_status);
		net_status = NULL;
	}
	
	if (hwrite(f, "AT^MONI\r\n") < 1) {
		hlog(LOG_ERR, "I/O error on module, reconnecting");
		return -2;
	}
	
	if (readuntil(f, buf, IBLEN, expect_ok, expect_errors, cmd_timeout) < 1) {
		hlog(LOG_ERR, "No response to AT^MONI, reconnecting");
		return -2;
	}
	
	if (string_in(buf, expect_errors)) {
		hlog(LOG_ERR, "Module responded with an ERROR for AT^MONI");
		if (wait_registration(f)) {
			state_change(STATE_DOWN_FAILQUIT, "Fatal error while checking for network status, giving up");
			return -1;
		}
	} else {
		/* check for queued or unsolicited messages in buffer */
		p = buf;
		while ((p = strstr(p, "CMT:")))
			p = mt_handle_pdu(p, f);
		p = buf;
		while ((p = strstr(p, "CBM:")))
			p = mt_handle_pdu(p, f);
		p = buf;
		while ((p = strstr(p, "CDS:")))
			p = mt_handle_pdu(p, f);
		
		if ((
		    (p = strstr(buf, "\nchann rs  dBm  PLMN  LAC cell NCC BCC PWR RXLev  C1 "))
		 || (p = strstr(buf, "\nchann rs  dBm  PLMN   LAI cell NCC BCC PWR RXlev C1 "))
		    ) && (p = strchr(++p, '\n'))) {
			p++;
			if ((e = strchr(p, '\n'))) {
				*e = 0;
				if (strlen(p) < 52) {
					//hlog(LOG_DEBUG, "MONI: Not connected to a network");
				} else {
					chan = rs = dbm = plmn = lac = cell = 0;
					//hlog(LOG_DEBUG, "MONI: %s", p);
					chan = next_key(&p);
					rs = next_key(&p);
					dbm = next_key(&p);
					plmn = next_key(&p);
					lac = next_key(&p);
					cell = next_key(&p);
					hfree(next_key(&p));
					hfree(next_key(&p));
					hfree(next_key(&p));
					rxlev = next_key(&p);
					/*hlog(LOG_DEBUG, "MONI: chan %s rs %s dbm %s plmn %s lac %s cell %s rxlev %s",
						chan, rs, dbm, plmn, lac, cell, rxlev);
					*/
					if (chan && rs && dbm && plmn && lac && cell && rxlev) {
						net_status = str_append(net_status, "channel:%s PLMN:%s LAC:%s cell:%s rs:%s/63 dBm:%s/%s",
							chan, plmn, lac, cell, rs, dbm, rxlev);
					}
					if (chan) hfree(chan);
					if (rs) hfree(rs);
					if (dbm) hfree(dbm);
					if (plmn) hfree(plmn);
					if (lac) hfree(lac);
					if (cell) hfree(cell);
					if (rxlev) hfree(rxlev);
				}
			}
		}
	}
	
	if (!net_status)
		return -3;
		
	if (hwrite(f, "AT+COPS?\r\n") < 1) {
		hlog(LOG_ERR, "I/O error on module, reconnecting");
		return -2;
	}
	
	if (readuntil(f, buf, IBLEN, expect_ok, expect_errors, cmd_timeout) < 1) {
		hlog(LOG_ERR, "No response to AT+COPS?, reconnecting");
		return -2;
	}
	
	if (string_in(buf, expect_errors)) {
		hlog(LOG_ERR, "Module responded with an ERROR for AT+COPS?");
		if (wait_registration(f)) {
			state_change(STATE_DOWN_FAILQUIT, "Fatal error while checking for GSM network selection, giving up");
			return -1;
		}
	} else {
		/* check for queued or unsolicited messages in buffer */
		p = buf;
		while ((p = strstr(p, "CMT:")))
			p = mt_handle_pdu(p, f);
		p = buf;
		while ((p = strstr(p, "CBM:")))
			p = mt_handle_pdu(p, f);
		p = buf;
		while ((p = strstr(p, "CDS:")))
			p = mt_handle_pdu(p, f);
		
		if ((p = strstr(buf, "COPS: "))
			&& (p = strchr(p, '"'))
			&& (e = strchr(++p, '"'))) {
			*e = 0;
			net_status = str_append(net_status, " Operator:\"%s\"", p);
		}
	}
#endif
	return 0;
}

/*
 *	Main
 */

int main(int argc, char **argv)
{
	int f = -1, i;
	char buf[IBLEN];
	char *p;
	time_t t, next_poll = 0;
	
	close(0);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGALRM, &sig_handler);
	signal(SIGINT, &sig_handler);
	signal(SIGTERM, &sig_handler);
	signal(SIGQUIT, &sig_handler);
	signal(SIGHUP, &sig_handler);
	signal(SIGUSR1, &sig_handler);
	signal(SIGUSR2, &sig_handler);
	signal(SIGPIPE, SIG_IGN);
	
	parse_cmdline(argc, argv);
	
	open_log(logname);
	state_change(STATE_DOWN_INIT, PROGNAME " " VERSION " starting up ...");
	
	if (fork_a_daemon) {
		i = fork();
		if (i < 0) {
			hlog(LOG_CRIT, "Fork to background failed: %s", strerror(errno));
			fprintf(stderr, "Fork to background failed: %s\n", strerror(errno));
			return 1;
		} else if (i == 0) {
			/* child */
		} else {
			/* parent, quitting */
			hlog(LOG_DEBUG, "Forked daemon process %d, parent quitting", i);
			return 0;
		}
	}
	
	if (pidfile)
		writepid(pidfile);
		
	hlog(LOG_NOTICE, PROGNAME " " VERSION " starting up ...");
	
	while (!shutting_down) {
		if (f >= 0) {
			close(f);
			f = -1;
			sleep(1);
		}
		state_change(STATE_DOWN_CONNECTING, "Connecting to module at %s", device);
		f = open_device(device);
		if (f == -1) {
			state_change(STATE_DOWN_FAILQUIT, "Fatal error while opening device %s, giving up", device);
			hlog(LOG_CRIT, "Fatal error while opening device %s, giving up", device);
			return 2;
		}
		if (f == -2) {
			hlog(LOG_INFO, "Temporary error while opening device, sleeping %d seconds", retry_sleep);
			sleep(retry_sleep);
			continue;
		}
		
		state_change(STATE_DOWN_HANDSHAKING, "Connecting, initializing module");
		hlog(LOG_INFO, "Connected, initializing module ...");
		if (ping_module(f)) {
			close(f);
			f = -1;
			if (host) {
				state_change(STATE_DOWN_RETRYSLEEP, "Module did not respond to ATE=0 after connecting, sleeping %d seconds before retry", retry_sleep);
				hlog(LOG_INFO, "Sleeping %d seconds before retry", retry_sleep);
				sleep(retry_sleep);
				continue;
			} else {
				state_change(STATE_DOWN_FAILQUIT, "Could not talk with a serial device connected module, giving up");
				hlog(LOG_CRIT, "Could not talk with a serial device connected module, giving up");
				return 3;
			}
		}
		
		i = send_pin(f, 0);
		if (i > 0) {
			close(f);
			f = -1;
			state_change(STATE_DOWN_FAILQUIT, "Fatal error while sending PIN code, giving up");
			hlog(LOG_CRIT, "Fatal error while sending PIN code, giving up");
			return 4;
		}
		if (i < 0) {
			close(f);
			f = -1;
			state_change(STATE_DOWN_RETRYSLEEP, "Non-fatal error while sending PIN, sleeping %d seconds before retry", retry_sleep);
			hlog(LOG_INFO, "Non-fatal error while sending PIN, sleeping %d seconds before retry", retry_sleep);
			sleep(retry_sleep);
			continue;
		}
		
		i = wait_registration(f);
		if (i > 0) {
			close(f);
			f = -1;
			state_change(STATE_DOWN_FAILQUIT, "Fatal error while waiting for registration, giving up");
			hlog(LOG_CRIT, "Fatal error while waiting for registration, giving up");
			return 5;
		}
		if (i < 0) {
			state_change(STATE_DOWN_RETRYSLEEP, "Non-fatal error while waiting for registration, sleeping %d seconds before retry", retry_sleep);
			hlog(LOG_INFO, "Non-fatal error while waiting for registration, sleeping %d seconds before retry", retry_sleep);
			close(f);
			f = -1;
			sleep(retry_sleep);
			continue;
		}
		
		state_change(STATE_UP_SLEEPING, "Connected, entering operational mode");
		hlog(LOG_NOTICE, PROGNAME " " VERSION " connected, entering operational mode.");
		
		while (!shutting_down) {
			if (running_state >= STATE_UP) {
				/* if there are any retries to send, do so */
				send_retries(f, mo_queue);
				
				/* check for MO, and poll immediately if MO was sent */
				if (check_spool(f))
					next_poll = 0;
			}
			
			time(&t);
			if (t > next_poll) {
				if (running_state >= STATE_UP)
					state_change(STATE_UP_POLLING, "Polling module");
				/* check for MT */
				next_poll = t + poll_time;
				hlog(LOG_DEBUG, "Polling module");
				
				/* poll the device for queued messages every poll_time */
				if (hwrite(f, "AT+CMGL=4\r\n") < 1) {
					hlog(LOG_ERR, "I/O error on module, reconnecting");
					break;
				}
				
				if (readuntil(f, buf, IBLEN, expect_ok, expect_errors, cmd_timeout) < 1) {
					hlog(LOG_ERR, "No response to AT+CMGL, reconnecting");
					break;
				}
				if (string_in(buf, expect_errors)) {
					hlog(LOG_ERR, "Module responded with an ERROR for AT+CMGL, sleeping 20s and checking registration");
					sleep(20);
					i = wait_registration(f);
					if (i == 4) {
						state_change(STATE_DOWN_FAILQUIT, "Fatal error while checking for registration, giving up");
						return 5;
					} else if (i) {
						state_change(STATE_DOWN_RETRYSLEEP, "Error while checking for registration, reconnecting");
						break;
					}
				} else {
					/* check for queued or unsolicited messages in buffer */
					p = buf;
					while ((p = strstr(p, "CMT:")))
						p = mt_handle_pdu(p, f);
					p = buf;
					while ((p = strstr(p, "CMGL:")))
						p = mt_handle_pdu(p, f);
					if (strstr(buf, "CMGL:")) {
						/* If we got any messages using CMGL, we might not be receiving
						 * unsolicited messages any more. Ack just to be sure.
						 */
						hwrite(f, "AT+CNMA=1\r\n");
						readuntil(f, buf, IBLEN, expect_ok, expect_errors, cmd_timeout);
						/*
						hlog(LOG_DEBUG, "Enabling unsolicited SMS message indications");
						if ((i = issue_cmd(f, "AT+CNMI=1,2,0,0", "reinit")) < 0)
							break;
						*/
					}
				}
#ifndef DISABLED_FOR_SOME_REASON
				if (poll_signal(f) == 0) {
					if ((i = issue_cmd(f, "AT+CNMI=1,2,0,0", "poll")) < 0) {
						hlog(LOG_ERR, "Could not enable unsolicited SMS message indications");
						break;
					}
					state_change(STATE_UP_SLEEPING, "Waiting for something to happen");
				} else
					state_change(STATE_DOWN_NONETWORK, "No GSM network connection");
#endif
				state_change(STATE_UP_SLEEPING, "Waiting for something to happen");
			}
			
			i = readuntil(f, buf, IBLEN, expect_mt, expect_errors, 500);
			if (i > 0) {
				p = buf + i;
				i = readuntil(f, p, buf + IBLEN - p, expect_linefeed, expect_errors, cmd_timeout);
				if (i < 0) {
					hlog(LOG_ERR, "I/O error on module, reconnecting");
					break;
				}
				p += i;
				i = readuntil(f, p, buf + IBLEN - p, expect_linefeed, expect_errors, cmd_timeout);
				if (i < 0) {
					hlog(LOG_ERR, "I/O error on module, reconnecting");
					break;
				}
				p = buf;
				while ((p = strstr(p, "CMT:")))
					p = mt_handle_pdu(p, f);
				p = buf;
				while ((p = strstr(p, "CBM:")))
					p = mt_handle_pdu(p, f);
				p = buf;
				while ((p = strstr(p, "CDS:")))
					p = mt_handle_pdu(p, f);
			} else if (i < 0) {
				hlog(LOG_ERR, "I/O error on module, reconnecting");
				break;
			}
		}
	}
	
	/* shutting down */
	state_change(STATE_DOWN_SHUTTINGDOWN, "Shutting down");
	if (f >= 0) {
		hlog(LOG_DEBUG, "Disabling unsolicited SMS message indications");
		issue_cmd(f, "AT+CNMI=0,0,0,0", "shutdown");
	}
	
	close(f);
	f = -1;
	
	log_stats();
	
	if (stats_mo_queue_len)
		hlog(LOG_ERR, "Lost %d queued messages!", stats_mo_queue_len);
	hlog(LOG_CRIT, "Shut down.");
	
	state_change(STATE_DOWN_SHUTDOWN, "Shut down.");
	return 0;
}

