#include "common_main.hpp"
#include <ctime>
#include <algorithm>
#include <sys/time.h>

extern char **av;
extern size_t CPUS;
extern long long MAX_FILES;
extern size_t TEMP_FILES;
extern long long diskfree;
extern int quiet;
extern int quiet_progress;
extern std::atomic<double> last_progress;
extern double progress_interval;
extern size_t memsize;
extern int additional[];
extern double preserve_point_density_threshold;

void checkdisk(std::vector<struct reader> *r) {
	long long used = 0;
	for (size_t i = 0; i < r->size(); i++) {
		used += 2 * (*r)[i].geompos + 2 * (*r)[i].indexpos + (*r)[i].poolfile->off + (*r)[i].treefile->off +
			(*r)[i].vertexpos + (*r)[i].nodepos;
	}

	static int warned = 0;
	if (used > diskfree * .9 && !warned) {
		fprintf(stderr, "You will probably run out of disk space.\n%lld bytes used or committed, of %lld originally available\n", used, diskfree);
		warned = 1;
	}
}

int atoi_require(const char *s, const char *what) {
	char *err = NULL;
	if (*s == '\0') {
		fprintf(stderr, "%s: %s must be a number (got %s)\n", *av, what, s);
		exit(EXIT_ARGS);
	}
	int ret = strtol(s, &err, 10);
	if (*err != '\0') {
		fprintf(stderr, "%s: %s must be a number (got %s)\n", *av, what, s);
		exit(EXIT_ARGS);
	}
	return ret;
}

double atof_require(const char *s, const char *what) {
	char *err = NULL;
	if (*s == '\0') {
		fprintf(stderr, "%s: %s must be a number (got %s)\n", *av, what, s);
		exit(EXIT_ARGS);
	}
	double ret = strtod(s, &err);
	if (*err != '\0') {
		fprintf(stderr, "%s: %s must be a number (got %s)\n", *av, what, s);
		exit(EXIT_ARGS);
	}
	return ret;
}

long long atoll_require(const char *s, const char *what) {
	char *err = NULL;
	if (*s == '\0') {
		fprintf(stderr, "%s: %s must be a number (got %s)\n", *av, what, s);
		exit(EXIT_ARGS);
	}
	long long ret = strtoll(s, &err, 10);
	if (*err != '\0') {
		fprintf(stderr, "%s: %s must be a number (got %s)\n", *av, what, s);
		exit(EXIT_ARGS);
	}
	return ret;
}

void init_cpus() {
	const char *TIPPECANOE_MAX_THREADS = getenv("TIPPECANOE_MAX_THREADS");

	if (TIPPECANOE_MAX_THREADS != NULL) {
		CPUS = atoi_require(TIPPECANOE_MAX_THREADS, "TIPPECANOE_MAX_THREADS");
	} else {
		CPUS = get_num_avail_cpus();
	}

	if (CPUS < 1) {
		CPUS = 1;
	}

	if (CPUS > 32767) {
		CPUS = 32767;
	}

	CPUS = 1 << (int) (log(CPUS) / log(2));

	MAX_FILES = get_max_open_files();

	if (MAX_FILES > 2000) {
		MAX_FILES = 2000;
	}

	std::vector<long long> fds_v(MAX_FILES);
	long long *fds = fds_v.data();
	long long i;
	for (i = 0; i < MAX_FILES; i++) {
		fds[i] = open(get_null_device(), O_RDONLY | O_CLOEXEC);
		if (fds[i] < 0) {
			break;
		}
	}
	long long j;
	for (j = 0; j < i; j++) {
		if (close(fds[j]) < 0) {
			perror("close");
			exit(EXIT_CLOSE);
		}
	}

	MAX_FILES = i * 3 / 4;
	if (MAX_FILES < 32) {
		fprintf(stderr, "Can't open a useful number of files: %lld\n", MAX_FILES);
		exit(EXIT_OPEN);
	}

	TEMP_FILES = (MAX_FILES - 10) / 2;
	if (TEMP_FILES > CPUS * 4) {
		TEMP_FILES = CPUS * 4;
	}
}

int indexcmp(const void *v1, const void *v2) {
	const struct index *i1 = (const struct index *) v1;
	const struct index *i2 = (const struct index *) v2;

	if (i1->ix < i2->ix) {
		return -1;
	} else if (i1->ix > i2->ix) {
		return 1;
	}

	if (i1->seq < i2->seq) {
		return -1;
	} else if (i1->seq > i2->seq) {
		return 1;
	}

	return 0;
}

static void insert(struct mergelist *m, struct mergelist **head, unsigned char *map) {
	while (*head != NULL && indexcmp(map + m->start, map + (*head)->start) > 0) {
		head = &((*head)->next);
	}
	m->next = *head;
	*head = m;
}

int calc_feature_minzoom(struct index *ix, struct drop_state *ds, int maxzoom, double gamma) {
	int feature_minzoom = 0;

	if (gamma >= 0 && (ix->t == VT_POINT ||
			   (additional[A_LINE_DROP] && ix->t == VT_LINE) ||
			   (additional[A_POLYGON_DROP] && ix->t == VT_POLYGON))) {
		for (ssize_t i = maxzoom; i >= 0; i--) {
			ds[i].seq++;
		}
		for (ssize_t i = maxzoom; i >= 0; i--) {
			if (ds[i].seq < 0) {
				feature_minzoom = i + 1;
				for (ssize_t j = i + 1; j <= maxzoom; j++) {
					ds[j].previndex = ix->ix;
				}
				break;
			} else {
				ds[i].seq -= ds[i].interval;
			}
		}

		if (preserve_point_density_threshold > 0) {
			for (ssize_t i = 0; i < feature_minzoom && i < maxzoom; i++) {
				if (ix->ix - ds[i].previndex > ((1LL << (32 - i)) / preserve_point_density_threshold) * ((1LL << (32 - i)) / preserve_point_density_threshold)) {
					feature_minzoom = i;
					for (ssize_t j = i; j <= maxzoom; j++) {
						ds[j].previndex = ix->ix;
					}
					break;
				}
			}
		}
	}

	return feature_minzoom;
}

static void merge(struct mergelist *merges, size_t nmerges, unsigned char *map, FILE *indexfile, int bytes, char *geom_map, FILE *geom_out, std::atomic<long long> *geompos, long long *progress, long long *progress_max, long long *progress_reported, int maxzoom, double gamma, struct drop_state *ds) {
	struct mergelist *head = NULL;

	for (size_t i = 0; i < nmerges; i++) {
		if (merges[i].start < merges[i].end) {
			insert(&(merges[i]), &head, map);
		}
	}

	last_progress = 0;

	while (head != NULL) {
		struct index ix = *((struct index *) (map + head->start));
		long long pos = *geompos;

		fwrite_check(geom_map + ix.start, 1, ix.end - ix.start - 1, geom_out, geompos, "merge geometry");
		int feature_minzoom = calc_feature_minzoom(&ix, ds, maxzoom, gamma);
		serialize_byte(geom_out, feature_minzoom, geompos, "merge geometry");

		*progress += (ix.end - ix.start) * 3 / 4;
		if (!quiet && !quiet_progress && progress_time() && 100 * *progress / *progress_max != *progress_reported) {
			fprintf(stderr, "Reordering geometry: %lld%% \r", 100 * *progress / *progress_max);
			fflush(stderr);
			*progress_reported = 100 * *progress / *progress_max;
		}

		ix.start = pos;
		ix.end = *geompos;
		std::atomic<long long> indexpos;
		fwrite_check(&ix, bytes, 1, indexfile, &indexpos, "merge temporary");
		head->start += bytes;

		struct mergelist *m = head;
		head = m->next;
		m->next = NULL;

		if (m->start < m->end) {
			insert(m, &head, map);
		}
	}
}

void *run_sort(void *v) {
	struct sort_arg *a = (struct sort_arg *) v;

	long long start;
	for (start = a->task * a->unit; start < a->indexpos; start += a->unit * a->cpus) {
		long long end = start + a->unit;
		if (end > a->indexpos) {
			end = a->indexpos;
		}

		a->merges[start / a->unit].start = start;
		a->merges[start / a->unit].end = end;
		a->merges[start / a->unit].next = NULL;

		std::string s;
		s.resize(end - start);

		if (pread(a->indexfd, (void *) s.c_str(), end - start, start) != end - start) {
			fprintf(stderr, "pread(index): %s\n", strerror(errno));
			exit(EXIT_READ);
		}

		qsort((void *) s.c_str(), (end - start) / a->bytes, a->bytes, indexcmp);

		if (pwrite(a->indexfd, s.c_str(), end - start, start) != end - start) {
			fprintf(stderr, "pwrite(index): %s\n", strerror(errno));
			exit(EXIT_WRITE);
		}
	}

	return NULL;
}

void radix1(int *geomfds_in, int *indexfds_in, int inputs, int prefix, int splits, long long mem, const char *tmpdir, long long *availfiles, FILE *geomfile, FILE *indexfile, std::atomic<long long> *geompos_out, long long *progress, long long *progress_max, long long *progress_reported, int maxzoom, int basezoom, double droprate, double gamma, struct drop_state *ds) {
	int splitbits = log(splits) / log(2);
	splits = 1 << splitbits;

	std::vector<FILE *> geomfiles_v(splits);
	std::vector<FILE *> indexfiles_v(splits);
	std::vector<int> geomfds_v(splits);
	std::vector<int> indexfds_v(splits);
	std::vector<std::atomic<long long>> sub_geompos_v(splits);
	FILE **geomfiles = geomfiles_v.data();
	FILE **indexfiles = indexfiles_v.data();
	int *geomfds = geomfds_v.data();
	int *indexfds = indexfds_v.data();
	std::atomic<long long> *sub_geompos = sub_geompos_v.data();

	int i;
	for (i = 0; i < splits; i++) {
		sub_geompos[i] = 0;

		char geomname[strlen(tmpdir) + strlen("/geom.XXXXXXXX") + 1];
		snprintf(geomname, sizeof(geomname), "%s%s", tmpdir, "/geom.XXXXXXXX");
		char indexname[strlen(tmpdir) + strlen("/index.XXXXXXXX") + 1];
		snprintf(indexname, sizeof(indexname), "%s%s", tmpdir, "/index.XXXXXXXX");

		geomfds[i] = mkstemp_cloexec(geomname);
		if (geomfds[i] < 0) {
			perror(geomname);
			exit(EXIT_OPEN);
		}
		indexfds[i] = mkstemp_cloexec(indexname);
		if (indexfds[i] < 0) {
			perror(indexname);
			exit(EXIT_OPEN);
		}

		geomfiles[i] = fopen_oflag(geomname, "wb", O_WRONLY | O_CLOEXEC);
		if (geomfiles[i] == NULL) {
			perror(geomname);
			exit(EXIT_OPEN);
		}
		indexfiles[i] = fopen_oflag(indexname, "wb", O_WRONLY | O_CLOEXEC);
		if (indexfiles[i] == NULL) {
			perror(indexname);
			exit(EXIT_OPEN);
		}

		*availfiles -= 4;

		unlink(geomname);
		unlink(indexname);
	}

	for (i = 0; i < inputs; i++) {
		struct stat geomst, indexst;
		if (fstat(geomfds_in[i], &geomst) < 0) {
			perror("stat geom");
			exit(EXIT_STAT);
		}
		if (fstat(indexfds_in[i], &indexst) < 0) {
			perror("stat index");
			exit(EXIT_STAT);
		}

		if (indexst.st_size != 0) {
			struct index *indexmap = (struct index *) mmap(NULL, indexst.st_size, PROT_READ, MAP_PRIVATE, indexfds_in[i], 0);
			if (indexmap == MAP_FAILED) {
				fprintf(stderr, "fd %lld, len %lld\n", (long long) indexfds_in[i], (long long) indexst.st_size);
				perror("map index");
				exit(EXIT_STAT);
			}
			madvise(indexmap, indexst.st_size, MADV_SEQUENTIAL);
			madvise(indexmap, indexst.st_size, MADV_WILLNEED);
			char *geommap = (char *) mmap(NULL, geomst.st_size, PROT_READ, MAP_PRIVATE, geomfds_in[i], 0);
			if (geommap == MAP_FAILED) {
				perror("map geom");
				exit(EXIT_MEMORY);
			}
			madvise(geommap, geomst.st_size, MADV_SEQUENTIAL);
			madvise(geommap, geomst.st_size, MADV_WILLNEED);

			for (size_t a = 0; a < indexst.st_size / sizeof(struct index); a++) {
				struct index ix = indexmap[a];
				unsigned long long which = (ix.ix << prefix) >> (64 - splitbits);
				long long pos = sub_geompos[which];

				fwrite_check(geommap + ix.start, ix.end - ix.start, 1, geomfiles[which], &sub_geompos[which], "geom");

				*progress += (ix.end - ix.start) / 4;
				if (!quiet && !quiet_progress && progress_time() && 100 * *progress / *progress_max != *progress_reported) {
					fprintf(stderr, "Reordering geometry: %lld%% \r", 100 * *progress / *progress_max);
					fflush(stderr);
					*progress_reported = 100 * *progress / *progress_max;
				}

				ix.start = pos;
				ix.end = sub_geompos[which];

				std::atomic<long long> indexpos;
				fwrite_check(&ix, sizeof(struct index), 1, indexfiles[which], &indexpos, "index");
			}

			madvise(indexmap, indexst.st_size, MADV_DONTNEED);
			if (munmap(indexmap, indexst.st_size) < 0) {
				perror("unmap index");
				exit(EXIT_MEMORY);
			}
			madvise(geommap, geomst.st_size, MADV_DONTNEED);
			if (munmap(geommap, geomst.st_size) < 0) {
				perror("unmap geom");
				exit(EXIT_MEMORY);
			}
		}

		if (close(geomfds_in[i]) < 0) {
			perror("close geom");
			exit(EXIT_CLOSE);
		}
		if (close(indexfds_in[i]) < 0) {
			perror("close index");
			exit(EXIT_CLOSE);
		}

		*availfiles += 2;
	}

	for (i = 0; i < splits; i++) {
		if (fclose(geomfiles[i]) != 0) {
			perror("fclose geom");
			exit(EXIT_CLOSE);
		}
		if (fclose(indexfiles[i]) != 0) {
			perror("fclose index");
			exit(EXIT_CLOSE);
		}

		*availfiles += 2;
	}

	for (i = 0; i < splits; i++) {
		int already_closed = 0;

		struct stat geomst, indexst;
		if (fstat(geomfds[i], &geomst) < 0) {
			perror("stat geom");
			exit(EXIT_STAT);
		}
		if (fstat(indexfds[i], &indexst) < 0) {
			perror("stat index");
			exit(EXIT_STAT);
		}

		if (indexst.st_size > 0) {
			if (indexst.st_size + geomst.st_size < mem) {
				std::atomic<long long> indexpos(indexst.st_size);
				int bytes = sizeof(struct index);

				int page = get_page_size();
				long long max_unit = 2LL * 1024 * 1024 * 1024;
				long long unit = ((indexpos / CPUS + bytes - 1) / bytes) * bytes;
				if (unit > max_unit) {
					unit = max_unit;
				}
				unit = ((unit + page - 1) / page) * page;
				if (unit < page) {
					unit = page;
				}

				size_t nmerges = (indexpos + unit - 1) / unit;
				std::vector<struct mergelist> merges_v(nmerges);
				struct mergelist *merges = merges_v.data();

				for (size_t a = 0; a < nmerges; a++) {
					merges[a].start = merges[a].end = 0;
				}

				std::vector<pthread_t> pthreads_v(CPUS);
				pthread_t *pthreads = pthreads_v.data();
				std::vector<sort_arg> args;

				for (size_t a = 0; a < CPUS; a++) {
					args.push_back(sort_arg(
						a,
						CPUS,
						indexpos,
						merges,
						indexfds[i],
						nmerges,
						unit,
						bytes));
				}

				for (size_t a = 0; a < CPUS; a++) {
					if (thread_create(&pthreads[a], NULL, run_sort, &args[a]) != 0) {
						perror("pthread_create");
						exit(EXIT_PTHREAD);
					}
				}

				for (size_t a = 0; a < CPUS; a++) {
					void *retval;
					if (pthread_join(pthreads[a], &retval) != 0) {
						perror("pthread_join 679");
					}
				}

				struct indexmap *indexmap = (struct indexmap *) mmap(NULL, indexst.st_size, PROT_READ, MAP_PRIVATE, indexfds[i], 0);
				if (indexmap == MAP_FAILED) {
					fprintf(stderr, "fd %lld, len %lld\n", (long long) indexfds[i], (long long) indexst.st_size);
					perror("map index");
					exit(EXIT_MEMORY);
				}
				madvise(indexmap, indexst.st_size, MADV_RANDOM);
				madvise(indexmap, indexst.st_size, MADV_WILLNEED);
				char *geommap = (char *) mmap(NULL, geomst.st_size, PROT_READ, MAP_PRIVATE, geomfds[i], 0);
				if (geommap == MAP_FAILED) {
					perror("map geom");
					exit(EXIT_MEMORY);
				}
				madvise(geommap, geomst.st_size, MADV_RANDOM);
				madvise(geommap, geomst.st_size, MADV_WILLNEED);

				merge(merges, nmerges, (unsigned char *) indexmap, indexfile, bytes, geommap, geomfile, geompos_out, progress, progress_max, progress_reported, maxzoom, gamma, ds);

				madvise(indexmap, indexst.st_size, MADV_DONTNEED);
				if (munmap(indexmap, indexst.st_size) < 0) {
					perror("unmap index");
					exit(EXIT_MEMORY);
				}
				madvise(geommap, geomst.st_size, MADV_DONTNEED);
				if (munmap(geommap, geomst.st_size) < 0) {
					perror("unmap geom");
					exit(EXIT_MEMORY);
				}
			} else if (indexst.st_size == sizeof(struct index) || prefix + splitbits >= 64) {
				struct index *indexmap = (struct index *) mmap(NULL, indexst.st_size, PROT_READ, MAP_PRIVATE, indexfds[i], 0);
				if (indexmap == MAP_FAILED) {
					fprintf(stderr, "fd %lld, len %lld\n", (long long) indexfds[i], (long long) indexst.st_size);
					perror("map index");
					exit(EXIT_MEMORY);
				}
				madvise(indexmap, indexst.st_size, MADV_SEQUENTIAL);
				madvise(indexmap, indexst.st_size, MADV_WILLNEED);
				char *geommap = (char *) mmap(NULL, geomst.st_size, PROT_READ, MAP_PRIVATE, geomfds[i], 0);
				if (geommap == MAP_FAILED) {
					perror("map geom");
					exit(EXIT_MEMORY);
				}
				madvise(geommap, geomst.st_size, MADV_RANDOM);
				madvise(geommap, geomst.st_size, MADV_WILLNEED);

				for (size_t a = 0; a < indexst.st_size / sizeof(struct index); a++) {
					struct index ix = indexmap[a];
					long long pos = *geompos_out;

					fwrite_check(geommap + ix.start, ix.end - ix.start, 1, geomfile, geompos_out, "geom");
					int feature_minzoom = calc_feature_minzoom(&ix, ds, maxzoom, gamma);
					serialize_byte(geomfile, feature_minzoom, geompos_out, "merge geometry");

					*progress += (ix.end - ix.start) * 3 / 4;
					if (!quiet && !quiet_progress && progress_time() && 100 * *progress / *progress_max != *progress_reported) {
						fprintf(stderr, "Reordering geometry: %lld%% \r", 100 * *progress / *progress_max);
						fflush(stderr);
						*progress_reported = 100 * *progress / *progress_max;
					}

					ix.start = pos;
					ix.end = *geompos_out;
					std::atomic<long long> indexpos;
					fwrite_check(&ix, sizeof(struct index), 1, indexfile, &indexpos, "index");
				}

				madvise(indexmap, indexst.st_size, MADV_DONTNEED);
				if (munmap(indexmap, indexst.st_size) < 0) {
					perror("unmap index");
					exit(EXIT_MEMORY);
				}
				madvise(geommap, geomst.st_size, MADV_DONTNEED);
				if (munmap(geommap, geomst.st_size) < 0) {
					perror("unmap geom");
					exit(EXIT_MEMORY);
				}
			} else {
				*progress_max += geomst.st_size / 4;
				radix1(&geomfds[i], &indexfds[i], 1, prefix + splitbits, *availfiles / 4, mem, tmpdir, availfiles, geomfile, indexfile, geompos_out, progress, progress_max, progress_reported, maxzoom, basezoom, droprate, gamma, ds);
				already_closed = 1;
			}
		}

		if (!already_closed) {
			if (close(geomfds[i]) < 0) {
				perror("close geom");
				exit(EXIT_CLOSE);
			}
			if (close(indexfds[i]) < 0) {
				perror("close index");
				exit(EXIT_CLOSE);
			}

			*availfiles += 2;
		}
	}
}

void prep_drop_states(struct drop_state *ds, int maxzoom, int basezoom, double droprate) {
	for (ssize_t i = 0; i <= maxzoom; i++) {
		ds[i].gap = 0;
		ds[i].previndex = 0;
		ds[i].interval = 0;

		if (i < basezoom) {
			ds[i].interval = std::exp(std::log(droprate) * (basezoom - i));
		}

		ds[i].seq = 0;
	}
}

void radix(std::vector<struct reader> &readers, int nreaders, FILE *geomfile, FILE *indexfile, const char *tmpdir, std::atomic<long long> *geompos, int maxzoom, int basezoom, double droprate, double gamma) {
	long long mem = memsize;

	if (additional[A_PREFER_RADIX_SORT]) {
		mem = 8192;
	}

	long long availfiles = MAX_FILES - 2 * nreaders - 3 - 4 - 3;

	int splits = availfiles / 4;

	mem /= 2;

	long long geom_total = 0;
	std::vector<int> geomfds_v(nreaders);
	std::vector<int> indexfds_v(nreaders);
	int *geomfds = geomfds_v.data();
	int *indexfds = indexfds_v.data();
	for (int i = 0; i < nreaders; i++) {
		geomfds[i] = readers[i].geomfd;
		indexfds[i] = readers[i].indexfd;

		struct stat geomst;
		if (fstat(readers[i].geomfd, &geomst) < 0) {
			perror("stat geom");
			exit(EXIT_STAT);
		}
		geom_total += geomst.st_size;
	}

	std::vector<struct drop_state> ds_v(maxzoom + 1);
	struct drop_state *ds = ds_v.data();
	prep_drop_states(ds, maxzoom, basezoom, droprate);

	long long progress = 0, progress_max = geom_total, progress_reported = -1;
	long long availfiles_before = availfiles;
	radix1(geomfds, indexfds, nreaders, 0, splits, mem, tmpdir, &availfiles, geomfile, indexfile, geompos, &progress, &progress_max, &progress_reported, maxzoom, basezoom, droprate, gamma, ds);

	if (availfiles - 2 * nreaders != availfiles_before) {
		fprintf(stderr, "Internal error: miscounted available file descriptors: %lld vs %lld\n", availfiles - 2 * nreaders, availfiles);
		exit(EXIT_IMPOSSIBLE);
	}
}

void choose_first_zoom(long long *file_bbox, long long *file_bbox1, long long *file_bbox2, std::vector<struct reader> &readers, unsigned *iz, unsigned *ix, unsigned *iy, int minzoom, int buffer) {
	for (size_t i = 0; i < CPUS; i++) {
		if (readers[i].file_bbox[0] < file_bbox[0]) {
			file_bbox[0] = readers[i].file_bbox[0];
		}
		if (readers[i].file_bbox[1] < file_bbox[1]) {
			file_bbox[1] = readers[i].file_bbox[1];
		}
		if (readers[i].file_bbox[2] > file_bbox[2]) {
			file_bbox[2] = readers[i].file_bbox[2];
		}
		if (readers[i].file_bbox[3] > file_bbox[3]) {
			file_bbox[3] = readers[i].file_bbox[3];
		}

		file_bbox1[0] = std::min(file_bbox1[0], readers[i].file_bbox1[0]);
		file_bbox1[1] = std::min(file_bbox1[1], readers[i].file_bbox1[1]);
		file_bbox1[2] = std::max(file_bbox1[2], readers[i].file_bbox1[2]);
		file_bbox1[3] = std::max(file_bbox1[3], readers[i].file_bbox1[3]);

		file_bbox2[0] = std::min(file_bbox2[0], readers[i].file_bbox2[0]);
		file_bbox2[1] = std::min(file_bbox2[1], readers[i].file_bbox2[1]);
		file_bbox2[2] = std::max(file_bbox2[2], readers[i].file_bbox2[2]);
		file_bbox2[3] = std::max(file_bbox2[3], readers[i].file_bbox2[3]);
	}

	if (file_bbox[0] < 0) {
		file_bbox[0] = 0;
		file_bbox[2] = (1LL << 32) - 1;
	}
	if (file_bbox[2] > (1LL << 32) - 1) {
		file_bbox[0] = 0;
		file_bbox[2] = (1LL << 32) - 1;
	}
	if (file_bbox[1] < 0) {
		file_bbox[1] = 0;
	}
	if (file_bbox[3] > (1LL << 32) - 1) {
		file_bbox[3] = (1LL << 32) - 1;
	}

	for (ssize_t z = minzoom; z >= 0; z--) {
		long long shift = 1LL << (32 - z);

		long long left = (file_bbox[0] - buffer * shift / 256) / shift;
		long long top = (file_bbox[1] - buffer * shift / 256) / shift;
		long long right = (file_bbox[2] + buffer * shift / 256) / shift;
		long long bottom = (file_bbox[3] + buffer * shift / 256) / shift;

		if (left == right && top == bottom) {
			*iz = z;
			*ix = left;
			*iy = top;
			break;
		}
	}
}

int vertexcmp(const void *void1, const void *void2) {
	vertex *v1 = (vertex *) void1;
	vertex *v2 = (vertex *) void2;

	if (v1->mid < v2->mid) {
		return -1;
	}
	if (v1->mid > v2->mid) {
		return 1;
	}

	if (v1->p1 < v2->p1) {
		return -1;
	}
	if (v1->p1 > v2->p1) {
		return 1;
	}

	if (v1->p2 < v2->p2) {
		return -1;
	}
	if (v1->p2 > v2->p2) {
		return 1;
	}

	return 0;
}

double round_droprate(double r) {
	return std::round(r * 100000.0) / 100000.0;
}

bool has_name(const char *s) {
	for (size_t i = 0; s[i]; i++) {
		if (s[i] == ':') {
			return true;
		}
	}
	return false;
}

int mkstemp_cloexec(char *name) {
	int fd = mkstemp(name);
	if (fd >= 0) {
		if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
			perror("cloexec for temporary file");
			exit(EXIT_OPEN);
		}
	}
	return fd;
}

FILE *fopen_oflag(const char *name, const char *mode, int oflag) {
	int fd = open(name, oflag);
	if (fd < 0) {
		return NULL;
	}
	return fdopen(fd, mode);
}

bool progress_time() {
	if (progress_interval == 0.0) {
		return true;
	}

	struct timeval tv;
	double now;
	if (gettimeofday(&tv, NULL) != 0) {
		fprintf(stderr, "%s: Can't get the time of day: %s\n", *av, strerror(errno));
		now = 0;
	} else {
		now = tv.tv_sec + tv.tv_usec / 1000000.0;
	}

	if (now - last_progress >= progress_interval) {
		last_progress = now;
		return true;
	} else {
		return false;
	}
}
