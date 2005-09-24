export CFLAGS = -Os -fomit-frame-pointer -Wall -g
#export CFLAGS = -g -Wall

#export CFLAGS += -DWORDS_BIGENDIAN

export CC = cc

all: libnut nututils

libnut: FORCE
	${MAKE} -C libnut
nututils: libnut FORCE
	${MAKE} -C nututils

clean:
	$(MAKE) -C libnut clean
	$(MAKE) -C example clean
	$(MAKE) -C nututils clean

FORCE:
