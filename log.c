
/*
 *	log.c
 *
 *	logging facility with configurable log levels and
 *	logging destinations
 */

#include <stdio.h>
#include <stdarg.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>

#include "log.h"
#include "hmalloc.h"

int log_dest = L_DEFDEST;	/* Logging destination */
int log_level = LOG_INFO;	/* Logging level */
int log_facility = LOG_LOCAL1;	/* Logging facility */
char *log_name = NULL;		/* Logging name */

char *log_levelnames[] = {
	"EMERG",
	"ALERT",
	"CRIT",
	"ERR",
	"WARNING",
	"NOTICE",
	"INFO",
	"DEBUG",
	NULL
};

char *log_destnames[] = {
	"none",
	"stderr",
	"syslog",
	NULL
};

/*
 *	Append a formatted string to a dynamically allocated string
 */

char *str_append(char *s, const char *fmt, ...)
{
	va_list args;
	char buf[LOG_LEN];
	int len;
	char *ret;
	
	va_start(args, fmt);
	vsnprintf(buf, LOG_LEN, fmt, args);
	va_end(args);
	buf[LOG_LEN-1] = 0;
	
	if (s)
		len = strlen(s);
	else
		len = 0;
	ret = hrealloc(s, len + strlen(buf) + 1);
	strcpy(ret + len, buf);
	
	return ret;
}

/*
 *	Pick a log level
 */

int pick_loglevel(char *s, char **names)
{
	int i;
	
	for (i = 0; (names[i]); i++)
		if (!strcasecmp(s, names[i]))
			return i;
			
	return -1;
}

/*
 *	Open log
 */
 
int open_log(char *name)
{
	if (log_name)
		hfree(log_name);
		
	if (!(log_name = hstrdup(name))) {
		fprintf(stderr, "m20d logger: out of memory!\n");
		exit(1);
	}
	
	if (log_dest & L_SYSLOG)
		openlog(name, LOG_NDELAY|LOG_PID, log_facility);
		
	return 0;
}

/*
 *	Log a message
 */

int hlog(int priority, const char *fmt, ...)
{
	va_list args;
	char s[LOG_LEN];
	time_t t;
	struct tm *lt;
	
	if (priority > 7)
		priority = 7;
	else if (priority < 0)
		priority = 0;
	
	if (priority > log_level)
		return 0;
	
	va_start(args, fmt);
	vsnprintf(s, LOG_LEN, fmt, args);
	va_end(args);
	
	if (log_dest & L_STDERR) {
		time(&t);
		lt = localtime(&t);
		fprintf(stderr, "%4.4d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d %s[%d] %s: %s\n",
			lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec,
			(log_name) ? log_name : "m20d", (int)getpid(), log_levelnames[priority], s);
	}
	if (log_dest & L_SYSLOG)
		syslog(priority, "%s: %s", log_levelnames[priority], s);
	
	return 1;
}

/*
 *	Write my PID to file
 */

int writepid(char *name)
{
	FILE *f;
	
	if (!(f = fopen(name, "w"))) {
		hlog(LOG_ERR, "Could not open %s for writing: %s",
			name, strerror(errno));
		return -1;
	}
	if (fprintf(f, "%ld\n", (long)getpid()) < 0) {
		hlog(LOG_ERR, "Could not write to %s: %s",
			name, strerror(errno));
		fclose(f);
		return -1;
	}
	
	if (fclose(f)) {
		hlog(LOG_ERR, "Could not close %s: %s",
			name, strerror(errno));
		return -1;
	}
	return 0;
}

