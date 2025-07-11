// Copyright(C) 2023 InfiniFlow, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <numeric>
#include <random>

#include "gtest/gtest.h"
import base_test;
import stl;
import storage;
import global_resource_usage;
import infinity_context;
import status;
import txn;
import buffer_manager;
import txn_manager;
import column_vector;

import table_def;
import value;
import physical_import;
import default_values;
import infinity_exception;
import base_table_ref;
import logical_type;
import internal_types;
import extra_ddl_info;
import column_def;
import data_type;
import data_block;
import segment_entry;
import block_entry;
import logger;
import txn_state;

using namespace infinity;

class SealingTaskTest : public BaseTestParamStr {
protected:
    void AppendBlocks(TxnManager *txn_mgr, const String &table_name, u32 row_cnt_input, BufferManager *buffer_mgr) {
        auto *txn = txn_mgr->BeginTxn(MakeUnique<String>("import blocks"), TransactionType::kNormal);
        auto [table_entry, status] = txn->GetTableByName("default_db", table_name);
        auto column_count = table_entry->ColumnCount();
        EXPECT_EQ(column_count, u32(1));
        while (row_cnt_input > 0) {
            SizeT write_size = std::min<SizeT>(SizeT(DEFAULT_BLOCK_CAPACITY), row_cnt_input);
            row_cnt_input -= write_size;
            Vector<SharedPtr<ColumnVector>> column_vectors;
            {
                auto column_vector = ColumnVector::Make(MakeShared<DataType>(LogicalType::kTinyInt));
                column_vector->Initialize();

                for (int i = 0; i < (int)write_size; ++i) {
                    Value v = Value::MakeTinyInt(static_cast<TinyIntT>(i % 127));
                    column_vector->AppendValue(v);
                }
                column_vectors.push_back(std::move(column_vector));
            }
            SharedPtr<DataBlock> block = DataBlock::Make();
            block->Init(column_vectors);
            txn->Append("default_db", table_name, block);
        }
        txn_mgr->CommitTxn(txn);
    }
};

INSTANTIATE_TEST_SUITE_P(TestWithDifferentParams, SealingTaskTest, ::testing::Values(BaseTestParamStr::NULL_CONFIG_PATH));

TEST_P(SealingTaskTest, append_unsealed_segment_sealed) {
    GTEST_SKIP() << "Skipping slow test.";
    {
        String table_name = "tbl1";
        Storage *storage = infinity::InfinityContext::instance().storage();
        BufferManager *buffer_manager = storage->buffer_manager();
        TxnManager *txn_mgr = storage->txn_manager();

        Vector<SharedPtr<ColumnDef>> columns;
        {
            i64 column_id = 0;
            {
                std::set<ConstraintType> constraints;
                auto column_def_ptr =
                    MakeShared<ColumnDef>(column_id++, MakeShared<DataType>(DataType(LogicalType::kTinyInt)), "tiny_int_col", constraints);
                columns.emplace_back(column_def_ptr);
            }
        }
        {
            // create table
            auto tbl1_def = MakeUnique<TableDef>(MakeShared<String>("default_db"), MakeShared<String>(table_name), MakeShared<String>(), columns);
            auto *txn = txn_mgr->BeginTxn(MakeUnique<String>("create table"), TransactionType::kNormal);

            Status status = txn->CreateTable("default_db", std::move(tbl1_def), ConflictType::kIgnore);
            EXPECT_TRUE(status.ok());

            txn_mgr->CommitTxn(txn);
        }
        u32 input_row_cnt = 8'192 * 1'024 * 3 + 1; // one full segment
        this->AppendBlocks(txn_mgr, table_name, input_row_cnt, buffer_manager);
        {
            auto txn = txn_mgr->BeginTxn(MakeUnique<String>("get table"), TransactionType::kRead);
            auto [table_entry, status] = txn->GetTableByName("default_db", table_name);
            EXPECT_NE(table_entry, nullptr);
            // 8'192 * 1'024 * 3 + 1
            int unsealed_cnt = 0, sealed_cnt = 0;
            auto &mp = table_entry->segment_map();
            for (auto &[_, seg] : mp) {
                switch (seg->status()) {
                    case SegmentStatus::kUnsealed:
                        unsealed_cnt++;
                        break;
                    case SegmentStatus::kSealed:
                        sealed_cnt++;
                        break;
                    default: {
                        String error_message = "Invalid segment status";
                        UnrecoverableError(error_message);
                    }
                }
            }
            EXPECT_EQ(unsealed_cnt, 1);
            EXPECT_EQ(sealed_cnt, 3);
            txn_mgr->CommitTxn(txn);
        }
    }
    /*
    ////////////////////////////////
    /// Restart the db instance...
    ////////////////////////////////
    system(tree_cmd.c_str());
    {
        // test wal
        String table_name = "tbl1";
        infinity::GlobalResourceUsage::Init();
        std::shared_ptr<std::string> config_path = nullptr;
        RemoveDbDirs();
        infinity::InfinityContext::instance().InitPhase1(config_path);
        infinity::InfinityContext::instance().InitPhase2();
        Storage *storage = infinity::InfinityContext::instance().storage();
        // BufferManager *buffer_manager = storage->buffer_manager();
        TxnManager *txn_mgr = storage->txn_manager();
        {
            auto txn = txn_mgr->BeginTxn();
            auto [table_entry, status] = txn->GetTableEntry("default_db", table_name);
            EXPECT_NE(table_entry, nullptr);
            // 8'192 * 1'024 * 3 + 1
            int unsealed_cnt = 0, sealed_cnt = 0;
            auto &mp = table_entry->segment_map();
            for (auto &[_, seg] : mp) {
                switch (seg->status()) {
                    case SegmentStatus::kUnsealed:
                        unsealed_cnt++;
                        break;
                    case SegmentStatus::kSealed:
                        sealed_cnt++;
                        break;
                    default:
                        UnrecoverableError("Invalid segment status");
                }
            }
            EXPECT_EQ(unsealed_cnt, 1);
            EXPECT_EQ(sealed_cnt, 3);
            txn_mgr->CommitTxn(txn);
        }
        infinity::InfinityContext::instance().UnInit();
        EXPECT_EQ(infinity::GlobalResourceUsage::GetObjectCount(), 0);
        EXPECT_EQ(infinity::GlobalResourceUsage::GetRawMemoryCount(), 0);
        infinity::GlobalResourceUsage::UnInit();
    }
    */
}
