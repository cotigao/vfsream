# Copyright (C) 2017 Vikram Fugro
#
#
# This software may be modified and distributed under the terms
# of the MIT license.
# 

CC = gccgo
CFLAGS = -fPIC -Wall -fgo-prefix=example -Wextra -O2 -g -pthread -I. -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/libsoup-2.4 -I/usr/include/libxml2 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/uuid -I/usr/include/gupnp-1.0 -I/usr/include/gssdp-1.0 -I/usr/include/libsoup-2.4 -I/usr/include/libxml2 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/uuid -I/usr/include/gupnp-av-1.0 -I/usr/include/gupnp-1.0 -I/usr/include/gssdp-1.0 -pthread -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/gssdp-1.0 -I/usr/include/libxml2 -I/usr/lib/x86_64-linux-gnu/gstreamer-1.0/include/
LDFLAGS = -shared -Wl,-export-dynamic -L/usr/lib/x86_64-linux-gnu -lgstbase-1.0 -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgupnp-1.0 -lgupnp-av-1.0 -lgssdp-1.0 -lxml2 -lpthread -lm -lz -licui18n -licuuc -licudata -llzma
RM = rm -f
TARGET_LIB = libtarget.so

SRCS = GstSource.c Upnp.c
OBJS = $(SRCS:.c=.o)

.PHONY: all
all: ${TARGET_LIB}

$(TARGET_LIB): $(OBJS)
	$(CC) ${LDFLAGS} -o $@ $^

$(SRCS:.c=.d):%.d:%.c
	$(CC) $(CFLAGS) -MM $< >$@

include $(SRCS:.c=.d)

.PHONY: clean
clean:
	-${RM} ${TARGET_LIB} ${OBJS} $(SRCS:.c=.d)
