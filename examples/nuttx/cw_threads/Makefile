include $(APPDIR)/Make.defs

PROGNAME  = cw_threads
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = $(CONFIG_DEFAULT_TASK_STACKSIZE)
MODULE    = $(CONFIG_CW_EXAMPLE_THREADS)

# Shared NuttX <-> Linux message formats (examples/common)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/cwexamples/../common

MAINSRC = cw_threads_main.c

include $(APPDIR)/Application.mk
