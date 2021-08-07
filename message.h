
#ifndef MESSAGE_H
#define MESSAGE_H

#include <time.h>

struct message {
	char *msgid;		/* Message identifier */
	time_t received;	/* Received for processing by daemon */
	char *smsc;		/* smsc address supplied in the PDU for MT */
	int type;		/* PDU type */
	int pid;		/* Protocol identifier */
	int dcs;		/* Data coding scheme */
	int is_binary;		/* Content represented as binary */
	int has_udh;		/* "Has User Data Header" bit in the header */
	int is_flash;		/* Is a flash message */
	int request_report;	/* Message requests delivery report */
	char *date;		/* MT: Date (received by SMSC) */
	char *time;		/* MT: Time (received by SMSC) */
	char *src;		/* Source address (only on MT) */
	char *dst;		/* Destination address (only on MO) */
	char *content;		/* content */
	int len;		/* length of content */
	
	char *spoolfile;	/* file name in spool */
	int tries;		/* number of sending attempts made */
	int retry_time;		/* current retry delay */
	time_t next_try;	/* time of next attempt */
	
	struct message *next;	/* message queue: next message */
	struct message **prevp;	/* message queue: location of *next in the previous message */
};

#define TON_UNKNOWN		0
#define TON_INTERNATIONAL	1
#define TON_NATIONAL		2
#define TON_NETWORKSPECIFIC	3
#define TON_SUBSCRIBER		4
#define TON_ALPHANUMERIC	5
#define TON_ABBREVIATED		6
#define TON_RESERVED		7

#define NPI_UNKNOWN		0
#define NPI_ISDN		1
#define NPI_DATA		3
#define NPI_TELEX		4
#define NPI_NATIONAL		8
#define NPI_PRIVATE		9
#define NPI_ERMES		10


extern char *tons[];
extern char *alphabets[];
extern char *messageclasses[];
extern char *messagewaitclasses[];

extern struct message *mo_queue;	/* Outbound message queue */

extern long stats_mo_queue_len;		/* MO: gauge: message queue length */
extern long stats_mo_queued;		/* MO: messages queued */

extern int convert_charset;		/* SMS <=> ISO-Lation-1 ASCII charset conversion */


extern struct message *alloc_message(void);
extern void free_message(struct message *m);
extern void queue_message(struct message *m);
extern void unqueue_message(struct message *m);
extern char *npis(int npi); /* Return a string representation of a NPI */
extern int octet2bin(char *octet); /* Convert an hex string octet value to 8-bit binary value */
extern void bin2hexstring(char *binary, int length, char *pdu);
extern int hexstring2bin(char *src, int len, char *dst, int dstlen);
extern int binary2ascii(char *bin, int binlen, char *ascii, int dstlen, int stopatnull);
extern int ascii2escaped(char *src, int len, char *dst, int dstlen);
extern void swapchars(char *string);
extern char *genmsgid(char *prefix);

#endif

