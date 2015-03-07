CC=gcc
#CFLAGS=-O2 -g
CFLAGS=-Og -g3
BUILD_CFLAGS=-std=gnu99 -I. -D_FILE_OFFSET_BITS=64 -pipe -Wall -pedantic -fstrict-aliasing
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
	rm vi
	bcc -ansi -0 -O -DNO_SIGNALS -Drestrict="" -Dinline="" -o vi vi.c

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
