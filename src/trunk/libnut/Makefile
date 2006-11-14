include ../config.mak

all: libnut.a

install:
	install -d $(prefix)/lib
	install -m 644 libnut.a $(prefix)/lib
	install -d $(prefix)/include
	install -m 644 libnut.h $(prefix)/include

uninstall:
	rm -f $(prefix)/lib/libnut.a
	rm -f $(prefix)/include/libnut.h

libnut.a: muxer.o demuxer.o reorder.o
	rm -f $@
	${AR} rc $@ $^
	$(RANLIB) $@

libnut.so: muxer.o demuxer.o reorder.o
	${CC} ${CFLAGS} -shared $^ -o $@

.c.o: priv.h nut.h
	${CC} ${CFLAGS} $^ -c -o $@

clean:
	rm -f *\~ libnut.so libnut.a *.o
