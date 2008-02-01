include config.mak

all: libnut nututils

libnut: FORCE
	$(MAKE) -C libnut

nututils: libnut FORCE
	$(MAKE) -C nututils

install:
	$(MAKE) -C libnut install


distclean: clean

clean:
	$(MAKE) -C libnut clean
	$(MAKE) -C nututils clean

FORCE:

.PHONY: all libnut nututils install distclean clean FORCE
