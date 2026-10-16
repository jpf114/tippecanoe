#include <string>
#include "jsonpull/jsonpull.h"

struct json_feature_action {
	std::string fname;

	virtual int add_feature(json_object *geometry, bool geometrycollection, json_object *properties, json_object *id, json_object *tippecanoe, json_object *feature) = 0;
	virtual void check_crs(json_object *j) = 0;
};

void parse_json(json_feature_action *action, json_pull *jp);

// v3.4 (M13): 读入阶段优雅退出钩子。checkpoint 模块注册 shutdown_requested，
// 使 SIGTERM/SIGINT 在漫长的读入阶段也能及时生效（此前仅在 zoom 边界检查，
// 大文件读入期间信号被静默吞掉）。钩子为函数指针而非直接依赖 checkpoint
// 符号：tippecanoe-json-tool 等不链接 checkpoint.o 的二进制复用 parse_json
// 时钩子保持 NULL，行为与上游一致。
void set_json_read_shutdown_hook(bool (*f)());
