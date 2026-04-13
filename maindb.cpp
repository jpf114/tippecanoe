#ifdef MTRACE
#include <mcheck.h>
#endif

#ifdef __APPLE__
#define _DARWIN_UNLIMITED_STREAMS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <pthread.h>
#include <getopt.h>
#include <signal.h>
#include <sys/time.h>
#include <zlib.h>
#include <algorithm>
#include <vector>
#include <thread>
#include <string>
#include <set>
#include <map>
#include <cmath>
#include <memory>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/param.h>
#include <sys/mount.h>
#else
#include <sys/statfs.h>
#endif

#include "jsonpull/jsonpull.h"
#include "mbtiles.hpp"
#include "tile-db.hpp"  // 使用 MongoDB 版本
#include "pool.hpp"
#include "projection.hpp"
#include "memfile.hpp"
#include "maindb.hpp"
#include "geojson.hpp"
#include "geometry.hpp"
#include "serial.hpp"
#include "options.hpp"
#include "mvt.hpp"
#include "dirtiles.hpp"
#include "evaluator.hpp"
#include "text.hpp"
#include "errors.hpp"
#include "read_json.hpp"
#include "sort.hpp"
#include "attribute.hpp"
#include "thread.hpp"
#include "platform.hpp"
#include "mongo.hpp"
#include "config.hpp"
#include "postgis_manager.hpp"
#include "mongo_manager.hpp"
#include "error_logger.hpp"
#include "common_main.hpp"

static int low_detail = 12;
static int full_detail = -1;
static int min_detail = 7;
int extra_detail = -1;

int quiet = 0;
int quiet_progress = 0;
json_logger logger;
double progress_interval = 0;
std::atomic<double> last_progress(0);
int geometry_scale = 0;
double simplification = 1;
double maxzoom_simplification = -1;
size_t max_tile_size = 500000;
size_t max_tile_features = 200000;
int cluster_distance = 0;
int tiny_polygon_size = 2;
int cluster_maxzoom = MAX_ZOOM;
long justx = -1, justy = -1;
std::string attribute_for_id = "";
size_t limit_tile_feature_count = 0;
size_t limit_tile_feature_count_at_maxzoom = 0;
unsigned int drop_denser = 0;
std::map<std::string, serial_val> set_attributes;
unsigned long long preserve_point_density_threshold = 0;
unsigned long long preserve_multiplier_density_threshold = 0;
long long extend_zooms_max = 0;
int retain_points_multiplier = 1;
std::vector<std::string> unidecode_data;
size_t maximum_string_attribute_length = 0;

// PostGIS configuration
bool use_postgis = false;
postgis_config postgis_cfg{};

// MongoDB configuration
bool use_mongo = false;
mongo_config mongo_cfg{};

std::vector<order_field> order_by;
bool order_reverse;
bool order_by_size = false;

int prevent[256];
int additional[256];



size_t CPUS;
size_t TEMP_FILES;
long long MAX_FILES;
size_t memsize;
long long diskfree;
char **av;

std::vector<clipbbox> clipbboxes;

static void validate_postgis_config() {
	if (!use_postgis) {
		fprintf(stderr, "Error: PostGIS configuration is required\n");
		exit(EXIT_ARGS);
	}

	if (!PostGIS::validate_config(postgis_cfg)) {
		fprintf(stderr, "Error: %s\n", PostGIS::get_validation_error().c_str());
		exit(EXIT_ARGS);
	}
}

static void validate_mongo_config() {
	if (!use_mongo) {
		fprintf(stderr, "Error: MongoDB output is required. Use --mongo or --mongo-dbname etc.\n");
		exit(EXIT_ARGS);
	}

	auto mongo_validation_error = MongoDB::validate_config(mongo_cfg);
	if (mongo_validation_error.has_value()) {
		fprintf(stderr, "Error: %s\n", mongo_validation_error.value().c_str());
		exit(EXIT_ARGS);
	}

	mongo_cfg.normalize();

	if (!quiet) {
		fprintf(stderr, "MongoDB output enabled: %s.%s (batch size: %zu, fail-on-discard: %s)\n",
				mongo_cfg.dbname.c_str(), mongo_cfg.collection.c_str(), mongo_cfg.batch_size,
				mongo_cfg.fail_on_discard ? "true" : "false");
	}
}

static void validate_runtime_config() {
	if (use_postgis) {
		if (!postgis_cfg.selected_columns_csv.empty() && split_csv_list(postgis_cfg.selected_columns_csv).empty()) {
			fprintf(stderr, "Error: --postgis-columns is empty after parsing\n");
			exit(EXIT_ARGS);
		}
		if (postgis_cfg.selected_columns_best_effort && postgis_cfg.selected_columns_csv.empty()) {
			fprintf(stderr, "Error: --postgis-columns-best-effort requires --postgis-columns\n");
			exit(EXIT_ARGS);
		}

		if ((postgis_cfg.shard_mode == "key" || postgis_cfg.shard_mode == "range") && postgis_cfg.shard_key.empty()) {
			fprintf(stderr, "Error: --postgis-shard-key is required when --postgis-shard-mode=%s\n", postgis_cfg.shard_mode.c_str());
			exit(EXIT_ARGS);
		}

		if (!postgis_cfg.shard_key.empty() && postgis_cfg.shard_mode == "none" && !quiet) {
			fprintf(stderr, "Warning: --postgis-shard-key is ignored when --postgis-shard-mode=none\n");
		}
	}

	if (use_mongo && !mongo_cfg.fail_on_discard && !quiet) {
		fprintf(stderr, "Warning: MongoDB is running in best-effort mode (--mongo-no-fail-on-discard)\n");
	}
}

static void auto_adjust_mongo_batch_size(size_t total_features, int maxzoom) {
	if (mongo_cfg.batch_size != DEFAULT_MONGO_BATCH_SIZE) {
		return;
	}

	size_t estimated_tiles = MongoDB::estimate_tile_count(total_features, 0, maxzoom);
	size_t suggested_batch = MongoDB::suggest_batch_size(estimated_tiles);

	mongo_cfg.batch_size = suggested_batch;
	mongo_cfg.normalize();

	if (!quiet) {
		fprintf(stderr, "Auto-adjusted MongoDB batch size to %zu "
				"(processed %zu features, estimated %zu tiles)\n",
				mongo_cfg.batch_size, total_features, estimated_tiles);
	}
}

static void read_postgis_data(std::string &layername, const char *fname,
                              std::vector<struct reader> &readers,
                              std::atomic<long long> &progress_seq,
                              int *initialized, unsigned *initial_x, unsigned *initial_y,
                              std::vector<std::map<std::string, layermap_entry>> &layermaps,
                              long long overall_offset,
                              bool guess_maxzoom, int maxzoom, int basezoom,
                              std::set<std::string> *exclude, std::set<std::string> *include,
                              int exclude_all, bool uses_gamma, double gamma,
                              std::unordered_map<std::string, int> const *attribute_types,
                              const char *tmpdir,
                              const char *prefilter, const char *postfilter,
                              double &dist_sum, size_t &dist_count, double &area_sum) {
	(void) fname;
	(void) gamma;
	(void) tmpdir;

	if (!quiet) {
		fprintf(stderr, "Reading from PostGIS database: %s@%s:%s/%s\n",
			postgis_cfg.user.c_str(), postgis_cfg.host.c_str(), postgis_cfg.port.c_str(), postgis_cfg.dbname.c_str());
	}

	layermap_entry e = layermap_entry(0);
	e.description = "PostGIS layer";
	std::map<std::string, layermap_entry> layermap;
	layermap.insert(std::pair<std::string, layermap_entry>(layername, e));
	layermaps.clear();
	for (size_t l = 0; l < CPUS; l++) {
		layermaps.push_back(layermap);
	}

	std::vector<struct serialization_state> sst;
	sst.resize(CPUS);

	auto layer_seq_arr = std::make_unique<std::atomic<long long>[]>(CPUS);
	std::vector<double> dist_sums_v(CPUS, 0);
	std::vector<size_t> dist_counts_v(CPUS, 0);
	std::vector<double> area_sums_v(CPUS, 0);
	std::atomic<long long> *layer_seq = layer_seq_arr.get();
	double *dist_sums = dist_sums_v.data();
	size_t *dist_counts = dist_counts_v.data();
	double *area_sums = area_sums_v.data();

	for (size_t i = 0; i < CPUS; i++) {
		layer_seq[i].store(overall_offset);

		sst[i].fname = "PostGIS";
		sst[i].line = 0;
		sst[i].layer_seq = &layer_seq[i];
		sst[i].progress_seq = &progress_seq;
		sst[i].readers = &readers;
		sst[i].segment = i;
		sst[i].initial_x = &initial_x[i];
		sst[i].initial_y = &initial_y[i];
		sst[i].initialized = &initialized[i];
		sst[i].dist_sum = &dist_sums[i];
		sst[i].dist_count = &dist_counts[i];
		sst[i].area_sum = &area_sums[i];
		sst[i].want_dist = guess_maxzoom;
		sst[i].maxzoom = maxzoom;
		sst[i].filters = prefilter != NULL || postfilter != NULL;
		sst[i].uses_gamma = uses_gamma;
		sst[i].layermap = &layermaps[i];
		sst[i].exclude = exclude;
		sst[i].include = include;
		sst[i].exclude_all = exclude_all;
		sst[i].basezoom = basezoom;
		sst[i].attribute_types = attribute_types;
	}

	std::atomic<size_t> total_features{0};

	PostGIS::ParallelReader parallel_reader(postgis_cfg, CPUS);
	if (!parallel_reader.read_parallel(sst, 0, layername)) {
		fprintf(stderr, "Failed to read features from PostGIS\n");
		exit(EXIT_READ);
	}
	total_features = parallel_reader.get_total_features();

	auto_adjust_mongo_batch_size(total_features.load(), maxzoom);

	for (size_t i = 0; i < CPUS; i++) {
		dist_sum += dist_sums[i];
		dist_count += dist_counts[i];
		area_sum += area_sums[i];
	}

	checkdisk(&readers);
}

std::pair<int, metadata> read_input(std::string &layername, const char *fname, int maxzoom, int minzoom, int basezoom, double basezoom_marker_width, sqlite3 *outdb, const char *outdir, std::set<std::string> *exclude, std::set<std::string> *include, int exclude_all, json_object *filter, double droprate, int buffer, const char *tmpdir, double gamma, int forcetable, const char *attribution, bool uses_gamma, long long *file_bbox, long long *file_bbox1, long long *file_bbox2, const char *prefilter, const char *postfilter, const char *description, bool guess_maxzoom, bool guess_cluster_maxzoom, std::unordered_map<std::string, int> const *attribute_types, const char *pgm, std::unordered_map<std::string, attribute_op> const *attribute_accum, std::map<std::string, std::string> const &attribute_descriptions, std::string const &commandline, int minimum_maxzoom) {
	int ret = EXIT_SUCCESS;

	std::vector<struct reader> readers;
	readers.resize(CPUS);
	for (size_t i = 0; i < CPUS; i++) {
		struct reader *r = &readers[i];

		char poolname[strlen(tmpdir) + strlen("/pool.XXXXXXXX") + 1];
		char treename[strlen(tmpdir) + strlen("/tree.XXXXXXXX") + 1];
		char geomname[strlen(tmpdir) + strlen("/geom.XXXXXXXX") + 1];
		char indexname[strlen(tmpdir) + strlen("/index.XXXXXXXX") + 1];
		char vertexname[strlen(tmpdir) + strlen("/vertex.XXXXXXXX") + 1];
		char nodename[strlen(tmpdir) + strlen("/node.XXXXXXXX") + 1];

		snprintf(poolname, sizeof(poolname), "%s%s", tmpdir, "/pool.XXXXXXXX");
		snprintf(treename, sizeof(treename), "%s%s", tmpdir, "/tree.XXXXXXXX");
		snprintf(geomname, sizeof(geomname), "%s%s", tmpdir, "/geom.XXXXXXXX");
		snprintf(indexname, sizeof(indexname), "%s%s", tmpdir, "/index.XXXXXXXX");
		snprintf(vertexname, sizeof(vertexname), "%s%s", tmpdir, "/vertex.XXXXXXXX");
		snprintf(nodename, sizeof(nodename), "%s%s", tmpdir, "/node.XXXXXXXX");

		r->poolfd = mkstemp_cloexec(poolname);
		if (r->poolfd < 0) {
			perror(poolname);
			exit(EXIT_OPEN);
		}
		r->treefd = mkstemp_cloexec(treename);
		if (r->treefd < 0) {
			perror(treename);
			exit(EXIT_OPEN);
		}
		r->geomfd = mkstemp_cloexec(geomname);
		if (r->geomfd < 0) {
			perror(geomname);
			exit(EXIT_OPEN);
		}
		r->indexfd = mkstemp_cloexec(indexname);
		if (r->indexfd < 0) {
			perror(indexname);
			exit(EXIT_OPEN);
		}
		r->vertexfd = mkstemp_cloexec(vertexname);
		if (r->vertexfd < 0) {
			perror(vertexname);
			exit(EXIT_OPEN);
		}
		r->nodefd = mkstemp_cloexec(nodename);
		if (r->nodefd < 0) {
			perror(nodename);
			exit(EXIT_OPEN);
		}

		r->poolfile = memfile_open(r->poolfd);
		if (r->poolfile == NULL) {
			perror(poolname);
			exit(EXIT_OPEN);
		}
		r->treefile = memfile_open(r->treefd);
		if (r->treefile == NULL) {
			perror(treename);
			exit(EXIT_OPEN);
		}
		r->geomfile = fopen_oflag(geomname, "wb", O_WRONLY | O_CLOEXEC);
		if (r->geomfile == NULL) {
			perror(geomname);
			exit(EXIT_OPEN);
		}
		r->indexfile = fopen_oflag(indexname, "wb", O_WRONLY | O_CLOEXEC);
		if (r->indexfile == NULL) {
			perror(indexname);
			exit(EXIT_OPEN);
		}
		r->vertexfile = fopen_oflag(vertexname, "w+b", O_RDWR | O_CLOEXEC);
		if (r->vertexfile == NULL) {
			perror(("open vertexfile " + std::string(vertexname)).c_str());
			exit(EXIT_OPEN);
		}
		r->nodefile = fopen_oflag(nodename, "w+b", O_RDWR | O_CLOEXEC);
		if (r->nodefile == NULL) {
			perror(nodename);
			exit(EXIT_OPEN);
		}
		r->geompos = 0;
		r->indexpos = 0;
		r->vertexpos = 0;
		r->nodepos = 0;

		unlink(poolname);
		unlink(treename);
		unlink(geomname);
		unlink(indexname);
		unlink(vertexname);
		unlink(nodename);

		// To distinguish a null value
		{
			struct stringpool p;
			bool in_memory;
			memfile_write(r->treefile, &p, sizeof(struct stringpool), in_memory);
		}

		r->file_bbox[0] = r->file_bbox[1] = UINT_MAX;
		r->file_bbox[2] = r->file_bbox[3] = 0;
	}

	struct statfs fsstat;
	if (fstatfs(readers[0].geomfd, &fsstat) != 0) {
		perror("Warning: fstatfs");
		fprintf(stderr, "Tippecanoe cannot check whether disk space will run out during tiling.\n");
		diskfree = LLONG_MAX;
	} else {
		diskfree = (long long) fsstat.f_bsize * fsstat.f_bavail;
	}

	std::atomic<long long> progress_seq(0);

	size_t init_count = 2 * CPUS;
	std::vector<int> initialized_v(init_count, 0);
	std::vector<unsigned> initial_x_v(init_count, 0);
	std::vector<unsigned> initial_y_v(init_count, 0);
	int *initialized = initialized_v.data();
	unsigned *initial_x = initial_x_v.data();
	unsigned *initial_y = initial_y_v.data();

	std::map<std::string, layermap_entry> layermap;
	std::vector<std::map<std::string, layermap_entry> > layermaps;

	long overall_offset = 0;

	int files_open_before_reading = open(get_null_device(), O_RDONLY | O_CLOEXEC);
	if (files_open_before_reading < 0) {
		perror("open /dev/null");
		exit(EXIT_OPEN);
	}
	if (close(files_open_before_reading) != 0) {
		perror("close");
		exit(EXIT_CLOSE);
	}

	validate_postgis_config();
	validate_mongo_config();
	validate_runtime_config();

	double dist_sum = 0;
	size_t dist_count = 0;
	double area_sum = 0;

	read_postgis_data(layername, fname, readers, progress_seq,
	                  initialized, initial_x, initial_y,
	                  layermaps, overall_offset,
	                  guess_maxzoom, maxzoom, basezoom,
	                  exclude, include, exclude_all,
	                  uses_gamma, gamma, attribute_types,
	                  tmpdir, prefilter, postfilter,
	                  dist_sum, dist_count, area_sum);

	int files_open_after_reading = open(get_null_device(), O_RDONLY | O_CLOEXEC);
	if (files_open_after_reading < 0) {
		perror("open /dev/null");
		exit(EXIT_OPEN);
	}
	if (close(files_open_after_reading) != 0) {
		perror("close");
		exit(EXIT_CLOSE);
	}

	if (files_open_after_reading > files_open_before_reading) {
		fprintf(stderr, "Internal error: Files left open after reading input. (%d vs %d)\n",
			files_open_before_reading, files_open_after_reading);
		ret = EXIT_IMPOSSIBLE;
	}

	if (!quiet) {
		fprintf(stderr, "                              \r");
		//     (stderr, "Read 10000.00 million features\r", *progress_seq / 1000000.0);
		fflush(stderr);
	}

	std::atomic<long long> vertexpos(0);
	std::atomic<long long> nodepos(0);

	for (size_t i = 0; i < CPUS; i++) {
		if (fclose(readers[i].geomfile) != 0) {
			perror("fclose geom");
			exit(EXIT_CLOSE);
		}
		if (fclose(readers[i].indexfile) != 0) {
			perror("fclose index");
			exit(EXIT_CLOSE);
		}
		memfile_close(readers[i].treefile);

		if (fstat(readers[i].geomfd, &readers[i].geomst) != 0) {
			perror("stat geom\n");
			exit(EXIT_STAT);
		}

		vertexpos += readers[i].vertexpos;
		nodepos += readers[i].nodepos;
	}

	if (!quiet) {
		fprintf(stderr, "Merging string pool           \r");
	}

	// Create a combined string pool
	// but keep track of the offsets into it since we still need
	// segment+offset to find the data.

	size_t pool_count = 2 * CPUS;
	std::vector<long long> pool_off_v(pool_count, 0);
	long long *pool_off = pool_off_v.data();

	char poolname[strlen(tmpdir) + strlen("/pool.XXXXXXXX") + 1];
	snprintf(poolname, sizeof(poolname), "%s%s", tmpdir, "/pool.XXXXXXXX");

	int poolfd = mkstemp_cloexec(poolname);
	if (poolfd < 0) {
		perror(poolname);
		exit(EXIT_OPEN);
	}

	FILE *poolfile = fopen_oflag(poolname, "wb", O_WRONLY | O_CLOEXEC);
	if (poolfile == NULL) {
		perror(poolname);
		exit(EXIT_OPEN);
	}

	unlink(poolname);
	std::atomic<long long> poolpos(0);

	for (size_t i = 0; i < CPUS; i++) {
		// If the memfile is not done yet, it is in memory, so just copy the memory.
		// Otherwise, we need to merge memory and file.

		if (readers[i].poolfile->fp == NULL) {
			// still in memory

			if (readers[i].poolfile->map.size() > 0) {
				if (fwrite(readers[i].poolfile->map.c_str(), readers[i].poolfile->map.size(), 1, poolfile) != 1) {
					perror("Reunify string pool");
					exit(EXIT_WRITE);
				}
			}

			pool_off[i] = poolpos;
			poolpos += readers[i].poolfile->map.size();
		} else {
			// split into memory and file

			if (fflush(readers[i].poolfile->fp) != 0) {
				perror("fflush poolfile");
				exit(EXIT_WRITE);
			}

			char *s = (char *) mmap(NULL, readers[i].poolfile->off, PROT_READ, MAP_PRIVATE, readers[i].poolfile->fd, 0);
			if (s == MAP_FAILED) {
				perror("mmap string pool for copy");
				exit(EXIT_MEMORY);
			}
			madvise(s, readers[i].poolfile->off, MADV_SEQUENTIAL);
			if (fwrite(s, sizeof(char), readers[i].poolfile->off, poolfile) != readers[i].poolfile->off) {
				perror("Reunify string pool (split)");
				exit(EXIT_WRITE);
			}
			if (munmap(s, readers[i].poolfile->off) != 0) {
				perror("unmap string pool for copy");
				exit(EXIT_MEMORY);
			}

			pool_off[i] = poolpos;
			poolpos += readers[i].poolfile->off;
		}

		memfile_close(readers[i].poolfile);
	}

	if (fclose(poolfile) != 0) {
		perror("fclose pool");
		exit(EXIT_CLOSE);
	}

	char *stringpool = NULL;
	if (poolpos > 0) {  // Will be 0 if -X was specified
		stringpool = (char *) mmap(NULL, poolpos, PROT_READ, MAP_PRIVATE, poolfd, 0);
		if (stringpool == MAP_FAILED) {
			perror("mmap string pool");
			exit(EXIT_MEMORY);
		}
		madvise(stringpool, poolpos, MADV_RANDOM);
	}

	if (!quiet) {
		fprintf(stderr, "Merging vertices              \r");
	}

	// Sort the vertices;
	// find nodes where the same central point is part of two different vertices
	{
		std::string tmpname = std::string(tmpdir) + "/vertex2.XXXXXX";
		int vertexfd = mkstemp((char *) tmpname.c_str());
		if (vertexfd < 0) {
			perror(("mkstemp vertexfile " + std::string(tmpname)).c_str());
			exit(EXIT_OPEN);
		}
		unlink(tmpname.c_str());
		FILE *vertex_out = fdopen(vertexfd, "w+b");
		if (vertex_out == NULL) {
			perror(tmpname.c_str());
			exit(EXIT_OPEN);
		}

		std::vector<FILE *> vertex_readers;
		for (size_t i = 0; i < CPUS; i++) {
			vertex_readers.push_back(readers[i].vertexfile);
			rewind(readers[i].vertexfile);
		}
		fqsort(vertex_readers, sizeof(vertex), vertexcmp, vertex_out, memsize / 20, tmpdir);

		for (size_t i = 0; i < CPUS; i++) {
			if (fclose(readers[i].vertexfile) != 0) {
				perror("fclose vertex");
				exit(EXIT_CLOSE);
			}
		}

		rewind(vertex_out);

		vertex prev(draw(VT_MOVETO, 0, 0), draw(VT_MOVETO, 0, 0), draw(VT_MOVETO, 0, 0));
		vertex v(draw(VT_MOVETO, 0, 0), draw(VT_MOVETO, 0, 0), draw(VT_MOVETO, 0, 0));
		while (fread((void *) &v, sizeof(vertex), 1, vertex_out)) {
			if (v.mid == prev.mid && (v.p1 != prev.p1 || v.p2 != prev.p2)) {
				long long x = v.mid.x * (1LL << geometry_scale);
				long long y = v.mid.y * (1LL << geometry_scale);

#if 0
				double lon, lat;
				tile2lonlat(x, y, 32, &lon, &lat);
				printf("{\"type\":\"Feature\", \"properties\":{}, \"geometry\":{\"type\":\"Point\", \"coordinates\":[%f,%f]}}\n", lon, lat);
#endif

				struct node n;
				n.index = encode_vertex((unsigned) x, (unsigned) y);

				fwrite_check((char *) &n, sizeof(struct node), 1, readers[0].nodefile, &readers[0].nodepos, "vertices");
			}
			prev = v;
		}

		fclose(vertex_out);
	}

	if (!quiet) {
		fprintf(stderr, "Merging nodes                 \r");
	}

	std::string shared_nodes_bloom;
	shared_nodes_bloom.resize(34567891);  // circa 34MB, size nowhere near a power of 2

	// Sort nodes that can't be simplified away; scan the list to remove duplicates

	FILE *shared_nodes;
	node *shared_nodes_map = NULL;	// will be null if there are no shared nodes
	{
		// sort

		std::string tmpname = std::string(tmpdir) + "/node2.XXXXXX";
		int nodefd = mkstemp((char *) tmpname.c_str());
		if (nodefd < 0) {
			perror(("mkstemp nodefile " + std::string(tmpname)).c_str());
			exit(EXIT_OPEN);
		}
		unlink(tmpname.c_str());
		FILE *node_out;
		node_out = fdopen(nodefd, "w+b");
		if (node_out == NULL) {
			perror(tmpname.c_str());
			exit(EXIT_OPEN);
		}

		std::vector<FILE *> node_readers;
		for (size_t i = 0; i < CPUS; i++) {
			node_readers.push_back(readers[i].nodefile);
			rewind(readers[i].nodefile);
		}

		fqsort(node_readers, sizeof(node), nodecmp, node_out, memsize / 20, tmpdir);

		for (size_t i = 0; i < CPUS; i++) {
			if (fclose(readers[i].nodefile) != 0) {
				perror("fclose node");
				exit(EXIT_CLOSE);
			}
		}

		rewind(node_out);

		// scan

		tmpname = std::string(tmpdir) + "/node3.XXXXXX";
		nodefd = mkstemp((char *) tmpname.c_str());
		if (nodefd < 0) {
			perror(("mkstemp nodefile " + std::string(tmpname)).c_str());
			exit(EXIT_OPEN);
		}
		unlink(tmpname.c_str());
		shared_nodes = fdopen(nodefd, "w+b");
		if (shared_nodes == NULL) {
			perror(tmpname.c_str());
			exit(EXIT_OPEN);
		}

		// `written` is to see if this node has already been preserved
		// and doesn't need to be preserved again
		struct node written;
		written.index = ULONG_MAX;

		nodepos = 0;
		struct node here;
		while (fread((void *) &here, sizeof(here), 1, node_out)) {
			if (nodecmp((void *) &here, (void *) &written) != 0) {
				fwrite_check((void *) &here, sizeof(here), 1, shared_nodes, &nodepos, "shared nodes");
				written = here;

				size_t bloom_ix = here.index % (shared_nodes_bloom.size() * 8);
				unsigned char bloom_mask = 1 << (bloom_ix & 7);
				bloom_ix >>= 3;
				shared_nodes_bloom[bloom_ix] |= bloom_mask;

#if 0
				unsigned wx, wy;
				decode_quadkey(here.index, &wx, &wy);
				double lon, lat;
				tile2lonlat(wx, wy, 32, &lon, &lat);
				printf("{\"type\":\"Feature\", \"properties\":{}, \"geometry\":{\"type\":\"Point\", \"coordinates\":[%f,%f]}}\n", lon, lat);
#endif
			}
		}

		fflush(shared_nodes);

		if (nodepos > 0) {
			shared_nodes_map = (node *) mmap(NULL, nodepos, PROT_READ, MAP_PRIVATE, nodefd, 0);
			if (shared_nodes_map == (node *) MAP_FAILED) {
				perror("mmap nodes");
				exit(EXIT_MEMORY);
			}
		}

		fclose(node_out);
	}

	if (!quiet) {
		fprintf(stderr, "Merging index                 \r");
	}

	char indexname[strlen(tmpdir) + strlen("/index.XXXXXXXX") + 1];
	snprintf(indexname, sizeof(indexname), "%s%s", tmpdir, "/index.XXXXXXXX");

	int indexfd = mkstemp_cloexec(indexname);
	if (indexfd < 0) {
		perror(indexname);
		exit(EXIT_OPEN);
	}
	FILE *indexfile = fopen_oflag(indexname, "wb", O_WRONLY | O_CLOEXEC);
	if (indexfile == NULL) {
		perror(indexname);
		exit(EXIT_OPEN);
	}

	unlink(indexname);

	char geomname[strlen(tmpdir) + strlen("/geom.XXXXXXXX") + 1];
	snprintf(geomname, sizeof(geomname), "%s%s", tmpdir, "/geom.XXXXXXXX");

	int geomfd = mkstemp_cloexec(geomname);
	if (geomfd < 0) {
		perror(geomname);
		exit(EXIT_CLOSE);
	}
	FILE *geomfile = fopen_oflag(geomname, "wb", O_WRONLY | O_CLOEXEC);
	if (geomfile == NULL) {
		perror(geomname);
		exit(EXIT_OPEN);
	}
	unlink(geomname);

	unsigned iz = 0, ix = 0, iy = 0;
	choose_first_zoom(file_bbox, file_bbox1, file_bbox2, readers, &iz, &ix, &iy, minzoom, buffer);

	if (justx >= 0) {
		iz = minzoom;
		ix = justx;
		iy = justy;
	}

	std::atomic<long long> geompos(0);

	/* initial tile is normally 0/0/0 but can be iz/ix/iy if limited to one tile */
	long long estimated_complexity = 0;  // to be replaced after writing the data
	fwrite_check(&estimated_complexity, sizeof(estimated_complexity), 1, geomfile, &geompos, fname);
	serialize_int(geomfile, iz, &geompos, fname);
	serialize_uint(geomfile, ix, &geompos, fname);
	serialize_uint(geomfile, iy, &geompos, fname);

	radix(readers, CPUS, geomfile, indexfile, tmpdir, &geompos, maxzoom, basezoom, droprate, gamma);

	/* end of tile */
	serialize_ulong_long(geomfile, 0, &geompos, fname);  // EOF

	estimated_complexity = geompos;
	fflush(geomfile);
	if (pwrite(fileno(geomfile), &estimated_complexity, sizeof(estimated_complexity), 0) != sizeof(estimated_complexity)) {
		perror("pwrite estimated complexity");
		exit(EXIT_WRITE);
	}

	if (fclose(geomfile) != 0) {
		perror("fclose geom");
		exit(EXIT_CLOSE);
	}
	if (fclose(indexfile) != 0) {
		perror("fclose index");
		exit(EXIT_CLOSE);
	}

	struct stat indexst;
	if (fstat(indexfd, &indexst) < 0) {
		perror("stat index");
		exit(EXIT_STAT);
	}
	std::atomic<long long> indexpos(indexst.st_size);
	progress_seq = indexpos / sizeof(struct index);

	last_progress = 0;
	if (!quiet) {
		long long s = progress_seq;
		long long geompos_print = geompos;
		long long poolpos_print = poolpos;
		long long vertexpos_print = vertexpos;
		long long nodepos_print = nodepos;
		fprintf(stderr, "%lld features, %lld bytes of geometry and attributes, %lld bytes of string pool, %lld bytes of vertices, %lld bytes of nodes\n", s, geompos_print, poolpos_print, vertexpos_print, nodepos_print);
	}

	if (indexpos == 0) {
		fprintf(stderr, "Did not read any valid geometries\n");
		if (outdb != NULL) {
			mbtiles_close(outdb, pgm);
		}
		exit(EXIT_NODATA);
	}

	struct index *map = (struct index *) mmap(NULL, indexpos, PROT_READ, MAP_PRIVATE, indexfd, 0);
	if (map == MAP_FAILED) {
		perror("mmap index for basezoom");
		exit(EXIT_MEMORY);
	}
	madvise(map, indexpos, MADV_SEQUENTIAL);
	madvise(map, indexpos, MADV_WILLNEED);
	long long indices = indexpos / sizeof(struct index);
	bool fix_dropping = false;

	if (guess_maxzoom) {
		double mean = 0;
		size_t count = 0;
		double m2 = 0;
		size_t dupes = 0;

		long long progress = -1;
		long long ip;
		for (ip = 1; ip < indices; ip++) {
			if (map[ip].ix != map[ip - 1].ix) {
#if 0
				// This #ifdef block provides data to empirically determine the relationship
				// between a difference in quadkey index and a ground distance in feet:
				//
				// $ ./tippecanoe --no-tile-size-limit -zg -f -o foo.mbtiles ne_10m_populated_places.json > /tmp/points
				// gnuplot> stats "/tmp/points" using (log($2)):(log($3))

				unsigned wx1, wy1, wx2, wy2;
				decode_quadkey(map[ip - 1].ix, &wx1, &wy1);
				decode_quadkey(map[ip].ix, &wx2, &wy2);

				double x1, y1, x2, y2;
				x1 = (wx1 * 360.0 / UINT_MAX - 180.0) / .00000274;
				y1 = (wy1 * 360.0 / UINT_MAX - 180.0) / .00000274;
				x2 = (wx2 * 360.0 / UINT_MAX - 180.0) / .00000274;
				y2 = (wy2 * 360.0 / UINT_MAX - 180.0) / .00000274;
				double dx = x1 - x2;
				double dy = y1 - y2;
				double d = sqrt(dx * dx + dy * dy);

				printf("%llu %llu %0.2f\n", map[ip].ix, map[ip].ix - map[ip - 1].ix, d);
#endif

				// https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm
				double newValue = log(map[ip].ix - map[ip - 1].ix);
				count++;
				double delta = newValue - mean;
				mean += delta / count;
				double delta2 = newValue - mean;
				m2 += delta * delta2;
			} else {
				dupes++;
			}

			long long nprogress = 100 * ip / indices;
			if (nprogress != progress) {
				progress = nprogress;
				if (!quiet && !quiet_progress && progress_time()) {
					fprintf(stderr, "Maxzoom: %lld%% \r", progress);
					fflush(stderr);
				}
			}
		}

		if (count == 0 && dist_count == 0 && minimum_maxzoom == 0) {
			fprintf(stderr, "Can't guess maxzoom (-zg) without at least two distinct feature locations\n");
			if (outdb != NULL) {
				mbtiles_close(outdb, pgm);
			}
			exit(EXIT_NODATA);
		}

		if (count == 0 && dist_count == 0) {
			maxzoom = minimum_maxzoom;
			if (droprate < 0) {
				droprate = 1;
			}
		} else if (count > 0) {
			double stddev = sqrt(m2 / count);

			// Geometric mean is appropriate because distances between features
			// are typically lognormally distributed. Two standard deviations
			// below the mean should be enough to distinguish most features.
			double avg = exp(mean);
			double nearby = exp(mean - 1.5 * stddev);

			// Convert approximately from tile units to feet.
			// See empirical data above for source
			double dist_ft = sqrt(avg) / 33;
			double nearby_ft = sqrt(nearby) / 33;

			// Go one zoom level beyond what is strictly necessary for nearby features.
			double want = nearby_ft / 2;

			maxzoom = ceil(log(360 / (.00000274 * want)) / log(2) - full_detail);
			if (maxzoom < 0) {
				maxzoom = 0;
			}
			if (!quiet) {
				fprintf(stderr,
					"Choosing a maxzoom of -z%d for features typically %d feet (%d meters) apart, ",
					maxzoom,
					(int) ceil(dist_ft), (int) ceil(dist_ft / 3.28084));
				fprintf(stderr, "and at least %d feet (%d meters) apart\n",
					(int) ceil(nearby_ft), (int) ceil(nearby_ft / 3.28084));
			}

			bool changed = false;
			while (maxzoom < 32 - full_detail && maxzoom < 33 - low_detail && maxzoom < cluster_maxzoom && cluster_distance > 0) {
				unsigned long long zoom_mingap = ((1LL << (32 - maxzoom)) / 256 * cluster_distance) * ((1LL << (32 - maxzoom)) / 256 * cluster_distance);
				if (avg > zoom_mingap) {
					break;
				}

				maxzoom++;
				changed = true;
			}
			if (changed) {
				printf("Choosing a maxzoom of -z%d to keep most features distinct with cluster distance %d and cluster maxzoom %d\n", maxzoom, cluster_distance, cluster_maxzoom);
			}

			if (droprate == -3) {
				// This mysterious formula is the result of eyeballing the appropriate drop rate
				// for several point tilesets using -zg and then fitting a curve to the pattern
				// that emerged. It appears that if the standard deviation of the distances between
				// features is small, the drop rate should be large because the features are evenly
				// spaced, and if the standard deviation is large, the drop rate can be small because
				// the features are in clumps.
				droprate = round_droprate(exp(-0.7681 * log(stddev) + 1.582));

				if (droprate < 0) {
					droprate = 0;
				}

				if (!quiet) {
					fprintf(stderr, "Choosing a drop rate of %f\n", droprate);
				}

				if (dupes != 0 && droprate != 0) {
					maxzoom += std::round(log((dupes + count) / count) / log(droprate));
					if (!quiet) {
						fprintf(stderr, "Increasing maxzoom to %d to account for %zu duplicate feature locations\n", maxzoom, dupes);
					}
				}
			}
		}

		if (dist_count != 0) {
			// no conversion to pseudo-feet here because that already happened within each feature
			double want2 = exp(dist_sum / dist_count) / 8;
			int mz = ceil(log(360 / (.00000274 * want2)) / log(2) - full_detail);

			if (mz > maxzoom || count <= 0) {
				if (!quiet) {
					fprintf(stderr, "Choosing a maxzoom of -z%d for resolution of about %d feet (%d meters) within features\n", mz, (int) exp(dist_sum / dist_count), (int) (exp(dist_sum / dist_count) / 3.28084));
				}
				maxzoom = mz;
			}
		}

		if (maxzoom < 0) {
			maxzoom = 0;
		}
		if (maxzoom > 32 - full_detail) {
			maxzoom = 32 - full_detail;
		}
		if (maxzoom > 33 - low_detail) {  // that is, maxzoom - 1 > 32 - low_detail
			maxzoom = 33 - low_detail;
		}

		double total_tile_count = 0;
		for (int i = 1; i <= maxzoom; i++) {
			double tile_count = ceil(area_sum / ((1LL << (32 - i)) * (1LL << (32 - i))));
			total_tile_count += tile_count;

			// 2M tiles is an arbitrary limit, chosen to make tiling jobs
			// that seem like they should finish in a few minutes
			// actually finish in a few minutes. It is large enough to
			// tile a polygon that covers the entire world to z10
			// or the United States to z13.

			if (total_tile_count > 2 * 1024 * 1024) {
				printf("Limiting maxzoom to -z%d to keep from generating %lld tiles\n", i - 1, (long long) total_tile_count);
				maxzoom = i - 1;
				break;
			}
		}

		if (basezoom == -2 && basezoom_marker_width == 1) {  // -Bg, not -Bg###
			basezoom = maxzoom;
			if (!quiet) {
				fprintf(stderr, "Using base zoom of -z%d\n", basezoom);
			}
		}

		if (maxzoom < minimum_maxzoom) {
			if (!quiet) {
				fprintf(stderr, "Using minimum maxzoom of -z%d\n", minimum_maxzoom);
			}
			maxzoom = minimum_maxzoom;
		}

		if (maxzoom < minzoom) {
			if (!quiet) {
				fprintf(stderr, "Can't use %d for maxzoom because minzoom is %d\n", maxzoom, minzoom);
			}
			maxzoom = minzoom;
		}

		fix_dropping = true;

		if (basezoom == -1) {  // basezoom unspecified
			basezoom = maxzoom;
		}
	}

	if (cluster_maxzoom >= maxzoom && guess_cluster_maxzoom) {
		cluster_maxzoom = maxzoom - 1;
		fprintf(stderr, "Choosing a cluster maxzoom of -k%d to make all features visible at maximum zoom %d\n", cluster_maxzoom, maxzoom);
	}

	if (basezoom < 0 || droprate < 0) {
		struct tile {
			unsigned x;
			unsigned y;
			long long count;
			long long fullcount;
			double gap;
			unsigned long long previndex;
		} tile[MAX_ZOOM + 1], max[MAX_ZOOM + 1];

		{
			int z;
			for (z = 0; z <= MAX_ZOOM; z++) {
				tile[z].x = tile[z].y = tile[z].count = tile[z].fullcount = tile[z].gap = tile[z].previndex = 0;
				max[z].x = max[z].y = max[z].count = max[z].fullcount = 0;
			}
		}

		long long progress = -1;

		long long ip;
		for (ip = 0; ip < indices; ip++) {
			unsigned xx, yy;
			decode_index(map[ip].ix, &xx, &yy);

			long long nprogress = 100 * ip / indices;
			if (nprogress != progress) {
				progress = nprogress;
				if (!quiet && !quiet_progress && progress_time()) {
					fprintf(stderr, "Base zoom/drop rate: %lld%% \r", progress);
					fflush(stderr);
				}
			}

			int z;
			for (z = 0; z <= MAX_ZOOM; z++) {
				unsigned xxx = 0, yyy = 0;
				if (z != 0) {
					// These are tile numbers, not pixels,
					// so shift, not round
					xxx = xx >> (32 - z);
					yyy = yy >> (32 - z);
				}

				double scale = (double) (1LL << (64 - 2 * (z + 8)));

				if (tile[z].x != xxx || tile[z].y != yyy) {
					if (tile[z].count > max[z].count) {
						max[z] = tile[z];
					}

					tile[z].x = xxx;
					tile[z].y = yyy;
					tile[z].count = 0;
					tile[z].fullcount = 0;
					tile[z].gap = 0;
					tile[z].previndex = 0;
				}

				tile[z].fullcount++;

				if (manage_gap(map[ip].ix, &tile[z].previndex, scale, gamma, &tile[z].gap)) {
					continue;
				}

				tile[z].count++;
			}
		}

		int z;
		for (z = MAX_ZOOM; z >= 0; z--) {
			if (tile[z].count > max[z].count) {
				max[z] = tile[z];
			}
		}

		int max_features = 50000 / (basezoom_marker_width * basezoom_marker_width);

		int obasezoom = basezoom;
		if (basezoom < 0) {
			basezoom = MAX_ZOOM;

			for (z = MAX_ZOOM; z >= 0; z--) {
				if (max[z].count < max_features) {
					basezoom = z;
				}

				// printf("%d/%u/%u %lld\n", z, max[z].x, max[z].y, max[z].count);
			}

			if (!quiet) {
				fprintf(stderr, "Choosing a base zoom of -B%d to keep %lld features in tile %d/%u/%u.\n", basezoom, max[basezoom].count, basezoom, max[basezoom].x, max[basezoom].y);
			}
		}

		if (obasezoom < 0 && basezoom > maxzoom && prevent[P_BASEZOOM_ABOVE_MAXZOOM]) {
			basezoom = maxzoom;
		}

		if (obasezoom < 0 && basezoom > maxzoom) {
			fprintf(stderr, "Couldn't find a suitable base zoom. Working from the other direction.\n");
			if (gamma == 0) {
				fprintf(stderr, "You might want to try -g1 to limit near-duplicates.\n");
			}

			if (droprate < 0) {
				if (maxzoom == 0) {
					droprate = 2.5;
				} else {
					droprate = round_droprate(exp(log((double) max[0].count / max[maxzoom].count) / (maxzoom)));
					if (!quiet) {
						fprintf(stderr, "Choosing a drop rate of -r%f to get from %lld to %lld in %d zooms\n", droprate, max[maxzoom].count, max[0].count, maxzoom);
					}
				}
			}

			basezoom = 0;
			for (z = 0; z <= maxzoom; z++) {
				double zoomdiff = log((double) max[z].count / max_features) / log(droprate);
				if (zoomdiff + z > basezoom) {
					basezoom = ceil(zoomdiff + z);
				}
			}

			if (!quiet) {
				fprintf(stderr, "Choosing a base zoom of -B%d to keep %f features in tile %d/%u/%u.\n", basezoom, max[maxzoom].count * exp(log(droprate) * (maxzoom - basezoom)), maxzoom, max[maxzoom].x, max[maxzoom].y);
			}
		} else if (droprate < 0) {
			droprate = 1;

			for (z = basezoom - 1; z >= 0; z--) {
				double interval = exp(log(droprate) * (basezoom - z));

				if (max[z].count / interval >= max_features) {
					interval = (double) max[z].count / max_features;
					droprate = round_droprate(exp(log(interval) / (basezoom - z)));
					interval = exp(log(droprate) * (basezoom - z));

					if (!quiet) {
						fprintf(stderr, "Choosing a drop rate of -r%f to keep %f features in tile %d/%u/%u.\n", droprate, max[z].count / interval, z, max[z].x, max[z].y);
					}
				}
			}
		}

		if (gamma > 0) {
			int effective = 0;

			for (z = 0; z < maxzoom; z++) {
				if (max[z].count < max[z].fullcount) {
					effective = z + 1;
				}
			}

			if (effective == 0) {
				if (!quiet) {
					fprintf(stderr, "With gamma, effective base zoom is 0, so no effective drop rate\n");
				}
			} else {
				double interval_0 = exp(log(droprate) * (basezoom - 0));
				double interval_eff = exp(log(droprate) * (basezoom - effective));
				if (effective > basezoom) {
					interval_eff = 1;
				}

				double scaled_0 = max[0].count / interval_0;
				double scaled_eff = max[effective].count / interval_eff;

				double rate_at_0 = scaled_0 / max[0].fullcount;
				double rate_at_eff = scaled_eff / max[effective].fullcount;

				double eff_drop = exp(log(rate_at_eff / rate_at_0) / (effective - 0));

				if (!quiet) {
					fprintf(stderr, "With gamma, effective base zoom of %d, effective drop rate of %f\n", effective, eff_drop);
				}
			}
		}

		fix_dropping = true;
	}

	if (fix_dropping || drop_denser > 0) {
		// Fix up the minzooms for features, now that we really know the base zoom
		// and drop rate.

		struct stat geomst;
		if (fstat(geomfd, &geomst) != 0) {
			perror("stat sorted geom\n");
			exit(EXIT_STAT);
		}
		char *geom = (char *) mmap(NULL, geomst.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, geomfd, 0);
		if (geom == MAP_FAILED) {
			perror("mmap geom for fixup");
			exit(EXIT_MEMORY);
		}
		madvise(geom, indexpos, MADV_SEQUENTIAL);
		madvise(geom, indexpos, MADV_WILLNEED);

		std::vector<struct drop_state> ds_v(maxzoom + 1);
		struct drop_state *ds = ds_v.data();
		prep_drop_states(ds, maxzoom, basezoom, droprate);

		if (drop_denser > 0) {
			std::vector<drop_densest> ddv;
			unsigned long long previndex = 0;

			for (long long ip = 0; ip < indices; ip++) {
				if (map[ip].t == VT_POINT ||
				    (additional[A_LINE_DROP] && map[ip].t == VT_LINE) ||
				    (additional[A_POLYGON_DROP] && map[ip].t == VT_POLYGON)) {
					if (map[ip].ix % 100 < drop_denser) {
						drop_densest dd;
						dd.gap = map[ip].ix - previndex;
						dd.seq = ip;
						ddv.push_back(dd);

						previndex = map[ip].ix;
					} else {
						int feature_minzoom = calc_feature_minzoom(&map[ip], ds, maxzoom, gamma);
						geom[map[ip].end - 1] = feature_minzoom;
					}
				}
			}

			std::stable_sort(ddv.begin(), ddv.end());

			size_t i = 0;
			for (int z = 0; z <= basezoom; z++) {
				double keep_fraction = 1.0 / std::exp(std::log(droprate) * (basezoom - z));
				size_t keep_count = ddv.size() * keep_fraction;

				for (; i < keep_count && i < ddv.size(); i++) {
					geom[map[ddv[i].seq].end - 1] = z;
				}
			}
			for (; i < ddv.size(); i++) {
				geom[map[ddv[i].seq].end - 1] = basezoom;
			}
		} else {
			for (long long ip = 0; ip < indices; ip++) {
				if (ip > 0 && map[ip].start != map[ip - 1].end) {
					fprintf(stderr, "Mismatched index at %lld: %lld vs %lld\n", ip, map[ip].start, map[ip].end);
				}
				int feature_minzoom = calc_feature_minzoom(&map[ip], ds, maxzoom, gamma);
				geom[map[ip].end - 1] = feature_minzoom;
			}
		}

		munmap(geom, geomst.st_size);
	}

	madvise(map, indexpos, MADV_DONTNEED);
	munmap(map, indexpos);

	if (close(indexfd) != 0) {
		perror("close sorted index");
	}

	/* Traverse and split the geometries for each zoom level */

	struct stat geomst;
	if (fstat(geomfd, &geomst) != 0) {
		perror("stat sorted geom\n");
		exit(EXIT_STAT);
	}

	int fd[TEMP_FILES];
	off_t size[TEMP_FILES];

	fd[0] = geomfd;
	size[0] = geomst.st_size;

	for (size_t j = 1; j < TEMP_FILES; j++) {
		fd[j] = -1;
		size[j] = 0;
	}

	std::atomic<unsigned> midx(0);
	std::atomic<unsigned> midy(0);
	std::vector<strategy> strategies;
	int written = traverse_zooms(fd, size, stringpool, &midx, &midy, maxzoom, minzoom, outdb, outdir, buffer, fname, tmpdir, gamma, full_detail, low_detail, min_detail, pool_off, initial_x, initial_y, simplification, maxzoom_simplification, layermaps, prefilter, postfilter, attribute_accum, filter, strategies, iz, shared_nodes_map, nodepos, shared_nodes_bloom, basezoom, droprate, unidecode_data);

	if (maxzoom != written) {
		if (written > minzoom) {
			fprintf(stderr, "\n\n\n*** NOTE TILES ONLY COMPLETE THROUGH ZOOM %d ***\n", written);
			maxzoom = written;
			ret = EXIT_INCOMPLETE;
		} else {
			fprintf(stderr, "%s: No zoom levels were successfully written\n", *av);
			exit(EXIT_NODATA);
		}
	}

	if (poolpos > 0) {
		madvise((void *) stringpool, poolpos, MADV_DONTNEED);
		if (munmap(stringpool, poolpos) != 0) {
			perror("munmap stringpool");
		}
	}
	if (close(poolfd) < 0) {
		perror("close pool");
	}

	fclose(shared_nodes);

	// mbtiles-style bounding box and center
	double minlat = 0, minlon = 0, maxlat = 0, maxlon = 0, midlat = 0, midlon = 0;

	tile2lonlat(midx, midy, maxzoom, &minlon, &maxlat);
	tile2lonlat(midx + 1, midy + 1, maxzoom, &maxlon, &minlat);

	midlat = (maxlat + minlat) / 2;
	midlon = (maxlon + minlon) / 2;

	tile2lonlat(file_bbox[0], file_bbox[1], 32, &minlon, &maxlat);
	tile2lonlat(file_bbox[2], file_bbox[3], 32, &maxlon, &minlat);

	if (midlat < minlat) {
		midlat = minlat;
	}
	if (midlat > maxlat) {
		midlat = maxlat;
	}
	if (midlon < minlon) {
		midlon = minlon;
	}
	if (midlon > maxlon) {
		midlon = maxlon;
	}

	// antimeridian-aware bounding box
	double minlat2 = 0, minlon2 = 0, maxlat2 = 0, maxlon2 = 0;
	// choose whichever of the two calculated bboxes is narrower
	if (file_bbox2[2] - file_bbox2[0] < file_bbox1[2] - file_bbox1[0]) {
		tile2lonlat(file_bbox2[0], file_bbox2[1], 32, &minlon2, &maxlat2);
		tile2lonlat(file_bbox2[2], file_bbox2[3], 32, &maxlon2, &minlat2);
	} else {
		tile2lonlat(file_bbox1[0], file_bbox1[1], 32, &minlon2, &maxlat2);
		tile2lonlat(file_bbox1[2], file_bbox1[3], 32, &maxlon2, &minlat2);
	}

	std::map<std::string, layermap_entry> merged_lm = merge_layermaps(layermaps);

	for (auto ai = merged_lm.begin(); ai != merged_lm.end(); ++ai) {
		ai->second.minzoom = minzoom;
		ai->second.maxzoom = maxzoom;
	}

	metadata m = make_metadata(fname, minzoom, maxzoom, minlat, minlon, maxlat, maxlon, minlat2, minlon2, maxlat2, maxlon2, midlat, midlon, attribution, merged_lm, true, description, !prevent[P_TILE_STATS], attribute_descriptions, "tippecanoe-db", commandline, strategies, basezoom, droprate, retain_points_multiplier);
	if (outdb != NULL) {
		mbtiles_write_metadata(outdb, m, forcetable);
	} else if (outdir != NULL) {
		dir_write_metadata(outdir, m);
	}

	return std::make_pair(ret, m);
}

static bool has_name(struct option *long_options, int *pl) {
	for (size_t lo = 0; long_options[lo].name != NULL; lo++) {
		if (long_options[lo].flag == pl) {
			return true;
		}
	}

	return false;
}

void set_attribute_type(std::unordered_map<std::string, int> &attribute_types, const char *arg) {
	const char *s = strchr(arg, ':');
	if (s == NULL) {
		fprintf(stderr, "-T%s option must be in the form -Tname:type\n", arg);
		exit(EXIT_ARGS);
	}

	std::string name = std::string(arg, s - arg);
	std::string type = std::string(s + 1);
	int t = -1;

	if (type == "int") {
		t = mvt_int;
	} else if (type == "float") {
		t = mvt_float;
	} else if (type == "string") {
		t = mvt_string;
	} else if (type == "bool") {
		t = mvt_bool;
	} else {
		fprintf(stderr, "Attribute type (%s) must be int, float, string, or bool\n", type.c_str());
		exit(EXIT_ARGS);
	}

	attribute_types.insert(std::pair<std::string, int>(name, t));
}

void set_attribute_value(const char *arg) {
	if (*arg == '{') {
		json_pull *jp = json_begin_string(arg);
		json_object *o = json_read_tree(jp);

		if (o == NULL) {
			fprintf(stderr, "%s: --set-attribute %s: %s\n", *av, arg, jp->error);
			exit(EXIT_JSON);
		}

		if (o->type != JSON_HASH) {
			fprintf(stderr, "%s: --set-attribute %s: not a JSON object\n", *av, arg);
			exit(EXIT_JSON);
		}

		for (size_t i = 0; i < o->value.object.length; i++) {
			json_object *k = o->value.object.keys[i];
			json_object *v = o->value.object.values[i];

			if (k->type != JSON_STRING) {
				fprintf(stderr, "%s: --set-attribute %s: key %zu not a string\n", *av, arg, i);
				exit(EXIT_JSON);
			}

			serial_val val = stringify_value(v, "json", 1, o);
			set_attributes.emplace(k->value.string.string, val);
		}

		json_free(o);
		json_end(jp);
		return;
	}

	const char *s = strchr(arg, ':');
	if (s == NULL) {
		fprintf(stderr, "--set-attribute %s option must be in the form --set-attribute name:value\n", arg);
		exit(EXIT_ARGS);
	}

	std::string name = std::string(arg, s - arg);
	std::string value = std::string(s + 1);

	serial_val val;
	if (isdigit(value[0]) || value[0] == '-') {
		val.type = mvt_double;
	} else {
		val.type = mvt_string;
	}

	val.s = value;
	set_attributes.insert(std::pair<std::string, serial_val>(name, val));
}


int maindb(int argc, char **argv) {
	DEBUG_LOG("Entering maindb()");
	fflush(stderr);
#ifdef MTRACE
	mtrace();
#endif

	av = argv;
	init_cpus();

	if (CPUS > MAX_POSTGRES_CONNECTIONS) {
		fprintf(stderr, "Limiting CPUS from %zu to %zu to control PostgreSQL connections\n",
		        CPUS, MAX_POSTGRES_CONNECTIONS);
		CPUS = MAX_POSTGRES_CONNECTIONS;
	}

	extern char *optarg;
	int i;

	char *out_mbtiles = NULL;
	char *out_dir = NULL;
	sqlite3 *outdb = NULL;
	int maxzoom = 14;
	int minzoom = 0;
	int basezoom = -1;
	double basezoom_marker_width = 1;
	int force = 0;
	int forcetable = 0;
	double droprate = 2.5;
	double gamma = 0;
	int buffer = 5;
	const char *tmpdir = "/tmp";
	const char *prefilter = NULL;
	const char *postfilter = NULL;
	bool guess_maxzoom = false;
	int minimum_maxzoom = 0;
	bool guess_cluster_maxzoom = false;

	std::set<std::string> exclude, include;
	std::unordered_map<std::string, int> attribute_types;
	std::unordered_map<std::string, attribute_op> attribute_accum;
	std::map<std::string, std::string> attribute_descriptions;
	int exclude_all = 0;
	int files_open_at_start;
	json_object *filter = NULL;

	memsize = calc_memsize();

	for (i = 0; i < 256; i++) {
		prevent[i] = 0;
		additional[i] = 0;
	}

	static struct option long_options_orig[] = {
		{"Output tileset", 0, 0, 0},
		{"output", required_argument, 0, 'o'},
		{"output-to-directory", required_argument, 0, 'e'},
		{"force", no_argument, 0, 'f'},
		{"allow-existing", no_argument, 0, 'F'},

		{"PostGIS database connection", 0, 0, 0},
		{"postgis", required_argument, 0, '~'},
		{"postgis-host", required_argument, 0, '~'},
		{"postgis-port", required_argument, 0, '~'},
		{"postgis-dbname", required_argument, 0, '~'},
		{"postgis-user", required_argument, 0, '~'},
		{"postgis-password", required_argument, 0, '~'},
		{"postgis-table", required_argument, 0, '~'},
		{"postgis-geometry-field", required_argument, 0, '~'},
		{"postgis-sql", required_argument, 0, '~'},
		{"postgis-columns", required_argument, 0, '~'},
		{"postgis-columns-best-effort", no_argument, 0, '~'},
		{"postgis-canonical-attr-order", no_argument, 0, '~'},
		{"postgis-no-canonical-attr-order", no_argument, 0, '~'},
		{"postgis-profile", no_argument, 0, '~'},
		{"postgis-shard-key", required_argument, 0, '~'},
		{"postgis-shard-mode", required_argument, 0, '~'},
		{"postgis-progress-count", no_argument, 0, '~'},

		{"MongoDB output", 0, 0, 0},
		{"mongo", required_argument, 0, '~'},
		{"mongo-host", required_argument, 0, '~'},
		{"mongo-port", required_argument, 0, '~'},
		{"mongo-dbname", required_argument, 0, '~'},
		{"mongo-collection", required_argument, 0, '~'},
		{"mongo-username", required_argument, 0, '~'},
		{"mongo-password", required_argument, 0, '~'},
		{"mongo-auth-source", required_argument, 0, '~'},
		{"mongo-batch-size", required_argument, 0, '~'},
		{"mongo-pool-size", required_argument, 0, '~'},
		{"mongo-timeout", required_argument, 0, '~'},
		{"mongo-no-indexes", no_argument, 0, '~'},
		{"mongo-drop-collection", no_argument, 0, '~'},
		{"mongo-metadata", no_argument, 0, '~'},
		{"mongo-fail-on-discard", no_argument, 0, '~'},
		{"mongo-no-fail-on-discard", no_argument, 0, '~'},

		{"Projection of input", 0, 0, 0},
		{"projection", required_argument, 0, 's'},

		{"Zoom levels", 0, 0, 0},
		{"maximum-zoom", required_argument, 0, 'z'},
		{"minimum-zoom", required_argument, 0, 'Z'},
		{"smallest-maximum-zoom-guess", required_argument, 0, '~'},
		{"extend-zooms-if-still-dropping", no_argument, &additional[A_EXTEND_ZOOMS], 1},
		{"extend-zooms-if-still-dropping-maximum", required_argument, 0, '~'},
		{"generate-variable-depth-tile-pyramid", no_argument, &additional[A_VARIABLE_DEPTH_PYRAMID], 1},
		{"one-tile", required_argument, 0, 'R'},

		{"Tile resolution", 0, 0, 0},
		{"full-detail", required_argument, 0, 'd'},
		{"low-detail", required_argument, 0, 'D'},
		{"minimum-detail", required_argument, 0, 'm'},
		{"extra-detail", required_argument, 0, '~'},

		{"Filtering feature attributes", 0, 0, 0},
		{"exclude", required_argument, 0, 'x'},
		{"include", required_argument, 0, 'y'},
		{"exclude-all", no_argument, 0, 'X'},

		{"Modifying feature attributes", 0, 0, 0},
		{"attribute-type", required_argument, 0, 'T'},
		{"attribute-description", required_argument, 0, 'Y'},
		{"accumulate-attribute", required_argument, 0, 'E'},
		{"convert-stringified-ids-to-numbers", no_argument, &additional[A_CONVERT_NUMERIC_IDS], 1},
		{"use-attribute-for-id", required_argument, 0, '~'},
		{"single-precision", no_argument, &prevent[P_SINGLE_PRECISION], 1},
		{"set-attribute", required_argument, 0, '~'},
		{"maximum-string-attribute-length", required_argument, 0, '~'},

		{"Filtering features by attributes", 0, 0, 0},
		{"feature-filter-file", required_argument, 0, 'J'},
		{"feature-filter", required_argument, 0, 'j'},
		{"unidecode-data", required_argument, 0, '~'},

		{"Dropping a fixed fraction of features by zoom level", 0, 0, 0},
		{"drop-rate", required_argument, 0, 'r'},
		{"retain-points-multiplier", required_argument, 0, '~'},
		{"base-zoom", required_argument, 0, 'B'},
		{"drop-denser", required_argument, 0, '~'},
		{"limit-base-zoom-to-maximum-zoom", no_argument, &prevent[P_BASEZOOM_ABOVE_MAXZOOM], 1},
		{"drop-lines", no_argument, &additional[A_LINE_DROP], 1},
		{"drop-polygons", no_argument, &additional[A_POLYGON_DROP], 1},
		{"cluster-distance", required_argument, 0, 'K'},
		{"cluster-maxzoom", required_argument, 0, 'k'},
		{"preserve-point-density-threshold", required_argument, 0, '~'},
		{"preserve-multiplier-density-threshold", required_argument, 0, '~'},

		{"Dropping or merging a fraction of features to keep under tile size limits", 0, 0, 0},
		{"drop-densest-as-needed", no_argument, &additional[A_DROP_DENSEST_AS_NEEDED], 1},
		{"drop-fraction-as-needed", no_argument, &additional[A_DROP_FRACTION_AS_NEEDED], 1},
		{"drop-smallest-as-needed", no_argument, &additional[A_DROP_SMALLEST_AS_NEEDED], 1},
		{"coalesce-densest-as-needed", no_argument, &additional[A_COALESCE_DENSEST_AS_NEEDED], 1},
		{"coalesce-fraction-as-needed", no_argument, &additional[A_COALESCE_FRACTION_AS_NEEDED], 1},
		{"coalesce-smallest-as-needed", no_argument, &additional[A_COALESCE_SMALLEST_AS_NEEDED], 1},
		{"force-feature-limit", no_argument, &prevent[P_DYNAMIC_DROP], 1},
		{"cluster-densest-as-needed", no_argument, &additional[A_CLUSTER_DENSEST_AS_NEEDED], 1},
		{"keep-point-cluster-position", no_argument, &additional[A_KEEP_POINT_CLUSTER_POSITION], 1},

		{"Dropping tightly overlapping features", 0, 0, 0},
		{"gamma", required_argument, 0, 'g'},
		{"increase-gamma-as-needed", no_argument, &additional[A_INCREASE_GAMMA_AS_NEEDED], 1},

		{"Line and polygon simplification", 0, 0, 0},
		{"simplification", required_argument, 0, 'S'},
		{"no-line-simplification", no_argument, &prevent[P_SIMPLIFY], 1},
		{"simplify-only-low-zooms", no_argument, &prevent[P_SIMPLIFY_LOW], 1},
		{"simplification-at-maximum-zoom", required_argument, 0, '~'},
		{"no-tiny-polygon-reduction", no_argument, &prevent[P_TINY_POLYGON_REDUCTION], 1},
		{"no-tiny-polygon-reduction-at-maximum-zoom", no_argument, &prevent[P_TINY_POLYGON_REDUCTION_AT_MAXZOOM], 1},
		{"tiny-polygon-size", required_argument, 0, '~'},
		{"no-simplification-of-shared-nodes", no_argument, &prevent[P_SIMPLIFY_SHARED_NODES], 1},
		{"visvalingam", no_argument, &additional[A_VISVALINGAM], 1},

		{"Attempts to improve shared polygon boundaries", 0, 0, 0},
		{"detect-shared-borders", no_argument, &additional[A_DETECT_SHARED_BORDERS], 1},
		{"grid-low-zooms", no_argument, &additional[A_GRID_LOW_ZOOMS], 1},

		{"Controlling clipping to tile boundaries", 0, 0, 0},
		{"buffer", required_argument, 0, 'b'},
		{"no-clipping", no_argument, &prevent[P_CLIPPING], 1},
		{"no-duplication", no_argument, &prevent[P_DUPLICATION], 1},

		{"Reordering features within each tile", 0, 0, 0},
		{"preserve-input-order", no_argument, &prevent[P_INPUT_ORDER], 1},
		{"reorder", no_argument, &additional[A_REORDER], 1},
		{"coalesce", no_argument, &additional[A_COALESCE], 1},
		{"reverse", no_argument, &additional[A_REVERSE], 1},
		{"hilbert", no_argument, &additional[A_HILBERT], 1},
		{"order-by", required_argument, 0, '~'},
		{"order-descending-by", required_argument, 0, '~'},
		{"order-smallest-first", no_argument, 0, '~'},
		{"order-largest-first", no_argument, 0, '~'},

		{"Adding calculated attributes", 0, 0, 0},
		{"calculate-feature-density", no_argument, &additional[A_CALCULATE_FEATURE_DENSITY], 1},
		{"generate-ids", no_argument, &additional[A_GENERATE_IDS], 1},
		{"calculate-feature-index", no_argument, &additional[A_CALCULATE_INDEX], 1},

		{"Trying to correct bad source geometry", 0, 0, 0},
		{"detect-longitude-wraparound", no_argument, &additional[A_DETECT_WRAPAROUND], 1},
		{"use-source-polygon-winding", no_argument, &prevent[P_USE_SOURCE_POLYGON_WINDING], 1},
		{"reverse-source-polygon-winding", no_argument, &prevent[P_REVERSE_SOURCE_POLYGON_WINDING], 1},
		{"clip-bounding-box", required_argument, 0, '~'},
		{"convert-polygons-to-label-points", no_argument, &additional[A_GENERATE_POLYGON_LABEL_POINTS], 1},

		{"Filtering tile contents", 0, 0, 0},
		{"prefilter", required_argument, 0, 'C'},
		{"postfilter", required_argument, 0, 'c'},

		{"Setting or disabling tile size limits", 0, 0, 0},
		{"maximum-tile-bytes", required_argument, 0, 'M'},
		{"maximum-tile-features", required_argument, 0, 'O'},
		{"limit-tile-feature-count", required_argument, 0, '~'},
		{"limit-tile-feature-count-at-maximum-zoom", required_argument, 0, '~'},
		{"no-feature-limit", no_argument, &prevent[P_FEATURE_LIMIT], 1},
		{"no-tile-size-limit", no_argument, &prevent[P_KILOBYTE_LIMIT], 1},
		{"no-tile-compression", no_argument, &prevent[P_TILE_COMPRESSION], 1},
		{"no-tile-stats", no_argument, &prevent[P_TILE_STATS], 1},
		{"tile-stats-attributes-limit", required_argument, 0, '~'},
		{"tile-stats-sample-values-limit", required_argument, 0, '~'},
		{"tile-stats-values-limit", required_argument, 0, '~'},

		{"Temporary storage", 0, 0, 0},
		{"temporary-directory", required_argument, 0, 't'},

		{"Progress indicator", 0, 0, 0},
		{"quiet", no_argument, 0, 'q'},
		{"no-progress-indicator", no_argument, 0, 'Q'},
		{"progress-interval", required_argument, 0, 'U'},
		{"json-progress", no_argument, 0, 'u'},
		{"version", no_argument, 0, 'v'},

		{"", 0, 0, 0},
		{"prevent", required_argument, 0, 'p'},
		{"additional", required_argument, 0, 'a'},
		{"check-polygons", no_argument, &additional[A_DEBUG_POLYGON], 1},
		{"no-polygon-splitting", no_argument, &prevent[P_POLYGON_SPLIT], 1},
		{"prefer-radix-sort", no_argument, &additional[A_PREFER_RADIX_SORT], 1},
		{"help", no_argument, 0, 'H'},

		{0, 0, 0, 0},
	};

	static struct option long_options[sizeof(long_options_orig) / sizeof(long_options_orig[0])];
	static char getopt_str[sizeof(long_options_orig) / sizeof(long_options_orig[0]) * 2 + 1];

	{
		size_t out = 0;
		size_t cout = 0;
		for (size_t lo = 0; long_options_orig[lo].name != NULL; lo++) {
			if (long_options_orig[lo].val != 0) {
				long_options[out++] = long_options_orig[lo];

				if (long_options_orig[lo].val > ' ') {
					getopt_str[cout++] = long_options_orig[lo].val;

					if (long_options_orig[lo].has_arg == required_argument) {
						getopt_str[cout++] = ':';
					}
				}
			}
		}
		long_options[out] = {0, 0, 0, 0};
		getopt_str[cout] = '\0';

		for (size_t lo = 0; long_options[lo].name != NULL; lo++) {
			if (long_options[lo].flag != NULL) {
				if (*long_options[lo].flag != 0) {
					fprintf(stderr, "Internal error: reused %s\n", long_options[lo].name);
					exit(EXIT_IMPOSSIBLE);
				}
				*long_options[lo].flag = 1;
			}
		}

		for (size_t lo = 0; long_options[lo].name != NULL; lo++) {
			if (long_options[lo].flag != NULL) {
				*long_options[lo].flag = 0;
			}
		}
	}

	std::string commandline = format_commandline(argc, argv);

	int option_index = 0;
	while ((i = getopt_long(argc, argv, getopt_str, long_options, &option_index)) != -1) {
		DEBUG_LOG("Processing option: %d, optarg: %s", i, optarg ? optarg : "NULL");
		switch (i) {
		case 0:
			break;

		case '~': {
			const char *opt = long_options[option_index].name;
			if (strcmp(opt, "tile-stats-attributes-limit") == 0) {
				max_tilestats_attributes = atoi(optarg);
			} else if (strcmp(opt, "tile-stats-sample-values-limit") == 0) {
				max_tilestats_sample_values = atoi(optarg);
			} else if (strcmp(opt, "tile-stats-values-limit") == 0) {
				max_tilestats_values = atoi(optarg);
			} else if (strcmp(opt, "clip-bounding-box") == 0) {
				clipbbox clip;
				if (sscanf(optarg, "%lf,%lf,%lf,%lf", &clip.lon1, &clip.lat1, &clip.lon2, &clip.lat2) == 4) {
					clipbboxes.push_back(clip);
				} else {
					fprintf(stderr, "%s: Can't parse bounding box --%s=%s\n", argv[0], opt, optarg);
					exit(EXIT_ARGS);
				}
			} else if (strcmp(opt, "use-attribute-for-id") == 0) {
				attribute_for_id = optarg;
			} else if (strcmp(opt, "set-attribute") == 0) {
				set_attribute_value(optarg);
			} else if (strcmp(opt, "smallest-maximum-zoom-guess") == 0) {
				maxzoom = MAX_ZOOM;
				guess_maxzoom = true;
				minimum_maxzoom = atoi_require(optarg, "Minimum maxzoom");
				if (minimum_maxzoom > MAX_ZOOM) {
					fprintf(stderr, "%s: %s: minimum maxzoom can be at most %d\n", argv[0], optarg, MAX_ZOOM);
					exit(EXIT_ARGS);
				}
			} else if (strcmp(opt, "tiny-polygon-size") == 0) {
				tiny_polygon_size = atoi(optarg);
			} else if (strcmp(opt, "extra-detail") == 0) {
				extra_detail = atoi_require(optarg, "Extra detail");
				if (extra_detail > 30) {
					// So the maximum geometry delta of just under 2 tile extents
					// is less than 2^31

					fprintf(stderr, "%s: --extra-detail can be at most 30\n", argv[0]);
					exit(EXIT_ARGS);
				}
			} else if (strcmp(opt, "order-by") == 0) {
				order_by.push_back(order_field(optarg, false));
			} else if (strcmp(opt, "order-descending-by") == 0) {
				order_by.push_back(order_field(optarg, true));
			} else if (strcmp(opt, "order-smallest-first") == 0) {
				order_by.push_back(order_field(ORDER_BY_SIZE, false));
				order_by_size = true;
			} else if (strcmp(opt, "order-largest-first") == 0) {
				order_by.push_back(order_field(ORDER_BY_SIZE, true));
				order_by_size = true;
			} else if (strcmp(opt, "simplification-at-maximum-zoom") == 0) {
				maxzoom_simplification = atof_require(optarg, "Mazoom simplification");
				if (maxzoom_simplification <= 0) {
					fprintf(stderr, "%s: --simplification-at-maximum-zoom must be > 0\n", argv[0]);
					exit(EXIT_ARGS);
				}
				break;
			} else if (strcmp(opt, "limit-tile-feature-count") == 0) {
				limit_tile_feature_count = atoll_require(optarg, "Limit tile feature count");
			} else if (strcmp(opt, "limit-tile-feature-count-at-maximum-zoom") == 0) {
				limit_tile_feature_count_at_maxzoom = atoll_require(optarg, "Limit tile feature count at maxzoom");
			} else if (strcmp(opt, "drop-denser") == 0) {
				drop_denser = atoi_require(optarg, "Drop denser rate");
				if (drop_denser > 100) {
					fprintf(stderr, "%s: --drop-denser can be at most 100\n", argv[0]);
					exit(EXIT_ARGS);
				}
			} else if (strcmp(opt, "preserve-point-density-threshold") == 0) {
				preserve_point_density_threshold = atoll_require(optarg, "Preserve point density threshold");
			} else if (strcmp(opt, "preserve-multiplier-density-threshold") == 0) {
				preserve_multiplier_density_threshold = atoll_require(optarg, "Preserve multiplier density threshold");
			} else if (strcmp(opt, "extend-zooms-if-still-dropping-maximum") == 0) {
				extend_zooms_max = atoll_require(optarg, "Maximum number by which to extend zooms");
			} else if (strcmp(opt, "retain-points-multiplier") == 0) {
				retain_points_multiplier = atoll_require(optarg, "Multiply the fraction of points retained by zoom level");
			} else if (strcmp(opt, "unidecode-data") == 0) {
				unidecode_data = read_unidecode(optarg);
			} else if (strcmp(opt, "maximum-string-attribute-length") == 0) {
				maximum_string_attribute_length = atoll_require(optarg, "Maximum string attribute length");
			} else if (strcmp(opt, "postgis") == 0) {
				use_postgis = true;
				if (!postgis_cfg.parse_connection_string(optarg)) {
					exit(EXIT_ARGS);
				}
			} else if (strcmp(opt, "postgis-host") == 0) {
				postgis_cfg.host = optarg;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-port") == 0) {
				postgis_cfg.port = optarg;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-dbname") == 0) {
				postgis_cfg.dbname = optarg;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-user") == 0) {
				postgis_cfg.user = optarg;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-password") == 0) {
				postgis_cfg.password = optarg;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-table") == 0) {
				postgis_cfg.table = optarg;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-geometry-field") == 0) {
				postgis_cfg.geometry_field = optarg;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-sql") == 0) {
				postgis_cfg.sql = optarg;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-columns") == 0) {
				postgis_cfg.selected_columns_csv = optarg;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-columns-best-effort") == 0) {
				postgis_cfg.selected_columns_best_effort = true;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-canonical-attr-order") == 0) {
				postgis_cfg.canonical_attr_order = true;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-no-canonical-attr-order") == 0) {
				postgis_cfg.canonical_attr_order = false;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-profile") == 0) {
				postgis_cfg.profile = true;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-shard-key") == 0) {
				postgis_cfg.shard_key = optarg;
				use_postgis = true;
			} else if (strcmp(opt, "postgis-shard-mode") == 0) {
				postgis_cfg.shard_mode = optarg;
				if (postgis_cfg.shard_mode != "auto" &&
				    postgis_cfg.shard_mode != "none" &&
				    postgis_cfg.shard_mode != "key" &&
				    postgis_cfg.shard_mode != "range") {
					fprintf(stderr, "%s: --postgis-shard-mode must be one of auto|none|key|range (got %s)\n", argv[0], optarg);
					exit(EXIT_ARGS);
				}
				use_postgis = true;
			} else if (strcmp(opt, "postgis-progress-count") == 0) {
				postgis_cfg.progress_with_exact_count = true;
				use_postgis = true;
			} else if (strcmp(opt, "mongo") == 0) {
				// 解析单行配置：host:port:dbname:user:password:auth_source:collection
				if (!mongo_cfg.parse_connection_string(optarg)) {
					// parse_connection_string 已经输出详细错误信息
					exit(EXIT_ARGS);
				}
				use_mongo = true;
			} else if (strcmp(opt, "mongo-host") == 0) {
				mongo_cfg.host = optarg;
				use_mongo = true;
			} else if (strcmp(opt, "mongo-port") == 0) {
				mongo_cfg.port = atoi_require(optarg, "MongoDB port");
				use_mongo = true;
			} else if (strcmp(opt, "mongo-dbname") == 0) {
				mongo_cfg.dbname = optarg;
				use_mongo = true;
			} else if (strcmp(opt, "mongo-collection") == 0) {
				mongo_cfg.collection = optarg;
				use_mongo = true;
			} else if (strcmp(opt, "mongo-username") == 0) {
				mongo_cfg.username = optarg;
				use_mongo = true;
			} else if (strcmp(opt, "mongo-password") == 0) {
				mongo_cfg.password = optarg;
				use_mongo = true;
			} else if (strcmp(opt, "mongo-auth-source") == 0) {
				mongo_cfg.auth_source = optarg;
				use_mongo = true;
			} else if (strcmp(opt, "mongo-batch-size") == 0) {
				mongo_cfg.batch_size = atoi_require(optarg, "MongoDB batch size");
				use_mongo = true;
			} else if (strcmp(opt, "mongo-pool-size") == 0) {
				mongo_cfg.connection_pool_size = atoi_require(optarg, "MongoDB connection pool size");
				use_mongo = true;
			} else if (strcmp(opt, "mongo-timeout") == 0) {
				mongo_cfg.timeout_ms = atoi_require(optarg, "MongoDB timeout");
				use_mongo = true;
			} else if (strcmp(opt, "mongo-no-indexes") == 0) {
				mongo_cfg.create_indexes = false;
				use_mongo = true;
			} else if (strcmp(opt, "mongo-drop-collection") == 0) {
				mongo_cfg.drop_collection_before_write = true;
				use_mongo = true;
			} else if (strcmp(opt, "mongo-metadata") == 0) {
				mongo_cfg.write_metadata = true;
				use_mongo = true;
			} else if (strcmp(opt, "mongo-fail-on-discard") == 0) {
				mongo_cfg.fail_on_discard = true;
				use_mongo = true;
			} else if (strcmp(opt, "mongo-no-fail-on-discard") == 0) {
				mongo_cfg.fail_on_discard = false;
				use_mongo = true;
			} else {
				fprintf(stderr, "%s: Unrecognized option --%s\n", argv[0], opt);
				exit(EXIT_ARGS);
			}
			break;
		}

		case 'z':
			if (strcmp(optarg, "g") == 0) {
				maxzoom = MAX_ZOOM;
				guess_maxzoom = true;
			} else {
				maxzoom = atoi_require(optarg, "Maxzoom");
			}
			break;

		case 'Z':
			minzoom = atoi_require(optarg, "Minzoom");
			break;

		case 'R': {
			unsigned z, x, y;
			if (sscanf(optarg, "%u/%u/%u", &z, &x, &y) == 3) {
				minzoom = z;
				maxzoom = z;
				justx = x;
				justy = y;
			} else {
				fprintf(stderr, "--one-tile argument must be z/x/y\n");
				exit(EXIT_ARGS);
			}
			break;
		}

		case 'B':
			if (strcmp(optarg, "g") == 0) {
				basezoom = -2;
			} else if (optarg[0] == 'g' || optarg[0] == 'f') {
				basezoom = -2;
				if (optarg[0] == 'g') {
					basezoom_marker_width = atof_require(optarg + 1, "Marker width");
				} else {
					basezoom_marker_width = sqrt(50000 / atof_require(optarg + 1, "Marker width"));
				}
				if (basezoom_marker_width == 0 || atof_require(optarg + 1, "Marker width") == 0) {
					fprintf(stderr, "%s: Must specify value >0 with -B%c\n", argv[0], optarg[0]);
					exit(EXIT_ARGS);
				}
			} else {
				basezoom = atoi_require(optarg, "Basezoom");
				if (basezoom == 0 && strcmp(optarg, "0") != 0) {
					fprintf(stderr, "%s: Couldn't understand -B%s\n", argv[0], optarg);
					exit(EXIT_ARGS);
				}
			}
			break;

		case 'K':
			cluster_distance = atoi_require(optarg, "Cluster distance");
			if (cluster_distance > 255) {
				fprintf(stderr, "%s: --cluster-distance %d is too big; limit is 255\n", argv[0], cluster_distance);
				exit(EXIT_ARGS);
			}
			break;

		case 'k':
			if (strcmp(optarg, "g") == 0) {
				cluster_maxzoom = MAX_ZOOM - 1;
				guess_cluster_maxzoom = true;
			} else {
				cluster_maxzoom = atoi_require(optarg, "Cluster maxzoom");
			}
			break;

		case 'd':
			full_detail = atoi_require(optarg, "Full detail");
			if (full_detail > 30) {
				// So the maximum geometry delta of just under 2 tile extents
				// is less than 2^31

				fprintf(stderr, "%s: --full-detail can be at most 30\n", argv[0]);
				exit(EXIT_ARGS);
			}
			break;

		case 'D':
			low_detail = atoi_require(optarg, "Low detail");
			if (low_detail > 30) {
				fprintf(stderr, "%s: --low-detail can be at most 30\n", argv[0]);
				exit(EXIT_ARGS);
			}
			break;

		case 'm':
			min_detail = atoi_require(optarg, "Min detail");
			break;

		case 'o':
			if (out_mbtiles != NULL) {
				fprintf(stderr, "%s: Can't specify both %s and %s as output\n", argv[0], out_mbtiles, optarg);
				exit(EXIT_ARGS);
			}
			if (out_dir != NULL) {
				fprintf(stderr, "%s: Can't specify both %s and %s as output\n", argv[0], out_dir, optarg);
				exit(EXIT_ARGS);
			}
			out_mbtiles = optarg;
			break;

		case 'e':
			if (out_mbtiles != NULL) {
				fprintf(stderr, "%s: Can't specify both %s and %s as output\n", argv[0], out_mbtiles, optarg);
				exit(EXIT_ARGS);
			}
			if (out_dir != NULL) {
				fprintf(stderr, "%s: Can't specify both %s and %s as output\n", argv[0], out_dir, optarg);
				exit(EXIT_ARGS);
			}
			out_dir = optarg;
			break;

		case 'x':
			exclude.insert(std::string(optarg));
			break;

		case 'y':
			exclude_all = 1;
			include.insert(std::string(optarg));
			break;

		case 'X':
			exclude_all = 1;
			break;

		case 'Y': {
			char *cp = strchr(optarg, ':');
			if (cp == NULL || cp == optarg) {
				fprintf(stderr, "%s: -Y requires attribute:description\n", argv[0]);
				exit(EXIT_ARGS);
			}
			std::string attrib = std::string(optarg).substr(0, cp - optarg);
			std::string desc = std::string(cp + 1);
			attribute_descriptions.insert(std::pair<std::string, std::string>(attrib, desc));
		} break;

		case 'J':
			filter = read_filter(optarg);
			break;

		case 'j':
			filter = parse_filter(optarg);
			break;

		case 'r':
			if (strcmp(optarg, "g") == 0) {
				droprate = -2;
			} else if (strcmp(optarg, "p") == 0) {
				droprate = -3;
			} else if (optarg[0] == 'g' || optarg[0] == 'f') {
				droprate = -2;
				if (optarg[0] == 'g') {
					basezoom_marker_width = atof_require(optarg + 1, "Marker width");
				} else {
					basezoom_marker_width = sqrt(50000 / atof_require(optarg + 1, "Marker width"));
				}
				if (basezoom_marker_width == 0 || atof_require(optarg + 1, "Marker width") == 0) {
					fprintf(stderr, "%s: Must specify value >0 with -r%c\n", argv[0], optarg[0]);
					exit(EXIT_ARGS);
				}
			} else {
				droprate = atof_require(optarg, "Drop rate");
			}
			break;

		case 'b':
			buffer = atoi_require(optarg, "Buffer");
			if (buffer > 127) {
				// So the maximum geometry delta is under 2 tile extents,
				// from less than half a tile beyond one side to less than
				// half a tile beyond the other.

				fprintf(stderr, "%s: --buffer can be at most 127\n", argv[0]);
				exit(EXIT_ARGS);
			}
			break;

		case 'f':
			force = 1;
			break;

		case 'F':
			forcetable = 1;
			break;

		case 't':
			tmpdir = optarg;
			if (tmpdir[0] != '/') {
				fprintf(stderr, "Warning: temp directory %s doesn't begin with /\n", tmpdir);
			}
			break;

		case 'g':
			gamma = atof_require(optarg, "Gamma");
			break;

		case 'q':
			quiet = 1;
			break;

		case 'Q':
			quiet_progress = 1;
			break;

		case 'u':
			quiet = 1;
			logger.json_enabled = true;
			break;

		case 'U':
			progress_interval = atof_require(optarg, "Progress interval");
			break;

		case 'p': {
			char *cp;
			for (cp = optarg; *cp != '\0'; cp++) {
				if (has_name(long_options, &prevent[*cp & 0xFF])) {
					prevent[*cp & 0xFF] = 1;
				} else {
					fprintf(stderr, "%s: Unknown option -p%c\n", argv[0], *cp);
					exit(EXIT_ARGS);
				}
			}
			break;
		}

		case 'a': {
			char *cp;
			for (cp = optarg; *cp != '\0'; cp++) {
				if (has_name(long_options, &additional[*cp & 0xFF])) {
					additional[*cp & 0xFF] = 1;
				} else {
					fprintf(stderr, "%s: Unknown option -a%c\n", argv[0], *cp);
					exit(EXIT_ARGS);
				}
			}
			break;
		}

		case 'v':
			fprintf(stderr, "tippecanoe %s\n", version_str().c_str());
			exit(EXIT_SUCCESS);

		case 's':
			set_projection_or_exit(optarg);
			break;

		case 'S':
			simplification = atof_require(optarg, "Simplification");
			if (simplification <= 0) {
				fprintf(stderr, "%s: --simplification must be > 0\n", argv[0]);
				exit(EXIT_ARGS);
			}
			break;

		case 'M':
			max_tile_size = atoll_require(optarg, "Max tile size");
			break;

		case 'O':
			max_tile_features = atoll_require(optarg, "Max tile features");
			break;

		case 'c':
			postfilter = optarg;
			break;

		case 'C':
			prefilter = optarg;
			break;

		case 'T':
			set_attribute_type(attribute_types, optarg);
			break;

		case 'E':
			set_attribute_accum(attribute_accum, optarg, argv);
			break;

		default: {
			if (i != 'H' && i != '?') {
				fprintf(stderr, "Unknown option -%c\n", i);
			}
			int width = 7 + strlen(argv[0]);
			fprintf(stderr, "Usage: %s [options]", argv[0]);
			for (size_t lo = 0; long_options_orig[lo].name != NULL && strlen(long_options_orig[lo].name) > 0; lo++) {
				if (long_options_orig[lo].val == 0) {
					fprintf(stderr, "\n  %s\n        ", long_options_orig[lo].name);
					width = 8;
					continue;
				}
				if (width + strlen(long_options_orig[lo].name) + 9 >= 80) {
					fprintf(stderr, "\n        ");
					width = 8;
				}
				width += strlen(long_options_orig[lo].name) + 9;
				if (strcmp(long_options_orig[lo].name, "output") == 0) {
					fprintf(stderr, " --%s=output.mbtiles", long_options_orig[lo].name);
					width += 9;
				} else if (long_options_orig[lo].has_arg) {
					fprintf(stderr, " [--%s=...]", long_options_orig[lo].name);
				} else {
					fprintf(stderr, " [--%s]", long_options_orig[lo].name);
				}
			}
			if (width + 16 >= 80) {
				fprintf(stderr, "\n        ");
				width = 8;
			}
			fprintf(stderr, "\n");
			if (i == 'H') {
				exit(EXIT_SUCCESS);
			} else {
				exit(EXIT_ARGS);
			}
		}
		}
	}

	if (additional[A_HILBERT]) {
		encode_index = encode_hilbert;
		decode_index = decode_hilbert;
	} else {
		encode_index = encode_quadkey;
		decode_index = decode_quadkey;
	}

	// Wait until here to project the bounding box, so that the behavior is
	// the same no matter what order the projection and bounding box are
	// specified in
	for (auto &c : clipbboxes) {
		projection->project(c.lon1, c.lat1, 32, &c.minx, &c.maxy);
		projection->project(c.lon2, c.lat2, 32, &c.maxx, &c.miny);
	}

	if (max_tilestats_sample_values < max_tilestats_values) {
		max_tilestats_sample_values = max_tilestats_values;
	}

	signal(SIGPIPE, SIG_IGN);

	files_open_at_start = open(get_null_device(), O_RDONLY | O_CLOEXEC);
	if (files_open_at_start < 0) {
		perror("open /dev/null");
		exit(EXIT_OPEN);
	}
	if (close(files_open_at_start) != 0) {
		perror("close");
		exit(EXIT_CLOSE);
	}

	if (full_detail <= 0) {
		full_detail = 12;
	}

	if (droprate == -3 && !guess_maxzoom) {
		fprintf(stderr, "Can't use -rp without either -zg or --smallest-maximum-zoom-guess\n");
		exit(EXIT_ARGS);
	}

	if (maxzoom > MAX_ZOOM) {
		maxzoom = MAX_ZOOM;
		fprintf(stderr, "Highest supported zoom is -z%d\n", maxzoom);
	}

	// Need two checks: one for geometry representation, the other for
	// index traversal when guessing base zoom and drop rate

	// This previously dropped the maxzoom rather than the detail when they were in conflict,
	// which proved to be annoying.
	if (!guess_maxzoom) {
		if (maxzoom > 32 - full_detail) {
			full_detail = 32 - maxzoom;
			fprintf(stderr, "Highest supported detail with maxzoom %d is %d\n", maxzoom, full_detail);
		}
		if (maxzoom > 33 - low_detail) {  // that is, maxzoom - 1 > 32 - low_detail
			low_detail = 33 - maxzoom;
			fprintf(stderr, "Highest supported low detail with maxzoom %d is %d\n", maxzoom, low_detail);
		}
	}
	if (minzoom > maxzoom) {
		fprintf(stderr, "%s: Minimum zoom -Z%d cannot be greater than maxzoom -z%d\n", argv[0], minzoom, maxzoom);
		exit(EXIT_ARGS);
	}

	if (full_detail < min_detail) {
		min_detail = full_detail;
		fprintf(stderr, "%s: Reducing minimum detail to match full detail %d\n", argv[0], min_detail);
	}

	if (low_detail < min_detail) {
		min_detail = low_detail;
		fprintf(stderr, "%s: Reducing minimum detail to match low detail %d\n", argv[0], min_detail);
	}

	if (basezoom == -1) {  // basezoom unspecified
		if (!guess_maxzoom) {
			basezoom = maxzoom;
		}
	}

	if (extra_detail >= 0 || prevent[P_SIMPLIFY_SHARED_NODES] || additional[A_EXTEND_ZOOMS] || extend_zooms_max > 0) {
		geometry_scale = 0;
	} else {
		geometry_scale = 32 - (full_detail + maxzoom);
		if (geometry_scale < 0) {
			geometry_scale = 0;
			if (!guess_maxzoom) {
				// This shouldn't be able to happen any more. Can it still?
				fprintf(stderr, "Full detail + maxzoom > 32, so you are asking for more detail than is available.\n");
			}
		}
	}

	if ((basezoom < 0 || droprate < 0) && (gamma < 0)) {
		// Can't use randomized (as opposed to evenly distributed) dot dropping
		// if rate and base aren't known during feature reading.
		gamma = 0;
		fprintf(stderr, "Forcing -g0 since -B or -r is not known\n");
	}

	if (out_mbtiles == NULL && out_dir == NULL && !use_mongo) {
		fprintf(stderr, "%s: must specify -o out.mbtiles or -e directory or use --mongo for MongoDB output\n", av[0]);
		exit(EXIT_ARGS);
	}

	if (out_mbtiles != NULL && out_dir != NULL) {
		fprintf(stderr, "%s: Options -o and -e cannot be used together\n", argv[0]);
		exit(EXIT_ARGS);
	}

	if (out_mbtiles != NULL) {
		if (force) {
			unlink(out_mbtiles);
		}

		outdb = mbtiles_open(out_mbtiles, argv, forcetable);
	}
	if (out_dir != NULL) {
		check_dir(out_dir, argv, force, forcetable);
	}

	int ret = EXIT_SUCCESS;

	long long file_bbox[4] = {UINT_MAX, UINT_MAX, 0, 0};

	long long file_bbox1[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0, 0};	      // standard -180 to 180 world plane
	long long file_bbox2[4] = {0x1FFFFFFFF, 0xFFFFFFFF, 0x100000000, 0};  // 0 to 360 world plane

	// 1. Initialize error logger
	{
		std::string exec_dir;
		if (argv[0] && argv[0][0] == '/') {
			exec_dir = argv[0];
			auto last_slash = exec_dir.rfind('/');
			if (last_slash != std::string::npos) {
				exec_dir = exec_dir.substr(0, last_slash);
			}
		} else {
			char cwd_buf[4096];
			if (getcwd(cwd_buf, sizeof(cwd_buf))) {
				exec_dir = cwd_buf;
			}
		}
		ErrorLogger::instance().initialize(exec_dir);
	}

	// 2. Initialize MongoDB (before read_input, since read_input calls traverse_zooms)
	if (use_mongo) {
		MongoDB::initialize_global();
	}

	DEBUG_LOG("Starting data ingestion pipeline (read_input)...");
	// 2. 调用 read_input() 读取 PostGIS 数据并写入瓦片
	// read_input 内部会调用 traverse_zooms 进行瓦片写入
	std::string postgis_layer = postgis_cfg.table.empty() ? "postgis" : postgis_cfg.table;
	const char *read_input_name = out_mbtiles ? out_mbtiles : (out_dir ? out_dir : "mongo-output");
	DEBUG_LOG("Layer configuration: %s", postgis_layer.c_str());
	auto input_ret = read_input(postgis_layer, read_input_name,
		    maxzoom, minzoom, basezoom, basezoom_marker_width, outdb, out_dir, &exclude, &include, exclude_all, filter, droprate, buffer, tmpdir, gamma, forcetable, NULL, gamma != 0, file_bbox, file_bbox1, file_bbox2, prefilter, postfilter, NULL, guess_maxzoom, guess_cluster_maxzoom, &attribute_types, argv[0], &attribute_accum, attribute_descriptions, commandline, minimum_maxzoom);

	ret = std::get<0>(input_ret);

	// Write metadata to MongoDB if requested
	if (use_mongo && mongo_cfg.write_metadata) {
		struct metadata meta = std::get<1>(input_ret);

		try {
			MongoWriter* meta_writer = MongoWriter::get_writer_instance(mongo_cfg);
			if (meta_writer) {
				meta_writer->write_metadata_bson(meta);
			}
		} catch (const std::exception &e) {
			fprintf(stderr, "Warning: failed to write MongoDB metadata: %s\n", e.what());
		}
	}

	if (outdb != NULL) {
		mbtiles_close(outdb, argv[0]);
	}

#ifdef MTRACE
	muntrace();
#endif

	if (filter != NULL) {
		json_free(filter);
	}

	// 3. 检查 MongoDB 错误统计（在清理之前）
	// 注意：工作线程使用 TLS 实例，需要通过 cleanup_global() 获取统计
	if (use_mongo) {
		MongoDB::cleanup_global();

		auto stats = MongoDB::get_global_stats();
		MongoDB::print_stats(stats, quiet);

		if (stats.total_errors > 0) {
			fprintf(stderr, "%s: MongoDB had %zu errors during write operations\n",
				mongo_cfg.fail_on_discard ? "Error" : "Warning",
				stats.total_errors);
			if (mongo_cfg.fail_on_discard && ret == 0) {
				ret = EXIT_MONGO;
			}
		}

		if (stats.total_discarded > 0) {
			fprintf(stderr, "Warning: MongoDB discarded %zu tiles due to persistent write failures\n", stats.total_discarded);
			if (mongo_cfg.fail_on_discard && ret == 0) {
				ret = EXIT_MONGO;
			}
		}

		MongoDB::shutdown_global();
	}

	if (!use_mongo) {
		i = open(get_null_device(), O_RDONLY | O_CLOEXEC);
		// i < files_open_at_start is not an error, because reading from a pipe closes stdin
		if (i > files_open_at_start) {
			fprintf(stderr, "Internal error: did not close all files: %d\n", i);
			exit(EXIT_IMPOSSIBLE);
		}
	}

	ErrorLogger::instance().print_summary(quiet);
	ErrorLogger::instance().close();

	return ret;
}

int main(int argc, char **argv) {
	return maindb(argc, argv);
}
