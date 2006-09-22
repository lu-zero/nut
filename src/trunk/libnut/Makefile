include ../config.mak

all: libnut.a

libnut.a: muxer.o demuxer.o reorder.o
	rm -f $@
	${AR} rc $@ $^
	$(RANLIB) $@

.c.o: priv.h nut.h
	${CC} ${CFLAGS} $^ -c -o $@
clean:
	rm -f *\~ libnut.so *.o
