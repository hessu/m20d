all: m20d

CC = gcc
LD = gcc
CFLAGS = -Wall -Wstrict-prototypes -g
#CFLAGS = -Wall -Wstrict-prototypes -O6
# Solaris:
SOLARIS_LIBS = -lnsl -lxnet

.c.o:
	$(CC) $(CFLAGS) $(OS_CFLAGS) -c $<

clean:
	rm -f *.o *~ */*~ core
distclean: clean
	rm -f m20d

BITS = m20d.o message.o log.o hmalloc.o charset.o device.o

LINKING = $(LD) $(LDFLAGS) $(OS_LDFLAGS) -o m20d $(BITS)

solaris: $(BITS)
	 $(LINKING) $(SOLARIS_LIBS)

m20d: $(BITS)
	$(LINKING)

m20d.o:		m20d.c hmalloc.h log.h charset.h message.h device.h
message.o:	message.c message.h hmalloc.h log.h
device.o:	device.c device.h hmalloc.h log.h
log.o:		log.c log.h
hmalloc.o:	hmalloc.c hmalloc.h
charset.o:	charset.c charset.h


