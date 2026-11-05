/****************************************************************************
 * chipwings-apps/hartinfo/hartinfo_main.c
 *
 * NSH command "hartinfo": prints the physical PolarFire SoC hart NuttX is
 * running on (mhartid) and how many CPUs the NuttX scheduler uses, to
 * confirm the AMP core assignment (NuttX alone on U54_1, hart 1).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sched.h>
#include <stdio.h>

#include <arch/csr.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static const char *hart_name(unsigned long hart)
{
  switch (hart)
    {
      case 0:
        return "E51 (monitor core)";
      case 1:
      case 2:
      case 3:
      case 4:
        return "U54 application core";
      default:
        return "unknown";
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
#ifdef CONFIG_ARCH_USE_S_MODE
  /* mhartid is a machine-mode CSR; reading it from S-mode traps. */

  printf("hartinfo: needs a machine-mode (flat) build\n");
  return 1;
#else
  static const char extensions[] = "imafdqcv";
  unsigned long hart = READ_CSR(CSR_MHARTID);
  unsigned long misa = READ_CSR(CSR_MISA);
  const char *ext;
  int ncpus;

#ifdef CONFIG_SMP
  ncpus = CONFIG_SMP_NCPUS;
#else
  ncpus = 1;
#endif

  printf("Physical hart (mhartid) : %lu  %s", hart, hart_name(hart));
  if (hart >= 1 && hart <= 4)
    {
      printf(" U54_%lu", hart);
    }

  /* misa has one bit per extension letter; print them in canonical order */

  printf("\nISA (misa)              : rv64");
  for (ext = extensions; *ext != '\0'; ext++)
    {
      if (misa & (1ul << (*ext - 'a')))
        {
          putchar(*ext);
        }
    }

  printf("\nNuttX SMP               : %s\n",
#ifdef CONFIG_SMP
         "enabled"
#else
         "disabled"
#endif
        );
  printf("CPUs used by scheduler  : %d\n", ncpus);
  /* Without SMP, sched_getcpu() returns up_cpu_index(), i.e. mhartid */

  printf("sched_getcpu()          : %d\n", sched_getcpu());
  return 0;
#endif
}
