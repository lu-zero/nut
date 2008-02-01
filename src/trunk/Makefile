include config.mak

all: libnut nututils

libnut:
	$(MAKE) -C libnut

nututils: libnut
	$(MAKE) -C nututils

install:
	$(MAKE) -C libnut install


distclean: clean

clean:
	$(MAKE) -C libnut clean
	$(MAKE) -C nututils clean

.PHONY: all libnut nututils install distclean clean FORCE
