PREFIX = /usr/local
prefix = $(DESTDIR)$(PREFIX)

CFLAGS = -Os -Wall -g
#CFLAGS = -fomit-frame-pointer -g -Wall

#CFLAGS += -DWORDS_BIGENDIAN
CFLAGS += -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64

CC = cc
RANLIB  = ranlib
AR = ar
