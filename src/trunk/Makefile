include config.mak

all: libnut nututils

libnut:
	$(MAKE) -C libnut

nututils: libnut nututils/nutmerge nututils/nutindex nututils/nutparse

nututils/nutmerge: nututils/demux_avi.c nututils/demux_ogg.c nututils/framer_mp3.c nututils/framer_mpeg4.c nututils/framer_vorbis.c nututils/nutmerge.h libnut/libnut.a # nututils/demux_nut.c

nututils/nutmerge nututils/nutindex nututils/nutparse: CFLAGS += -Ilibnut

install:
	install -d $(prefix)/lib
	install -m 644 libnut/libnut.a $(prefix)/lib
	install -d $(prefix)/include
	install -m 644 libnut/libnut.h $(prefix)/include

uninstall:
	rm -f $(prefix)/lib/libnut.a
	rm -f $(prefix)/include/libnut.h

clean distclean:
	rm -f libnut/*\~ libnut/*.o libnut/libnut.so libnut/libnut.a
	rm -f nututils/*\~ nututils/*.o nututils/nutmerge nututils/nutindex nututils/nutparse

.PHONY: all libnut nututils install uninstall clean distclean
