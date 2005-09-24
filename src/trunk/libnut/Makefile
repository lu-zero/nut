all: libnut.so

libnut.so: muxer.o demuxer.o reorder.o
	${CC} ${CFLAGS} -shared $^ -o $@
.c.o: priv.h nut.h
	${CC} ${CFLAGS} -fPIC $^ -c -o $@
clean:
	rm -f *\~ libnut.so *.o
