CC=gcc
ELKS_CC=bcc
CFLAGS=-O2 -g
#CFLAGS=-Og -g3
ELKS_CFLAGS=-ansi -0 -O -s -DNO_SIGNALS
BUILD_CFLAGS = -std=gnu99 -I. -D_FILE_OFFSET_BITS=64 -pipe -fstrict-aliasing
BUILD_CFLAGS += -Wall -Wextra -Wcast-align -Wstrict-aliasing -pedantic -Wstrict-overflow -Wno-unused-parameter
#LDFLAGS=-s
LDFLAGS=

prefix=/usr
exec_prefix=${prefix}
bindir=${exec_prefix}/bin
mandir=${prefix}/man
datarootdir=${prefix}/share
datadir=${datarootdir}
sysconfdir=${prefix}/etc

OBJS=vi.o

all: vi manual

elks:
	$(ELKS_CC) $(ELKS_CFLAGS) -o vi vi.c

vi: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(BUILD_CFLAGS) -o vi $(OBJS)

manual:
#	gzip -9 < vi.1 > vi.1.gz

.c.o:
	$(CC) -c $(BUILD_CFLAGS) $(CFLAGS) $<

clean:
	rm -f *.o *~ vi debug.log *.?.gz

distclean:
	rm -f *.o *~ vi debug.log *.?.gz vi*.pkg.tar.*

install: all
	install -D -o root -g root -m 0644 vi.1.gz $(DESTDIR)/$(mandir)/man1/vi.1.gz
	install -D -o root -g root -m 0755 -s vi $(DESTDIR)/$(bindir)/vi

package:
	+./chroot_build.sh
