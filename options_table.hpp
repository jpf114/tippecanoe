#ifndef OPTIONS_TABLE_HPP
#define OPTIONS_TABLE_HPP

// v3.4 (重构): 命令行选项单一来源（消除 main.cpp/maindb.cpp 双表复制债）。
// 结构：按"逻辑组"分层的 X-macro——每个选项只在此定义一次：
//   - COMMON_* 组：tippecanoe 与 tippecanoe-db 共有。新增后两边表自动同步；
//     case 处理仍需各自实现，语义必须一致（由 unit 表一致性测试守护）。
//   - MAIN_ONLY_* 组：仅 tippecanoe（checkpoint、文件图层、read-parallel、
//     drop-by-attribute 等——db 的 tile-db 循环无对应逻辑；-s 在 db 侧显式
//     拒绝，PostGIS 输入恒被转为 EPSG:4326）。
//   - DB_ONLY_* 组：仅 tippecanoe-db（PostGIS/Mongo 连接、help-advanced）。
// 组标题(G)与组顺序由各入口的 MAIN_OPTIONS_TABLE / DB_OPTIONS_TABLE 自控
// （db 将 PostGIS/Mongo 业务组置前；两边共有部分的相对顺序一致）。
// X 宏四参数：X(名字, has_arg, flag表达式或0, val)——flag 绑定 prevent/additional
// 位标志的上游形态（val=1，getopt 返回 0 并自动置位）原样保留。

#include <getopt.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include "errors.hpp"
#include "options.hpp"

// ---------------- 通用选项池（main/db 共有） ----------------

// [Output tileset]
#define COMMON_OUTPUT_TILESET_OPTIONS(X) \
	X("output", required_argument, 0, 'o') \
	X("output-to-directory", required_argument, 0, 'e') \
	X("force", no_argument, 0, 'f') \
	X("allow-existing", no_argument, 0, 'F')

// [Tileset description and attribution]
#define COMMON_TILESET_DESCRIPTION_AND_ATTRIBUTION_OPTIONS(X) \
	X("name", required_argument, 0, 'n') \
	X("attribution", required_argument, 0, 'A') \
	X("description", required_argument, 0, 'N')

// [Projection of input]
#define COMMON_PROJECTION_OF_INPUT_OPTIONS(X) \
	X("projection", required_argument, 0, 's') \
	X("output-projection", required_argument, 0, '~')

// [Zoom levels]
#define COMMON_ZOOM_LEVELS_OPTIONS(X) \
	X("maximum-zoom", required_argument, 0, 'z') \
	X("minimum-zoom", required_argument, 0, 'Z') \
	X("smallest-maximum-zoom-guess", required_argument, 0, '~') \
	X("extend-zooms-if-still-dropping", no_argument, &additional[A_EXTEND_ZOOMS], 1) \
	X("extend-zooms-if-still-dropping-maximum", required_argument, 0, '~') \
	X("generate-variable-depth-tile-pyramid", no_argument, &additional[A_VARIABLE_DEPTH_PYRAMID], 1) \
	X("one-tile", required_argument, 0, 'R')

// [Tile resolution]
#define COMMON_TILE_RESOLUTION_OPTIONS(X) \
	X("full-detail", required_argument, 0, 'd') \
	X("low-detail", required_argument, 0, 'D') \
	X("minimum-detail", required_argument, 0, 'm') \
	X("extra-detail", required_argument, 0, '~')

// [Filtering feature attributes]
#define COMMON_FILTERING_FEATURE_ATTRIBUTES_OPTIONS(X) \
	X("exclude", required_argument, 0, 'x') \
	X("include", required_argument, 0, 'y') \
	X("exclude-all", no_argument, 0, 'X')

// [Modifying feature attributes]
#define COMMON_MODIFYING_FEATURE_ATTRIBUTES_OPTIONS(X) \
	X("attribute-type", required_argument, 0, 'T') \
	X("attribute-description", required_argument, 0, 'Y') \
	X("accumulate-attribute", required_argument, 0, 'E') \
	X("empty-csv-columns-are-null", no_argument, &prevent[P_EMPTY_CSV_COLUMNS], 1) \
	X("convert-stringified-ids-to-numbers", no_argument, &additional[A_CONVERT_NUMERIC_IDS], 1) \
	X("use-attribute-for-id", required_argument, 0, '~') \
	X("single-precision", no_argument, &prevent[P_SINGLE_PRECISION], 1) \
	X("set-attribute", required_argument, 0, '~') \
	X("maximum-string-attribute-length", required_argument, 0, '~')

// [Filtering features by attributes]
#define COMMON_FILTERING_FEATURES_BY_ATTRIBUTES_OPTIONS(X) \
	X("feature-filter-file", required_argument, 0, 'J') \
	X("feature-filter", required_argument, 0, 'j') \
	X("unidecode-data", required_argument, 0, '~')

// [Dropping a fixed fraction of features by zoom level]
#define COMMON_DROPPING_A_FIXED_FRACTION_OF_FEATURES_BY_ZOOM_LEVEL_OPTIONS(X) \
	X("drop-rate", required_argument, 0, 'r') \
	X("retain-points-multiplier", required_argument, 0, '~') \
	X("base-zoom", required_argument, 0, 'B') \
	X("drop-denser", required_argument, 0, '~') \
	X("limit-base-zoom-to-maximum-zoom", no_argument, &prevent[P_BASEZOOM_ABOVE_MAXZOOM], 1) \
	X("drop-lines", no_argument, &additional[A_LINE_DROP], 1) \
	X("drop-polygons", no_argument, &additional[A_POLYGON_DROP], 1) \
	X("cluster-distance", required_argument, 0, 'K') \
	X("cluster-maxzoom", required_argument, 0, 'k') \
	X("preserve-point-density-threshold", required_argument, 0, '~') \
	X("preserve-multiplier-density-threshold", required_argument, 0, '~')

// [Dropping or merging a fraction of features to keep under tile size limits]
#define COMMON_DROPPING_OR_MERGING_A_FRACTION_OF_FEATURES_TO_KEEP_UNDER_TILE_SIZE_LIMITS_OPTIONS(X) \
	X("drop-densest-as-needed", no_argument, &additional[A_DROP_DENSEST_AS_NEEDED], 1) \
	X("drop-fraction-as-needed", no_argument, &additional[A_DROP_FRACTION_AS_NEEDED], 1) \
	X("drop-smallest-as-needed", no_argument, &additional[A_DROP_SMALLEST_AS_NEEDED], 1) \
	X("coalesce-densest-as-needed", no_argument, &additional[A_COALESCE_DENSEST_AS_NEEDED], 1) \
	X("coalesce-fraction-as-needed", no_argument, &additional[A_COALESCE_FRACTION_AS_NEEDED], 1) \
	X("coalesce-smallest-as-needed", no_argument, &additional[A_COALESCE_SMALLEST_AS_NEEDED], 1) \
	X("force-feature-limit", no_argument, &prevent[P_DYNAMIC_DROP], 1) \
	X("cluster-densest-as-needed", no_argument, &additional[A_CLUSTER_DENSEST_AS_NEEDED], 1) \
	X("keep-point-cluster-position", no_argument, &additional[A_KEEP_POINT_CLUSTER_POSITION], 1)

// [Dropping tightly overlapping features]
#define COMMON_DROPPING_TIGHTLY_OVERLAPPING_FEATURES_OPTIONS(X) \
	X("gamma", required_argument, 0, 'g') \
	X("increase-gamma-as-needed", no_argument, &additional[A_INCREASE_GAMMA_AS_NEEDED], 1)

// [Line and polygon simplification]
#define COMMON_LINE_AND_POLYGON_SIMPLIFICATION_OPTIONS(X) \
	X("simplification", required_argument, 0, 'S') \
	X("no-line-simplification", no_argument, &prevent[P_SIMPLIFY], 1) \
	X("simplify-only-low-zooms", no_argument, &prevent[P_SIMPLIFY_LOW], 1) \
	X("simplification-at-maximum-zoom", required_argument, 0, '~') \
	X("no-tiny-polygon-reduction", no_argument, &prevent[P_TINY_POLYGON_REDUCTION], 1) \
	X("no-tiny-polygon-reduction-at-maximum-zoom", no_argument, &prevent[P_TINY_POLYGON_REDUCTION_AT_MAXZOOM], 1) \
	X("tiny-polygon-size", required_argument, 0, '~') \
	X("no-simplification-of-shared-nodes", no_argument, &prevent[P_SIMPLIFY_SHARED_NODES], 1) \
	X("visvalingam", no_argument, &additional[A_VISVALINGAM], 1)

// [Attempts to improve shared polygon boundaries]
#define COMMON_ATTEMPTS_TO_IMPROVE_SHARED_POLYGON_BOUNDARIES_OPTIONS(X) \
	X("detect-shared-borders", no_argument, &additional[A_DETECT_SHARED_BORDERS], 1) \
	X("grid-low-zooms", no_argument, &additional[A_GRID_LOW_ZOOMS], 1)

// [Controlling clipping to tile boundaries]
#define COMMON_CONTROLLING_CLIPPING_TO_TILE_BOUNDARIES_OPTIONS(X) \
	X("buffer", required_argument, 0, 'b') \
	X("no-clipping", no_argument, &prevent[P_CLIPPING], 1) \
	X("no-duplication", no_argument, &prevent[P_DUPLICATION], 1)

// [Reordering features within each tile]
#define COMMON_REORDERING_FEATURES_WITHIN_EACH_TILE_OPTIONS(X) \
	X("preserve-input-order", no_argument, &prevent[P_INPUT_ORDER], 1) \
	X("reorder", no_argument, &additional[A_REORDER], 1) \
	X("coalesce", no_argument, &additional[A_COALESCE], 1) \
	X("reverse", no_argument, &additional[A_REVERSE], 1) \
	X("hilbert", no_argument, &additional[A_HILBERT], 1) \
	X("order-by", required_argument, 0, '~') \
	X("order-descending-by", required_argument, 0, '~') \
	X("order-smallest-first", no_argument, 0, '~') \
	X("order-largest-first", no_argument, 0, '~')

// [Adding calculated attributes]
#define COMMON_ADDING_CALCULATED_ATTRIBUTES_OPTIONS(X) \
	X("calculate-feature-density", no_argument, &additional[A_CALCULATE_FEATURE_DENSITY], 1) \
	X("generate-ids", no_argument, &additional[A_GENERATE_IDS], 1) \
	X("calculate-feature-index", no_argument, &additional[A_CALCULATE_INDEX], 1)

// [Trying to correct bad source geometry]
#define COMMON_TRYING_TO_CORRECT_BAD_SOURCE_GEOMETRY_OPTIONS(X) \
	X("detect-longitude-wraparound", no_argument, &additional[A_DETECT_WRAPAROUND], 1) \
	X("use-source-polygon-winding", no_argument, &prevent[P_USE_SOURCE_POLYGON_WINDING], 1) \
	X("reverse-source-polygon-winding", no_argument, &prevent[P_REVERSE_SOURCE_POLYGON_WINDING], 1) \
	X("clip-bounding-box", required_argument, 0, '~') \
	X("convert-polygons-to-label-points", no_argument, &additional[A_GENERATE_POLYGON_LABEL_POINTS], 1)

// [Filtering tile contents]
#define COMMON_FILTERING_TILE_CONTENTS_OPTIONS(X) \
	X("prefilter", required_argument, 0, 'C') \
	X("postfilter", required_argument, 0, 'c')

// [Setting or disabling tile size limits]
#define COMMON_SETTING_OR_DISABLING_TILE_SIZE_LIMITS_OPTIONS(X) \
	X("maximum-tile-bytes", required_argument, 0, 'M') \
	X("maximum-tile-features", required_argument, 0, 'O') \
	X("limit-tile-feature-count", required_argument, 0, '~') \
	X("limit-tile-feature-count-at-maximum-zoom", required_argument, 0, '~') \
	X("no-feature-limit", no_argument, &prevent[P_FEATURE_LIMIT], 1) \
	X("no-tile-size-limit", no_argument, &prevent[P_KILOBYTE_LIMIT], 1) \
	X("no-tile-compression", no_argument, &prevent[P_TILE_COMPRESSION], 1) \
	X("no-tile-stats", no_argument, &prevent[P_TILE_STATS], 1) \
	X("tile-stats-attributes-limit", required_argument, 0, '~') \
	X("tile-stats-sample-values-limit", required_argument, 0, '~') \
	X("tile-stats-values-limit", required_argument, 0, '~')

// [Temporary storage]
#define COMMON_TEMPORARY_STORAGE_OPTIONS(X) \
	X("temporary-directory", required_argument, 0, 't')

// [Progress indicator]
#define COMMON_PROGRESS_INDICATOR_OPTIONS(X) \
	X("quiet", no_argument, 0, 'q') \
	X("no-progress-indicator", no_argument, 0, 'Q') \
	X("progress-interval", required_argument, 0, 'U') \
	X("json-progress", no_argument, 0, 'u') \
	X("version", no_argument, 0, 'v') \
	X("prevent", required_argument, 0, 'p') \
	X("additional", required_argument, 0, 'a') \
	X("check-polygons", no_argument, &additional[A_DEBUG_POLYGON], 1) \
	X("no-polygon-splitting", no_argument, &prevent[P_POLYGON_SPLIT], 1) \
	X("prefer-radix-sort", no_argument, &additional[A_PREFER_RADIX_SORT], 1) \
	X("help", no_argument, 0, 'H')

// ---------------- main（tippecanoe）专属 ----------------

// [Checkpoint / resume]
#define MAIN_CHECKPOINT_RESUME_OPTIONS(X) \
	X("checkpoint-dir", required_argument, 0, '~') \
	X("resume", required_argument, 0, '~') \
	X("checkpoint-force", no_argument, 0, '~') \
	X("checkpoint-status", required_argument, 0, '~') \
	X("checkpoint-prune", required_argument, 0, '~')

// [Input files and layer names]
#define MAIN_INPUT_FILES_AND_LAYER_NAMES_OPTIONS(X) \
	X("layer", required_argument, 0, 'l') \
	X("named-layer", required_argument, 0, 'L')

// [Parallel processing of input]
#define MAIN_PARALLEL_PROCESSING_OF_INPUT_OPTIONS(X) \
	X("read-parallel", no_argument, 0, 'P')

// [Dropping or merging a fraction of features to keep under tile size limits]
#define MAIN_DROPPING_OR_MERGING_A_FRACTION_OF_FEATURES_TO_KEEP_UNDER_TILE_SIZE_LIMITS_OPTIONS(X) \
	X("drop-by-attribute-as-needed", required_argument, 0, '~') \
	X("drop-by-attribute-order", required_argument, 0, '~')

// ---------------- db（tippecanoe-db）专属 ----------------

// [PostGIS database connection]
#define DB_POSTGIS_DATABASE_CONNECTION_OPTIONS(X) \
	X("postgis", required_argument, 0, '~') \
	X("postgis-host", required_argument, 0, '~') \
	X("postgis-port", required_argument, 0, '~') \
	X("postgis-dbname", required_argument, 0, '~') \
	X("postgis-user", required_argument, 0, '~') \
	X("postgis-password", required_argument, 0, '~') \
	X("postgis-table", required_argument, 0, '~') \
	X("postgis-geometry-field", required_argument, 0, '~') \
	X("postgis-sql", required_argument, 0, '~') \
	X("postgis-columns", required_argument, 0, '~') \
	X("postgis-columns-best-effort", no_argument, 0, '~') \
	X("postgis-canonical-attr-order", no_argument, 0, '~') \
	X("postgis-no-canonical-attr-order", no_argument, 0, '~') \
	X("postgis-profile", no_argument, 0, '~') \
	X("postgis-shard-key", required_argument, 0, '~') \
	X("postgis-shard-mode", required_argument, 0, '~') \
	X("postgis-progress-count", no_argument, 0, '~')

// [MongoDB output]
#define DB_MONGODB_OUTPUT_OPTIONS(X) \
	X("mongo", required_argument, 0, '~') \
	X("mongo-host", required_argument, 0, '~') \
	X("mongo-port", required_argument, 0, '~') \
	X("mongo-dbname", required_argument, 0, '~') \
	X("mongo-collection", required_argument, 0, '~') \
	X("mongo-username", required_argument, 0, '~') \
	X("mongo-password", required_argument, 0, '~') \
	X("mongo-auth-source", required_argument, 0, '~') \
	X("mongo-batch-size", required_argument, 0, '~') \
	X("mongo-pool-size", required_argument, 0, '~') \
	X("mongo-timeout", required_argument, 0, '~') \
	X("mongo-no-indexes", no_argument, 0, '~') \
	X("mongo-drop-collection", no_argument, 0, '~') \
	X("mongo-metadata", no_argument, 0, '~') \
	X("mongo-no-metadata", no_argument, 0, '~') \
	X("mongo-fail-on-discard", no_argument, 0, '~') \
	X("mongo-no-fail-on-discard", no_argument, 0, '~')

// [Progress indicator]
#define DB_PROGRESS_INDICATOR_OPTIONS(X) \
	X("help-advanced", no_argument, 0, '~')

// 展开条目（供 *_TABLE 使用）
#define X_OPTION(name, arg, flag, val) \
	{ name, arg, flag, val },
#define GROUP_TITLE(title) \
	{ title, 0, 0, 0 },

// ---------------- tippecanoe 表序列（= 上游原序） ----------------
#define MAIN_OPTIONS_TABLE(X, G) \
	G("Output tileset") \
	COMMON_OUTPUT_TILESET_OPTIONS(X) \
	G("Checkpoint / resume") \
	MAIN_CHECKPOINT_RESUME_OPTIONS(X) \
	G("Tileset description and attribution") \
	COMMON_TILESET_DESCRIPTION_AND_ATTRIBUTION_OPTIONS(X) \
	G("Input files and layer names") \
	MAIN_INPUT_FILES_AND_LAYER_NAMES_OPTIONS(X) \
	G("Parallel processing of input") \
	MAIN_PARALLEL_PROCESSING_OF_INPUT_OPTIONS(X) \
	G("Projection of input") \
	COMMON_PROJECTION_OF_INPUT_OPTIONS(X) \
	G("Zoom levels") \
	COMMON_ZOOM_LEVELS_OPTIONS(X) \
	G("Tile resolution") \
	COMMON_TILE_RESOLUTION_OPTIONS(X) \
	G("Filtering feature attributes") \
	COMMON_FILTERING_FEATURE_ATTRIBUTES_OPTIONS(X) \
	G("Modifying feature attributes") \
	COMMON_MODIFYING_FEATURE_ATTRIBUTES_OPTIONS(X) \
	G("Filtering features by attributes") \
	COMMON_FILTERING_FEATURES_BY_ATTRIBUTES_OPTIONS(X) \
	G("Dropping a fixed fraction of features by zoom level") \
	COMMON_DROPPING_A_FIXED_FRACTION_OF_FEATURES_BY_ZOOM_LEVEL_OPTIONS(X) \
	G("Dropping or merging a fraction of features to keep under tile size limits") \
	MAIN_DROPPING_OR_MERGING_A_FRACTION_OF_FEATURES_TO_KEEP_UNDER_TILE_SIZE_LIMITS_OPTIONS(X) \
	COMMON_DROPPING_OR_MERGING_A_FRACTION_OF_FEATURES_TO_KEEP_UNDER_TILE_SIZE_LIMITS_OPTIONS(X) \
	G("Dropping tightly overlapping features") \
	COMMON_DROPPING_TIGHTLY_OVERLAPPING_FEATURES_OPTIONS(X) \
	G("Line and polygon simplification") \
	COMMON_LINE_AND_POLYGON_SIMPLIFICATION_OPTIONS(X) \
	G("Attempts to improve shared polygon boundaries") \
	COMMON_ATTEMPTS_TO_IMPROVE_SHARED_POLYGON_BOUNDARIES_OPTIONS(X) \
	G("Controlling clipping to tile boundaries") \
	COMMON_CONTROLLING_CLIPPING_TO_TILE_BOUNDARIES_OPTIONS(X) \
	G("Reordering features within each tile") \
	COMMON_REORDERING_FEATURES_WITHIN_EACH_TILE_OPTIONS(X) \
	G("Adding calculated attributes") \
	COMMON_ADDING_CALCULATED_ATTRIBUTES_OPTIONS(X) \
	G("Trying to correct bad source geometry") \
	COMMON_TRYING_TO_CORRECT_BAD_SOURCE_GEOMETRY_OPTIONS(X) \
	G("Filtering tile contents") \
	COMMON_FILTERING_TILE_CONTENTS_OPTIONS(X) \
	G("Setting or disabling tile size limits") \
	COMMON_SETTING_OR_DISABLING_TILE_SIZE_LIMITS_OPTIONS(X) \
	G("Temporary storage") \
	COMMON_TEMPORARY_STORAGE_OPTIONS(X) \
	G("Progress indicator") \
	COMMON_PROGRESS_INDICATOR_OPTIONS(X) \
	{0, 0, 0, 0}

// ---------------- tippecanoe-db 表序列（PostGIS/Mongo 组置前） ----------------
#define DB_OPTIONS_TABLE(X, G) \
	G("Output tileset") \
	COMMON_OUTPUT_TILESET_OPTIONS(X) \
	G("Tileset description and attribution") \
	COMMON_TILESET_DESCRIPTION_AND_ATTRIBUTION_OPTIONS(X) \
	G("PostGIS database connection") \
	DB_POSTGIS_DATABASE_CONNECTION_OPTIONS(X) \
	G("MongoDB output") \
	DB_MONGODB_OUTPUT_OPTIONS(X) \
	G("Projection of input") \
	COMMON_PROJECTION_OF_INPUT_OPTIONS(X) \
	G("Zoom levels") \
	COMMON_ZOOM_LEVELS_OPTIONS(X) \
	G("Tile resolution") \
	COMMON_TILE_RESOLUTION_OPTIONS(X) \
	G("Filtering feature attributes") \
	COMMON_FILTERING_FEATURE_ATTRIBUTES_OPTIONS(X) \
	G("Modifying feature attributes") \
	COMMON_MODIFYING_FEATURE_ATTRIBUTES_OPTIONS(X) \
	G("Filtering features by attributes") \
	COMMON_FILTERING_FEATURES_BY_ATTRIBUTES_OPTIONS(X) \
	G("Dropping a fixed fraction of features by zoom level") \
	COMMON_DROPPING_A_FIXED_FRACTION_OF_FEATURES_BY_ZOOM_LEVEL_OPTIONS(X) \
	G("Dropping or merging a fraction of features to keep under tile size limits") \
	COMMON_DROPPING_OR_MERGING_A_FRACTION_OF_FEATURES_TO_KEEP_UNDER_TILE_SIZE_LIMITS_OPTIONS(X) \
	G("Dropping tightly overlapping features") \
	COMMON_DROPPING_TIGHTLY_OVERLAPPING_FEATURES_OPTIONS(X) \
	G("Line and polygon simplification") \
	COMMON_LINE_AND_POLYGON_SIMPLIFICATION_OPTIONS(X) \
	G("Attempts to improve shared polygon boundaries") \
	COMMON_ATTEMPTS_TO_IMPROVE_SHARED_POLYGON_BOUNDARIES_OPTIONS(X) \
	G("Controlling clipping to tile boundaries") \
	COMMON_CONTROLLING_CLIPPING_TO_TILE_BOUNDARIES_OPTIONS(X) \
	G("Reordering features within each tile") \
	COMMON_REORDERING_FEATURES_WITHIN_EACH_TILE_OPTIONS(X) \
	G("Adding calculated attributes") \
	COMMON_ADDING_CALCULATED_ATTRIBUTES_OPTIONS(X) \
	G("Trying to correct bad source geometry") \
	COMMON_TRYING_TO_CORRECT_BAD_SOURCE_GEOMETRY_OPTIONS(X) \
	G("Filtering tile contents") \
	COMMON_FILTERING_TILE_CONTENTS_OPTIONS(X) \
	G("Setting or disabling tile size limits") \
	COMMON_SETTING_OR_DISABLING_TILE_SIZE_LIMITS_OPTIONS(X) \
	G("Temporary storage") \
	COMMON_TEMPORARY_STORAGE_OPTIONS(X) \
	G("Progress indicator") \
	DB_PROGRESS_INDICATOR_OPTIONS(X) \
	COMMON_PROGRESS_INDICATOR_OPTIONS(X) \
	{0, 0, 0, 0}

// ---------------- 共享 getopt 骨架 ----------------
// 从 long_options_orig 构建 getopt 短串并校验 flag 重用（两入口原为重复代码）。
inline void build_getopt_tables(struct option *long_options_orig, size_t orig_n,
                                struct option *long_options, char *getopt_str) {
	size_t out = 0, cout = 0;
	for (size_t lo = 0; lo < orig_n && long_options_orig[lo].name != NULL; lo++) {
		if (long_options_orig[lo].val != 0) {
			long_options[out++] = long_options_orig[lo];
			if (long_options_orig[lo].val > ' ') {
				getopt_str[cout++] = static_cast<char>(long_options_orig[lo].val);
				if (long_options_orig[lo].has_arg == required_argument) {
					getopt_str[cout++] = ':';
				}
			}
		}
	}
	long_options[out] = {0, 0, 0, 0};
	getopt_str[cout] = '\0';

	for (size_t lo = 0; lo < orig_n && long_options[lo].name != NULL; lo++) {
		if (long_options[lo].flag != NULL) {
			if (*long_options[lo].flag != 0) {
				fprintf(stderr, "Internal error: reused %s\n", long_options[lo].name);
				exit(EXIT_IMPOSSIBLE);
			}
			*long_options[lo].flag = 1;
		}
	}

	for (size_t lo = 0; lo < orig_n && long_options[lo].name != NULL; lo++) {
		if (long_options[lo].flag != NULL) {
			*long_options[lo].flag = 0;
		}
	}
}

#endif // OPTIONS_TABLE_HPP
