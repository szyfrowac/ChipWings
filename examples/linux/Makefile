# examples/linux: Linux-side test programs (run on harts 2-4)
#
# Built and installed into the root filesystem (/usr/bin) by the Buildroot package
# sw/br2-chipwings/package/chipwings-examples. By hand, with the Buildroot toolchain:
#   make CC=$PWD/sw/br2-out/host/bin/riscv64-linux-gcc -C examples/linux

CC      ?= gcc
CFLAGS  ?= -O2
override CFLAGS += -Wall -Wextra -Wno-unused-parameter -I../common
override LDLIBS += -lpthread -lm

PROGS = hartcheck rpmsg_ping rpmsg_echo rpmsg_imu

all: $(PROGS)

hartcheck: hartcheck.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

rpmsg_%: rpmsg_%.c cw_rpmsg_util.c cw_rpmsg_util.h ../common/cw_rpmsg_proto.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

install: $(PROGS)
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(PROGS) $(DESTDIR)/usr/bin/

clean:
	rm -f $(PROGS)

.PHONY: all install clean
