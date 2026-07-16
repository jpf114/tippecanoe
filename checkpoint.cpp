#include "checkpoint.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/sendfile.h>
#endif

#include "errors.hpp"
#include "jsonpull/jsonpull.h"
#include "serial.hpp"

extern char **av;

namespace checkpoint {

// ===========================================================================
// 匿名命名空间：内部辅助函数
// ===========================================================================

namespace {

// --- SHA-256 指纹计算（用于 checkpoint 内部一致性校验）---

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

// --- 信号处理 ---

volatile sig_atomic_t g_shutdown_requested = 0;

void signal_handler(int sig) {
	(void) sig;
	g_shutdown_requested = 1;
}

// --- JSON 序列化辅助 ---

void jw_str(std::string &out, const char *key, std::string const &val, bool first = false) {
	if (!first) out += ",";
	out += "\"";
	out += key;
	out += "\":\"";
	json_escape(val, out);
	out += "\"";
}

void jw_int(std::string &out, const char *key, long long val, bool first = false) {
	if (!first) out += ",";
	out += "\"";
	out += key;
	out += "\":";
	out += std::to_string(val);
}

void jw_uint(std::string &out, const char *key, uint64_t val, bool first = false) {
	if (!first) out += ",";
	out += "\"";
	out += key;
	out += "\":";
	out += std::to_string(val);
}

void jw_bool(std::string &out, const char *key, bool val, bool first = false) {
	if (!first) out += ",";
	out += "\"";
	out += key;
	out += "\":";
	out += val ? "true" : "false";
}

// 序列化 CheckpointState 为 JSON 字符串
std::string serialize_state(CheckpointState const &s) {
	std::string j;
	j += "{";
	jw_int(j, "format_version", s.format_version, true);
	jw_str(j, "fingerprint", s.fingerprint);
	jw_str(j, "command_line", s.command_line);
	jw_str(j, "normalized_cmd", s.normalized_cmd);
	jw_str(j, "output_mode", s.output_mode);
	jw_str(j, "output_path", s.output_path);
	jw_int(j, "temp_files", (long long) s.temp_files);
	jw_int(j, "cpus", (long long) s.cpus);
	jw_int(j, "created_at", s.created_at);
	jw_int(j, "updated_at", s.updated_at);

	// input_files 数组
	j += ",\"input_files\":[";
	for (size_t i = 0; i < s.input_files.size(); i++) {
		if (i > 0) j += ",";
		j += "{";
		jw_str(j, "path", s.input_files[i].path, true);
		jw_int(j, "size", s.input_files[i].size);
		jw_int(j, "mtime_sec", s.input_files[i].mtime_sec);
		jw_int(j, "mtime_nsec", s.input_files[i].mtime_nsec);
		j += "}";
	}
	j += "]";

	// tiling 对象
	j += ",\"tiling\":{";
	jw_int(j, "iz", s.iz, true);
	jw_int(j, "minzoom", s.minzoom);
	jw_int(j, "maxzoom", s.maxzoom);
	jw_int(j, "basezoom", s.basezoom);
	jw_int(j, "last_completed_zoom", s.last_completed_zoom);
	jw_int(j, "midx", (long long) s.midx);
	jw_int(j, "midy", (long long) s.midy);
	jw_int(j, "nodepos", s.nodepos);
	jw_bool(j, "entry_snapshot_done", s.entry_snapshot_done);
	j += "}";

	jw_uint(j, "generation", s.generation);

	// zoom_commits 对象
	j += ",\"zoom_commits\":{";
	bool first_commit = true;
	for (auto const &kv : s.zoom_commits) {
		if (!first_commit) j += ",";
		first_commit = false;
		j += "\"";
		j += std::to_string(kv.first);
		j += "\":{";
		jw_int(j, "committed_at", kv.second.committed_at, true);
		jw_int(j, "geom_total_bytes", kv.second.geom_total_bytes);
		jw_uint(j, "generation", kv.second.generation);
		j += "}";
	}
	j += "}";

	j += "}";
	return j;
}

// 从 JSON 字符串解析 CheckpointState（使用 jsonpull）
bool parse_state(std::string const &json_str, CheckpointState &out) {
	json_pull *jp = json_begin_string(json_str.c_str());
	if (jp == NULL) {
		return false;
	}
	json_object *root = json_read_tree(jp);
	if (root == NULL || root->type != JSON_HASH) {
		if (root) json_free(root);
		json_end(jp);
		return false;
	}

	json_object *v;

	v = json_hash_get(root, "format_version");
	if (v && v->type == JSON_NUMBER) out.format_version = (int) v->value.number.number;

	v = json_hash_get(root, "fingerprint");
	if (v && v->type == JSON_STRING) out.fingerprint = v->value.string.string;

	v = json_hash_get(root, "command_line");
	if (v && v->type == JSON_STRING) out.command_line = v->value.string.string;

	v = json_hash_get(root, "normalized_cmd");
	if (v && v->type == JSON_STRING) out.normalized_cmd = v->value.string.string;

	v = json_hash_get(root, "output_mode");
	if (v && v->type == JSON_STRING) out.output_mode = v->value.string.string;

	v = json_hash_get(root, "output_path");
	if (v && v->type == JSON_STRING) out.output_path = v->value.string.string;

	v = json_hash_get(root, "temp_files");
	if (v && v->type == JSON_NUMBER) out.temp_files = (size_t) v->value.number.number;

	v = json_hash_get(root, "cpus");
	if (v && v->type == JSON_NUMBER) out.cpus = (size_t) v->value.number.number;

	v = json_hash_get(root, "created_at");
	if (v && v->type == JSON_NUMBER) out.created_at = (int64_t) v->value.number.number;

	v = json_hash_get(root, "updated_at");
	if (v && v->type == JSON_NUMBER) out.updated_at = (int64_t) v->value.number.number;

	// input_files
	v = json_hash_get(root, "input_files");
	if (v && v->type == JSON_ARRAY) {
		for (size_t i = 0; i < v->value.array.length; i++) {
			json_object *item = v->value.array.array[i];
			if (item && item->type == JSON_HASH) {
				InputFileStat ifs;
				json_object *p = json_hash_get(item, "path");
				if (p && p->type == JSON_STRING) ifs.path = p->value.string.string;
				json_object *sz = json_hash_get(item, "size");
				if (sz && sz->type == JSON_NUMBER) ifs.size = (int64_t) sz->value.number.number;
				json_object *ms = json_hash_get(item, "mtime_sec");
				if (ms && ms->type == JSON_NUMBER) ifs.mtime_sec = (int64_t) ms->value.number.number;
				json_object *mn = json_hash_get(item, "mtime_nsec");
				if (mn && mn->type == JSON_NUMBER) ifs.mtime_nsec = (int64_t) mn->value.number.number;
				out.input_files.push_back(ifs);
			}
		}
	}

	// tiling
	json_object *t = json_hash_get(root, "tiling");
	if (t && t->type == JSON_HASH) {
		v = json_hash_get(t, "iz");
		if (v && v->type == JSON_NUMBER) out.iz = (int) v->value.number.number;
		v = json_hash_get(t, "minzoom");
		if (v && v->type == JSON_NUMBER) out.minzoom = (int) v->value.number.number;
		v = json_hash_get(t, "maxzoom");
		if (v && v->type == JSON_NUMBER) out.maxzoom = (int) v->value.number.number;
		v = json_hash_get(t, "basezoom");
		if (v && v->type == JSON_NUMBER) out.basezoom = (int) v->value.number.number;
		v = json_hash_get(t, "last_completed_zoom");
		if (v && v->type == JSON_NUMBER) out.last_completed_zoom = (int) v->value.number.number;
		v = json_hash_get(t, "midx");
		if (v && v->type == JSON_NUMBER) out.midx = (unsigned) v->value.number.number;
		v = json_hash_get(t, "midy");
		if (v && v->type == JSON_NUMBER) out.midy = (unsigned) v->value.number.number;
		v = json_hash_get(t, "nodepos");
		if (v && v->type == JSON_NUMBER) out.nodepos = (int64_t) v->value.number.number;
		v = json_hash_get(t, "entry_snapshot_done");
		if (v && (v->type == JSON_TRUE || v->type == JSON_FALSE)) out.entry_snapshot_done = (v->type == JSON_TRUE);
	}

	v = json_hash_get(root, "generation");
	if (v && v->type == JSON_NUMBER) out.generation = (uint64_t) v->value.number.number;

	// zoom_commits
	json_object *zc = json_hash_get(root, "zoom_commits");
	if (zc && zc->type == JSON_HASH) {
		for (size_t i = 0; i < zc->value.object.length; i++) {
			json_object *key = zc->value.object.keys[i];
			json_object *val = zc->value.object.values[i];
			if (key && key->type == JSON_STRING && val && val->type == JSON_HASH) {
				int zoom = atoi(key->value.string.string);
				ZoomCommit commit;
				json_object *ca = json_hash_get(val, "committed_at");
				if (ca && ca->type == JSON_NUMBER) commit.committed_at = (int64_t) ca->value.number.number;
				json_object *gtb = json_hash_get(val, "geom_total_bytes");
				if (gtb && gtb->type == JSON_NUMBER) commit.geom_total_bytes = (int64_t) gtb->value.number.number;
				json_object *gen = json_hash_get(val, "generation");
				if (gen && gen->type == JSON_NUMBER) commit.generation = (uint64_t) gen->value.number.number;
				out.zoom_commits[zoom] = commit;
			}
		}
	}

	json_free(root);
	json_end(jp);
	return true;
}

// 读取整个文件为字符串
std::string read_file_to_string(std::string const &path) {
	FILE *fp = fopen(path.c_str(), "rb");
	if (fp == NULL) {
		return "";
	}
	std::string out;
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz > 0) {
		out.resize((size_t) sz);
		if (fread(&out[0], 1, (size_t) sz, fp) != (size_t) sz) {
			fclose(fp);
			return "";
		}
	}
	fclose(fp);
	return out;
}

// C 递归删除目录（替代 system("rm -rf")，避免命令注入）
void rmrf_path(std::string const &path) {
	struct stat st;
	if (stat(path.c_str(), &st) != 0) {
		return;
	}
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path.c_str());
		if (d != NULL) {
			struct dirent *dp;
			while ((dp = readdir(d)) != NULL) {
				if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0) {
					continue;
				}
				std::string child = path + "/" + dp->d_name;
				rmrf_path(child);
			}
			closedir(d);
		}
		rmdir(path.c_str());
	} else {
		unlink(path.c_str());
	}
}

}  // namespace

// ===========================================================================
// 信号处理公开接口
// ===========================================================================

void install_signal_handlers() {
	struct sigaction sa;
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
}

bool shutdown_requested() {
	return g_shutdown_requested != 0;
}

// ===========================================================================
// layermaps blob 格式 v3（与之前版本完全一致）
// ===========================================================================

constexpr uint32_t LAYERMAPS_FORMAT_VERSION = 3;

static void write_str_field(FILE *fp, std::string const &s) {
	uint32_t nlen = (uint32_t) s.size();
	fwrite(&nlen, sizeof(nlen), 1, fp);
	if (nlen > 0) {
		fwrite(s.data(), 1, nlen, fp);
	}
}

static bool read_str_field(FILE *fp, std::string &out) {
	uint32_t nlen = 0;
	if (fread(&nlen, sizeof(nlen), 1, fp) != 1) {
		return false;
	}
	out.assign(nlen, '\0');
	if (nlen > 0 && fread(&out[0], 1, nlen, fp) != nlen) {
		return false;
	}
	return true;
}

static void write_tilestat_entry(FILE *fp, std::string const &attr_name, tilestat const &ts) {
	write_str_field(fp, attr_name);
	fwrite(&ts.min, sizeof(double), 1, fp);
	fwrite(&ts.max, sizeof(double), 1, fp);
	int32_t type = ts.type;
	fwrite(&type, sizeof(type), 1, fp);
	uint32_t scount = (uint32_t) ts.sample_values.size();
	fwrite(&scount, sizeof(scount), 1, fp);
	for (auto const &sv : ts.sample_values) {
		int32_t sv_type = sv.type;
		fwrite(&sv_type, sizeof(sv_type), 1, fp);
		write_str_field(fp, sv.s);
	}
}

static bool read_tilestat_entry(FILE *fp, std::string &attr_name, tilestat &ts) {
	if (!read_str_field(fp, attr_name)) {
		return false;
	}
	if (fread(&ts.min, sizeof(double), 1, fp) != 1) return false;
	if (fread(&ts.max, sizeof(double), 1, fp) != 1) return false;
	int32_t type = 0;
	if (fread(&type, sizeof(type), 1, fp) != 1) return false;
	ts.type = type;
	uint32_t scount = 0;
	if (fread(&scount, sizeof(scount), 1, fp) != 1) return false;
	ts.sample_values.clear();
	for (uint32_t i = 0; i < scount; i++) {
		int32_t sv_type = 0;
		if (fread(&sv_type, sizeof(sv_type), 1, fp) != 1) return false;
		std::string s;
		if (!read_str_field(fp, s)) return false;
		ts.sample_values.push_back(serial_val(sv_type, s));
	}
	return true;
}

void write_layermaps_blob(std::string const &path, std::vector<std::map<std::string, layermap_entry>> const &layermaps) {
	FILE *fp = fopen(path.c_str(), "wb");
	if (fp == NULL) {
		perror(path.c_str());
		exit(EXIT_WRITE);
	}
	uint32_t version = LAYERMAPS_FORMAT_VERSION;
	fwrite(&version, sizeof(version), 1, fp);
	uint32_t segs = (uint32_t) layermaps.size();
	fwrite(&segs, sizeof(segs), 1, fp);
	for (auto const &seg : layermaps) {
		uint32_t layers = (uint32_t) seg.size();
		fwrite(&layers, sizeof(layers), 1, fp);
		for (auto const &kv : seg) {
			write_str_field(fp, kv.first);
			uint32_t id = (uint32_t) kv.second.id;
			int32_t minz = kv.second.minzoom;
			int32_t maxz = kv.second.maxzoom;
			size_t points = kv.second.points;
			size_t lines = kv.second.lines;
			size_t polygons = kv.second.polygons;
			size_t retain = kv.second.retain;
			fwrite(&id, sizeof(id), 1, fp);
			fwrite(&minz, sizeof(minz), 1, fp);
			fwrite(&maxz, sizeof(maxz), 1, fp);
			fwrite(&points, sizeof(points), 1, fp);
			fwrite(&lines, sizeof(lines), 1, fp);
			fwrite(&polygons, sizeof(polygons), 1, fp);
			fwrite(&retain, sizeof(retain), 1, fp);
			write_str_field(fp, kv.second.description);
			uint32_t tcount = (uint32_t) kv.second.tilestats.size();
			fwrite(&tcount, sizeof(tcount), 1, fp);
			for (auto const &ts : kv.second.tilestats) {
				write_tilestat_entry(fp, ts.first, ts.second);
			}
		}
	}
	fclose(fp);
}

void read_layermaps_blob(std::string const &path, std::vector<std::map<std::string, layermap_entry>> &layermaps) {
	FILE *fp = fopen(path.c_str(), "rb");
	if (fp == NULL) {
		return;
	}
	uint32_t version = 0;
	if (fread(&version, sizeof(version), 1, fp) != 1) {
		fclose(fp);
		return;
	}
	if (version != LAYERMAPS_FORMAT_VERSION) {
		fprintf(stderr, "%s: warning: layermaps.bin format version %u does not match expected %u; vector_layers metadata will be empty\n",
			*av, version, LAYERMAPS_FORMAT_VERSION);
		fclose(fp);
		return;
	}
	uint32_t segs = 0;
	if (fread(&segs, sizeof(segs), 1, fp) != 1) {
		fclose(fp);
		return;
	}
	layermaps.resize(segs);
	for (uint32_t s = 0; s < segs; s++) {
		uint32_t layers = 0;
		if (fread(&layers, sizeof(layers), 1, fp) != 1) {
			break;
		}
		for (uint32_t l = 0; l < layers; l++) {
			std::string name;
			if (!read_str_field(fp, name)) {
				break;
			}
			uint32_t id = 0;
			int32_t minz = 0, maxz = 0;
			size_t points = 0, lines = 0, polygons = 0, retain = 0;
			if (fread(&id, sizeof(id), 1, fp) != 1) break;
			if (fread(&minz, sizeof(minz), 1, fp) != 1) break;
			if (fread(&maxz, sizeof(maxz), 1, fp) != 1) break;
			if (fread(&points, sizeof(points), 1, fp) != 1) break;
			if (fread(&lines, sizeof(lines), 1, fp) != 1) break;
			if (fread(&polygons, sizeof(polygons), 1, fp) != 1) break;
			if (fread(&retain, sizeof(retain), 1, fp) != 1) break;
			std::string description;
			if (!read_str_field(fp, description)) break;
			uint32_t tcount = 0;
			if (fread(&tcount, sizeof(tcount), 1, fp) != 1) break;
			layermap_entry e(id);
			e.minzoom = minz;
			e.maxzoom = maxz;
			e.points = points;
			e.lines = lines;
			e.polygons = polygons;
			e.retain = retain;
			e.description = description;
			for (uint32_t t = 0; t < tcount; t++) {
				std::string attr_name;
				tilestat ts;
				if (!read_tilestat_entry(fp, attr_name, ts)) {
					break;
				}
				e.tilestats.emplace(attr_name, std::move(ts));
			}
			layermaps[s].emplace(name, std::move(e));
		}
	}
	fclose(fp);
}

// ===========================================================================
// skip_children / strategies blob 读写（与之前版本完全一致）
// ===========================================================================

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
		return;
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
		return;
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

// ===========================================================================
// TilingRestore 构造/析构（与之前版本一致）
// ===========================================================================

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

// ===========================================================================
// 路径与指纹工具函数（与之前版本一致）
// ===========================================================================

std::string absolute_path_or_die(const char *path) {
	if (path == NULL || path[0] == '\0') {
		return "";
	}
	char resolved[PATH_MAX];
	if (realpath(path, resolved) == NULL) {
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

static bool is_fingerprint_ignored_flag_with_arg(std::string const &tok) {
	return tok == "--checkpoint-dir" || tok == "--resume";
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
	bool skip_next = false;
	for (size_t i = 0; i <= command_line.size(); i++) {
		if (i == command_line.size() || command_line[i] == ' ') {
			if (!tok.empty()) {
				std::string bare = strip_shell_quotes(tok);
				if (skip_next) {
					skip_next = false;
				} else if (!is_fingerprint_ignored_flag(bare)) {
					if (!first) {
						out.push_back(' ');
					}
					out += bare;
					first = false;
				}
				if (is_fingerprint_ignored_flag_with_arg(bare)) {
					skip_next = true;
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

// ===========================================================================
// Session 实现
// ===========================================================================

Session::Session(const char *dir, bool is_resume)
    : dir_(dir),
      active_(true),
      is_resume_(is_resume),
      start_time_(now_unix()) {
}

Session::~Session() {
	release_lock();
}

std::string Session::path_blob(const char *name) const {
	return dir_ + "/blobs/" + name;
}

std::string Session::path_staging(const char *name) const {
	return dir_ + "/staging/" + name;
}

std::string Session::path_commits() const {
	return dir_ + "/commits";
}

void Session::fsync_path(std::string const &path) {
	int fd = open(path.c_str(), O_RDONLY);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

void Session::fsync_dir(std::string const &dirpath) {
	int fd = open(dirpath.c_str(), O_RDONLY | O_DIRECTORY);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

void Session::copy_fd_to_file(int fd, size_t nbytes, std::string const &dest) {
	off_t saved_pos = lseek(fd, 0, SEEK_CUR);
	int outfd = open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (outfd < 0) {
		perror(dest.c_str());
		exit(EXIT_WRITE);
	}

	size_t remaining = nbytes;

#ifdef __linux__
	// 使用 sendfile 零拷贝（Linux 专有），不修改 fd 偏移量
	{
		off_t offset = 0;
		while (remaining > 0) {
			ssize_t n = sendfile(outfd, fd, &offset, remaining);
			if (n > 0) {
				remaining -= (size_t) n;
				continue;
			}
			if (n == 0) break;
			if (errno == EINVAL || errno == ENOSYS) break;  // 回退到 read+write
			perror("sendfile checkpoint copy");
			exit(EXIT_WRITE);
		}
	}
#endif

	// read+write 回退路径（1MB 缓冲区）
	if (remaining > 0) {
		if (lseek(fd, 0, SEEK_SET) < 0) {
			perror("lseek checkpoint copy");
			exit(EXIT_SEEK);
		}
		char buf[1 << 20];  // 1MB
		while (remaining > 0) {
			size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
			ssize_t rn = read(fd, buf, chunk);
			if (rn <= 0) break;
			ssize_t wn = 0;
			while (wn < rn) {
				ssize_t w = write(outfd, (char *) buf + wn, (size_t)(rn - wn));
				if (w < 0) {
					perror("write checkpoint copy");
					exit(EXIT_WRITE);
				}
				wn += w;
			}
			remaining -= (size_t) rn;
		}
	}

	if (fsync(outfd) < 0) {
		perror("fsync checkpoint copy");
	}
	if (close(outfd) < 0) {
		perror("close checkpoint copy");
		exit(EXIT_CLOSE);
	}
	lseek(fd, saved_pos, SEEK_SET);  // 恢复原始偏移量
}

void Session::rmrf(std::string const &path) {
	rmrf_path(path);
}

// --- 并发锁（flock）---

void Session::acquire_lock() {
	std::string lockpath = dir_ + "/.lock";
	lock_fd_ = open(lockpath.c_str(), O_CREAT | O_RDWR, 0644);
	if (lock_fd_ < 0) {
		perror(lockpath.c_str());
		exit(EXIT_OPEN);
	}
	if (flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
		fprintf(stderr, "%s: checkpoint directory %s is locked by another process\n", *av, dir_.c_str());
		close(lock_fd_);
		lock_fd_ = -1;
		exit(EXIT_ARGS);
	}
}

void Session::release_lock() {
	if (lock_fd_ >= 0) {
		flock(lock_fd_, LOCK_UN);
		close(lock_fd_);
		lock_fd_ = -1;
	}
}

// --- JSON state 原子读写 ---

void Session::load_state() {
	std::string jsonpath = dir_ + "/state.json";
	std::string json_str = read_file_to_string(jsonpath);
	if (json_str.empty()) {
		fprintf(stderr, "%s: cannot read checkpoint state: %s\n", *av, jsonpath.c_str());
		exit(EXIT_ARGS);
	}
	if (!parse_state(json_str, state_)) {
		fprintf(stderr, "%s: checkpoint state is corrupt: %s\n", *av, jsonpath.c_str());
		exit(EXIT_ARGS);
	}
}

void Session::save_state_atomic() {
	state_.updated_at = now_unix();
	std::string json = serialize_state(state_);

	std::string tmppath = dir_ + "/state.json.tmp";
	FILE *fp = fopen(tmppath.c_str(), "wb");
	if (fp == NULL) {
		perror(tmppath.c_str());
		exit(EXIT_WRITE);
	}
	if (fwrite(json.data(), 1, json.size(), fp) != json.size()) {
		perror(tmppath.c_str());
		exit(EXIT_WRITE);
	}
	if (fflush(fp) != 0) {
		perror(tmppath.c_str());
		exit(EXIT_WRITE);
	}
	if (fsync(fileno(fp)) < 0) {
		perror("fsync state.json.tmp");
	}
	if (fclose(fp) != 0) {
		perror(tmppath.c_str());
		exit(EXIT_CLOSE);
	}

	// 原子 rename
	std::string jsonpath = dir_ + "/state.json";
	if (rename(tmppath.c_str(), jsonpath.c_str()) != 0) {
		perror("rename state.json");
		exit(EXIT_WRITE);
	}

	// fsync 父目录，保证 rename 持久化
	fsync_dir(dir_);
}

void Session::save_state() {
	save_state_atomic();
}

// --- 工作区清理 ---

void Session::clear_workspace() {
	rmrf_path(dir_ + "/blobs");
	rmrf_path(dir_ + "/staging");
	rmrf_path(dir_ + "/commits");
	mkdir_p(dir_ + "/blobs");
	mkdir_p(dir_ + "/staging");
	mkdir_p(dir_ + "/commits");
	// 删除旧的 state.json（若存在）
	unlink((dir_ + "/state.json").c_str());
	unlink((dir_ + "/state.json.tmp").c_str());
}

// --- 指纹内部一致性校验 ---

void Session::verify_fingerprint_internal() {
	if (state_.output_mode == "pmtiles") {
		fprintf(stderr, "%s: --resume is not supported for PMTiles output; use -o out.mbtiles or -e directory first\n", *av);
		exit(EXIT_ARGS);
	}

	// 从 state_ 重建 FingerprintParams，重新计算指纹，与存储的指纹比较
	FingerprintParams fp;
	fp.command_line = state_.normalized_cmd;  // 已规范化
	fp.output_mode = state_.output_mode;
	fp.output_path = state_.output_path;
	fp.temp_files = state_.temp_files;
	fp.cpus = state_.cpus;
	fp.inputs = state_.input_files;

	std::string expected = compute_fingerprint(fp);
	if (state_.fingerprint != expected) {
		fprintf(stderr, "%s: checkpoint fingerprint mismatch (state.json is corrupt or has been tampered with)\n", *av);
		fprintf(stderr, "  stored:   %s\n", state_.fingerprint.c_str());
		fprintf(stderr, "  expected: %s\n", expected.c_str());
		exit(EXIT_ARGS);
	}
}

// --- 进度报告 ---

void Session::report_progress(int zoom) const {
	int completed = zoom - state_.minzoom + 1;
	int total = state_.maxzoom - state_.minzoom + 1;
	if (total <= 0) total = 1;
	int percent = 100 * completed / total;

	int64_t elapsed = now_unix() - start_time_;
	if (elapsed < 0) elapsed = 0;

	if (elapsed > 0 && completed > 1) {
		int64_t est_total = elapsed * total / completed;
		int64_t est_remaining = est_total - elapsed;
		if (est_remaining < 0) est_remaining = 0;
		fprintf(stderr, "[checkpoint] zoom %d/%d committed (%d%%), elapsed %llds, est. remaining %llds\n",
			zoom, state_.maxzoom, percent, (long long) elapsed, (long long) est_remaining);
	} else {
		fprintf(stderr, "[checkpoint] zoom %d/%d committed (%d%%), elapsed %llds\n",
			zoom, state_.maxzoom, percent, (long long) elapsed);
	}
}

// --- open_new / open_resume ---

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

	// 检查是否已有作业
	std::string jsonpath = std::string(dir) + "/state.json";
	bool has_job = stat(jsonpath.c_str(), &st) == 0;
	if (has_job && !force) {
		fprintf(stderr, "%s: checkpoint directory %s already has a job; use --checkpoint-force to replace\n", *av, dir);
		exit(EXIT_EXISTS);
	}

	// 创建目录结构
	mkdir_p(std::string(dir) + "/blobs");
	mkdir_p(std::string(dir) + "/staging");
	mkdir_p(std::string(dir) + "/commits");

	// 获取并发锁
	s->acquire_lock();

	if (has_job && force) {
		s->clear_workspace();
	}

	// 初始化 state
	s->state_.format_version = TIPPECANOE_CHECKPOINT_FORMAT;
	s->state_.fingerprint = compute_fingerprint(params);
	s->state_.command_line = params.command_line;
	s->state_.normalized_cmd = normalize_command_line_for_fingerprint(params.command_line);
	s->state_.output_mode = params.output_mode;
	s->state_.output_path = params.output_path;
	s->state_.temp_files = params.temp_files;
	s->state_.cpus = params.cpus;
	s->state_.created_at = now_unix();
	s->state_.updated_at = s->state_.created_at;
	s->state_.input_files = params.inputs;
	s->state_.last_completed_zoom = -1;
	s->state_.entry_snapshot_done = false;
	s->state_.generation = 0;

	s->save_state_atomic();
	return s;
}

std::unique_ptr<Session> Session::open_resume(const char *dir) {
	struct stat st;
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "%s: checkpoint directory %s does not exist\n", *av, dir);
		exit(EXIT_ARGS);
	}

	auto s = std::unique_ptr<Session>(new Session(dir, true));

	// 清理可能残留的 state.json.tmp
	std::string tmppath = std::string(dir) + "/state.json.tmp";
	if (stat(tmppath.c_str(), &st) == 0) {
		unlink(tmppath.c_str());
	}

	// 获取并发锁
	s->acquire_lock();

	// 加载 state.json
	s->load_state();

	// 校验指纹（内部一致性）
	s->verify_fingerprint_internal();

	// 同步到成员变量
	s->entry_snapshot_done_ = s->state_.entry_snapshot_done;
	s->last_completed_zoom_ = s->state_.last_completed_zoom;
	s->resume_iz_ = s->state_.iz;
	s->tiling_minzoom_ = s->state_.minzoom;
	s->tiling_maxzoom_ = s->state_.maxzoom;
	s->tiling_basezoom_ = s->state_.basezoom;
	s->tiling_midx_ = s->state_.midx;
	s->tiling_midy_ = s->state_.midy;
	s->generation_ = s->state_.generation;

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

// --- snapshot_tiling_entry ---

void Session::snapshot_tiling_entry(int poolfd, size_t pool_size, long long const *pool_off, unsigned const *initial_x, unsigned const *initial_y,
				   int geomfd, off_t geom_size, node *shared_nodes_map, size_t nodepos, std::string const &shared_nodes_bloom,
				   std::vector<std::map<std::string, layermap_entry>> const &layermaps, int iz, int minzoom, int maxzoom, int basezoom,
				   long long const *file_bbox, long long const *file_bbox1, long long const *file_bbox2) {
	if (!active_) {
		return;
	}

	std::string staging = dir_ + "/staging";
	mkdir_p(staging);

	if (pool_size > 0) {
		copy_fd_to_file(poolfd, pool_size, path_staging("stringpool"));
	}
	if (geom_size > 0 && geomfd >= 0) {
		copy_fd_to_file(geomfd, (size_t) geom_size, path_staging("geom.initial"));
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
	if (file_bbox != nullptr) {
		write_blob_file(path_staging("file_bbox.bin"), file_bbox, sizeof(long long) * 4);
	}
	if (file_bbox1 != nullptr) {
		write_blob_file(path_staging("file_bbox1.bin"), file_bbox1, sizeof(long long) * 4);
	}
	if (file_bbox2 != nullptr) {
		write_blob_file(path_staging("file_bbox2.bin"), file_bbox2, sizeof(long long) * 4);
	}

	// 原子 promote: staging → blobs
	rename(path_staging("stringpool").c_str(), path_blob("stringpool").c_str());
	if (geom_size > 0) {
		rename(path_staging("geom.initial").c_str(), path_blob("geom.initial").c_str());
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
	if (file_bbox != nullptr) {
		rename(path_staging("file_bbox.bin").c_str(), path_blob("file_bbox.bin").c_str());
	}
	if (file_bbox1 != nullptr) {
		rename(path_staging("file_bbox1.bin").c_str(), path_blob("file_bbox1.bin").c_str());
	}
	if (file_bbox2 != nullptr) {
		rename(path_staging("file_bbox2.bin").c_str(), path_blob("file_bbox2.bin").c_str());
	}

	// fsync blobs 目录
	fsync_dir(dir_ + "/blobs");

	// 更新 state
	state_.generation++;
	state_.iz = iz;
	state_.minzoom = minzoom;
	state_.maxzoom = maxzoom;
	state_.basezoom = basezoom;
	state_.nodepos = (int64_t) nodepos;
	state_.entry_snapshot_done = true;
	state_.last_completed_zoom = -1;  // 尚未完成任何 zoom

	save_state_atomic();

	entry_snapshot_done_ = true;
	resume_iz_ = iz;
	tiling_minzoom_ = minzoom;
	tiling_maxzoom_ = maxzoom;
	tiling_basezoom_ = basezoom;
}

// --- restore_tiling ---

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
	int geom_loaded = 0;
	off_t geom_total = 0;

	if (last_completed_zoom_ < 0) {
		// 无 zoom 完成 — 加载初始快照（未压缩）
		std::string geom_init = path_blob("geom.initial");
		struct stat gst;
		if (stat(geom_init.c_str(), &gst) == 0) {
			out.geomfd[0] = open(geom_init.c_str(), O_RDONLY | O_CLOEXEC);
			out.geom_size[0] = gst.st_size;
			geom_loaded++;
			geom_total += gst.st_size;
		}
	} else {
		// 有 zoom 完成 — 加载最新代数的 geom 文件
		uint64_t latest_gen = state_.generation;

		for (size_t j = 0; j < TEMP_FILES; j++) {
			char name[64];
			snprintf(name, sizeof(name), "geom.%zu.g%llu", j, (unsigned long long) latest_gen);
			std::string gp = path_blob(name);
			struct stat gs;
			if (stat(gp.c_str(), &gs) == 0) {
				out.geomfd[j] = open(gp.c_str(), O_RDONLY | O_CLOEXEC);
				out.geom_size[j] = gs.st_size;
				geom_loaded++;
				geom_total += gs.st_size;
			}
		}

		// 清理旧代数和初始快照
		DIR *d = opendir((dir_ + "/blobs").c_str());
		if (d != NULL) {
			struct dirent *dp;
			while ((dp = readdir(d)) != NULL) {
				if (strncmp(dp->d_name, "geom.", 5) == 0) {
					const char *dot = strrchr(dp->d_name, '.');
					if (dot != NULL && strncmp(dot, ".g", 2) == 0) {
						uint64_t file_gen = (uint64_t) strtoull(dot + 2, NULL, 10);
						if (file_gen != latest_gen) {
							std::string old = dir_ + "/blobs/" + dp->d_name;
							unlink(old.c_str());
						}
					} else if (strcmp(dp->d_name, "geom.initial") == 0) {
						std::string old = dir_ + "/blobs/" + dp->d_name;
						unlink(old.c_str());
					}
				}
			}
			closedir(d);
		}
	}
	fprintf(stderr, "  [checkpoint restore_tiling] loaded %d geom files, total=%lld bytes, iz=%d maxzoom=%d\n",
		geom_loaded, (long long) geom_total, resume_iz_, tiling_maxzoom_);

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

	// 恢复 file_bbox / file_bbox1 / file_bbox2
	for (int which = 0; which < 3; which++) {
		const char *name = which == 0 ? "file_bbox.bin" : which == 1 ? "file_bbox1.bin" : "file_bbox2.bin";
		long long *dst = which == 0 ? out.file_bbox : which == 1 ? out.file_bbox1 : out.file_bbox2;
		std::string p = path_blob(name);
		FILE *bfp = fopen(p.c_str(), "rb");
		if (bfp != NULL) {
			if (fread(dst, sizeof(long long), 4, bfp) != 4) {
				// 短读 — 保留默认值
			}
			fclose(bfp);
		}
	}

	out.iz = resume_iz_;
	out.minzoom = tiling_minzoom_;
	out.maxzoom = tiling_maxzoom_;
	out.basezoom = tiling_basezoom_;
	out.midx = tiling_midx_;
	out.midy = tiling_midy_;

	return true;
}

// --- on_zoom_complete ---

void Session::on_zoom_complete(ZoomCompleteContext const &ctx) {
	if (!active_ || ctx.geomfd == nullptr || ctx.geom_size == nullptr) {
		return;
	}

	state_.generation++;
	uint64_t gen = state_.generation;

	std::string staging = dir_ + "/staging";
	mkdir_p(staging);

	off_t total_geom = 0;
	int saved_count = 0;
	for (size_t j = 0; j < TEMP_FILES; j++) {
		char name[64];
		snprintf(name, sizeof(name), "geom.%zu.g%llu", j, (unsigned long long) gen);
		if (ctx.geomfd[j] >= 0 && ctx.geom_size[j] > 0) {
			copy_fd_to_file(ctx.geomfd[j], (size_t) ctx.geom_size[j], path_staging(name));
			total_geom += ctx.geom_size[j];
			saved_count++;
		}
	}
	fprintf(stderr, "  [checkpoint on_zoom_complete zoom=%d] saved %d geom files, total=%lld bytes\n",
		ctx.zoom, saved_count, (long long) total_geom);
	if (ctx.skip_children != nullptr) {
		write_skip_children_blob(path_staging("skip_children.bin"), *ctx.skip_children);
	}
	if (ctx.strategies != nullptr) {
		write_strategies_blob(path_staging("strategies.bin"), *ctx.strategies);
	}

	// 原子替换: 先 rename staging → blobs
	for (size_t j = 0; j < TEMP_FILES; j++) {
		char name[64];
		snprintf(name, sizeof(name), "geom.%zu.g%llu", j, (unsigned long long) gen);
		std::string sp = path_staging(name);
		struct stat st;
		if (stat(sp.c_str(), &st) == 0) {
			rename(sp.c_str(), path_blob(name).c_str());
		}
	}

	// 清理旧代数 geom 文件（新代数已安全 rename 到 blobs/）
	{
		DIR *d = opendir((dir_ + "/blobs").c_str());
		if (d != NULL) {
			struct dirent *dp;
			while ((dp = readdir(d)) != NULL) {
				if (strncmp(dp->d_name, "geom.", 5) == 0) {
					const char *dot = strrchr(dp->d_name, '.');
					if (dot != NULL && strncmp(dot, ".g", 2) == 0) {
						uint64_t file_gen = (uint64_t) strtoull(dot + 2, NULL, 10);
						if (file_gen != gen) {
							std::string old = dir_ + "/blobs/" + dp->d_name;
							unlink(old.c_str());
						}
					}
				}
			}
			closedir(d);
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

	// fsync blobs 目录
	fsync_dir(dir_ + "/blobs");

	// 原子更新 state.json（包含 last_completed_zoom 和 generation）
	int64_t ts = now_unix();
	state_.last_completed_zoom = ctx.zoom;
	state_.maxzoom = ctx.maxzoom;
	state_.midx = ctx.midx;
	state_.midy = ctx.midy;

	ZoomCommit commit;
	commit.committed_at = ts;
	commit.geom_total_bytes = (int64_t) total_geom;
	commit.generation = gen;
	state_.zoom_commits[ctx.zoom] = commit;

	save_state_atomic();

	// 创建 zoom 完成标记文件（防御性校验）
	{
		char marker[64];
		snprintf(marker, sizeof(marker), "%s/zoom-%d.done", path_commits().c_str(), ctx.zoom);
		FILE *mf = fopen(marker, "w");
		if (mf != NULL) {
			fprintf(mf, "%lld\n", (long long) ts);
			fclose(mf);
		}
	}

	// 更新成员变量
	last_completed_zoom_ = ctx.zoom;
	tiling_maxzoom_ = ctx.maxzoom;
	tiling_midx_ = ctx.midx;
	tiling_midy_ = ctx.midy;
	resume_iz_ = ctx.zoom + 1;
	generation_ = gen;

	// 进度报告
	report_progress(ctx.zoom);
}

// --- finalize_success ---

void Session::finalize_success() {
	if (!active_) {
		return;
	}
	state_.updated_at = now_unix();
	save_state_atomic();
	// 释放锁
	release_lock();
}

// ===========================================================================
// 输出清理辅助函数（操作 mbtiles 输出文件，不是 checkpoint 状态）
// ===========================================================================

void cleanup_resume_wal(const char *output_path) {
	// 清理 WAL 和 SHM 文件（kill -9 残留）
	// 注意：不先删除 -journal 文件，SQLite 需要回滚热日志中的未完成事务
	std::string wal = std::string(output_path) + "-wal";
	std::string shm = std::string(output_path) + "-shm";
	std::string journal = std::string(output_path) + "-journal";

	unlink(wal.c_str());
	unlink(shm.c_str());

	// 若 DB header 仍指示 WAL 模式（bytes 18-19 = 0x02 0x00），
	// 重置为 DELETE 模式（0x01 0x00），否则 SQLite 无法打开
	int fd = open(output_path, O_RDWR);
	if (fd >= 0) {
		unsigned char buf[2];
		if (pread(fd, buf, 2, 18) == 2) {
			if (buf[0] == 0x02 && buf[1] == 0x00) {
				buf[0] = 0x01;
				pwrite(fd, buf, 2, 18);
			}
		}
		close(fd);
	}

	// 打开数据库回滚热日志
	sqlite3 *db = NULL;
	if (sqlite3_open(output_path, &db) == SQLITE_OK) {
		sqlite3_busy_timeout(db, 30000);
		char *err = NULL;
		int rc = sqlite3_exec(db, "PRAGMA journal_mode=DELETE;", NULL, NULL, &err);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "checkpoint: warning: journal recovery for %s failed: %s\n", output_path, err ? err : "(unknown)");
		}
		sqlite3_free(err);
		sqlite3_close(db);
	}

	// 回滚完成后清理残留 journal
	unlink(journal.c_str());
}

void cleanup_resume_dirs(const char *output_path, int last_completed_zoom, int maxzoom) {
	for (int z = last_completed_zoom + 1; z <= maxzoom; z++) {
		std::string zdir = std::string(output_path) + "/" + std::to_string(z);
		struct stat st;
		if (stat(zdir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
			rmrf_path(zdir);
		}
	}
}

void cleanup_resume_tiles(sqlite3 *outdb, int last_completed_zoom) {
	if (outdb == NULL || last_completed_zoom < 0) {
		return;
	}

	char *err = NULL;
	if (sqlite3_exec(outdb, "PRAGMA journal_mode=DELETE;", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: warning: could not set journal mode: %s\n", *av, err ? err : "unknown error");
		sqlite3_free(err);
	}

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(outdb, "DELETE FROM map WHERE zoom_level > ?;", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int(stmt, 1, last_completed_zoom);
		int rc = sqlite3_step(stmt);
		int changes = sqlite3_changes(outdb);
		sqlite3_finalize(stmt);
		(void) rc;
		fprintf(stderr, "%s: cleaned up %d uncommitted tiles (zoom > %d)\n", *av, changes, last_completed_zoom);
	}

	if (sqlite3_prepare_v2(outdb, "DELETE FROM images WHERE tile_id NOT IN (SELECT tile_id FROM map);",
			       -1, &stmt, NULL) == SQLITE_OK) {
		int rc = sqlite3_step(stmt);
		(void) rc;
		sqlite3_finalize(stmt);
	}
}

// ===========================================================================
// read_resume_info（从 state.json 读取，不打开 Session）
// ===========================================================================

ResumeInfo read_resume_info(const char *dir) {
	struct stat st;
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "%s: checkpoint directory %s does not exist\n", *av, dir);
		exit(EXIT_ARGS);
	}

	// 清理残留的 state.json.tmp
	std::string tmppath = std::string(dir) + "/state.json.tmp";
	if (stat(tmppath.c_str(), &st) == 0) {
		unlink(tmppath.c_str());
	}

	std::string jsonpath = std::string(dir) + "/state.json";
	std::string json_str = read_file_to_string(jsonpath);
	if (json_str.empty()) {
		fprintf(stderr, "%s: cannot read checkpoint state: %s\n", *av, jsonpath.c_str());
		exit(EXIT_ARGS);
	}

	CheckpointState state;
	if (!parse_state(json_str, state)) {
		fprintf(stderr, "%s: checkpoint state is corrupt: %s\n", *av, jsonpath.c_str());
		exit(EXIT_ARGS);
	}

	ResumeInfo info;
	info.output_mode = state.output_mode;
	info.output_path = state.output_path;
	info.original_cmd = state.command_line;
	info.normalized_cmd = state.normalized_cmd;
	info.maxzoom = state.maxzoom;
	info.minzoom = state.minzoom;
	info.basezoom = state.basezoom;
	info.last_completed_zoom = state.last_completed_zoom;
	info.entry_snapshot_done = state.entry_snapshot_done;

	for (auto const &f : state.input_files) {
		info.input_files.push_back(f.path);
	}

	return info;
}

// ===========================================================================
// 管理命令
// ===========================================================================

int checkpoint_status(const char *dir) {
	struct stat st;
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "%s: checkpoint directory %s does not exist\n", *av, dir);
		return EXIT_ARGS;
	}

	std::string jsonpath = std::string(dir) + "/state.json";
	std::string json_str = read_file_to_string(jsonpath);
	if (json_str.empty()) {
		fprintf(stderr, "%s: no checkpoint state found: %s\n", *av, dir);
		return EXIT_ARGS;
	}

	CheckpointState state;
	if (!parse_state(json_str, state)) {
		fprintf(stderr, "%s: checkpoint state is corrupt\n", *av);
		return EXIT_ARGS;
	}

	fprintf(stderr, "=== Checkpoint: %s ===\n", dir);
	fprintf(stderr, "  Command: %s\n", state.command_line.c_str());
	fprintf(stderr, "  Output: %s (%s)\n", state.output_path.c_str(), state.output_mode.c_str());

	if (!state.input_files.empty()) {
		fprintf(stderr, "  Input: ");
		for (size_t i = 0; i < state.input_files.size(); i++) {
			if (i > 0) fprintf(stderr, ", ");
			fprintf(stderr, "%s", state.input_files[i].path.c_str());
		}
		fprintf(stderr, "\n");
	}

	fprintf(stderr, "  Zoom range: %d-%d", state.minzoom, state.maxzoom);
	if (state.last_completed_zoom >= 0) {
		int total = state.maxzoom - state.minzoom + 1;
		int completed = state.last_completed_zoom - state.minzoom + 1;
		if (total <= 0) total = 1;
		int pct = 100 * completed / total;
		fprintf(stderr, ", completed: %d/%d (%d%%)", completed, total, pct);
	} else {
		fprintf(stderr, ", no zooms completed yet");
	}
	fprintf(stderr, "\n");

	fprintf(stderr, "  Entry snapshot: %s\n", state.entry_snapshot_done ? "done" : "pending");
	fprintf(stderr, "  Generation: %llu\n", (unsigned long long) state.generation);
	fprintf(stderr, "  Zoom commits: %zu\n", state.zoom_commits.size());

	// 计算磁盘占用
	long long blob_size = 0;
	std::string blobs_dir = std::string(dir) + "/blobs";
	DIR *d = opendir(blobs_dir.c_str());
	if (d != NULL) {
		struct dirent *dp;
		while ((dp = readdir(d)) != NULL) {
			if (dp->d_name[0] == '.') continue;
			std::string fp = blobs_dir + "/" + dp->d_name;
			struct stat bs;
			if (stat(fp.c_str(), &bs) == 0) {
				blob_size += bs.st_size;
			}
		}
		closedir(d);
	}
	fprintf(stderr, "  Blob size: %lld bytes (%.1f MB)\n", blob_size, blob_size / (1024.0 * 1024.0));

	time_t created = (time_t) state.created_at;
	time_t updated = (time_t) state.updated_at;
	fprintf(stderr, "  Created: %s", ctime(&created));
	fprintf(stderr, "  Updated: %s", ctime(&updated));

	return 0;
}

int checkpoint_prune(const char *dir) {
	struct stat st;
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "%s: checkpoint directory %s does not exist\n", *av, dir);
		return EXIT_ARGS;
	}

	std::string jsonpath = std::string(dir) + "/state.json";
	if (stat(jsonpath.c_str(), &st) != 0) {
		fprintf(stderr, "%s: no checkpoint state found: %s\n", *av, dir);
		return EXIT_ARGS;
	}

	// 删除 blobs/、staging/、commits/，保留 state.json 作为日志
	long long freed = 0;

	// 计算 blobs 大小
	std::string blobs_dir = std::string(dir) + "/blobs";
	DIR *d = opendir(blobs_dir.c_str());
	if (d != NULL) {
		struct dirent *dp;
		while ((dp = readdir(d)) != NULL) {
			if (dp->d_name[0] == '.') continue;
			std::string fp = blobs_dir + "/" + dp->d_name;
			struct stat bs;
			if (stat(fp.c_str(), &bs) == 0) {
				freed += bs.st_size;
			}
		}
		closedir(d);
	}

	rmrf_path(blobs_dir);
	rmrf_path(std::string(dir) + "/staging");
	rmrf_path(std::string(dir) + "/commits");
	unlink((std::string(dir) + "/.lock").c_str());

	fprintf(stderr, "%s: pruned checkpoint %s, freed %lld bytes (%.1f MB)\n",
		*av, dir, freed, freed / (1024.0 * 1024.0));
	fprintf(stderr, "  state.json preserved as log\n");

	return 0;
}

}  // namespace checkpoint
