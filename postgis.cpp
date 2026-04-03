#include "postgis.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <libpq-fe.h>
#include <string.h>
#include <unistd.h>
#include "geometry.hpp"
#include "serial.hpp"
#include "geojson.hpp"

/**
 * @brief PostGIS 读取器构造函数
 * 
 * 初始化 PostGIS 读取器，配置数据库连接参数和性能优化参数
 * 自动调整批次大小到合理范围内（MIN_BATCH_SIZE 到 MAX_BATCH_SIZE）
 * 
 * @param cfg PostGIS 配置结构，包含数据库连接信息和性能参数
 */
PostGISReader::PostGISReader(const postgis_config &cfg) : config(cfg), conn(NULL)
{
    if (config.batch_size < MIN_BATCH_SIZE)
    {
        config.batch_size = MIN_BATCH_SIZE;
    }
    else if (config.batch_size > MAX_BATCH_SIZE)
    {
        config.batch_size = MAX_BATCH_SIZE;
    }
}

/**
 * @brief PostGIS 读取器析构函数
 * 
 * 释放数据库连接资源，确保没有内存泄漏
 */
PostGISReader::~PostGISReader()
{
    if (conn)
    {
        PQfinish((PGconn *)conn);
    }
}

/**
 * @brief 连接到 PostgreSQL/PostGIS 数据库
 * 
 * 使用配置的连接参数建立数据库连接，设置 30 秒连接超时
 * 
 * @return bool 连接成功返回 true，失败返回 false
 */
bool PostGISReader::connect()
{
    const char *keywords[] = {"host", "port", "dbname", "user", "password", "connect_timeout", NULL};
    std::string timeout_str = std::to_string(CONNECTION_TIMEOUT_SEC);
    const char *values[] = {
        config.host.c_str(),
        config.port.c_str(),
        config.dbname.c_str(),
        config.user.c_str(),
        config.password.c_str(),
        timeout_str.c_str(),
        NULL};
    
    fprintf(stderr, "Debug: Connecting to PostGIS (%s:%s/%s)...\n", config.host.c_str(), config.port.c_str(), config.dbname.c_str());
    conn = PQconnectdbParams(keywords, values, 0);

    if (PQstatus((PGconn *)conn) != CONNECTION_OK)
    {
        fprintf(stderr, "Debug: Connection to PostGIS failed: %s\n", PQerrorMessage((PGconn *)conn));
        return false;
    }

    fprintf(stderr, "Debug: Connected to PostGIS successfully.\n");
    return true;
}

/**
 * @brief 检查内存使用情况
 * 
 * 检查当前内存使用是否超过配置的最大限制
 * 
 * @return bool 内存使用正常返回 true，超出限制返回 false
 */
bool PostGISReader::check_memory_usage()
{
    size_t estimated = current_memory_usage.load();
    size_t limit = config.max_memory_mb * 1024 * 1024;
    
    if (estimated > limit)
    {
        fprintf(stderr, "Warning: Memory usage (%zu MB) exceeds limit (%zu MB)\n",
                estimated / (1024 * 1024), config.max_memory_mb);
        return false;
    }
    return true;
}

/**
 * @brief 记录处理进度
 * 
 * 当启用进度报告时，显示当前处理阶段的进度信息
 * 
 * @param processed 已处理的要素数量
 * @param total 总要素数量（如果为 0 则不显示百分比）
 * @param stage 当前处理阶段的描述
 */
void PostGISReader::log_progress(size_t processed, size_t total, const char *stage)
{
    if (!config.enable_progress_report)
        return;

    if (total > 0)
    {
        double percent = (processed * 100.0) / total;
        fprintf(stderr, "Progress: %s - %zu/%zu (%.1f%%)\n", stage, processed, total, percent);
    }
    else
    {
        fprintf(stderr, "Progress: %s - %zu features processed\n", stage, processed);
    }
}

/**
 * @brief 转义 JSON 字符串
 * 
 * 对字符串中的特殊字符进行 JSON 转义，确保生成的 JSON 格式正确
 * 支持转义：双引号、反斜杠、换行符、回车符、制表符等
 * 
 * @param value 需要转义的原始字符串
 * @return std::string 转义后的字符串
 */
std::string PostGISReader::escape_json_string(const char *value)
{
    if (!value)
        return "";

    std::string result;
    const size_t len = strlen(value);
    result.reserve(len * 2);

    for (size_t k = 0; k < len; k++)
    {
        char c = value[k];
        switch (c)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                result += buf;
            }
            else
            {
                result += c;
            }
            break;
        }
    }

    return result;
}

/**
 * @brief 处理单个要素
 * 
 * 从数据库查询结果中提取单个要素，构建 GeoJSON Feature 格式，
 * 并调用解析器将其转换为 Tippecanoe 内部格式
 * 
 * @param res PostgreSQL 查询结果集
 * @param row 当前处理的行号
 * @param nfields 字段总数
 * @param geom_field_index 几何字段的索引位置
 * @param sst 序列化状态向量
 * @param layer 当前图层索引
 * @param layername 图层名称
 * @param thread_id 当前线程ID，用于获取独立的 sst 槽
 */
void PostGISReader::process_feature(PGresult *res, int row, int nfields, int geom_field_index,
                                    const std::vector<std::string> &field_names,
                                    std::vector<struct serialization_state> &sst, size_t layer,
                                    const std::string &layername, size_t thread_id)
{
    feature_buffer.clear();
    // 提高预分配：4KB 通常能容纳绝大多数要素，减少扩容开销
    if (feature_buffer.capacity() < 4096) {
        feature_buffer.reserve(4096);
    }

    feature_buffer.append("{\"type\":\"Feature\",");

    char *geom_value = PQgetvalue(res, row, geom_field_index);
    if (geom_value == NULL || strlen(geom_value) == 0)
    {
        return;
    }

    feature_buffer.append("\"geometry\":");
    feature_buffer.append(geom_value);

    bool has_properties = false;
    for (int field = 0; field < nfields; field++)
    {
        if (field != geom_field_index)
        {
            char *value = PQgetvalue(res, row, field);
            if (value != NULL && value[0] != '\0')
            {
                has_properties = true;
                break;
            }
        }
    }

    if (has_properties)
    {
        feature_buffer.append(",\"properties\":{");
        bool first_property = true;

        for (int field = 0; field < nfields; field++)
        {
            if (field != geom_field_index)
            {
                const std::string &fieldname = field_names[field];
                if (fieldname.empty() || 
                    fieldname == config.geometry_field)
                {
                    continue;
                }

                char *value = PQgetvalue(res, row, field);
                if (value == NULL || value[0] == '\0')
                {
                    continue;
                }

                if (!first_property)
                {
                    feature_buffer.append(",");
                }
                first_property = false;

                feature_buffer.append("\"");
                feature_buffer.append(fieldname);
                feature_buffer.append("\":");

                Oid type = PQftype(res, field);

                if (type == 20 || type == 21 || type == 23 || type == 700 || type == 701)
                {
                    feature_buffer.append(value);
                }
                else if (type == 16)
                {
                    feature_buffer.append((value[0] == 't') ? "true" : "false");
                }
                else
                {
                    feature_buffer.append("\"");
                    feature_buffer.append(escape_json_string(value));
                    feature_buffer.append("\"");
                }
            }
        }
        feature_buffer.append("}");
    }

    feature_buffer.append("}");

    // 优化：直接使用 feature_buffer 内存，避免 strdup 的额外分配和拷贝
    // 由于 parse_json 是同步执行的，在 json_end_map 之前 feature_buffer 保证有效。
    struct json_pull *jp = json_begin_map((char *) feature_buffer.c_str(), feature_buffer.length());
    if (jp != NULL)
    {
        struct serialization_state temp_sst = sst[thread_id % sst.size()];
        temp_sst.fname = "PostGIS";
        temp_sst.line = row + 1;

        parse_json(&temp_sst, jp, layer, layername);
        json_end_map(jp);
    }

    total_features_processed.fetch_add(1);

    current_memory_usage.fetch_add(feature_buffer.capacity());
}

/**
 * @brief 处理一批数据
 * 
 * 遍历查询结果集，逐条处理每个要素，并监控内存使用情况
 * 
 * @param res PostgreSQL 查询结果集
 * @param sst 序列化状态向量
 * @param layer 当前图层索引
 * @param layername 图层名称
 * @param geom_field_index 几何字段的索引位置
 * @param thread_id 线程ID
 */
void PostGISReader::process_batch(PGresult *res, std::vector<struct serialization_state> &sst,
                                  size_t layer, const std::string &layername, int geom_field_index,
                                  size_t thread_id)
{
    int ntuples = PQntuples(res);
    int nfields = PQnfields(res);

    // 性能优化：在批处理开始前缓存所有字段名，避免在行循环中重复调用 PQfname
    std::vector<std::string> field_names;
    field_names.reserve(nfields);
    for (int i = 0; i < nfields; i++) {
        field_names.push_back(PQfname(res, i));
    }

    for (int i = 0; i < ntuples; i++)
    {
        process_feature(res, i, nfields, geom_field_index, field_names, sst, layer, layername, thread_id);

        if (i % 100 == 0 && !check_memory_usage())
        {
            fprintf(stderr, "Warning: Memory pressure detected at feature %d\n", i);
        }
    }

    total_batches_processed.fetch_add(1);

    log_progress(total_features_processed.load(), 0, "Processing features");
}

/**
 * @brief 执行 SQL 查询（带重试机制）
 * 
 * 执行 SQL 查询，失败时自动重试，最多重试 config.max_retries 次
 * 
 * @param query SQL 查询语句
 * @return bool 执行成功返回 true，失败返回 false
 */
bool PostGISReader::execute_query_with_retry(const std::string &query)
{
    int retries = 0;
    while (retries < config.max_retries)
    {
        if (execute_query(query))
        {
            return true;
        }

        retries++;
        if (retries < config.max_retries)
        {
            fprintf(stderr, "Retrying query (attempt %d/%d)...\n", retries + 1, config.max_retries);
            usleep(1000000);
        }
    }

    return false;
}

/**
 * @brief 执行 SQL 查询
 * 
 * 执行单个 SQL 查询，不自动重试
 * 
 * @param query SQL 查询语句
 * @return bool 执行成功返回 true，失败返回 false
 */
bool PostGISReader::execute_query(const std::string &query)
{
    if (!conn)
    {
        fprintf(stderr, "Error: Not connected to database\n");
        return false;
    }

    PGresult *res = PQexec((PGconn *)conn, query.c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)
    {
        fprintf(stderr, "Query failed: %s\n", PQerrorMessage((PGconn *)conn));
        PQclear(res);
        return false;
    }

    PQclear(res);
    return true;
}

/**
 * @brief 获取主键范围
 * 
 * 获取指定主键列的最小值和最大值，用于并行分区扫描
 * 
 * @param min_val 传出的最小值
 * @param max_val 传出的最大值
 * @return bool 成功返回 true，失败返回 false
 */
bool PostGISReader::get_pk_range(long long &min_val, long long &max_val)
{
    if (!conn && !connect())
    {
        return false;
    }
    
    if (config.pk_field.empty() || config.table.empty()) {
        fprintf(stderr, "Error: postgis-pk and postgis-table are required for get_pk_range\n");
        return false;
    }

    std::string query = "SELECT MIN(" + config.pk_field + "), MAX(" + config.pk_field + ") FROM " + config.table;
    PGresult *res = PQexec((PGconn *)conn, query.c_str());
    
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "Failed to get PK range: %s\n", PQerrorMessage((PGconn *)conn));
        PQclear(res);
        return false;
    }
    
    if (PQntuples(res) == 0 || PQgetisnull(res, 0, 0) || PQgetisnull(res, 0, 1))
    {
        fprintf(stderr, "Warning: Table seems to be empty or PK field is NULL\n");
        PQclear(res);
        return false;
    }

    min_val = strtoll(PQgetvalue(res, 0, 0), NULL, 10);
    max_val = strtoll(PQgetvalue(res, 0, 1), NULL, 10);
    
    PQclear(res);
    return true;
}

/**
 * @brief 读取地理空间要素的主函数
 * 
 * 从 PostGIS 数据库读取地理空间数据，支持自定义 SQL 查询或自动生成查询
 * 支持游标模式（大数据集）和普通模式（小数据集）
 * 自动检测几何字段，支持数据类型转换，实时监控进度
 * 
 * @param sst 序列化状态向量，用于存储瓦片数据
 * @param layer 当前图层索引
 * @param layername 图层名称
 * @param min_pk 该分区的最小主键（包含）
 * @param max_pk 该分区的最大主键（不包含）
 * @param has_range 是否启用了分区限制
 * @param thread_id 当前线程ID
 * @return bool 读取成功返回 true，失败返回 false
 */
bool PostGISReader::read_features(std::vector<struct serialization_state> &sst, size_t layer, const std::string &layername,
                                  long long min_pk, long long max_pk, bool has_range, size_t thread_id)
{
    if (!conn)
    {
        if (!connect())
        {
            return false;
        }
    }

    std::string base_query;
    if (!config.sql.empty())
    {
        base_query = config.sql;
    }
    else if (!config.table.empty() && !config.geometry_field.empty())
    {
        base_query = "SELECT ST_AsGeoJSON(ST_Transform(" + config.geometry_field + ", 4326)) as geojson, * FROM " + config.table;
    }
    else
    {
        fprintf(stderr, "Error: Either --postgis-sql or both --postgis-table and --postgis-geometry-field are required\n");
        return false;
    }

    if (has_range && !config.pk_field.empty())
    {
        // 如果是自定义 SQL，这里强行拼接 WHERE 可能导致语法错误，但通常我们通过 --postgis-table 工作
        // 简单处理：将子查询包装起来或直接附带 WHERE
        std::string range_cond = config.pk_field + " >= " + std::to_string(min_pk) + 
                                 " AND " + config.pk_field + " < " + std::to_string(max_pk);
        
        if (!config.sql.empty()) {
            base_query = "SELECT * FROM (" + config.sql + ") AS subq WHERE " + range_cond;
        } else {
            base_query += " WHERE " + range_cond;
        }
    }

    size_t total_count = 0;
    if (config.enable_progress_report)
    {
        std::string count_query = "SELECT COUNT(*) FROM (" + base_query + ") AS subquery";
        fprintf(stderr, "Debug: Executing count query: %s\n", count_query.c_str());
        PGresult *count_res = PQexec((PGconn *)conn, count_query.c_str());
        if (PQresultStatus(count_res) == PGRES_TUPLES_OK)
        {
            total_count = strtoull(PQgetvalue(count_res, 0, 0), NULL, 10);
        }
        else {
            fprintf(stderr, "Debug: Count query failed: %s\n", PQerrorMessage((PGconn *)conn));
        }
        PQclear(count_res);
    }

    if (config.use_cursor && total_count > config.batch_size)
    {
        fprintf(stderr, "Using cursor-based batch processing (batch size: %zu)\n", config.batch_size);

        // 使用当前线程 ID 保证游标名字唯一
        std::string cursor_name = "postgis_cursor_" + std::to_string(getpid()) + "_" + std::to_string(min_pk);
        std::string declare_query = "DECLARE " + cursor_name + " CURSOR FOR " + base_query;

        if (!execute_query(declare_query))
        {
            fprintf(stderr, "Failed to declare cursor, falling back to non-cursor mode\n");
            goto non_cursor_mode;
        }

        size_t offset = 0;
        while (true)
        {
            std::string fetch_query = "FETCH FORWARD " + std::to_string(config.batch_size) + " FROM " + cursor_name;
            PGresult *res = PQexec((PGconn *)conn, fetch_query.c_str());

            if (PQresultStatus(res) != PGRES_TUPLES_OK)
            {
                fprintf(stderr, "Fetch failed: %s\n", PQerrorMessage((PGconn *)conn));
                PQclear(res);
                break;
            }

            int ntuples = PQntuples(res);
            if (ntuples == 0)
            {
                PQclear(res);
                break;
            }

            int geom_field_index = -1;
            int nfields = PQnfields(res);
            for (int i = 0; i < nfields; i++)
            {
                if (strcmp(PQfname(res, i), "geojson") == 0)
                {
                    geom_field_index = i;
                    break;
                }
            }
            if (geom_field_index == -1)
            {
                geom_field_index = 0;
            }

            process_batch(res, sst, layer, layername, geom_field_index, thread_id);

            offset += ntuples;
            log_progress(offset, total_count, "Fetching batches");

            PQclear(res);

            if (!check_memory_usage())
            {
                fprintf(stderr, "Memory pressure detected, pausing...\n");
                usleep(100000);
            }
        }

        std::string close_query = "CLOSE " + cursor_name;
        execute_query(close_query);
    }
    else
    {
    non_cursor_mode:
        fprintf(stderr, "Using standard query mode\n");

        PGresult *res = PQexec((PGconn *)conn, base_query.c_str());
        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            fprintf(stderr, "Query failed: %s\n", PQerrorMessage((PGconn *)conn));
            PQclear(res);
            return false;
        }

        int ntuples = PQntuples(res);
        fprintf(stderr, "Processing %d features\n", ntuples);

        int geom_field_index = -1;
        int nfields = PQnfields(res);
        for (int i = 0; i < nfields; i++)
        {
            if (strcmp(PQfname(res, i), "geojson") == 0)
            {
                geom_field_index = i;
                break;
            }
        }

        if (geom_field_index == -1 && !config.geometry_field.empty())
        {
            for (int i = 0; i < nfields; i++)
            {
                if (strcmp(PQfname(res, i), config.geometry_field.c_str()) == 0)
                {
                    geom_field_index = i;
                    break;
                }
            }
        }

        if (geom_field_index == -1)
        {
            fprintf(stderr, "Warning: Geometry column not found, using first column\n");
            geom_field_index = 0;
        }

        process_batch(res, sst, layer, layername, geom_field_index, thread_id);

        PQclear(res);
    }

    log_progress(total_features_processed.load(), total_count, "Completed");
    fprintf(stderr, "Total features processed: %zu in %zu batches\n",
            total_features_processed.load(), total_batches_processed.load());

    return true;
}
