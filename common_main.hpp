#ifndef COMMON_MAIN_HPP
#define COMMON_MAIN_HPP

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <atomic>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "serial.hpp"
#include "mvt.hpp"
#include "errors.hpp"
#include "thread.hpp"
#include "platform.hpp"
#include "pool.hpp"
#include "memfile.hpp"
#include "geometry.hpp"
#include "text.hpp"
#include "main.hpp"
#include "options.hpp"

struct mergelist {
	long long start;
	long long end;
	struct mergelist *next;
};

struct drop_state {
	double gap;
	unsigned long long previndex;
	double interval;
	double seq;
};

struct drop_densest {
	unsigned long long gap;
	size_t seq;
	bool operator<(const drop_densest &o) const {
		return gap > o.gap;
	}
};

struct sort_arg {
	int task;
	int cpus;
	long long indexpos;
	struct mergelist *merges;
	int indexfd;
	size_t nmerges;
	long long unit;
	int bytes;

	sort_arg(int task1, int cpus1, long long indexpos1, struct mergelist *merges1, int indexfd1, size_t nmerges1, long long unit1, int bytes1)
	    : task(task1), cpus(cpus1), indexpos(indexpos1), merges(merges1), indexfd(indexfd1), nmerges(nmerges1), unit(unit1), bytes(bytes1) {
	}
};

void checkdisk(std::vector<struct reader> *readers);
int atoi_require(const char *s, const char *msg);
double atof_require(const char *s, const char *msg);
long long atoll_require(const char *s, const char *msg);
void init_cpus();
int indexcmp(const void *v1, const void *v2);
int calc_feature_minzoom(struct index *ix, struct drop_state *ds, int maxzoom, double gamma);
void *run_sort(void *v);
void radix1(int *geomfds_in, int *indexfds_in, int inputs, int prefix, int splits, long long mem, const char *tmpdir, long long *availfiles, FILE *geomfile, FILE *indexfile, std::atomic<long long> *geompos_out, long long *progress, long long *progress_max, long long *progress_reported, int maxzoom, int basezoom, double droprate, double gamma, struct drop_state *ds);
void prep_drop_states(struct drop_state *ds, int maxzoom, int basezoom, double droprate);
void radix(std::vector<struct reader> &readers, int nreaders, FILE *geomfile, FILE *indexfile, const char *tmpdir, std::atomic<long long> *geompos, int maxzoom, int basezoom, double droprate, double gamma);
void choose_first_zoom(long long *file_bbox, long long *file_bbox1, long long *file_bbox2, std::vector<struct reader> &readers, unsigned *iz, unsigned *ix, unsigned *iy, int minzoom, int buffer);
int vertexcmp(const void *v1, const void *v2);
double round_droprate(double droprate);
bool has_name(const char *s);
int mkstemp_cloexec(char *name);
FILE *fopen_oflag(const char *name, const char *mode, int oflag);
bool progress_time();

#endif // COMMON_MAIN_HPP
