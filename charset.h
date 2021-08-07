/*
SMS Server Tools
   
Copyright (C) Stefan Frings 2002

This program is free software unless you got it under another license directly
from the author. You can redistribute it and/or modify it under the terms of 
the GNU General Public License as published by the Free Software Foundation.
Either version 2 of the License, or (at your option) any later version.

http://www.isis.de/members/~s.frings
mailto:s.frings@mail.isis.de
*/

#ifndef CHARSET_H
#define CHARSET_H

#define CS_ISO 0   // ISO 8859-1 (default Unix and Windows character set)
#define CS_SMS 1   // for SM using PDU messages GSM 3.38
#define CS_MO  2   // for mobile originated SM using UCP messages (IA5 alphabet)
#define CS_MT  3   // for mobile terminated SM using UCP messages (IA5 alphabet)

// Converts the character c from one of the above character sets to another character set. 

char convert(char c,int from, int to);

// Convert extended character c from CS_SMS to CS_ISO or vice versa.

char ext_convert(char c,int from, int to);

#endif
