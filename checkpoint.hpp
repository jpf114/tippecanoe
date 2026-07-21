#ifndef CHECKPOINT_HPP
#define CHECKPOINT_HPP

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>  // 仅用于 mbtiles 输出格式的 WAL 清理，不用于 checkpoint 状态存储

#include "dirtiles.hpp"
#include "main.hpp"
#include "mbtiles.hpp"
#include "options.hpp"
#include "tile.hpp"

namespace checkpoint {

constexpr int TIPPECANOE_CHECKPOINT_FORMAT = 3;  // v3: 纯文件系统方案 + CRC32 校验 + 磁盘空间检查
                                                  // v2: 纯文件系统方案（JSON state），向后兼容

// ---------------------------------------------------------------------------
// 信号处理：优雅退出
// ---------------------------------------------------------------------------

// 注册 SIGTERM/SIGINT 处理器，设置优雅退出标志。
void install_signal_handlers();

// 检查是否收到退出信号（SIGTERM/SIGINT）。
// 在 zoom 边界调用，若返回 true 则提交当前 zoom 后退出。
bool shutdown_requested();

// ---------------------------------------------------------------------------
// 数据结构
// ---------------------------------------------------------------------------

struct InputFileStat {
	std::string path;
	int64_t size = 0;
	int64_t mtime_sec = 0;
	int64_t mtime_nsec = 0;
	std::string content_hash;  // v3: 小文件（< 100MB）的 SHA-256 内容哈希，大文件为空
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

// 单个 zoom 的提交记录
struct ZoomCommit {
	int64_t committed_at = 0;
	int64_t geom_total_bytes = 0;
	uint64_t generation = 0;
};

// checkpoint 的完整状态，序列化为 state.json
// 使用原子 rename 更新（state.json.tmp → state.json），保证崩溃一致性。
struct CheckpointState {
	// --- job ---
	int format_version = TIPPECANOE_CHECKPOINT_FORMAT;
	std::string fingerprint;
	std::string command_line;     // 原始命令行（用于显示）
	std::string normalized_cmd;   // 规范化命令行（用于指纹计算）
	std::string output_mode;      // "mbtiles" / "directory"
	std::string output_path;      // 绝对输出路径
	size_t temp_files = 0;
	size_t cpus = 0;
	int64_t created_at = 0;
	int64_t updated_at = 0;
	std::vector<InputFileStat> input_files;

	// --- tiling_state ---
	int iz = 0;
	int minzoom = 0;
	int maxzoom = 0;
	int basezoom = 0;
	int last_completed_zoom = -1;  // -1 表示尚无 zoom 完成
	unsigned midx = 0;
	unsigned midy = 0;
	int64_t nodepos = 0;
	bool entry_snapshot_done = false;

	// --- generation ---
	uint64_t generation = 0;

	// --- zoom commits ---
	std::map<int, ZoomCommit> zoom_commits;

	// --- v3 新增字段 ---
	// 当前 generation 的 geom 文件列表（世代清理索引化，避免 O(n) 目录扫描）
	std::vector<std::string> current_gen_files;
	// 磁盘空间预算（字节），0 表示不限制
	int64_t disk_budget = 0;
	// 已使用的 blob 空间（字节，估算值）
	int64_t blob_size_estimate = 0;
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
	// Data global bbox in z=32 pixel coordinates, used to reconstruct
	// metadata.bounds / antimeridian_adjusted_bounds on resume.
	long long file_bbox[4] = {UINT_MAX, UINT_MAX, 0, 0};
	long long file_bbox1[4] = {0xFFFFFFFF, 0xFFFFFFFF, 0, 0};
	long long file_bbox2[4] = {0x1FFFFFFFF, 0xFFFFFFFF, 0x100000000, 0};

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

// 从 state.json 读取的续跑元数据（不需要打开完整 Session）
struct ResumeInfo {
	std::string output_mode;
	std::string output_path;
	std::vector<std::string> input_files;
	int maxzoom = 0;
	int minzoom = 0;
	int basezoom = 0;
	int last_completed_zoom = -1;
	bool entry_snapshot_done = false;
	std::string original_cmd;
	std::string normalized_cmd;
};

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------

std::string normalize_command_line_for_fingerprint(std::string const &command_line);
std::string compute_fingerprint(FingerprintParams const &params);
std::vector<InputFileStat> stat_input_paths(std::vector<std::string> const &paths);
std::string absolute_path_or_die(const char *path);

// 从 state.json 读取续跑元数据（不打开 Session）
ResumeInfo read_resume_info(const char *dir);

// 输出清理辅助函数（操作 mbtiles 输出文件，不是 checkpoint 状态）
void cleanup_resume_wal(const char *output_path);
void cleanup_resume_dirs(const char *output_path, int last_completed_zoom, int maxzoom);
void cleanup_resume_tiles(sqlite3 *outdb, int last_completed_zoom);

// ---------------------------------------------------------------------------
// 管理命令
// ---------------------------------------------------------------------------

// 查询 checkpoint 状态（不启动 resume），打印到 stderr
int checkpoint_status(const char *dir);

// 清理 checkpoint 目录中的 blobs/（保留 state.json 作为日志）
int checkpoint_prune(const char *dir);

// ---------------------------------------------------------------------------
// Session 类
// ---------------------------------------------------------------------------

class Session {
      public:
	static std::unique_ptr<Session> open_new(const char *dir, bool force, FingerprintParams const &params);
	static std::unique_ptr<Session> open_resume(const char *dir);
	~Session();

	Session(Session const &) = delete;
	Session &operator=(Session const &) = delete;

	bool active() const { return active_; }
	bool is_resume() const { return is_resume_; }
	bool can_resume_tiling() const;
	int resume_iz() const;

	void snapshot_tiling_entry(int poolfd, size_t pool_size, long long const *pool_off, unsigned const *initial_x, unsigned const *initial_y,
				   int geomfd, off_t geom_size, node *shared_nodes_map, size_t nodepos, std::string const &shared_nodes_bloom,
				   std::vector<std::map<std::string, layermap_entry>> const &layermaps, int iz, int minzoom, int maxzoom, int basezoom,
				   long long const *file_bbox, long long const *file_bbox1, long long const *file_bbox2);

	bool restore_tiling(TilingRestore &out);

	void on_zoom_complete(ZoomCompleteContext const &ctx);

	void finalize_success();

      private:
	Session(const char *dir, bool is_resume);

	// JSON state 原子读写
	void load_state();
	void save_state();
	void save_state_atomic();  // 写 state.json.tmp → fsync → rename → fsync dir

	// 并发锁
	void acquire_lock();
	void release_lock();

	// 文件系统辅助
	std::string path_blob(const char *name) const;
	std::string path_staging(const char *name) const;
	std::string path_commits() const;
	void fsync_path(std::string const &path);
	void fsync_dir(std::string const &dirpath);
	void copy_fd_to_file(int fd, size_t nbytes, std::string const &dest);
	void rmrf(std::string const &path);  // C 递归删除（替代 system("rm -rf")）
	void clear_workspace();
	void verify_fingerprint_internal();
	// v3.1: open_resume 时检查 blobs/ 文件存在性，防止断电后 state.json
	// 与 blobs 不一致导致续做失败。
	void verify_blobs_consistency();

	// v3 新增：CRC32 校验
	bool blob_has_crc() const;  // 当前 state 是否启用 CRC（format_version >= 3）
	void write_blob_with_crc(std::string const &path, void const *data, size_t len);
	// 读取整个 blob 文件（含 CRC 校验，返回 data 部分）
	std::vector<unsigned char> read_blob_with_crc(std::string const &path);
	// 校验已 mmap 的 blob（data 位于 mmap 区域末尾的 4 字节为 CRC32）
	bool verify_mmap_crc(void const *mapped, size_t mapped_size, std::string const &path);

	// v3 新增：磁盘空间检查
	bool check_disk_space(int64_t required_bytes) const;

	// v3 新增：世代清理索引化
	void cleanup_old_gen_files_indexed();

	// v3 新增：异步 fsync（后台线程）
	// v3.1: 改用 std::future + 析构时 wait，保证进程退出前 fsync 完成
	void async_fsync_blob_dir();
	// v3.1: 等待 pending fsync 完成（用于 save_state_atomic 之前和析构时）
	void wait_pending_fsync();

	// 进度报告
	void report_progress(int zoom) const;

	CheckpointState state_;
	int lock_fd_ = -1;
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
	int64_t start_time_ = 0;  // 用于进度报告
	std::future<void> pending_fsync_;  // v3.1: 异步 fsync 的 future，析构时 wait
};

}  // namespace checkpoint

#endif
