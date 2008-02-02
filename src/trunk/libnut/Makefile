include ../config.mak

OBJS = muxer.o demuxer.o reorder.o framecode.o

all: libnut.a

libnut.a: $(OBJS)
	rm -f $@
	$(AR) rc $@ $^
	$(RANLIB) $@

libnut.so: $(OBJS)
	$(CC) $(CFLAGS) -shared $^ -o $@

*.o: priv.h libnut.h

.PHONY: all
