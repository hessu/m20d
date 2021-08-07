
/*
 *	message.c
 *
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

#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#include "message.h"
#include "hmalloc.h"
#include "log.h"
#include "charset.h"

struct message *mo_queue = NULL;	/* Outbound message queue */

long stats_mo_queue_len = 0;	/* MO: gauge: message queue length */
long stats_mo_queued = 0;	/* MO: messages queued */

int msgid_counter = 0;		/* running counter for message IDs, wraps after 99 */

int convert_charset = 1;	/* SMS <=> ISO-Lation-1 ASCII charset conversion */

char *tons[] = { "Unknown", "International", "National", "Network specific", "Subscriber", "Alphanumeric", "Abbreviated", "Reserved", NULL };
char *alphabets[] = { "Default", "8-bit data", "16-bit UCS2", "Reserved", NULL };
char *messageclasses[] = { "Alert", "ME-specific", "SIM-specific", "TE-specific", NULL };
char *messagewaitclasses[] = { "Voicemail", "Fax", "Electronic Mail", "Other", NULL };

/*
 *	Allocate & free a message structure
 */

struct message *alloc_message(void)
{
	struct message *m;
	
	m = hmalloc(sizeof(*m));
	bzero(m, sizeof(*m));
	
	return m;
}

void free_message(struct message *m)
{
	if (m->msgid)
		hfree(m->msgid);
	if (m->smsc)
		hfree(m->smsc);
	if (m->date)
		hfree(m->date);
	if (m->time)
		hfree(m->time);
	if (m->src)
		hfree(m->src);
	if (m->dst)
		hfree(m->dst);
	if (m->content)
		hfree(m->content);
	if (m->spoolfile)
		hfree(m->spoolfile);
	hfree(m);
}

/*
 *	Queue and unqueue a message
 */

void queue_message(struct message *m)
{
	if ((m->next) || (m->prevp))
		hlog(LOG_CRIT, "queue_message() called on an already-queued message! BUG!");
		
	m->next = mo_queue;
	if (m->next)
		m->next->prevp = &m->next;
	mo_queue = m;
	m->prevp = &mo_queue;
	
	stats_mo_queued++;
	stats_mo_queue_len++;
}

void unqueue_message(struct message *m)
{
	if (!m->prevp)
		hlog(LOG_CRIT, "unqueue_message() called on a non-queued message, no m->prevp! BUG!");
	else
		*m->prevp = m->next;
		
	if (m->next)
		m->next->prevp = m->prevp;
		
	stats_mo_queue_len--;
}

/*
 *	Return a string representation of a NPI
 */

char *npis(int npi)
{
	static char npi_unknown[] = "Unknown";
	static char npi_isdn[] = "ISDN/Telephone";
	static char npi_data[] = "Data/X.121";
	static char npi_telex[] = "Telex";
	static char npi_national[] = "National";
	static char npi_private[] = "Private";
	static char npi_ermes[] = "Ermes";
	static char npi_reserved[] = "Reserved";
	
	switch (npi) {
	case NPI_UNKNOWN:
		return npi_unknown;
	case NPI_ISDN:
		return npi_isdn;
	case NPI_DATA:
		return npi_data;
	case NPI_TELEX:
		return npi_telex;
	case NPI_NATIONAL:
		return npi_national;
	case NPI_PRIVATE:
		return npi_private;
	case NPI_ERMES:
		return npi_ermes;
	default:
		return npi_reserved;
	}
}

/*
 *	Convert an hex string octet value to 8-bit binary value
 */

int octet2bin(char *octet) /* converts an ASCII octet to a 8-Bit value */
{
	int result = 0;
	
	if (octet[0] > 57)
		result += octet[0] - 55;
	else
		result += octet[0] - 48;
	
	result = result << 4;
	
	if (octet[1] > 57)
		result += octet[1] - 55;
	else
		result += octet[1] - 48;
	
	return result;
}

/*
 *	Convert a binary PDU to hex format
 */

void bin2hexstring(char *binary, int length, char *pdu)
{
	int i;
	
	if (length > 140)
		length = 140;
	
	pdu[0] = 0;
	
	for (i = 0; i < length; i++)
		pdu += sprintf(pdu, "%02X", (unsigned char)binary[i]);
}

/*
 *	Convert a hex string to binary
 */

int hexstring2bin(char *src, int len, char *dst, int dstlen)
{
	int i;
	
	for (i = 0; i < len && i < dstlen; i++)
		dst[i] = octet2bin(src + (i << 1));
		
	return i;
}

/*
 *	Convert GSM TS 03.38 7-bit default alphabet encoding to ASCII
 */
 
int binary2ascii(char *bin, int binlen, char *ascii, int dstlen, int stopatnull)
{
	int l;
	unsigned char c;
	int bit, bitpos = 0;
	int bytepos, byteofs;
	int extended = 0, ext_characters = 0;
	
	for (l = 0; l < binlen && l < dstlen; l++) {
		c = 0;
		for (bit = 0; bit < 7; bit++) {
			bytepos = bitpos / 8;
			byteofs = bitpos % 8;
			if (bin[bytepos] & (1 << byteofs))
				c = c | 128;
			bitpos++;
			c = (c >> 1) & 127;
		}
		if (c == 0 && stopatnull) {
			ascii[l-ext_characters] = 0;
			break;
		}
		if (convert_charset) {
			/* SMS => ISO-Latin-1 character conversion */
			if (extended) {
				/* second byte of an extended character */
				ascii[l-ext_characters] = ext_convert(c, CS_SMS, CS_ISO);
				extended = 0;
			} else {
				ascii[l-ext_characters] = convert(c, CS_SMS, CS_ISO);
				// If this is an ESC character, then an extended character code follows
				if (ascii[l-ext_characters] == 0x1B) { 
					ext_characters++;
					extended = 1;
				}
			}
		} else if (c == 0)
			ascii[l-ext_characters] = 183;
		else
			ascii[l-ext_characters] = c;
	}
	
	ascii[l] = 0;
	
	return l;
}

/*
 *	Escape non-printable characters in a string
 */

int ascii2escaped(char *src, int len, char *dst, int dstlen)
{
	int i;
	char *p = dst;
	
	for (i = 0; i < len && p < dst + dstlen; i++) {
		if ((src[i] >= 0 && src[i] <= 6) || (src[i] >= 14 && src[i] <= 31) || src[i] == 127) {
			*p++ = '\\';
			p += sprintf(p, "\\%02X", src[i]);
		} else {
			switch (src[i]) {
			case '\a':
				*p++ = 'a';
				break;
			case '\b':
				*p++ = '\\';
				*p++ = 'b';
				break;
			case '\t':
				*p++ = '\\';
				*p++ = 't';
				break;
			case '\n':
				*p++ = '\\';
				*p++ = 'n';
				break;
			case '\v':
				*p++ = '\\';
				*p++ = 'v';
				break;
			case '\f':
				*p++ = '\\';
				*p++ = 'f';
				break;
			case '\r':
				*p++ = '\\';
				*p++ = 'r';
				break;
			case '\\':
				*p++ = '\\';
				*p++ = '\\';
				break;
			case '"':
				*p++ = '\\';
				*p++ = '"';
				break;
			default:
				*p++ = src[i];
			}
		}
	}
	
	*p++ = 0;
	dst[dstlen-1] = 0;
	
	return i;
}

/*
 *	Swap every second character
 */

void swapchars(char *string)
{
	int l;
	int pos;
	char c;
	
	l = strlen(string);
	for (pos = 0; pos < l-1; pos += 2) {
		c = string[pos];
		string[pos] = string[pos + 1];
		string[pos + 1] = c;
	}
}

/*
 *	Encode an integer to a character set
 *	returns a pointer to the last character inserted to dest
 */

char *msgid_encode(char *dest, int maxlen, long long i)
{
	char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	long long setlen = strlen(charset);
	long long d;
	int c = 0;
	
	while (i > setlen && c < maxlen - 2) {
		d = i % setlen;
		*dest = charset[d];
		i = i / setlen;
		dest++;
		c++;
	}
	*dest = charset[i];
	*(dest + 1) = 0;
	
	return dest;
}

/*
 *	Generate a short but unique message ID
 */

char *genmsgid(char *prefix)
{
	static char s[50], r[50], *p, *rp;
	struct timeval tv;
	unsigned long long li;
	
	if (gettimeofday(&tv, NULL)) {
		hlog(LOG_CRIT, "gettimeofday() failed: %s", strerror(errno));
		exit(10);
	}
	
	li = (long long)tv.tv_sec * (long long)1000
	   + (long long)tv.tv_usec / (long long)1000;
	
	p = msgid_encode(s, 50, li);
	
	strncpy(r, prefix, 50);
	rp = r + strlen(prefix);
	
	while (p >= s) {
		*rp++ = *p;
		p--;
	}
	rp += snprintf(rp, 3, "%02d", msgid_counter);
	*rp++ = 0;
	
	msgid_counter++;
	msgid_counter = msgid_counter % 100;
	
	return r;
}

