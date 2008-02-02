PREFIX = /usr/local
prefix = $(DESTDIR)$(PREFIX)

CFLAGS = -Os -fomit-frame-pointer -Wall
#CFLAGS = -g -DDEBUG -Wall

#CFLAGS += -DWORDS_BIGENDIAN
CFLAGS += -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64

CC = cc
RANLIB  = ranlib
AR = ar
