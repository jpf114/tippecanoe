#include "checkpoint.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "errors.hpp"
#include "serial.hpp"

extern char **av;

namespace checkpoint {

namespace {

void sha256(const unsigned char *data, size_t len, unsigned char out[32]) {
	static const uint32_t k[64] = {
	    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

	auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };

	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	std::vector<unsigned char> msg(data, data + len);
	uint64_t bitlen = (uint64_t) len * 8;
	msg.push_back(0x80);
	while ((msg.size() % 64) != 56) {
		msg.push_back(0);
	}
	for (int i = 7; i >= 0; i--) {
		msg.push_back((unsigned char) ((bitlen >> (i * 8)) & 0xff));
	}

	for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
		uint32_t w[64];
		for (int i = 0; i < 16; i++) {
			w[i] = ((uint32_t) msg[chunk + i * 4] << 24) | ((uint32_t) msg[chunk + i * 4 + 1] << 16) |
			       ((uint32_t) msg[chunk + i * 4 + 2] << 8) | (uint32_t) msg[chunk + i * 4 + 3];
		}
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++) {
			uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
			uint32_t ch = (e & f) ^ ((~e) & g);
			uint32_t t1 = hh + S1 + ch + k[i] + w[i];
			uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
			uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			uint32_t t2 = S0 + maj;
			hh = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
		h[5] += f;
		h[6] += g;
		h[7] += hh;
	}
	for (int i = 0; i < 8; i++) {
		out[i * 4] = (unsigned char) ((h[i] >> 24) & 0xff);
		out[i * 4 + 1] = (unsigned char) ((h[i] >> 16) & 0xff);
		out[i * 4 + 2] = (unsigned char) ((h[i] >> 8) & 0xff);
		out[i * 4 + 3] = (unsigned char) (h[i] & 0xff);
	}
}

std::string hex_encode(unsigned char const *data, size_t len) {
	static const char *hex = "0123456789abcdef";
	std::string out;
	out.resize(len * 2);
	for (size_t i = 0; i < len; i++) {
		out[i * 2] = hex[data[i] >> 4];
		out[i * 2 + 1] = hex[data[i] & 0x0f];
	}
	return out;
}

void json_escape(std::string const &s, std::string &out) {
	for (char c : s) {
		if (c == '\\' || c == '"') {
			out.push_back('\\');
			out.push_back(c);
		} else if (c == '\n') {
			out += "\\n";
		} else if (c == '\r') {
			out += "\\r";
		} else if ((unsigned char) c < 0x20) {
			char buf[8];
			snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char) c);
			out += buf;
		} else {
			out.push_back(c);
		}
	}
}

void exec_sql(sqlite3 *db, char const *sql) {
	char *err = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: checkpoint sqlite: %s\n", *av, err ? err : "unknown error");
		sqlite3_free(err);
		exit(EXIT_SQLITE);
	}
}

int64_t now_unix() {
	return (int64_t) time(NULL);
}

bool mkdir_p(std::string const &path) {
	if (path.empty()) {
		return true;
	}
	struct stat st;
	if (stat(path.c_str(), &st) == 0) {
		return S_ISDIR(st.st_mode);
	}
	size_t slash = path.find_last_of('/');
	if (slash != std::string::npos && slash > 0) {
		if (!mkdir_p(path.substr(0, slash))) {
			return false;
		}
	}
	if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
		return false;
	}
	return true;
}

void write_blob_file(std::string const &path, void const *data, size_t len) {
	FILE *fp = fopen(path.c_str(), "wb");
	if (fp == NULL) {
		perror(path.c_str());
		exit(EXIT_WRITE);
	}
	if (len > 0 && fwrite(data, len, 1, fp) != 1) {
		perror(path.c_str());
		exit(EXIT_WRITE);
	}
	if (fclose(fp) != 0) {
		perror(path.c_str());
		exit(EXIT_CLOSE);
	}
}

void write_layermaps_blob(std::string const &path, std::vector<std::map<std::string, layermap_entry>> const &layermaps) {
	FILE *fp = fopen(path.c_str(), "wb");
	if (fp == NULL) {
		perror(path.c_str());
		exit(EXIT_WRITE);
	}
	uint32_t segs = (uint32_t) layermaps.size();
	fwrite(&segs, sizeof(segs), 1, fp);
	for (auto const &seg : layermaps) {
		uint32_t layers = (uint32_t) seg.size();
		fwrite(&layers, sizeof(layers), 1, fp);
		for (auto const &kv : seg) {
			uint32_t nlen = (uint32_t) kv.first.size();
			fwrite(&nlen, sizeof(nlen), 1, fp);
			fwrite(kv.first.c_str(), 1, nlen, fp);
			uint32_t id = (uint32_t) kv.second.id;
			int32_t minz = kv.second.minzoom;
			int32_t maxz = kv.second.maxzoom;
			fwrite(&id, sizeof(id), 1, fp);
			fwrite(&minz, sizeof(minz), 1, fp);
			fwrite(&maxz, sizeof(maxz), 1, fp);
		}
	}
	fclose(fp);
}

void read_layermaps_blob(std::string const &path, std::vector<std::map<std::string, layermap_entry>> &layermaps) {
	FILE *fp = fopen(path.c_str(), "rb");
	if (fp == NULL) {
		perror(path.c_str());
		exit(EXIT_OPEN);
	}
	uint32_t segs = 0;
	if (fread(&segs, sizeof(segs), 1, fp) != 1) {
		fclose(fp);
		return;
	}
	layermaps.resize(segs);
	for (uint32_t s = 0; s < segs; s++) {
		uint32_t layers = 0;
		fread(&layers, sizeof(layers), 1, fp);
		for (uint32_t l = 0; l < layers; l++) {
			uint32_t nlen = 0;
			fread(&nlen, sizeof(nlen), 1, fp);
			std::string name(nlen, '\0');
			fread(&name[0], 1, nlen, fp);
			uint32_t id = 0;
			int32_t minz = 0, maxz = 0;
			fread(&id, sizeof(id), 1, fp);
			fread(&minz, sizeof(minz), 1, fp);
			fread(&maxz, sizeof(maxz), 1, fp);
			layermap_entry e(id);
			e.minzoom = minz;
			e.maxzoom = maxz;
			layermaps[s].emplace(name, e);
		}
	}
	fclose(fp);
}

void write_skip_children_blob(std::string const &path, std::set<zxy> const &skip) {
	FILE *fp = fopen(path.c_str(), "wb");
	if (fp == NULL) {
		perror(path.c_str());
		exit(EXIT_WRITE);
	}
	uint32_t count = (uint32_t) skip.size();
	fwrite(&count, sizeof(count), 1, fp);
	for (auto const &t : skip) {
		int32_t z = (int32_t) t.z;
		int32_t x = (int32_t) t.x;
		int32_t y = (int32_t) t.y;
		fwrite(&z, sizeof(z), 1, fp);
		fwrite(&x, sizeof(x), 1, fp);
		fwrite(&y, sizeof(y), 1, fp);
	}
	fclose(fp);
}

void read_skip_children_blob(std::string const &path, std::set<zxy> &skip) {
	FILE *fp = fopen(path.c_str(), "rb");
	if (fp == NULL) {
		perror(path.c_str());
		exit(EXIT_OPEN);
	}
	uint32_t count = 0;
	if (fread(&count, sizeof(count), 1, fp) != 1) {
		fclose(fp);
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		int32_t z, x, y;
		fread(&z, sizeof(z), 1, fp);
		fread(&x, sizeof(x), 1, fp);
		fread(&y, sizeof(y), 1, fp);
		skip.insert(zxy(z, x, y));
	}
	fclose(fp);
}

void write_strategies_blob(std::string const &path, std::vector<strategy> const &strategies) {
	FILE *fp = fopen(path.c_str(), "wb");
	if (fp == NULL) {
		perror(path.c_str());
		exit(EXIT_WRITE);
	}
	uint32_t count = (uint32_t) strategies.size();
	fwrite(&count, sizeof(count), 1, fp);
	for (auto const &s : strategies) {
		uint64_t fields[] = {s.dropped_by_rate, s.dropped_by_gamma, s.dropped_as_needed, s.coalesced_as_needed,
				     s.detail_reduced, s.tiny_polygons, s.truncated_zooms, s.tile_size, s.feature_count};
		fwrite(fields, sizeof(fields), 1, fp);
	}
	fclose(fp);
}

void read_strategies_blob(std::string const &path, std::vector<strategy> &strategies) {
	FILE *fp = fopen(path.c_str(), "rb");
	if (fp == NULL) {
		perror(path.c_str());
		exit(EXIT_OPEN);
	}
	uint32_t count = 0;
	if (fread(&count, sizeof(count), 1, fp) != 1) {
		fclose(fp);
		return;
	}
	strategies.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		uint64_t fields[9];
		if (fread(fields, sizeof(fields), 1, fp) != 1) {
			break;
		}
		strategy s;
		s.dropped_by_rate = fields[0];
		s.dropped_by_gamma = fields[1];
		s.dropped_as_needed = fields[2];
		s.coalesced_as_needed = fields[3];
		s.detail_reduced = fields[4];
		s.tiny_polygons = fields[5];
		s.truncated_zooms = fields[6];
		s.tile_size = fields[7];
		s.feature_count = fields[8];
		strategies[i] = s;
	}
	fclose(fp);
}

}  // namespace

TilingRestore::TilingRestore() {
	geomfd.resize(TEMP_FILES, -1);
	geom_size.resize(TEMP_FILES, 0);
}

TilingRestore::~TilingRestore() {
	if (stringpool != nullptr && pool_size > 0) {
		munmap(stringpool, pool_size);
		stringpool = nullptr;
	}
	if (poolfd >= 0) {
		close(poolfd);
		poolfd = -1;
	}
	for (size_t j = 0; j < geomfd.size(); j++) {
		if (geomfd[j] >= 0) {
			close(geomfd[j]);
			geomfd[j] = -1;
		}
	}
	if (shared_nodes_map != nullptr && nodepos > 0) {
		munmap(shared_nodes_map, nodepos);
		shared_nodes_map = nullptr;
	}
}

std::string absolute_path_or_die(const char *path) {
	if (path == NULL || path[0] == '\0') {
		return "";
	}
	char resolved[PATH_MAX];
	if (realpath(path, resolved) == NULL) {
		// File may not exist yet (output); resolve directory + basename
		std::string p(path);
		size_t slash = p.find_last_of('/');
		if (slash == std::string::npos) {
			if (getcwd(resolved, sizeof(resolved)) == NULL) {
				perror("getcwd");
				exit(EXIT_STAT);
			}
			return std::string(resolved) + "/" + p;
		}
		std::string dir = p.substr(0, slash);
		std::string base = p.substr(slash + 1);
		char dirbuf[PATH_MAX];
		if (realpath(dir.c_str(), dirbuf) == NULL) {
			perror(dir.c_str());
			exit(EXIT_ARGS);
		}
		return std::string(dirbuf) + "/" + base;
	}
	return std::string(resolved);
}

std::vector<InputFileStat> stat_input_paths(std::vector<std::string> const &paths) {
	std::vector<InputFileStat> out;
	for (auto const &p : paths) {
		if (p.empty()) {
			continue;
		}
		struct stat st;
		if (stat(p.c_str(), &st) != 0) {
			perror(p.c_str());
			exit(EXIT_STAT);
		}
		InputFileStat s;
		s.path = absolute_path_or_die(p.c_str());
		s.size = st.st_size;
#if defined(__APPLE__) || defined(__FreeBSD__)
		s.mtime_sec = st.st_mtimespec.tv_sec;
		s.mtime_nsec = st.st_mtimespec.tv_nsec;
#else
		s.mtime_sec = st.st_mtim.tv_sec;
		s.mtime_nsec = st.st_mtim.tv_nsec;
#endif
		out.push_back(s);
	}
	std::sort(out.begin(), out.end(), [](InputFileStat const &a, InputFileStat const &b) { return a.path < b.path; });
	return out;
}

static bool is_fingerprint_ignored_flag(std::string const &tok) {
	return tok == "--checkpoint-force" || tok.rfind("--checkpoint-dir=", 0) == 0 || tok.rfind("--resume=", 0) == 0 ||
	       tok == "--checkpoint-dir" || tok == "--resume" || tok == "-F" || tok == "--allow-existing" || tok == "-f" ||
	       tok == "--force";
}

static std::string strip_shell_quotes(std::string const &tok) {
	if (tok.size() >= 2 && tok.front() == '\'' && tok.back() == '\'') {
		return tok.substr(1, tok.size() - 2);
	}
	return tok;
}

std::string normalize_command_line_for_fingerprint(std::string const &command_line) {
	std::string out;
	std::string tok;
	bool first = true;
	for (size_t i = 0; i <= command_line.size(); i++) {
		if (i == command_line.size() || command_line[i] == ' ') {
			if (!tok.empty()) {
				std::string bare = strip_shell_quotes(tok);
				if (!is_fingerprint_ignored_flag(bare)) {
					if (!first) {
						out.push_back(' ');
					}
					out += bare;
					first = false;
				}
				tok.clear();
			}
		} else {
			tok.push_back(command_line[i]);
		}
	}
	return out;
}

std::string compute_fingerprint(FingerprintParams const &params) {
	std::string normalized_cmd = normalize_command_line_for_fingerprint(params.command_line);
	std::string body = "{\"format_version\":";
	body += std::to_string(TIPPECANOE_CHECKPOINT_FORMAT);
	body += ",\"command_line\":\"";
	std::string esc;
	json_escape(normalized_cmd, esc);
	body += esc;
	body += "\",\"output_mode\":\"";
	esc.clear();
	json_escape(params.output_mode, esc);
	body += esc;
	body += "\",\"output_path\":\"";
	esc.clear();
	json_escape(params.output_path, esc);
	body += esc;
	body += "\",\"temp_files\":";
	body += std::to_string(params.temp_files);
	body += ",\"cpus\":";
	body += std::to_string(params.cpus);
	body += ",\"prevent\":\"";
	for (int i = 0; i < 256; i++) {
		if (params.prevent[i]) {
			body.push_back((char) i);
		}
	}
	body += "\",\"additional\":\"";
	for (int i = 0; i < 256; i++) {
		if (params.additional[i]) {
			body.push_back((char) i);
		}
	}
	body += "\",\"inputs\":[";
	for (size_t i = 0; i < params.inputs.size(); i++) {
		if (i > 0) {
			body += ",";
		}
		body += "{\"path\":\"";
		esc.clear();
		json_escape(params.inputs[i].path, esc);
		body += esc;
		body += "\",\"size\":";
		body += std::to_string(params.inputs[i].size);
		body += ",\"mtime_sec\":";
		body += std::to_string(params.inputs[i].mtime_sec);
		body += ",\"mtime_nsec\":";
		body += std::to_string(params.inputs[i].mtime_nsec);
		body += "}";
	}
	body += "]}";
	unsigned char hash[32];
	sha256((unsigned char const *) body.c_str(), body.size(), hash);
	return hex_encode(hash, 32);
}

Session::Session(const char *dir, bool is_resume)
    : dir_(dir),
      active_(true),
      is_resume_(is_resume) {
}

Session::~Session() {
	if (db_ != nullptr) {
		sqlite3_close(db_);
		db_ = nullptr;
	}
}

std::string Session::path_blob(const char *name) const {
	return dir_ + "/blobs/" + name;
}

std::string Session::path_staging(const char *name) const {
	return dir_ + "/staging/" + name;
}

void Session::fsync_path(std::string const &path) {
	int fd = open(path.c_str(), O_RDONLY);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

void Session::copy_fd_to_file(int fd, size_t nbytes, std::string const &dest) {
	FILE *out = fopen(dest.c_str(), "wb");
	if (out == NULL) {
		perror(dest.c_str());
		exit(EXIT_WRITE);
	}
	if (lseek(fd, 0, SEEK_SET) < 0) {
		perror("lseek checkpoint copy");
		exit(EXIT_SEEK);
	}
	char buf[65536];
	size_t remaining = nbytes;
	while (remaining > 0) {
		size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
		ssize_t n = read(fd, buf, chunk);
		if (n < 0) {
			perror("read checkpoint copy");
			exit(EXIT_READ);
		}
		if (n == 0) {
			break;
		}
		if (fwrite(buf, 1, (size_t) n, out) != (size_t) n) {
			perror(dest.c_str());
			exit(EXIT_WRITE);
		}
		remaining -= (size_t) n;
	}
	if (fclose(out) != 0) {
		perror(dest.c_str());
		exit(EXIT_CLOSE);
	}
	fsync_path(dest);
}

void Session::open_db(bool create) {
	std::string dbpath = dir_ + "/state.sqlite";
	if (create) {
		mkdir_p(dir_);
		mkdir_p(dir_ + "/blobs");
		mkdir_p(dir_ + "/staging");
	}
	if (sqlite3_open(dbpath.c_str(), &db_) != SQLITE_OK) {
		fprintf(stderr, "%s: checkpoint: cannot open %s: %s\n", *av, dbpath.c_str(), sqlite3_errmsg(db_));
		exit(EXIT_SQLITE);
	}
	exec_sql(db_, "PRAGMA journal_mode=WAL;");
	if (create) {
		init_schema();
	}
}

void Session::init_schema() {
	exec_sql(db_,
		 "CREATE TABLE IF NOT EXISTS job ("
		 "id INTEGER PRIMARY KEY CHECK (id = 1),"
		 "format_version INTEGER NOT NULL,"
		 "fingerprint TEXT NOT NULL,"
		 "command_line TEXT NOT NULL,"
		 "output_mode TEXT NOT NULL,"
		 "output_path TEXT NOT NULL,"
		 "temp_files INTEGER NOT NULL,"
		 "cpus INTEGER NOT NULL,"
		 "created_at INTEGER NOT NULL,"
		 "updated_at INTEGER NOT NULL"
		 ");"
		 "CREATE TABLE IF NOT EXISTS input_file ("
		 "path TEXT PRIMARY KEY,"
		 "size INTEGER NOT NULL,"
		 "mtime_sec INTEGER NOT NULL,"
		 "mtime_nsec INTEGER NOT NULL"
		 ");"
		 "CREATE TABLE IF NOT EXISTS tiling_state ("
		 "id INTEGER PRIMARY KEY CHECK (id = 1),"
		 "iz INTEGER NOT NULL,"
		 "minzoom INTEGER NOT NULL,"
		 "maxzoom INTEGER NOT NULL,"
		 "basezoom INTEGER NOT NULL,"
		 "last_completed_zoom INTEGER,"
		 "midx INTEGER NOT NULL,"
		 "midy INTEGER NOT NULL,"
		 "pool_off INTEGER NOT NULL,"
		 "initial_x INTEGER NOT NULL,"
		 "initial_y INTEGER NOT NULL,"
		 "nodepos INTEGER NOT NULL,"
		 "entry_snapshot_done INTEGER NOT NULL DEFAULT 0"
		 ");"
		 "CREATE TABLE IF NOT EXISTS zoom_commit ("
		 "zoom INTEGER PRIMARY KEY,"
		 "status TEXT NOT NULL,"
		 "committed_at INTEGER NOT NULL,"
		 "geom_total_bytes INTEGER NOT NULL,"
		 "generation INTEGER NOT NULL"
		 ");");
}

void Session::clear_workspace() {
	// Remove blobs and staging contents; keep state.sqlite shell for rewrite
	std::string rm_blobs = "rm -rf '" + dir_ + "/blobs' '" + dir_ + "/staging'";
	if (system(rm_blobs.c_str()) != 0) {
		fprintf(stderr, "%s: warning: could not clear checkpoint blobs\n", *av);
	}
	mkdir_p(dir_ + "/blobs");
	mkdir_p(dir_ + "/staging");
	exec_sql(db_, "DELETE FROM input_file; DELETE FROM tiling_state; DELETE FROM zoom_commit; DELETE FROM job;");
}

void Session::write_job_row(FingerprintParams const &params, std::string const &fingerprint) {
	sqlite3_stmt *stmt = NULL;
	int64_t ts = now_unix();
	const char *sql =
	    "INSERT OR REPLACE INTO job (id, format_version, fingerprint, command_line, output_mode, output_path, temp_files, cpus, created_at, updated_at) "
	    "VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL) != SQLITE_OK) {
		exit(EXIT_SQLITE);
	}
	sqlite3_bind_int(stmt, 1, TIPPECANOE_CHECKPOINT_FORMAT);
	sqlite3_bind_text(stmt, 2, fingerprint.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 3, params.command_line.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, params.output_mode.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 5, params.output_path.c_str(), -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 6, (sqlite3_int64) params.temp_files);
	sqlite3_bind_int64(stmt, 7, (sqlite3_int64) params.cpus);
	sqlite3_bind_int64(stmt, 8, ts);
	sqlite3_bind_int64(stmt, 9, ts);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		exit(EXIT_SQLITE);
	}
	sqlite3_finalize(stmt);

	exec_sql(db_, "DELETE FROM input_file;");
	for (auto const &in : params.inputs) {
		const char *isql = "INSERT INTO input_file (path, size, mtime_sec, mtime_nsec) VALUES (?, ?, ?, ?);";
		if (sqlite3_prepare_v2(db_, isql, -1, &stmt, NULL) != SQLITE_OK) {
			exit(EXIT_SQLITE);
		}
		sqlite3_bind_text(stmt, 1, in.path.c_str(), -1, SQLITE_STATIC);
		sqlite3_bind_int64(stmt, 2, in.size);
		sqlite3_bind_int64(stmt, 3, in.mtime_sec);
		sqlite3_bind_int64(stmt, 4, in.mtime_nsec);
		if (sqlite3_step(stmt) != SQLITE_DONE) {
			exit(EXIT_SQLITE);
		}
		sqlite3_finalize(stmt);
	}
}

void Session::verify_fingerprint_or_exit(FingerprintParams const &params) {
	std::string expected = compute_fingerprint(params);
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db_, "SELECT fingerprint FROM job WHERE id=1;", -1, &stmt, NULL) != SQLITE_OK) {
		exit(EXIT_SQLITE);
	}
	std::string stored;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		stored = (char const *) sqlite3_column_text(stmt, 0);
	}
	sqlite3_finalize(stmt);
	if (stored != expected) {
		fprintf(stderr, "%s: checkpoint fingerprint mismatch (command line, output path, or input files changed)\n", *av);
		fprintf(stderr, "  stored:   %s\n", stored.c_str());
		fprintf(stderr, "  expected: %s\n", expected.c_str());
		exit(EXIT_ARGS);
	}
}

std::unique_ptr<Session> Session::open_new(const char *dir, bool force, FingerprintParams const &params) {
	auto s = std::unique_ptr<Session>(new Session(dir, false));
	struct stat st;
	if (stat(dir, &st) == 0) {
		if (!S_ISDIR(st.st_mode)) {
			fprintf(stderr, "%s: %s is not a directory\n", *av, dir);
			exit(EXIT_ARGS);
		}
	} else {
		mkdir_p(dir);
	}

	std::string dbpath = std::string(dir) + "/state.sqlite";
	struct stat dbst;
	bool has_job = stat(dbpath.c_str(), &dbst) == 0;
	if (has_job && !force) {
		fprintf(stderr, "%s: checkpoint directory %s already has a job; use --checkpoint-force to replace\n", *av, dir);
		exit(EXIT_EXISTS);
	}

	s->open_db(true);
	if (has_job && force) {
		s->clear_workspace();
	}
	std::string fp = compute_fingerprint(params);
	s->write_job_row(params, fp);
	return s;
}

std::unique_ptr<Session> Session::open_resume(const char *dir, FingerprintParams const &params, bool allow_existing_output) {
	(void) allow_existing_output;
	if (params.output_mode == "pmtiles") {
		fprintf(stderr, "%s: --resume is not supported for PMTiles output; use -o out.mbtiles or -e directory first\n", *av);
		exit(EXIT_ARGS);
	}
	struct stat st;
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "%s: checkpoint directory %s does not exist\n", *av, dir);
		exit(EXIT_ARGS);
	}
	auto s = std::unique_ptr<Session>(new Session(dir, true));
	s->open_db(false);
	s->verify_fingerprint_or_exit(params);

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(s->db_,
			       "SELECT entry_snapshot_done, last_completed_zoom, iz, minzoom, maxzoom, basezoom, midx, midy FROM tiling_state WHERE id=1;",
			       -1, &stmt, NULL) != SQLITE_OK) {
		exit(EXIT_SQLITE);
	}
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		s->entry_snapshot_done_ = sqlite3_column_int(stmt, 0) != 0;
		if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
			s->last_completed_zoom_ = sqlite3_column_int(stmt, 1);
		}
		s->resume_iz_ = sqlite3_column_int(stmt, 2);
		s->tiling_minzoom_ = sqlite3_column_int(stmt, 3);
		s->tiling_maxzoom_ = sqlite3_column_int(stmt, 4);
		s->tiling_basezoom_ = sqlite3_column_int(stmt, 5);
		s->tiling_midx_ = (unsigned) sqlite3_column_int(stmt, 6);
		s->tiling_midy_ = (unsigned) sqlite3_column_int(stmt, 7);
	}
	sqlite3_finalize(stmt);

	if (sqlite3_prepare_v2(s->db_, "SELECT MAX(generation) FROM zoom_commit;", -1, &stmt, NULL) == SQLITE_OK) {
		if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
			s->generation_ = (uint64_t) sqlite3_column_int64(stmt, 0);
		}
		sqlite3_finalize(stmt);
	}

	if (s->last_completed_zoom_ >= 0) {
		s->resume_iz_ = s->last_completed_zoom_ + 1;
	}

	return s;
}

bool Session::can_resume_tiling() const {
	return is_resume_ && entry_snapshot_done_;
}

int Session::resume_iz() const {
	return resume_iz_;
}

void Session::commit_generation() {
	generation_++;
}

void Session::snapshot_tiling_entry(int poolfd, size_t pool_size, long long const *pool_off, unsigned const *initial_x, unsigned const *initial_y,
				   int geomfd, off_t geom_size, node *shared_nodes_map, size_t nodepos, std::string const &shared_nodes_bloom,
				   std::vector<std::map<std::string, layermap_entry>> const &layermaps, int iz, int minzoom, int maxzoom, int basezoom) {
	if (!active_) {
		return;
	}

	std::string staging = dir_ + "/staging";
	mkdir_p(staging);

	if (pool_size > 0) {
		copy_fd_to_file(poolfd, pool_size, path_staging("stringpool"));
	}
	if (geom_size > 0 && geomfd >= 0) {
		copy_fd_to_file(geomfd, (size_t) geom_size, path_staging("geom.0"));
	}
	if (nodepos > 0 && shared_nodes_map != nullptr) {
		write_blob_file(path_staging("shared_nodes"), shared_nodes_map, nodepos);
	}
	if (!shared_nodes_bloom.empty()) {
		write_blob_file(path_staging("shared_nodes.bloom"), shared_nodes_bloom.data(), shared_nodes_bloom.size());
	}

	write_blob_file(path_staging("pool_off.bin"), pool_off, sizeof(long long) * CPUS);
	write_blob_file(path_staging("initial_x.bin"), initial_x, sizeof(unsigned) * CPUS);
	write_blob_file(path_staging("initial_y.bin"), initial_y, sizeof(unsigned) * CPUS);
	write_layermaps_blob(path_staging("layermaps.bin"), layermaps);

	// Promote staging -> blobs
	rename(path_staging("stringpool").c_str(), path_blob("stringpool").c_str());
	if (geom_size > 0) {
		rename(path_staging("geom.0").c_str(), path_blob("geom.0").c_str());
	}
	if (nodepos > 0) {
		rename(path_staging("shared_nodes").c_str(), path_blob("shared_nodes").c_str());
	}
	if (!shared_nodes_bloom.empty()) {
		rename(path_staging("shared_nodes.bloom").c_str(), path_blob("shared_nodes.bloom").c_str());
	}
	rename(path_staging("pool_off.bin").c_str(), path_blob("pool_off.bin").c_str());
	rename(path_staging("initial_x.bin").c_str(), path_blob("initial_x.bin").c_str());
	rename(path_staging("initial_y.bin").c_str(), path_blob("initial_y.bin").c_str());
	rename(path_staging("layermaps.bin").c_str(), path_blob("layermaps.bin").c_str());

	commit_generation();

	sqlite3_stmt *stmt = NULL;
	const char *sql =
	    "INSERT OR REPLACE INTO tiling_state (id, iz, minzoom, maxzoom, basezoom, last_completed_zoom, midx, midy, pool_off, initial_x, initial_y, nodepos, entry_snapshot_done) "
	    "VALUES (1, ?, ?, ?, ?, NULL, 0, 0, 0, 0, 0, ?, 1);";
	if (sqlite3_prepare_v2(db_, sql, -1, &stmt, NULL) != SQLITE_OK) {
		exit(EXIT_SQLITE);
	}
	sqlite3_bind_int(stmt, 1, iz);
	sqlite3_bind_int(stmt, 2, minzoom);
	sqlite3_bind_int(stmt, 3, maxzoom);
	sqlite3_bind_int(stmt, 4, basezoom);
	sqlite3_bind_int64(stmt, 5, (sqlite3_int64) nodepos);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		exit(EXIT_SQLITE);
	}
	sqlite3_finalize(stmt);

	entry_snapshot_done_ = true;
	resume_iz_ = iz;
	tiling_minzoom_ = minzoom;
	tiling_maxzoom_ = maxzoom;
	tiling_basezoom_ = basezoom;
}

bool Session::restore_tiling(TilingRestore &out) {
	if (!can_resume_tiling()) {
		return false;
	}

	struct stat pst;
	std::string poolpath = path_blob("stringpool");
	if (stat(poolpath.c_str(), &pst) == 0 && pst.st_size > 0) {
		out.pool_size = (size_t) pst.st_size;
		out.poolfd = open(poolpath.c_str(), O_RDONLY | O_CLOEXEC);
		if (out.poolfd < 0) {
			perror(poolpath.c_str());
			exit(EXIT_OPEN);
		}
		out.stringpool = (char *) mmap(NULL, out.pool_size, PROT_READ, MAP_PRIVATE, out.poolfd, 0);
		if (out.stringpool == MAP_FAILED) {
			perror("mmap checkpoint stringpool");
			exit(EXIT_MEMORY);
		}
	}

	out.geomfd.resize(TEMP_FILES, -1);
	out.geom_size.resize(TEMP_FILES, 0);
	std::string geom0 = path_blob("geom.0");
	struct stat gst;
	if (stat(geom0.c_str(), &gst) == 0) {
		out.geomfd[0] = open(geom0.c_str(), O_RDONLY | O_CLOEXEC);
		out.geom_size[0] = gst.st_size;
	}
	for (size_t j = 1; j < TEMP_FILES; j++) {
		char name[32];
		snprintf(name, sizeof(name), "geom.%zu", j);
		std::string gp = path_blob(name);
		struct stat gs;
		if (stat(gp.c_str(), &gs) == 0) {
			out.geomfd[j] = open(gp.c_str(), O_RDONLY | O_CLOEXEC);
			out.geom_size[j] = gs.st_size;
		}
	}

	FILE *fp = fopen(path_blob("pool_off.bin").c_str(), "rb");
	if (fp != NULL) {
		out.pool_off.resize(CPUS);
		fread(out.pool_off.data(), sizeof(long long), CPUS, fp);
		fclose(fp);
	}
	fp = fopen(path_blob("initial_x.bin").c_str(), "rb");
	if (fp != NULL) {
		out.initial_x.resize(CPUS);
		fread(out.initial_x.data(), sizeof(unsigned), CPUS, fp);
		fclose(fp);
	}
	fp = fopen(path_blob("initial_y.bin").c_str(), "rb");
	if (fp != NULL) {
		out.initial_y.resize(CPUS);
		fread(out.initial_y.data(), sizeof(unsigned), CPUS, fp);
		fclose(fp);
	}

	read_layermaps_blob(path_blob("layermaps.bin"), out.layermaps);

	std::string nodespath = path_blob("shared_nodes");
	struct stat nst;
	if (stat(nodespath.c_str(), &nst) == 0 && nst.st_size > 0) {
		out.nodepos = (size_t) nst.st_size;
		int nfd = open(nodespath.c_str(), O_RDONLY | O_CLOEXEC);
		out.shared_nodes_map = (node *) mmap(NULL, out.nodepos, PROT_READ, MAP_PRIVATE, nfd, 0);
		close(nfd);
		if (out.shared_nodes_map == MAP_FAILED) {
			perror("mmap shared_nodes");
			exit(EXIT_MEMORY);
		}
	}

	std::string bloompath = path_blob("shared_nodes.bloom");
	struct stat bst;
	if (stat(bloompath.c_str(), &bst) == 0) {
		out.shared_nodes_bloom.resize((size_t) bst.st_size);
		fp = fopen(bloompath.c_str(), "rb");
		if (fp != NULL) {
			fread(&out.shared_nodes_bloom[0], 1, out.shared_nodes_bloom.size(), fp);
			fclose(fp);
		}
	}

	read_skip_children_blob(path_blob("skip_children.bin"), out.skip_children);
	read_strategies_blob(path_blob("strategies.bin"), out.strategies);

	out.iz = resume_iz_;
	out.minzoom = tiling_minzoom_;
	out.maxzoom = tiling_maxzoom_;
	out.basezoom = tiling_basezoom_;
	out.midx = tiling_midx_;
	out.midy = tiling_midy_;

	return true;
}

void Session::on_zoom_complete(ZoomCompleteContext const &ctx) {
	if (!active_ || ctx.geomfd == nullptr || ctx.geom_size == nullptr) {
		return;
	}

	std::string staging = dir_ + "/staging";
	mkdir_p(staging);

	off_t total_geom = 0;
	for (size_t j = 0; j < TEMP_FILES; j++) {
		char name[32];
		snprintf(name, sizeof(name), "geom.%zu", j);
		if (ctx.geomfd[j] >= 0 && ctx.geom_size[j] > 0) {
			copy_fd_to_file(ctx.geomfd[j], (size_t) ctx.geom_size[j], path_staging(name));
			total_geom += ctx.geom_size[j];
		}
	}
	if (ctx.skip_children != nullptr) {
		write_skip_children_blob(path_staging("skip_children.bin"), *ctx.skip_children);
	}
	if (ctx.strategies != nullptr) {
		write_strategies_blob(path_staging("strategies.bin"), *ctx.strategies);
	}

	for (size_t j = 0; j < TEMP_FILES; j++) {
		char name[32];
		snprintf(name, sizeof(name), "geom.%zu", j);
		std::string sp = path_staging(name);
		struct stat st;
		if (stat(sp.c_str(), &st) == 0) {
			rename(sp.c_str(), path_blob(name).c_str());
		}
	}
	{
		std::string sc = path_staging("skip_children.bin");
		struct stat st;
		if (stat(sc.c_str(), &st) == 0) {
			rename(sc.c_str(), path_blob("skip_children.bin").c_str());
		}
	}
	{
		std::string stg = path_staging("strategies.bin");
		struct stat st;
		if (stat(stg.c_str(), &st) == 0) {
			rename(stg.c_str(), path_blob("strategies.bin").c_str());
		}
	}

	commit_generation();

	int64_t ts = now_unix();
	sqlite3_stmt *stmt = NULL;
	const char *zsql =
	    "INSERT OR REPLACE INTO zoom_commit (zoom, status, committed_at, geom_total_bytes, generation) VALUES (?, 'committed', ?, ?, ?);";
	if (sqlite3_prepare_v2(db_, zsql, -1, &stmt, NULL) != SQLITE_OK) {
		exit(EXIT_SQLITE);
	}
	sqlite3_bind_int(stmt, 1, ctx.zoom);
	sqlite3_bind_int64(stmt, 2, ts);
	sqlite3_bind_int64(stmt, 3, (sqlite3_int64) total_geom);
	sqlite3_bind_int64(stmt, 4, (sqlite3_int64) generation_);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		exit(EXIT_SQLITE);
	}
	sqlite3_finalize(stmt);

	const char *tsql = "UPDATE tiling_state SET last_completed_zoom=?, maxzoom=?, midx=?, midy=? WHERE id=1;";
	if (sqlite3_prepare_v2(db_, tsql, -1, &stmt, NULL) != SQLITE_OK) {
		exit(EXIT_SQLITE);
	}
	sqlite3_bind_int(stmt, 1, ctx.zoom);
	sqlite3_bind_int(stmt, 2, ctx.maxzoom);
	sqlite3_bind_int(stmt, 3, (int) ctx.midx);
	sqlite3_bind_int(stmt, 4, (int) ctx.midy);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		exit(EXIT_SQLITE);
	}
	sqlite3_finalize(stmt);

	last_completed_zoom_ = ctx.zoom;
	tiling_maxzoom_ = ctx.maxzoom;
	tiling_midx_ = ctx.midx;
	tiling_midy_ = ctx.midy;
	resume_iz_ = ctx.zoom + 1;
}

void Session::finalize_success() {
	if (!active_ || db_ == nullptr) {
		return;
	}
	int64_t ts = now_unix();
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db_, "UPDATE job SET updated_at=? WHERE id=1;", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, ts);
		sqlite3_step(stmt);
		sqlite3_finalize(stmt);
	}
	if (sqlite3_close(db_) != SQLITE_OK) {
		fprintf(stderr, "%s: checkpoint: sqlite close: %s\n", *av, sqlite3_errmsg(db_));
	}
	db_ = nullptr;
}

}  // namespace checkpoint
