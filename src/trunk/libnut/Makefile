include ../config.mak

OBJS = muxer.o demuxer.o reorder.o framecode.o

all: libnut.a

install:
	install -d $(prefix)/lib
	install -m 644 libnut.a $(prefix)/lib
	install -d $(prefix)/include
	install -m 644 libnut.h $(prefix)/include

uninstall:
	rm -f $(prefix)/lib/libnut.a
	rm -f $(prefix)/include/libnut.h

libnut.a: $(OBJS)
	rm -f $@
	$(AR) rc $@ $^
	$(RANLIB) $@

libnut.so: $(OBJS)
	$(CC) $(CFLAGS) -shared $^ -o $@

*.o: priv.h libnut.h

.PHONY: all install uninstall
