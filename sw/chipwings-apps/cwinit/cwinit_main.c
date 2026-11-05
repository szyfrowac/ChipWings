/****************************************************************************
 * chipwings-apps/cwinit/cwinit_main.c
 *
 * Start-up entry point of the flight-control core (CONFIG_INIT_ENTRYPOINT=
 * "cwinit_main"). Brings up the RPMsg link to Linux, then runs NSH.
 *
 * The mpfs IHC driver registers its rptun device without autostart. Starting
 * it here fills in the RPMsg resource table at 0xA2300000, which Linux's
 * miv_remoteproc reads when it attaches to the already-running NuttX, so it
 * has to happen before Linux boots that far.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef CONFIG_RPTUN
#  include <nuttx/rptun/rptun.h>
#endif

/****************************************************************************
 * External Function Prototypes
 ****************************************************************************/

int nsh_main(int argc, FAR char *argv[]);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_RPTUN
static void start_rptun(FAR const char *path)
{
  int fd;
  int ret;

  fd = open(path, O_WRONLY);
  if (fd < 0)
    {
      printf("cwinit: %s not found, RPMsg to Linux not started\n", path);
      return;
    }

  ret = ioctl(fd, RPTUNIOC_START, 0);
  close(fd);

  printf("cwinit: %s %s\n", path, ret < 0 ? "start failed" : "started");
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
#ifdef CONFIG_RPTUN
  start_rptun(CONFIG_CHIPWINGS_CWINIT_RPTUN);
#endif

  return nsh_main(argc, argv);
}
