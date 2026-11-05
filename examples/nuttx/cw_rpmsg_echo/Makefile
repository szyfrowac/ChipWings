include $(APPDIR)/Make.defs

PROGNAME  = cw_rpmsg_echo
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = $(CONFIG_DEFAULT_TASK_STACKSIZE)
MODULE    = $(CONFIG_CW_EXAMPLE_RPMSG_ECHO)

# Shared NuttX <-> Linux message formats (examples/common)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/cwexamples/../common

MAINSRC = cw_rpmsg_echo_main.c

include $(APPDIR)/Application.mk
