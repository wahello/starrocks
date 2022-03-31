// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/test/olap/rowset/segment_v2/bitmap_index_test.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <gtest/gtest.h>

#include <string>

#include "env/env_memory.h"
#include "runtime/mem_pool.h"
#include "runtime/mem_tracker.h"
#include "storage/fs/file_block_manager.h"
#include "storage/key_coder.h"
#include "storage/olap_common.h"
#include "storage/rowset/bitmap_index_reader.h"
#include "storage/rowset/bitmap_index_writer.h"
#include "storage/types.h"

namespace starrocks {

class BitmapIndexTest : public testing::Test {
public:
    const std::string kTestDir = "/bitmap_index_test";

    BitmapIndexTest() {}

protected:
    void SetUp() override {
        StoragePageCache::create_global_cache(&_tracker, 1000000000);
        _env = std::make_shared<EnvMemory>();
        _block_mgr = std::make_shared<fs::FileBlockManager>(_env, fs::BlockManagerOptions());
        ASSERT_TRUE(_env->create_dir(kTestDir).ok());
    }
    void TearDown() override { StoragePageCache::release_global_cache(); }

    void get_bitmap_reader_iter(std::string& file_name, const ColumnIndexMetaPB& meta, BitmapIndexReader** reader,
                                BitmapIndexIterator** iter) {
        *reader = new BitmapIndexReader();
        auto st = (*reader)->load(_block_mgr.get(), file_name, &meta.bitmap_index(), true, false);
        ASSERT_TRUE(st.ok());

        st = (*reader)->new_iterator(iter);
        ASSERT_TRUE(st.ok());
    }

    template <FieldType type>
    void write_index_file(std::string& filename, const void* values, size_t value_count, size_t null_count,
                          ColumnIndexMetaPB* meta) {
        TypeInfoPtr type_info = get_type_info(type);
        {
            std::unique_ptr<fs::WritableBlock> wblock;
            fs::CreateBlockOptions opts({filename});
            ASSERT_TRUE(_block_mgr->create_block(opts, &wblock).ok());

            std::unique_ptr<BitmapIndexWriter> writer;
            BitmapIndexWriter::create(type_info, &writer);
            writer->add_values(values, value_count);
            writer->add_nulls(null_count);
            ASSERT_TRUE(writer->finish(wblock.get(), meta).ok());
            ASSERT_EQ(BITMAP_INDEX, meta->type());
            ASSERT_TRUE(wblock->close().ok());
        }
    }

    std::shared_ptr<EnvMemory> _env = nullptr;
    std::shared_ptr<fs::FileBlockManager> _block_mgr = nullptr;
    MemTracker _tracker;
    MemPool _pool;
};

TEST_F(BitmapIndexTest, test_invert) {
    size_t num_uint8_rows = 1024 * 10;
    int* val = new int[num_uint8_rows];
    for (int i = 0; i < num_uint8_rows; ++i) {
        val[i] = i;
    }

    std::string file_name = kTestDir + "/invert";
    ColumnIndexMetaPB meta;
    write_index_file<OLAP_FIELD_TYPE_INT>(file_name, val, num_uint8_rows, 0, &meta);
    {
        std::unique_ptr<RandomAccessFile> rfile;
        BitmapIndexReader* reader = nullptr;
        BitmapIndexIterator* iter = nullptr;
        get_bitmap_reader_iter(file_name, meta, &reader, &iter);

        int value = 2;
        bool exact_match;
        Status st = iter->seek_dictionary(&value, &exact_match);
        ASSERT_TRUE(st.ok());
        ASSERT_TRUE(exact_match);
        ASSERT_EQ(2, iter->current_ordinal());

        Roaring bitmap;
        iter->read_bitmap(iter->current_ordinal(), &bitmap);
        ASSERT_TRUE(Roaring::bitmapOf(1, 2) == bitmap);

        int value2 = 1024 * 9;
        st = iter->seek_dictionary(&value2, &exact_match);
        ASSERT_TRUE(st.ok());
        ASSERT_TRUE(exact_match);
        ASSERT_EQ(1024 * 9, iter->current_ordinal());

        iter->read_union_bitmap(iter->current_ordinal(), iter->bitmap_nums(), &bitmap);
        ASSERT_EQ(1025, bitmap.cardinality());

        int value3 = 1024;
        iter->seek_dictionary(&value3, &exact_match);
        ASSERT_EQ(1024, iter->current_ordinal());

        Roaring bitmap2;
        iter->read_union_bitmap(0, iter->current_ordinal(), &bitmap2);
        ASSERT_EQ(1024, bitmap2.cardinality());

        delete reader;
        delete iter;
    }
    delete[] val;
}

TEST_F(BitmapIndexTest, test_invert_2) {
    size_t num_uint8_rows = 1024 * 10;
    int* val = new int[num_uint8_rows];
    for (int i = 0; i < 1024; ++i) {
        val[i] = i;
    }

    for (int i = 1024; i < num_uint8_rows; ++i) {
        val[i] = i * 10;
    }

    std::string file_name = kTestDir + "/invert2";
    ColumnIndexMetaPB meta;
    write_index_file<OLAP_FIELD_TYPE_INT>(file_name, val, num_uint8_rows, 0, &meta);

    {
        BitmapIndexReader* reader = nullptr;
        BitmapIndexIterator* iter = nullptr;
        get_bitmap_reader_iter(file_name, meta, &reader, &iter);

        int value = 1026;
        bool exact_match;
        auto st = iter->seek_dictionary(&value, &exact_match);
        ASSERT_TRUE(st.ok());
        ASSERT_TRUE(!exact_match);

        ASSERT_EQ(1024, iter->current_ordinal());

        Roaring bitmap;
        iter->read_union_bitmap(0, iter->current_ordinal(), &bitmap);
        ASSERT_EQ(1024, bitmap.cardinality());

        delete reader;
        delete iter;
    }
    delete[] val;
}

TEST_F(BitmapIndexTest, test_multi_pages) {
    size_t num_uint8_rows = 1024 * 1024;
    int64_t* val = new int64_t[num_uint8_rows];
    for (int i = 0; i < num_uint8_rows; ++i) {
        val[i] = random() + 10000;
    }
    val[1024 * 510] = 2019;

    std::string file_name = kTestDir + "/mul";
    ColumnIndexMetaPB meta;
    write_index_file<OLAP_FIELD_TYPE_BIGINT>(file_name, val, num_uint8_rows, 0, &meta);
    {
        BitmapIndexReader* reader = nullptr;
        BitmapIndexIterator* iter = nullptr;
        get_bitmap_reader_iter(file_name, meta, &reader, &iter);

        int64_t value = 2019;
        bool exact_match;
        auto st = iter->seek_dictionary(&value, &exact_match);
        ASSERT_TRUE(st.ok()) << "status:" << st.to_string();
        ASSERT_EQ(0, iter->current_ordinal());

        Roaring bitmap;
        iter->read_bitmap(iter->current_ordinal(), &bitmap);
        ASSERT_EQ(1, bitmap.cardinality());

        delete reader;
        delete iter;
    }
    delete[] val;
}

TEST_F(BitmapIndexTest, test_null) {
    size_t num_uint8_rows = 1024;
    int64_t* val = new int64_t[num_uint8_rows];
    for (int i = 0; i < num_uint8_rows; ++i) {
        val[i] = i;
    }

    std::string file_name = kTestDir + "/null";
    ColumnIndexMetaPB meta;
    write_index_file<OLAP_FIELD_TYPE_BIGINT>(file_name, val, num_uint8_rows, 30, &meta);
    {
        BitmapIndexReader* reader = nullptr;
        BitmapIndexIterator* iter = nullptr;
        get_bitmap_reader_iter(file_name, meta, &reader, &iter);

        Roaring bitmap;
        iter->read_null_bitmap(&bitmap);
        ASSERT_EQ(30, bitmap.cardinality());

        delete reader;
        delete iter;
    }
    delete[] val;
}

} // namespace starrocks