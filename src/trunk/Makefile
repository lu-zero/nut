include config.mak

all: libnut nututils

libnut: FORCE
	${MAKE} -C libnut
nututils: libnut FORCE
	${MAKE} -C nututils


distclean: clean

clean:
	$(MAKE) -C libnut clean
	$(MAKE) -C nututils clean

FORCE:
