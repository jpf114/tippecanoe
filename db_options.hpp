#ifndef DB_OPTIONS_HPP
#define DB_OPTIONS_HPP

#include <cstring>
#include <string>

struct db_runtime_config {
	bool write_metadata = false;
	bool metadata_explicitly_set = false;
};

struct db_output_mode {
	bool business_output = false;
	bool has_mbtiles_verification = false;
	bool has_directory_tiles = false;

	bool requires_mongo_output() const {
		return business_output;
	}

	bool has_local_tiles_output() const {
		return has_mbtiles_verification || has_directory_tiles;
	}

	bool is_local_only() const {
		return !business_output && has_local_tiles_output();
	}

	bool is_business_only() const {
		return business_output && !has_local_tiles_output();
	}

	bool is_dual_output() const {
		return business_output && has_local_tiles_output();
	}
};

struct db_runtime_defaults {
	bool enable_mongo_metadata = false;
	bool note_missing_mbtiles_verification = false;
	bool note_dual_output_mode = false;
};

inline bool is_named_db_help_option(const char *name, const char *const *options) {
	if (name == NULL || name[0] == '\0') {
		return false;
	}

	for (size_t i = 0; options[i] != NULL; i++) {
		if (std::strcmp(name, options[i]) == 0) {
			return true;
		}
	}

	return false;
}

inline const char *const *core_db_help_options() {
	static const char *core_options[] = {
		"output",
		"force",
		"postgis",
		"postgis-table",
		"postgis-sql",
		"postgis-geometry-field",
		"mongo",
		"maximum-zoom",
		"minimum-zoom",
		"help",
		NULL,
	};

	return core_options;
}

inline const char *const *advanced_db_help_options() {
	static const char *advanced_options[] = {
		"postgis-host",
		"postgis-port",
		"postgis-dbname",
		"postgis-user",
		"postgis-password",
		"postgis-columns",
		"mongo-host",
		"mongo-port",
		"mongo-dbname",
		"mongo-collection",
		"mongo-username",
		"mongo-password",
		"mongo-auth-source",
		"mongo-drop-collection",
		"mongo-no-metadata",
		"mongo-no-indexes",
		"mongo-no-fail-on-discard",
		NULL,
	};

	return advanced_options;
}

inline const char *const *expert_db_help_options() {
	static const char *expert_options[] = {
		"postgis-columns-best-effort",
		"postgis-shard-key",
		"postgis-shard-mode",
		"mongo-batch-size",
		"mongo-pool-size",
		"mongo-timeout",
		NULL,
	};

	return expert_options;
}

inline bool is_core_db_help_option(const char *name) {
	return is_named_db_help_option(name, core_db_help_options());
}

inline bool is_core_db_help_option(const std::string &name) {
	return is_core_db_help_option(name.c_str());
}

inline bool is_advanced_db_help_option(const char *name) {
	return is_named_db_help_option(name, advanced_db_help_options());
}

inline bool is_expert_db_help_option(const char *name) {
	return is_named_db_help_option(name, expert_db_help_options());
}

inline db_output_mode determine_db_output_mode(bool mongo_enabled, bool has_mbtiles_output, bool has_dir_output) {
	db_output_mode mode;
	mode.business_output = mongo_enabled;
	mode.has_mbtiles_verification = has_mbtiles_output;
	mode.has_directory_tiles = has_dir_output;
	return mode;
}

template <typename TConfig>
inline db_runtime_defaults normalize_db_runtime_defaults(bool mongo_enabled, bool has_mbtiles_output, bool has_dir_output, TConfig &cfg) {
	db_output_mode mode = determine_db_output_mode(mongo_enabled, has_mbtiles_output, has_dir_output);
	db_runtime_defaults defaults;

	if (mode.business_output && !cfg.metadata_explicitly_set) {
		cfg.write_metadata = true;
		defaults.enable_mongo_metadata = true;
	}

	if (mode.is_business_only()) {
		defaults.note_missing_mbtiles_verification = true;
	}

	if (mode.is_dual_output()) {
		defaults.note_dual_output_mode = true;
	}

	return defaults;
}

#endif
