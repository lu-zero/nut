include config.mak

LIBNUT_OBJS = libnut/muxer.o libnut/demuxer.o libnut/reorder.o libnut/framecode.o
NUTUTILS_PROGS = nututils/nutmerge nututils/nutindex nututils/nutparse

all: libnut nututils

libnut: libnut/libnut.a

libnut/libnut.a: $(LIBNUT_OBJS)
	rm -f $@
	$(AR) rc $@ $^
	$(RANLIB) $@

libnut/libnut.so: $(LIBNUT_OBJS)
	$(CC) $(CFLAGS) -shared $^ -o $@

$(LIBNUT_OBJS): libnut/priv.h libnut/libnut.h

nututils: $(NUTUTILS_PROGS)

nututils/nutmerge: nututils/demux_avi.c nututils/demux_ogg.c nututils/framer_mp3.c nututils/framer_mpeg4.c nututils/framer_vorbis.c nututils/nutmerge.h libnut/libnut.a # nututils/demux_nut.c

$(NUTUTILS_PROGS): CFLAGS += -Ilibnut

install: install-libnut install-nututils

install-libnut: libnut
	install -d $(prefix)/lib
	install -m 644 libnut/libnut.a $(prefix)/lib
	test -e libnut/libnut.so && install -m 644 libnut/libnut.so $(prefix)/lib
	install -d $(prefix)/include
	install -m 644 libnut/libnut.h $(prefix)/include

install-nututils: nututils
	install -d $(prefix)/bin
	install -m 755 $(NUTUTILS_PROGS) $(prefix)/bin

uninstall: uninstall-libnut uninstall-nututils

uninstall-libnut:
	rm -f $(prefix)/lib/libnut.a
	rm -f $(prefix)/lib/libnut.so
	rm -f $(prefix)/include/libnut.h

uninstall-nututils:
	rm -f $(addprefix $(prefix)/bin/, $(subst nututils/,,$(NUTUTILS_PROGS)))

clean distclean:
	rm -f libnut/*\~ libnut/*.o libnut/libnut.so libnut/libnut.a
	rm -f nututils/*\~ nututils/*.o  $(NUTUTILS_PROGS)

.PHONY: all libnut nututils install* uninstall* clean distclean
