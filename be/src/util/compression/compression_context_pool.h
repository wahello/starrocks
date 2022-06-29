// This file is made available under Elastic License 2.0.
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <memory>
#include <shared_mutex>

#include "common/status.h"
#include "common/statusor.h"
#include "util/starrocks_metrics.h"

namespace starrocks::compression {

/**
 * A compression context pool preserves many unused compression context.
 *
 * The caller can apply a compression context from the pool through `get()` and then use it.
 * When the compression context is finished being used, it will automatically call the delete
 * function of unique_ptr `ReturnToPoolDeleter` and then return the context to the pool.
 *
 * Under normal circumstances, compression context will not be cleaned up, if you want to free
 * all the context in the pool, you can call `flush_deep()`.
 */
template <typename T, typename Creator, typename Deleter, typename Resetter>
class CompressionContextPool {
private:
    using InternalRef = std::unique_ptr<T, Deleter>;

    class ReturnToPoolDeleter {
    public:
        using Pool = CompressionContextPool<T, Creator, Deleter, Resetter>;

        explicit ReturnToPoolDeleter(Pool* pool) : _pool(pool) { DCHECK(pool); }

        void operator()(T* t) {
            InternalRef ptr(t, _pool->_deleter);
            _pool->add(std::move(ptr));
        }

    private:
        Pool* _pool;
    };

public:
    using Object = T;
    using Ref = std::unique_ptr<T, ReturnToPoolDeleter>;

    explicit CompressionContextPool(const std::string& pool_name, Creator creator = Creator(),
                                    Deleter deleter = Deleter(), Resetter resetter = Resetter())
            : _creator(std::move(creator)),
              _deleter(std::move(deleter)),
              _resetter(std::move(resetter)),
              _stack(),
              _created_counter(0) {
        auto metrics = StarRocksMetrics::instance()->metrics();
        std::string full_name = pool_name + "_context_pool_create_count";
        _created_counter_metrics = std::make_unique<UIntGauge>(MetricUnit::NOUNIT);
        metrics->register_metric(full_name, _created_counter_metrics.get());
        metrics->register_hook(full_name, [this]() { _created_counter_metrics->set_value(_created_counter.load()); });
    }

    StatusOr<Ref> get() {
        std::lock_guard<std::mutex> l(_stack_lock);
        if (_stack.empty()) {
            ASSIGN_OR_RETURN(T * t, _creator());
            _created_counter++;
            return Ref(t, get_deleter());
        }
        auto ptr = std::move(_stack.back());
        _stack.pop_back();
        DCHECK(ptr);
        return Ref(ptr.release(), get_deleter());
    }

    size_t created_count() const { return _created_counter.load(); }

    ReturnToPoolDeleter get_deleter() { return ReturnToPoolDeleter(this); }

    Resetter& get_resetter() { return _resetter; }

    void flush_deep() {
        flush_shallow();
        // no backing stack, so deep == shallow
    }

    void flush_shallow() {
        std::lock_guard<std::mutex> l(_stack_lock);
        _stack->resize(0);
    }

private:
    void add(InternalRef ptr) {
        DCHECK(ptr);
        _resetter(ptr.get());
        Status status = _resetter(ptr.get());
        // if reset fail, then delete this context
        if (!status.ok()) {
            return;
        }

        std::lock_guard<std::mutex> l(_stack_lock);
        _stack.push_back(std::move(ptr));
    }

    Creator _creator;
    Deleter _deleter;
    Resetter _resetter;

    std::mutex _stack_lock;
    std::vector<InternalRef> _stack;

    std::unique_ptr<UIntGauge> _created_counter_metrics;
    std::atomic<size_t> _created_counter;
};
} // namespace starrocks::compression