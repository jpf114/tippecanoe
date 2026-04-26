#define CATCH_CONFIG_MAIN
#include "catch/catch.hpp"
#include "text.hpp"
#include "sort.hpp"
#include "tile-cache.hpp"
#include "mvt.hpp"
#include "mbtiles.hpp"
#include "projection.hpp"
#include "geometry.hpp"
#include "postgis.hpp"
#include "mongo.hpp"
#include "db_options.hpp"
#include <unistd.h>
#include <limits.h>

// Minimal globals and stubs so unit tests can link without pulling in the full CLI entrypoints.
char **av = nullptr;
size_t CPUS = 1;
size_t TEMP_FILES = 0;
long long MAX_FILES = 0;
size_t memsize = 0;
long long diskfree = 0;
int quiet = 1;
int quiet_progress = 1;
std::atomic<double> last_progress(0);
double progress_interval = 0;
int geometry_scale = 0;
int cluster_distance = 0;
std::string attribute_for_id = "";
std::map<std::string, serial_val> set_attributes;
unsigned long long preserve_point_density_threshold = 0;
unsigned long long preserve_multiplier_density_threshold = 0;
int retain_points_multiplier = 1;
size_t maximum_string_attribute_length = 0;
bool order_by_size = false;
int prevent[256] = {0};
int additional[256] = {0};
std::vector<clipbbox> clipbboxes;

void checkdisk(std::vector<struct reader> *) {
}

bool progress_time() {
	return false;
}

long long addpool(struct memfile *, struct memfile *, const char *, char, std::vector<ssize_t> &) {
	return 0;
}

void add_to_tilestats(std::map<std::string, tilestat> &, std::string const &, serial_val const &) {
}

TEST_CASE("UTF-8 enforcement", "[utf8]") {
	REQUIRE(check_utf8("") == std::string(""));
	REQUIRE(check_utf8("hello world") == std::string(""));
	REQUIRE(check_utf8("Καλημέρα κόσμε") == std::string(""));
	REQUIRE(check_utf8("こんにちは 世界") == std::string(""));
	REQUIRE(check_utf8("👋🌏") == std::string(""));
	REQUIRE(check_utf8("Hola m\xF3n") == std::string("\"Hola m\xF3n\" is not valid UTF-8 (0xF3 0x6E)"));
}

TEST_CASE("UTF-8 truncation", "[trunc]") {
	REQUIRE(truncate16("0123456789abcdefghi", 16) == std::string("0123456789abcdef"));
	REQUIRE(truncate16("0123456789éîôüéîôüç", 16) == std::string("0123456789éîôüéî"));
	REQUIRE(truncate16("0123456789😀😬😁😂😃😄😅😆", 16) == std::string("0123456789😀😬😁"));
	REQUIRE(truncate16("0123456789😀😬😁😂😃😄😅😆", 17) == std::string("0123456789😀😬😁"));
	REQUIRE(truncate16("0123456789あいうえおかきくけこさ", 16) == std::string("0123456789あいうえおか"));

	REQUIRE(truncate_string("789éîôüéîôüç", 3) == std::string("789"));
	REQUIRE(truncate_string("789éîôüéîôüç", 4) == std::string("789"));
	REQUIRE(truncate_string("789éîôüéîôüç", 5) == std::string("789é"));
	REQUIRE(truncate_string("789éîôüéîôüç", 6) == std::string("789é"));
	REQUIRE(truncate_string("789éîôüéîôüç", 7) == std::string("789éî"));
	REQUIRE(truncate_string("789éîôüéîôüç", 8) == std::string("789éî"));

	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 10) == std::string("0123456789"));
	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 11) == std::string("0123456789"));
	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 12) == std::string("0123456789"));
	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 13) == std::string("0123456789"));
	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 14) == std::string("0123456789😀"));

	REQUIRE(truncate_string("😀", 4) == std::string("😀"));
	REQUIRE(truncate_string("😀", 3) == std::string(""));
	REQUIRE(truncate_string("😀", 2) == std::string(""));
	REQUIRE(truncate_string("😀", 1) == std::string(""));
	REQUIRE(truncate_string("😀", 0) == std::string(""));
}

int intcmp(const void *v1, const void *v2) {
	return *((int *) v1) - *((int *) v2);
}

TEST_CASE("External quicksort", "fqsort") {
	std::vector<FILE *> inputs;

	size_t written = 0;
	for (size_t i = 0; i < 5; i++) {
		std::string tmpname = "/tmp/in.XXXXXXX";
		int fd = mkstemp((char *) tmpname.c_str());
		unlink(tmpname.c_str());
		FILE *f = fdopen(fd, "w+b");
		inputs.emplace_back(f);
		size_t iterations = 2000 + rand() % 200;
		for (size_t j = 0; j < iterations; j++) {
			int n = rand();
			fwrite((void *) &n, sizeof(int), 1, f);
			written++;
		}
		rewind(f);
	}

	std::string tmpname = "/tmp/out.XXXXXX";
	int fd = mkstemp((char *) tmpname.c_str());
	unlink(tmpname.c_str());
	FILE *f = fdopen(fd, "w+b");

	fqsort(inputs, sizeof(int), intcmp, f, 256, "/tmp");
	rewind(f);

	int prev = INT_MIN;
	int here;
	size_t nread = 0;
	while (fread((void *) &here, sizeof(int), 1, f)) {
		REQUIRE(here >= prev);
		prev = here;
		nread++;
	}

	fclose(f);
	REQUIRE(nread == written);
}

mvt_tile mock_get_tile(zxy tile) {
	mvt_layer l;
	l.name = std::to_string(tile.z) + "/" + std::to_string(tile.x) + "/" + std::to_string(tile.y);
	mvt_tile t;
	t.layers.push_back(l);
	return t;
}

TEST_CASE("Tile-join cache", "tile cache") {
	tile_cache tc;
	tc.capacity = 5;

	REQUIRE(tc.get(zxy(11, 327, 791), mock_get_tile).layers[0].name == "11/327/791");
	REQUIRE(tc.get(zxy(11, 5, 7), mock_get_tile).layers[0].name == "11/5/7");
	REQUIRE(tc.get(zxy(11, 5, 8), mock_get_tile).layers[0].name == "11/5/8");
	REQUIRE(tc.get(zxy(11, 5, 9), mock_get_tile).layers[0].name == "11/5/9");
	REQUIRE(tc.get(zxy(11, 5, 10), mock_get_tile).layers[0].name == "11/5/10");
	REQUIRE(tc.get(zxy(11, 327, 791), mock_get_tile).layers[0].name == "11/327/791");
	REQUIRE(tc.overzoom_cache.size() == 5);
	REQUIRE(tc.overzoom_cache.find(zxy(11, 327, 791)) != tc.overzoom_cache.end());
	REQUIRE(tc.overzoom_cache.find(zxy(11, 5, 7)) != tc.overzoom_cache.end());

	// verify that additional gets evict the least-recently-used elements

	REQUIRE(tc.get(zxy(11, 5, 11), mock_get_tile).layers[0].name == "11/5/11");
	REQUIRE(tc.overzoom_cache.size() == 5);
	REQUIRE(tc.overzoom_cache.find(zxy(11, 5, 7)) == tc.overzoom_cache.end());

	REQUIRE(tc.get(zxy(11, 5, 12), mock_get_tile).layers[0].name == "11/5/12");
	REQUIRE(tc.overzoom_cache.size() == 5);
	REQUIRE(tc.overzoom_cache.find(zxy(11, 5, 8)) == tc.overzoom_cache.end());
}

TEST_CASE("Bit reversal", "bit reversal") {
	REQUIRE(bit_reverse(1) == 0x8000000000000000);
	REQUIRE(bit_reverse(0x1234567812489BCF) == 0xF3D912481E6A2C48);
	REQUIRE(bit_reverse(0xF3D912481E6A2C48) == 0x1234567812489BCF);
}

TEST_CASE("line_is_too_small") {
	drawvec dv;
	dv.emplace_back(VT_MOVETO, 4243099709, 2683872952);
	dv.emplace_back(VT_LINETO, 4243102487, 2683873977);
	dv.emplace_back(VT_MOVETO, -51867587, 2683872952);
	dv.emplace_back(VT_LINETO, -51864809, 2683873977);
	REQUIRE(line_is_too_small(dv, 0, 10));
}

TEST_CASE("PostGIS shard condition wraps outer query", "[postgis]") {
	std::string base = "SELECT ST_AsBinary(geom) AS wkb, name FROM (SELECT geom, name FROM roads WHERE type = 'highway' ORDER BY id) AS _subq";
	std::string shard = "(abs(hashtext(ctid::text)) % 4) = 1";
	std::string wrapped = PostGISReader::build_sharded_query(base, shard);

	REQUIRE(wrapped == "SELECT * FROM (" + base + ") AS _shard_src WHERE " + shard);
	REQUIRE(wrapped.find("WHERE type = 'highway' ORDER BY id) AS _subq") != std::string::npos);
	REQUIRE(wrapped.find("_shard_src WHERE " + shard) != std::string::npos);
}

TEST_CASE("PostGIS short connection string keeps advanced fields optional", "[postgis]") {
	postgis_config cfg;
	REQUIRE(cfg.parse_connection_string("gis"));
	REQUIRE(cfg.dbname == "gis");
	REQUIRE(cfg.host == "localhost");
	REQUIRE(cfg.port == "5432");
	REQUIRE(cfg.user.empty());
	REQUIRE(cfg.password.empty());

	postgis_config auth_cfg;
	REQUIRE(auth_cfg.parse_connection_string("gis:alice:secret"));
	REQUIRE(auth_cfg.dbname == "gis");
	REQUIRE(auth_cfg.user == "alice");
	REQUIRE(auth_cfg.password == "secret");
	REQUIRE(auth_cfg.host == "localhost");
	REQUIRE(auth_cfg.port == "5432");

	postgis_config legacy_cfg;
	REQUIRE(legacy_cfg.parse_connection_string("db.example.com:5433:gis:alice:secret"));
	REQUIRE(legacy_cfg.host == "db.example.com");
	REQUIRE(legacy_cfg.port == "5433");
	REQUIRE(legacy_cfg.dbname == "gis");
	REQUIRE(legacy_cfg.user == "alice");
	REQUIRE(legacy_cfg.password == "secret");
}

TEST_CASE("PostGIS normalize centralizes runtime defaults", "[postgis]") {
	postgis_config cfg;
	cfg.shard_mode = "";
	cfg.batch_size = 1;
	cfg.max_retries = 0;

	cfg.normalize();
	REQUIRE(cfg.shard_mode == "auto");
	REQUIRE(cfg.batch_size == MIN_POSTGIS_BATCH_SIZE);
	REQUIRE(cfg.max_retries == 1);

	postgis_config high_cfg;
	high_cfg.batch_size = MAX_POSTGIS_BATCH_SIZE + 100;
	high_cfg.normalize();
	REQUIRE(high_cfg.batch_size == MAX_POSTGIS_BATCH_SIZE);
}

TEST_CASE("PostGIS config exposes a single input-source semantic", "[postgis]") {
	postgis_config cfg;
	REQUIRE_FALSE(cfg.has_input_source());
	REQUIRE_FALSE(cfg.has_sql_input());
	REQUIRE_FALSE(cfg.has_table_input());
	REQUIRE(cfg.effective_shard_mode() == "auto");
	REQUIRE(cfg.has_supported_shard_mode());
	REQUIRE(cfg.is_auto_shard_mode());
	REQUIRE_FALSE(cfg.is_none_shard_mode());
	REQUIRE_FALSE(cfg.is_key_shard_mode());
	REQUIRE_FALSE(cfg.is_range_shard_mode());
	REQUIRE(cfg.can_attempt_range_sharding());
	REQUIRE(cfg.can_attempt_key_sharding());
	REQUIRE(cfg.can_attempt_ctid_sharding());
	REQUIRE_FALSE(cfg.should_report_geometry_field());
	REQUIRE_FALSE(cfg.should_report_selected_columns());
	REQUIRE_FALSE(cfg.should_report_shard_strategy());
	REQUIRE_FALSE(cfg.requires_selected_columns_for_best_effort());
	REQUIRE_FALSE(cfg.requires_shard_key());
	REQUIRE_FALSE(cfg.ignores_shard_key());
	REQUIRE_FALSE(cfg.has_read_tuning_overrides());
	REQUIRE_FALSE(cfg.has_attribute_strategy_overrides());
	REQUIRE_FALSE(cfg.has_debug_strategy_overrides());
	REQUIRE(cfg.uses_default_batch_size());
	REQUIRE(cfg.uses_cursor_by_default());
	REQUIRE(cfg.uses_default_memory_limit());
	REQUIRE(cfg.uses_default_retry_policy());
	REQUIRE(cfg.uses_default_progress_report());
	REQUIRE(cfg.uses_default_attribute_order());
	REQUIRE(cfg.uses_default_exact_count_strategy());
	REQUIRE(cfg.uses_default_profile_strategy());

	cfg.table = "roads";
	REQUIRE(cfg.has_input_source());
	REQUIRE(cfg.has_table_input());
	REQUIRE_FALSE(cfg.has_sql_input());
	REQUIRE(cfg.input_mode_label() == "table");
	REQUIRE(cfg.effective_layer_name() == "roads");

	cfg.sql = "select * from roads";
	REQUIRE(cfg.has_sql_input());
	REQUIRE(cfg.sql_takes_precedence());
	REQUIRE(cfg.input_mode_label() == "sql");
	REQUIRE(cfg.effective_layer_name() == "roads");

	cfg.geometry_field = "geom";
	cfg.selected_columns_csv = "name,type";
	cfg.selected_columns_best_effort = true;
	cfg.canonical_attr_order = true;
	cfg.shard_mode = "range";
	cfg.shard_key = "gid";
	cfg.batch_size = DEFAULT_POSTGIS_BATCH_SIZE + 1;
	cfg.use_cursor = false;
	cfg.max_memory_mb = MAX_POSTGIS_MEMORY_USAGE_MB + 1;
	cfg.max_retries = MAX_POSTGIS_RETRIES + 1;
	cfg.enable_progress_report = false;
	cfg.progress_with_exact_count = true;
	cfg.profile = true;
	REQUIRE(cfg.should_report_geometry_field());
	REQUIRE(cfg.should_report_selected_columns());
	REQUIRE(cfg.should_report_shard_strategy());
	REQUIRE(cfg.effective_shard_mode() == "range");
	REQUIRE(cfg.has_supported_shard_mode());
	REQUIRE_FALSE(cfg.is_auto_shard_mode());
	REQUIRE_FALSE(cfg.is_none_shard_mode());
	REQUIRE_FALSE(cfg.is_key_shard_mode());
	REQUIRE(cfg.is_range_shard_mode());
	REQUIRE(cfg.can_attempt_range_sharding());
	REQUIRE_FALSE(cfg.can_attempt_key_sharding());
	REQUIRE_FALSE(cfg.can_attempt_ctid_sharding());
	REQUIRE_FALSE(cfg.requires_selected_columns_for_best_effort());
	REQUIRE_FALSE(cfg.requires_shard_key());
	REQUIRE_FALSE(cfg.ignores_shard_key());
	REQUIRE(cfg.has_read_tuning_overrides());
	REQUIRE(cfg.has_attribute_strategy_overrides());
	REQUIRE(cfg.has_debug_strategy_overrides());
	REQUIRE_FALSE(cfg.uses_default_batch_size());
	REQUIRE_FALSE(cfg.uses_cursor_by_default());
	REQUIRE_FALSE(cfg.uses_default_memory_limit());
	REQUIRE_FALSE(cfg.uses_default_retry_policy());
	REQUIRE_FALSE(cfg.uses_default_progress_report());
	REQUIRE_FALSE(cfg.uses_default_attribute_order());
	REQUIRE_FALSE(cfg.uses_default_exact_count_strategy());
	REQUIRE_FALSE(cfg.uses_default_profile_strategy());

	postgis_config best_effort_missing_columns;
	best_effort_missing_columns.selected_columns_best_effort = true;
	REQUIRE(best_effort_missing_columns.requires_selected_columns_for_best_effort());

	postgis_config shard_key_required;
	shard_key_required.shard_mode = "key";
	REQUIRE(shard_key_required.requires_shard_key());

	postgis_config ignored_shard_key;
	ignored_shard_key.shard_mode = "none";
	ignored_shard_key.shard_key = "gid";
	REQUIRE(ignored_shard_key.has_supported_shard_mode());
	REQUIRE_FALSE(ignored_shard_key.can_attempt_range_sharding());
	REQUIRE_FALSE(ignored_shard_key.can_attempt_key_sharding());
	REQUIRE_FALSE(ignored_shard_key.can_attempt_ctid_sharding());
	REQUIRE(ignored_shard_key.ignores_shard_key());

	postgis_config invalid_shard_mode;
	invalid_shard_mode.shard_mode = "mystery";
	REQUIRE_FALSE(invalid_shard_mode.has_supported_shard_mode());
	REQUIRE(postgis_config::is_supported_shard_mode("auto"));
	REQUIRE(postgis_config::is_supported_shard_mode("none"));
	REQUIRE(postgis_config::is_supported_shard_mode("key"));
	REQUIRE(postgis_config::is_supported_shard_mode("range"));
	REQUIRE_FALSE(postgis_config::is_supported_shard_mode("mystery"));
}

TEST_CASE("tippecanoe-db core help options are intentionally narrow", "[db-options]") {
	REQUIRE(is_core_db_help_option("output"));
	REQUIRE(is_core_db_help_option("postgis"));
	REQUIRE(is_core_db_help_option("postgis-table"));
	REQUIRE(is_core_db_help_option("postgis-sql"));
	REQUIRE(is_core_db_help_option("postgis-geometry-field"));
	REQUIRE(is_core_db_help_option("mongo"));
	REQUIRE(is_core_db_help_option("maximum-zoom"));
	REQUIRE(is_core_db_help_option("minimum-zoom"));
	REQUIRE(is_core_db_help_option("force"));
	REQUIRE(is_core_db_help_option("help"));

	REQUIRE_FALSE(is_core_db_help_option("postgis-shard-mode"));
	REQUIRE_FALSE(is_core_db_help_option("postgis-progress-count"));
	REQUIRE_FALSE(is_core_db_help_option("mongo-batch-size"));
	REQUIRE_FALSE(is_core_db_help_option("mongo-timeout"));
	REQUIRE_FALSE(is_core_db_help_option("mongo-metadata"));
}

TEST_CASE("tippecanoe-db advanced and expert options stay off the default help path", "[db-options]") {
	REQUIRE(is_advanced_db_help_option("postgis-host"));
	REQUIRE(is_advanced_db_help_option("postgis-columns"));
	REQUIRE(is_advanced_db_help_option("mongo-collection"));
	REQUIRE(is_advanced_db_help_option("mongo-no-fail-on-discard"));
	REQUIRE(is_advanced_db_help_option("mongo-no-metadata"));

	REQUIRE(is_expert_db_help_option("postgis-shard-mode"));
	REQUIRE(is_expert_db_help_option("mongo-batch-size"));

	REQUIRE_FALSE(is_advanced_db_help_option("output"));
	REQUIRE_FALSE(is_expert_db_help_option("output"));
	REQUIRE_FALSE(is_advanced_db_help_option("maximum-zoom"));
	REQUIRE_FALSE(is_expert_db_help_option("mongo"));
	REQUIRE_FALSE(is_expert_db_help_option("postgis-canonical-attr-order"));
	REQUIRE_FALSE(is_expert_db_help_option("postgis-no-canonical-attr-order"));
	REQUIRE_FALSE(is_expert_db_help_option("postgis-profile"));
	REQUIRE_FALSE(is_expert_db_help_option("postgis-progress-count"));
	REQUIRE_FALSE(is_expert_db_help_option("mongo-metadata"));
	REQUIRE_FALSE(is_expert_db_help_option("mongo-fail-on-discard"));
}

TEST_CASE("tippecanoe-db runtime defaults prefer Mongo metadata and verification hints", "[db-options]") {
	db_runtime_config cfg{};
	REQUIRE_FALSE(cfg.write_metadata);

	db_runtime_defaults normalized = normalize_db_runtime_defaults(true, false, false, cfg);
	REQUIRE(cfg.write_metadata);
	REQUIRE(normalized.enable_mongo_metadata);
	REQUIRE(normalized.note_missing_mbtiles_verification);
	REQUIRE_FALSE(normalized.note_dual_output_mode);

	db_runtime_config dual_cfg{};
	db_runtime_defaults dual_mode = normalize_db_runtime_defaults(true, true, false, dual_cfg);
	REQUIRE(dual_cfg.write_metadata);
	REQUIRE_FALSE(dual_mode.note_missing_mbtiles_verification);
	REQUIRE(dual_mode.note_dual_output_mode);

	db_runtime_config local_only_cfg{};
	db_runtime_defaults local_only = normalize_db_runtime_defaults(false, true, false, local_only_cfg);
	REQUIRE_FALSE(local_only.enable_mongo_metadata);
	REQUIRE_FALSE(local_only.note_missing_mbtiles_verification);
	REQUIRE_FALSE(local_only.note_dual_output_mode);
	REQUIRE_FALSE(local_only_cfg.write_metadata);

	db_runtime_config explicit_disable_cfg{};
	explicit_disable_cfg.metadata_explicitly_set = true;
	explicit_disable_cfg.write_metadata = false;
	db_runtime_defaults explicit_disable = normalize_db_runtime_defaults(true, true, false, explicit_disable_cfg);
	REQUIRE_FALSE(explicit_disable.enable_mongo_metadata);
	REQUIRE_FALSE(explicit_disable_cfg.write_metadata);
	REQUIRE(explicit_disable.note_dual_output_mode);

	db_runtime_config explicit_enable_cfg{};
	explicit_enable_cfg.metadata_explicitly_set = true;
	explicit_enable_cfg.write_metadata = true;
	db_runtime_defaults explicit_enable = normalize_db_runtime_defaults(true, false, false, explicit_enable_cfg);
	REQUIRE_FALSE(explicit_enable.enable_mongo_metadata);
	REQUIRE(explicit_enable_cfg.write_metadata);
	REQUIRE(explicit_enable.note_missing_mbtiles_verification);
}

TEST_CASE("Mongo config can tell whether runtime settings still use defaults", "[db-options]") {
	mongo_config cfg;
	REQUIRE(cfg.uses_default_pool_size());
	REQUIRE(cfg.uses_default_timeout());
	REQUIRE(cfg.uses_default_indexes());
	REQUIRE(cfg.uses_default_fail_policy());

	cfg.connection_pool_size = DEFAULT_MONGO_CONNECTION_POOL_SIZE + 1;
	cfg.timeout_ms = MONGO_TIMEOUT_MS + 1000;
	cfg.create_indexes = false;
	cfg.fail_on_discard = false;
	REQUIRE_FALSE(cfg.uses_default_pool_size());
	REQUIRE_FALSE(cfg.uses_default_timeout());
	REQUIRE_FALSE(cfg.uses_default_indexes());
	REQUIRE_FALSE(cfg.uses_default_fail_policy());
}

TEST_CASE("tippecanoe-db output mode captures business and verification roles", "[db-options]") {
	db_output_mode business_only = determine_db_output_mode(true, false, false);
	REQUIRE(business_only.requires_mongo_output());
	REQUIRE(business_only.is_business_only());
	REQUIRE_FALSE(business_only.is_dual_output());
	REQUIRE_FALSE(business_only.is_local_only());

	db_output_mode dual_output = determine_db_output_mode(true, true, false);
	REQUIRE(dual_output.requires_mongo_output());
	REQUIRE(dual_output.is_dual_output());
	REQUIRE_FALSE(dual_output.is_business_only());

	db_output_mode directory_dual_output = determine_db_output_mode(true, false, true);
	REQUIRE(directory_dual_output.requires_mongo_output());
	REQUIRE(directory_dual_output.is_dual_output());
	REQUIRE(directory_dual_output.has_directory_tiles);
	REQUIRE_FALSE(directory_dual_output.is_business_only());

	db_output_mode local_verification = determine_db_output_mode(false, true, false);
	REQUIRE_FALSE(local_verification.requires_mongo_output());
	REQUIRE(local_verification.is_local_only());
	REQUIRE(local_verification.has_local_tiles_output());
	REQUIRE_FALSE(local_verification.is_business_only());

	db_output_mode directory_only = determine_db_output_mode(false, false, true);
	REQUIRE_FALSE(directory_only.requires_mongo_output());
	REQUIRE(directory_only.is_local_only());
	REQUIRE(directory_only.has_local_tiles_output());
	REQUIRE_FALSE(directory_only.has_mbtiles_verification);
}
