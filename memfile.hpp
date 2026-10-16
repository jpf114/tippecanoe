#ifndef MEMFILE_HPP
#define MEMFILE_HPP

#include <atomic>
#include <string>
#include "errors.hpp"

// Flush threshold constants
#define FLUSH_THRESHOLD_BASE (100 * 1024 * 1024)    // 100 MB base threshold
#define FLUSH_THRESHOLD_MIN (5 * 1024 * 1024)       // 5 MB minimum threshold
#define FLUSH_THRESHOLD_MAX (100 * 1024 * 1024)     // 100 MB maximum threshold

/**
 * @brief Get dynamic flush threshold based on CPU count
 * 
 * Dynamically adjusts the flush threshold to control memory usage:
 * - Low thread count (<8): Keep high threshold for better performance
 * - Medium thread count (8-32): Reduce threshold to control memory
 * - High thread count (>32): Aggressive threshold to prevent OOM
 * 
 * @return size_t Flush threshold in bytes
 */
inline size_t get_flush_threshold() {
	extern size_t CPUS;
	
	if (CPUS <= 8) {
		return FLUSH_THRESHOLD_BASE;  // 100 MB for low thread count
	} else if (CPUS <= 16) {
		return 50 * 1024 * 1024;      // 50 MB for medium thread count
	} else if (CPUS <= 32) {
		return 20 * 1024 * 1024;      // 20 MB for high thread count
	} else {
		return 10 * 1024 * 1024;      // 10 MB for very high thread count
	}
}

struct memfile {
	int fd = 0;
	std::string map;
	unsigned long tree = 0;
	FILE *fp = NULL;
	size_t off = 0;
};

struct memfile *memfile_open(int fd);
int memfile_close(struct memfile *file);
int memfile_write(struct memfile *file, void *s, long long len, bool &in_memory);
void memfile_full(struct memfile *file);

#endif
