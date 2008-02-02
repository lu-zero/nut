include config.mak

all: libnut nututils

libnut:
	$(MAKE) -C libnut

nututils: libnut
	$(MAKE) -C nututils

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
