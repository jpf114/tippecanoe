#ifndef CHECKPOINT_HPP
#define CHECKPOINT_HPP

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "dirtiles.hpp"
#include "main.hpp"
#include "mbtiles.hpp"
#include "options.hpp"
#include "tile.hpp"

namespace checkpoint {

constexpr int TIPPECANOE_CHECKPOINT_FORMAT = 1;

struct InputFileStat {
	std::string path;
	int64_t size = 0;
	int64_t mtime_sec = 0;
	int64_t mtime_nsec = 0;
};

struct FingerprintParams {
	std::string command_line;
	std::string output_mode;
	std::string output_path;
	size_t temp_files = 0;
	size_t cpus = 0;
	int prevent[256]{};
	int additional[256]{};
	std::vector<InputFileStat> inputs;
};

struct TilingRestore {
	char *stringpool = nullptr;
	size_t pool_size = 0;
	int poolfd = -1;
	std::vector<int> geomfd;
	std::vector<off_t> geom_size;
	std::vector<long long> pool_off;
	std::vector<unsigned> initial_x;
	std::vector<unsigned> initial_y;
	node *shared_nodes_map = nullptr;
	size_t nodepos = 0;
	std::string shared_nodes_bloom;
	std::vector<std::map<std::string, layermap_entry>> layermaps;
	std::set<zxy> skip_children;
	std::vector<strategy> strategies;
	std::atomic<unsigned> midx{0};
	std::atomic<unsigned> midy{0};
	int iz = 0;
	int minzoom = 0;
	int maxzoom = 0;
	int basezoom = 0;

	TilingRestore();
	~TilingRestore();
};

struct ZoomCompleteContext {
	int zoom = 0;
	int *geomfd = nullptr;
	off_t *geom_size = nullptr;
	std::set<zxy> const *skip_children = nullptr;
	std::vector<strategy> const *strategies = nullptr;
	int maxzoom = 0;
	unsigned midx = 0;
	unsigned midy = 0;
};

std::string normalize_command_line_for_fingerprint(std::string const &command_line);
std::string compute_fingerprint(FingerprintParams const &params);
std::vector<InputFileStat> stat_input_paths(std::vector<std::string> const &paths);
std::string absolute_path_or_die(const char *path);

class Session {
      public:
	static std::unique_ptr<Session> open_new(const char *dir, bool force, FingerprintParams const &params);
	static std::unique_ptr<Session> open_resume(const char *dir, FingerprintParams const &params, bool allow_existing_output);

	~Session();

	Session(Session const &) = delete;
	Session &operator=(Session const &) = delete;

	bool active() const { return active_; }
	bool is_resume() const { return is_resume_; }
	bool can_resume_tiling() const;
	int resume_iz() const;

	void snapshot_tiling_entry(int poolfd, size_t pool_size, long long const *pool_off, unsigned const *initial_x, unsigned const *initial_y,
				   int geomfd, off_t geom_size, node *shared_nodes_map, size_t nodepos, std::string const &shared_nodes_bloom,
				   std::vector<std::map<std::string, layermap_entry>> const &layermaps, int iz, int minzoom, int maxzoom, int basezoom);

	bool restore_tiling(TilingRestore &out);

	void on_zoom_complete(ZoomCompleteContext const &ctx);

	void finalize_success();

      private:
	Session(const char *dir, bool is_resume);

	void open_db(bool create);
	void init_schema();
	void clear_workspace();
	void write_job_row(FingerprintParams const &params, std::string const &fingerprint);
	void verify_fingerprint_or_exit(FingerprintParams const &params);
	std::string path_blob(const char *name) const;
	std::string path_staging(const char *name) const;
	void fsync_path(std::string const &path);
	void copy_fd_to_file(int fd, size_t nbytes, std::string const &dest);
	void commit_generation();

	sqlite3 *db_ = nullptr;
	std::string dir_;
	bool active_ = false;
	bool is_resume_ = false;
	bool entry_snapshot_done_ = false;
	int last_completed_zoom_ = -1;
	int resume_iz_ = 0;
	int tiling_minzoom_ = 0;
	int tiling_maxzoom_ = 0;
	int tiling_basezoom_ = 0;
	unsigned tiling_midx_ = 0;
	unsigned tiling_midy_ = 0;
	uint64_t generation_ = 0;
};

}  // namespace checkpoint

#endif
