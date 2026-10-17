// tile-db.cpp —— tippecanoe-db 的瓦片输出注入层（v3.4 副本收敛后的薄壳）
//
// 瓦片循环主体（traverse_zooms 及其全部辅助）已收敛到 tile.cpp 单份实现；
// 本文件只提供：
//   1. MongoDB 附加写出的 sink 实现（write / erase_zoom / 工作线程清理）
//   2. maindb 启动时的注入入口 set_tile_db_output_sinks()
// 原 tile-db.cpp 的其它差异（checkpoint 能力、drop-by-attribute）在
// tippecanoe-db 中按"能力禁用"处理：maindb 调 tile.cpp 的 traverse_zooms 时
// 传 session=NULL、drop_by_attribute=NULL/false，能力代码路径不进入。
//
// Mongo 写出顺序与收敛前的 tile-db 一致：write 为先 Mongo 后 mbtiles/dir
//（tile.cpp 中 sink 调用位于 db_lock 之前），erase 为先本地后 Mongo。

#include <stdio.h>
#include <string>

#include "maindb.hpp"      // use_mongo / mongo_cfg
#include "mongo.hpp"
#include "tile.hpp"        // tile_output_sink 等契约
#include "error_logger.hpp"

// 每瓦片：Mongo 附加写（原 write_tile_outputs 的 Mongo 部分）。mbtiles/dir
// 直写留在 tile.cpp 的 db_lock 保护内；Mongo 走线程本地 writer，无锁。
static void db_output_sink(int z, unsigned tx, unsigned ty, const std::string &compressed) {
	if (!use_mongo) {
		return;
	}
	try {
		MongoWriter *writer = MongoWriter::get_writer_instance(mongo_cfg);
		if (writer != NULL) {
			writer->write_tile(z, tx, ty, compressed.data(), compressed.size());
		} else {
			fprintf(stderr, "Error: Failed to get MongoDB writer instance for tile %d/%u/%u\n", z, tx, ty);
			ErrorLogger::instance().log_mongo_error(z, tx, ty, "get_writer_instance", "Failed to get MongoDB writer instance");
		}
	} catch (const std::exception &e) {
		fprintf(stderr, "Error: MongoDB write failed for tile %d/%u/%u: %s\n", z, tx, ty, e.what());
		ErrorLogger::instance().log_mongo_error(z, tx, ty, "write_tile", e.what());
	} catch (...) {
		fprintf(stderr, "Error: MongoDB write failed with unknown error for tile %d/%u/%u\n", z, tx, ty);
		ErrorLogger::instance().log_mongo_error(z, tx, ty, "write_tile", "Unknown MongoDB write error");
	}
}

// 每 zoom 回滚：Mongo erase（原 erase_zoom_outputs 的 Mongo 部分）。
static void db_erase_zoom_sink(int z) {
	if (!use_mongo) {
		return;
	}
	try {
		MongoWriter *writer = MongoWriter::get_writer_instance(mongo_cfg);
		if (writer != NULL) {
			writer->erase_zoom(z);
		} else {
			fprintf(stderr, "Error: Failed to get MongoDB writer instance for zoom erase %d\n", z);
			ErrorLogger::instance().log_mongo_error(z, 0, 0, "get_writer_instance", "Failed to get MongoDB writer instance for erase_zoom");
		}
	} catch (const std::exception &e) {
		fprintf(stderr, "Error: MongoDB erase zoom failed for z=%d: %s\n", z, e.what());
		ErrorLogger::instance().log_mongo_error(z, 0, 0, "erase_zoom", e.what());
	} catch (...) {
		fprintf(stderr, "Error: MongoDB erase zoom failed with unknown error for z=%d\n", z);
		ErrorLogger::instance().log_mongo_error(z, 0, 0, "erase_zoom", "Unknown MongoDB erase zoom error");
	}
}

// 工作线程收尾：flush 并销毁线程本地 writer 实例（原 traverse_zooms 线程尾部）。
static void db_thread_cleanup_sink() {
	if (use_mongo) {
		MongoWriter::destroy_current_thread_instance();
	}
}

void set_tile_db_output_sinks() {
	tile_output_sink = db_output_sink;
	tile_erase_zoom_sink = db_erase_zoom_sink;
	tile_thread_cleanup_sink = db_thread_cleanup_sink;
}
