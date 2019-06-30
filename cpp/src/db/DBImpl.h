/*******************************************************************************
 * Copyright 上海赜睿信息科技有限公司(Zilliz) - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited.
 * Proprietary and confidential.
 ******************************************************************************/
#pragma once

#include "DB.h"
#include "MemManager.h"
#include "Types.h"
#include "utils/ThreadPool.h"

#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>
#include <thread>
#include <list>
#include <set>
#include "scheduler/context/SearchContext.h"


namespace zilliz {
namespace milvus {
namespace engine {

class Env;

namespace meta {
class Meta;
}

class DBImpl : public DB {
 public:
    using MetaPtr = meta::Meta::Ptr;
    using MemManagerPtr = typename MemManager::Ptr;

    explicit DBImpl(const Options &options);

    Status
    CreateTable(meta::TableSchema &table_schema) override;

    Status
    DeleteTable(const std::string &table_id, const meta::DatesT &dates) override;

    Status
    DescribeTable(meta::TableSchema &table_schema) override;

    Status
    HasTable(const std::string &table_id, bool &has_or_not) override;

    Status
    AllTables(std::vector<meta::TableSchema> &table_schema_array) override;

    Status
    GetTableRowCount(const std::string &table_id, uint64_t &row_count) override;

    Status
    InsertVectors(const std::string &table_id, uint64_t n, const float *vectors, IDNumbers &vector_ids) override;

    Status
    Query(const std::string &table_id, uint64_t k, uint64_t nq, const float *vectors, QueryResults &results) override;

    Status
    Query(const std::string &table_id,
          uint64_t k,
          uint64_t nq,
          const float *vectors,
          const meta::DatesT &dates,
          QueryResults &results) override;

    Status
    Query(const std::string &table_id,
          const std::vector<std::string> &file_ids,
          uint64_t k,
          uint64_t nq,
          const float *vectors,
          const meta::DatesT &dates,
          QueryResults &results) override;

    Status DropAll() override;

    Status Size(uint64_t &result) override;

    ~DBImpl() override;

 private:
    Status
    QuerySync(const std::string &table_id,
              uint64_t k,
              uint64_t nq,
              const float *vectors,
              const meta::DatesT &dates,
              QueryResults &results);

    Status
    QueryAsync(const std::string &table_id,
               const meta::TableFilesSchema &files,
               uint64_t k,
               uint64_t nq,
               const float *vectors,
               const meta::DatesT &dates,
               QueryResults &results);


    void StartTimerTasks();
    void BackgroundTimerTask();

    void StartMetricTask();

    void StartCompactionTask();
    Status MergeFiles(const std::string &table_id,
                      const meta::DateT &date,
                      const meta::TableFilesSchema &files);
    Status BackgroundMergeFiles(const std::string &table_id);
    void BackgroundCompaction(std::set<std::string> table_ids);

    void StartBuildIndexTask();
    void BackgroundBuildIndex();

    Status
    BuildIndex(const meta::TableFileSchema &);

 private:

    const Options options_;

    Status bg_error_;
    std::atomic<bool> shutting_down_;

    std::thread bg_timer_thread_;

    MetaPtr meta_ptr_;
    MemManagerPtr mem_mgr_;

    server::ThreadPool compact_thread_pool_;
    std::list<std::future<void>> compact_thread_results_;
    std::set<std::string> compact_table_ids_;

    server::ThreadPool index_thread_pool_;
    std::list<std::future<void>> index_thread_results_;

}; // DBImpl


} // namespace engine
} // namespace milvus
} // namespace zilliz
