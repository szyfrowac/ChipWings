/*
 * examples/linux/hartcheck.c
 *
 * hartcheck
 *
 * Checks that Linux owns exactly the mission-computer harts (2, 3, 4) and
 * never the flight-control hart 1: reads the logical CPU -> hart mapping from
 * /proc/cpuinfo, then pins a thread to every CPU Linux schedules on and
 * confirms it really runs there (sched_getcpu).
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CPUS 16

static const int expected_harts[] = { 2, 3, 4 };

struct pin_result {
	int cpu;
	int ran_on;
};

static void *pinned(void *arg)
{
	struct pin_result *r = arg;
	cpu_set_t set;
	volatile unsigned long spin;

	CPU_ZERO(&set);
	CPU_SET(r->cpu, &set);
	if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set)) {
		r->ran_on = -1;
		return NULL;
	}
	for (spin = 0; spin < 1000000; spin++)
		;
	r->ran_on = sched_getcpu();
	return NULL;
}

int main(void)
{
	int hart_of_cpu[MAX_CPUS];
	struct pin_result results[MAX_CPUS];
	pthread_t threads[MAX_CPUS];
	char line[256];
	int cpu = -1, ncpus = 0, nharts, i, j, ok = 1;
	FILE *f = fopen("/proc/cpuinfo", "r");

	if (!f) {
		perror("/proc/cpuinfo");
		return 1;
	}
	for (i = 0; i < MAX_CPUS; i++)
		hart_of_cpu[i] = -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "processor : %d", &cpu) == 1)
			continue;
		if (cpu >= 0 && cpu < MAX_CPUS && !strncmp(line, "hart", 4) &&
		    sscanf(line, "hart : %d", &hart_of_cpu[cpu]) == 1)
			ncpus = cpu + 1 > ncpus ? cpu + 1 : ncpus;
	}
	fclose(f);

	printf("hartcheck: Linux sees %d CPUs\n", ncpus);
	for (i = 0; i < ncpus; i++) {
		results[i].cpu = i;
		results[i].ran_on = -1;
		pthread_create(&threads[i], NULL, pinned, &results[i]);
	}
	for (i = 0; i < ncpus; i++) {
		pthread_join(threads[i], NULL);
		printf("hartcheck: CPU %d = hart %d, pinned thread ran on CPU %d\n",
		       i, hart_of_cpu[i], results[i].ran_on);
		if (results[i].ran_on != i)
			ok = 0;
		if (hart_of_cpu[i] == 1) {
			printf("hartcheck: hart 1 belongs to NuttX but Linux uses it\n");
			ok = 0;
		}
	}

	nharts = (int)(sizeof(expected_harts) / sizeof(expected_harts[0]));
	if (ncpus != nharts)
		ok = 0;
	for (j = 0; j < nharts; j++) {
		int seen = 0;

		for (i = 0; i < ncpus; i++)
			seen |= hart_of_cpu[i] == expected_harts[j];
		if (!seen) {
			printf("hartcheck: hart %d missing\n", expected_harts[j]);
			ok = 0;
		}
	}

	printf("hartcheck: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
