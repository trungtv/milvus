#include <assert.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <cstring>
#include <wrapper/Topk.h>
#include <easylogging++.h>
#include <cache/CpuCacheMgr.h>

#include "DBImpl.h"
#include "DBMetaImpl.h"
#include "Env.h"
#include "FaissExecutionEngine.h"

namespace zilliz {
namespace vecwise {
namespace engine {

DBImpl::DBImpl(const Options& options)
    : _env(options.env),
      _options(options),
      _bg_compaction_scheduled(false),
      _shutting_down(false),
      bg_build_index_started_(false),
      _pMeta(new meta::DBMetaImpl(_options.meta)),
      _pMemMgr(new MemManager(_pMeta, _options)) {
    start_timer_task(_options.memory_sync_interval);
}

Status DBImpl::add_group(meta::GroupSchema& group_info) {
    return _pMeta->add_group(group_info);
}

Status DBImpl::get_group(meta::GroupSchema& group_info) {
    return _pMeta->get_group(group_info);
}

Status DBImpl::has_group(const std::string& group_id_, bool& has_or_not_) {
    return _pMeta->has_group(group_id_, has_or_not_);
}

Status DBImpl::get_group_files(const std::string& group_id,
                               const int date_delta,
                               meta::GroupFilesSchema& group_files_info) {
    return _pMeta->get_group_files(group_id, date_delta, group_files_info);

}

Status DBImpl::add_vectors(const std::string& group_id_,
        size_t n, const float* vectors, IDNumbers& vector_ids_) {
    Status status = _pMemMgr->add_vectors(group_id_, n, vectors, vector_ids_);
    if (!status.ok()) {
        return status;
    }
}

Status DBImpl::search(const std::string &group_id, size_t k, size_t nq,
                      const float *vectors, QueryResults &results) {
    meta::DatesT dates = {meta::Meta::GetDate()};
    return search(group_id, k, nq, vectors, dates, results);
}

Status DBImpl::search(const std::string& group_id, size_t k, size_t nq,
        const float* vectors, const meta::DatesT& dates, QueryResults& results) {

    meta::DatePartionedGroupFilesSchema files;
    auto status = _pMeta->files_to_search(group_id, dates, files);
    if (!status.ok()) { return status; }

    /* LOG(DEBUG) << "Search DateT Size=" << files.size(); */

    meta::GroupFilesSchema index_files;
    meta::GroupFilesSchema raw_files;
    for (auto &day_files : files) {
        for (auto &file : day_files.second) {
            file.file_type == meta::GroupFileSchema::INDEX ?
            index_files.push_back(file) : raw_files.push_back(file);
        }
    }

    int dim = 0;
    if (!index_files.empty()) {
        dim = index_files[0].dimension;
    } else if (!raw_files.empty()) {
        dim = raw_files[0].dimension;
    } else {
        return Status::OK();
    }

    {
        // [{ids, distence}, ...]
        using SearchResult = std::pair<std::vector<long>, std::vector<float>>;
        std::vector<SearchResult> batchresult(nq); // allocate nq cells.

        auto cluster = [&](long *nns, float *dis) -> void {
            for (int i = 0; i < nq; ++i) {
                auto f_begin = batchresult[i].first.cbegin();
                auto s_begin = batchresult[i].second.cbegin();
                batchresult[i].first.insert(f_begin, nns + i * k, nns + i * k + k);
                batchresult[i].second.insert(s_begin, dis + i * k, dis + i * k + k);
            }
        };

        // Allocate Memory
        float *output_distence;
        long *output_ids;
        output_distence = (float *) malloc(k * nq * sizeof(float));
        output_ids = (long *) malloc(k * nq * sizeof(long));
        memset(output_distence, 0, k * nq * sizeof(float));
        memset(output_ids, 0, k * nq * sizeof(long));

        long search_set_size = 0;

        auto search_in_index = [&](meta::GroupFilesSchema& file_vec) -> void {
            for (auto &file : file_vec) {
                FaissExecutionEngine index(file.dimension, file.location);
                index.Load();
                auto file_size = index.PhysicalSize()/(1024*1024);
                search_set_size += file_size;
                LOG(DEBUG) << "Search file_type " << file.file_type << " Of Size: "
                    << file_size << " M";
                index.Search(nq, vectors, k, output_distence, output_ids);
                cluster(output_ids, output_distence); // cluster to each query
                memset(output_distence, 0, k * nq * sizeof(float));
                memset(output_ids, 0, k * nq * sizeof(long));
            }
        };

        auto cluster_topk = [&]() -> void {
            QueryResult res;
            for (auto &result_pair : batchresult) {
                auto &dis = result_pair.second;
                auto &nns = result_pair.first;
                TopK(dis.data(), dis.size(), k, output_distence, output_ids);
                for (int i = 0; i < k; ++i) {
                    res.emplace_back(nns[output_ids[i]]); // mapping
                }
                results.push_back(res); // append to result list
                res.clear();
                memset(output_distence, 0, k * nq * sizeof(float));
                memset(output_ids, 0, k * nq * sizeof(long));
            }
        };

        search_in_index(raw_files);
        search_in_index(index_files);

        LOG(DEBUG) << "Search Overall Set Size=" << search_set_size << " M";
        cluster_topk();

        free(output_distence);
        free(output_ids);
    }

    if (results.empty()) {
        return Status::NotFound("Group " + group_id + ", search result not found!");
    }
    return Status::OK();
}

void DBImpl::start_timer_task(int interval_) {
    std::thread bg_task(&DBImpl::background_timer_task, this, interval_);
    bg_task.detach();
}

void DBImpl::background_timer_task(int interval_) {
    Status status;
    while (true) {
        if (!_bg_error.ok()) break;
        if (_shutting_down.load(std::memory_order_acquire)) break;

        std::this_thread::sleep_for(std::chrono::seconds(interval_));

        try_schedule_compaction();
    }
}

void DBImpl::try_schedule_compaction() {
    if (_bg_compaction_scheduled) return;
    if (!_bg_error.ok()) return;

    _bg_compaction_scheduled = true;
    _env->schedule(&DBImpl::BGWork, this);
}

void DBImpl::BGWork(void* db_) {
    reinterpret_cast<DBImpl*>(db_)->background_call();
}

void DBImpl::background_call() {
    std::lock_guard<std::mutex> lock(_mutex);
    assert(_bg_compaction_scheduled);

    if (!_bg_error.ok() || _shutting_down.load(std::memory_order_acquire))
        return ;

    background_compaction();

    _bg_compaction_scheduled = false;
    _bg_work_finish_signal.notify_all();
}


Status DBImpl::merge_files(const std::string& group_id, const meta::DateT& date,
        const meta::GroupFilesSchema& files) {
    meta::GroupFileSchema group_file;
    group_file.group_id = group_id;
    group_file.date = date;
    Status status = _pMeta->add_group_file(group_file);

    if (!status.ok()) {
        LOG(INFO) << status.ToString() << std::endl;
        return status;
    }

    std::shared_ptr<ExecutionEngine> execution_engine(
            new FaissExecutionEngine(group_file.dimension, group_file.location));

    meta::GroupFilesSchema updated;
    long  index_size = 0;

    for (auto& file : files) {
        execution_engine->Merge(file.location);
        auto file_schema = file;
        file_schema.file_type = meta::GroupFileSchema::TO_DELETE;
        updated.push_back(file_schema);
        /* LOG(DEBUG) << "About to merge file " << file_schema.file_id << */
        /*     " of size=" << file_schema.rows; */
        index_size = execution_engine->Size();

        if (index_size >= _options.index_trigger_size) break;
    }

    execution_engine->Serialize();

    if (index_size >= _options.index_trigger_size) {
        group_file.file_type = meta::GroupFileSchema::TO_INDEX;
    } else {
        group_file.file_type = meta::GroupFileSchema::RAW;
    }
    group_file.rows = index_size;
    updated.push_back(group_file);
    status = _pMeta->update_files(updated);
    /* LOG(DEBUG) << "New merged file " << group_file.file_id << */
    /*     " of size=" << group_file.rows; */

    execution_engine->Cache();

    return status;
}

Status DBImpl::background_merge_files(const std::string& group_id) {
    meta::DatePartionedGroupFilesSchema raw_files;
    auto status = _pMeta->files_to_merge(group_id, raw_files);
    if (!status.ok()) {
        return status;
    }

    /* if (raw_files.size() == 0) { */
    /*     return Status::OK(); */
    /* } */

    bool has_merge = false;

    for (auto& kv : raw_files) {
        auto files = kv.second;
        if (files.size() <= _options.merge_trigger_number) {
            continue;
        }
        has_merge = true;
        merge_files(group_id, kv.first, kv.second);
    }

    try_build_index();

    _pMeta->cleanup_ttl_files(1);

    return Status::OK();
}

Status DBImpl::build_index(const meta::GroupFileSchema& file) {
    meta::GroupFileSchema group_file;
    group_file.group_id = file.group_id;
    group_file.date = file.date;
    Status status = _pMeta->add_group_file(group_file);
    if (!status.ok()) {
        return status;
    }

    FaissExecutionEngine to_index(file.dimension, file.location);

    to_index.Load();
    auto index = to_index.BuildIndex(group_file.location);

    group_file.file_type = meta::GroupFileSchema::INDEX;
    group_file.rows = index->Size();

    auto to_remove = file;
    to_remove.file_type = meta::GroupFileSchema::TO_DELETE;

    meta::GroupFilesSchema update_files = {to_remove, group_file};
    _pMeta->update_files(update_files);

    index->Cache();

    return Status::OK();
}

void DBImpl::background_build_index() {
    std::lock_guard<std::mutex> lock(build_index_mutex_);
    assert(bg_build_index_started_);
    meta::GroupFilesSchema to_index_files;
    _pMeta->files_to_index(to_index_files);
    Status status;
    for (auto& file : to_index_files) {
        /* LOG(DEBUG) << "Buiding index for " << file.location; */
        status = build_index(file);
        if (!status.ok()) {
            _bg_error = status;
            return;
        }
    }
    /* LOG(DEBUG) << "All Buiding index Done"; */

    bg_build_index_started_ = false;
    bg_build_index_finish_signal_.notify_all();
}

Status DBImpl::try_build_index() {
    if (bg_build_index_started_) return Status::OK();
    if (_shutting_down.load(std::memory_order_acquire)) return Status::OK();
    bg_build_index_started_ = true;
    std::thread build_index_task(&DBImpl::background_build_index, this);
    build_index_task.detach();
    return Status::OK();
}

void DBImpl::background_compaction() {
    std::vector<std::string> group_ids;
    _pMemMgr->serialize(group_ids);

    Status status;
    for (auto group_id : group_ids) {
        status = background_merge_files(group_id);
        if (!status.ok()) {
            _bg_error = status;
            return;
        }
    }
}

Status DBImpl::drop_all() {
    return _pMeta->drop_all();
}

Status DBImpl::count(const std::string& group_id, long& result) {
    return _pMeta->count(group_id, result);
}

DBImpl::~DBImpl() {
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _shutting_down.store(true, std::memory_order_release);
        while (_bg_compaction_scheduled) {
            _bg_work_finish_signal.wait(lock);
        }
    }
    {
        std::unique_lock<std::mutex> lock(build_index_mutex_);
        while (bg_build_index_started_) {
            bg_build_index_finish_signal_.wait(lock);
        }
    }
    std::vector<std::string> ids;
    _pMemMgr->serialize(ids);
}

/*
 *  DB
 */

DB::~DB() {}

void DB::Open(const Options& options, DB** dbptr) {
    *dbptr = nullptr;
    *dbptr = new DBImpl(options);
    return;
}

} // namespace engine
} // namespace vecwise
} // namespace zilliz
