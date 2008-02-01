include config.mak

all: libnut nututils

libnut:
	$(MAKE) -C libnut

nututils: libnut
	$(MAKE) -C nututils

install:
	$(MAKE) -C libnut install

clean distclean:
	$(MAKE) -C libnut clean
	$(MAKE) -C nututils clean

.PHONY: all libnut nututils install distclean clean
