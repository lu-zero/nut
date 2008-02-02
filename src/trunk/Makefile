include config.mak

all: libnut nututils

libnut:
	$(MAKE) -C libnut

nututils: libnut
	$(MAKE) -C nututils

install:
	$(MAKE) -C libnut install

clean distclean:
	rm -f libnut/*\~ libnut/*.o libnut/libnut.so libnut/libnut.a
	rm -f nututils/*\~ nututils/*.o nututils/nutmerge nututils/nutindex nututils/nutparse

.PHONY: all libnut nututils install distclean clean
