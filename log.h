
#ifndef LOG_H
#define LOG_H

#define LOG_LEN	2048

#define L_STDERR        1  /* Log to stderror */
#define L_SYSLOG        2  /* Log to syslog */

#define L_DEFDEST	(L_STDERR)

#define LOG_LEVELS "emerg alert crit err warning notice info debug"
#define LOG_DESTS "syslog stderr"

#include <syslog.h>

extern char *log_levelnames[];
extern char *log_destnames[];

extern int log_dest;     /* Logging destination */
extern int log_level;	 /* Logging level */

extern char *str_append(char *s, const char *fmt, ...);

extern int pick_loglevel(char *s, char **names);
extern int open_log(char *name);
extern int hlog(int priority, const char *fmt, ...);

extern int writepid(char *name);

#endif
