// Copyright [2017] <eliotwang>
#include <memory>
#include <utility>
#include <thread>
#include "gtest/gtest.h"
#include "glog/logging.h"
#include "tendisplus/server/server_entry.h"
#include "tendisplus/server/index_manager.h"
#include "tendisplus/server/segment_manager.h"
#include "tendisplus/commands/command.h"
#include "tendisplus/network/network.h"
#include "tendisplus/utils/test_util.h"
#include "tendisplus/utils/scopeguard.h"
#include "tendisplus/utils/sync_point.h"
#include "tendisplus/utils/invariant.h"

namespace tendisplus {

std::string master_dir = "repltest_master";
std::string slave_dir = "repltest_slave";
std::string slave1_dir = "repltest_slave1";
std::string slave2_dir = "repltest_slave2";
uint32_t master_port = 1111;
uint32_t slave_port = 1112;
uint32_t slave1_port = 2111;
uint32_t slave2_port = 2112;

AllKeys initData(std::shared_ptr<ServerEntry>& server,
                uint32_t count) {
    auto ctx1 = std::make_shared<asio::io_context>();
    auto sess1 = makeSession(server, ctx1);
    WorkLoad work(server, sess1);
    work.init();

    AllKeys all_keys;

    auto kv_keys = work.writeWork(RecordType::RT_KV, count);
    all_keys.emplace_back(kv_keys);

    auto list_keys = work.writeWork(RecordType::RT_LIST_META, count, 50);
    all_keys.emplace_back(list_keys);

//#ifdef _WIN32
    work.flush();
//#endif

    auto hash_keys = work.writeWork(RecordType::RT_HASH_META, count, 50);
    all_keys.emplace_back(hash_keys);

    auto set_keys = work.writeWork(RecordType::RT_SET_META, count, 50);
    all_keys.emplace_back(set_keys);

    auto zset_keys = work.writeWork(RecordType::RT_ZSET_META, count, 50);
    all_keys.emplace_back(zset_keys);

    return std::move(all_keys);
}

void waitSlaveCatchup(const std::shared_ptr<ServerEntry>& master,
    const std::shared_ptr<ServerEntry>& slave) {
    auto ctx1 = std::make_shared<asio::io_context>();
    auto sess1 = makeSession(master, ctx1);
    WorkLoad work1(master, sess1);
    work1.init();

    auto ctx2 = std::make_shared<asio::io_context>();
    auto sess2 = makeSession(slave, ctx2);
    WorkLoad work2(master, sess2);
    work2.init();

    INVARIANT(master->getKVStoreCount() == slave->getKVStoreCount());

    for (size_t i = 0; i < master->getKVStoreCount(); i++) {
        auto binlogPos1 = work1.getIntResult({ "binlogpos", std::to_string(i) });
        while (true) {
            auto binlogPos2 = work2.getIntResult({ "binlogpos", std::to_string(i) });
            if (!binlogPos2.ok()) {
                EXPECT_TRUE(binlogPos2.status().code() == ErrorCodes::ERR_EXHAUST);
                EXPECT_TRUE(binlogPos1.status().code() == ErrorCodes::ERR_EXHAUST);
                break;
            }
            if (binlogPos2.value() < binlogPos1.value()) {
                LOG(WARNING) << "store id " << i << " : binlogpos (" << binlogPos1.value()
                    << ">" << binlogPos2.value() << ");";
                std::this_thread::sleep_for(std::chrono::seconds(1));
            } else {
                EXPECT_EQ(binlogPos1.value(), binlogPos2.value());
                break;
            }
        }
    }
}

void compareData(const std::shared_ptr<ServerEntry>& master,
    const std::shared_ptr<ServerEntry>& slave) {
    INVARIANT(master->getKVStoreCount() == slave->getKVStoreCount());

    for (size_t i = 0; i < master->getKVStoreCount(); i++) {
        uint64_t count1 = 0;
        uint64_t count2 = 0;
        auto kvstore1 = master->getStores()[i];
        auto kvstore2 = slave->getStores()[i];

        auto ptxn2 = kvstore2->createTransaction(nullptr);
        EXPECT_TRUE(ptxn2.ok());
        std::unique_ptr<Transaction> txn2 = std::move(ptxn2.value());

        auto ptxn1 = kvstore1->createTransaction(nullptr);
        EXPECT_TRUE(ptxn1.ok());
        std::unique_ptr<Transaction> txn1 = std::move(ptxn1.value());
        auto cursor1 = txn1->createCursor();
        cursor1->seek("");
        while (true) {
            Expected<Record> exptRcd1 = cursor1->next();
            if (exptRcd1.status().code() == ErrorCodes::ERR_EXHAUST) {
                break;
            }
            INVARIANT(exptRcd1.ok());
            count1++;

            // check the binlog together
            auto exptRcdv2 = kvstore2->getKV(exptRcd1.value().getRecordKey(), txn2.get());
            EXPECT_TRUE(exptRcdv2.ok());
            EXPECT_EQ(exptRcd1.value().getRecordValue(), exptRcdv2.value());
        }

        auto cursor2 = txn2->createCursor();
        cursor2->seek("");
        while (true) {
            Expected<Record> exptRcd2 = cursor2->next();
            if (exptRcd2.status().code() == ErrorCodes::ERR_EXHAUST) {
                break;
            }
            INVARIANT(exptRcd2.ok());
            count2++;
        }

        EXPECT_EQ(count1, count2);
        LOG(INFO) << "compare data: store " << i << " record count " << count1;
    }
}

std::pair<std::shared_ptr<ServerEntry>, std::shared_ptr<ServerEntry>> 
makeReplEnv(uint32_t storeCnt) {
    EXPECT_TRUE(setupEnv(master_dir));
    EXPECT_TRUE(setupEnv(slave_dir));

    auto cfg1 = makeServerParam(master_port, storeCnt, master_dir);
    auto cfg2 = makeServerParam(slave_port, storeCnt, slave_dir);

    auto master = std::make_shared<ServerEntry>(cfg1);
    auto s = master->startup(cfg1);
    INVARIANT(s.ok());

    auto slave = std::make_shared<ServerEntry>(cfg2);
    s = slave->startup(cfg2);
    INVARIANT(s.ok());
    {
        auto ctx = std::make_shared<asio::io_context>();
        auto session = makeSession(slave, ctx);

        WorkLoad work(slave, session);
        work.init();
        work.slaveof("127.0.0.1", master_port);
    }

    return std::make_pair(master, slave);
}

std::shared_ptr<ServerEntry>
makeAnotherSlave(const std::string& name, uint32_t storeCnt, uint32_t port) {
    INVARIANT(name != master_dir && name != slave_dir);
	INVARIANT(port != master_port && port != slave_port);
    EXPECT_TRUE(setupEnv(name));

    auto cfg1 = makeServerParam(port, storeCnt, name);

    auto slave = std::make_shared<ServerEntry>(cfg1);
    auto s = slave->startup(cfg1);
    INVARIANT(s.ok());

    {
        auto ctx = std::make_shared<asio::io_context>();
        auto session = makeSession(slave, ctx);

        WorkLoad work(slave, session);
        work.init();
        work.slaveof("127.0.0.1", master_port);
    }

    return slave;
}

#ifdef _WIN32 
size_t recordSize = 10;
#else
size_t recordSize = 1000;
#endif // 

TEST(Repl, Common) {
#ifdef _WIN32
    size_t i = 0;
    {
#else
    for(size_t i = 0; i<9; i++) {
#endif
        LOG(INFO) << ">>>>>> test store count:" << i;
        const auto guard = MakeGuard([] {
                destroyReplEnv();
                destroyEnv(slave1_dir);
                destroyEnv(slave2_dir);
                std::this_thread::sleep_for(std::chrono::seconds(5));
                });

        auto hosts = makeReplEnv(i);

        auto& master = hosts.first;
        auto& slave = hosts.second;

        // make sure slaveof is ok
        std::this_thread::sleep_for(std::chrono::seconds(5));
        auto ctx1 = std::make_shared<asio::io_context>();
        auto sess1 = makeSession(master, ctx1);
        WorkLoad work(master, sess1);
        work.init();
        work.flush();

        auto allKeys = initData(master, recordSize);

        waitSlaveCatchup(master, slave);
        compareData(master, slave);

        auto slave1 = makeAnotherSlave(slave1_dir, i, slave1_port);

        // delete all the keys
        for (auto k : allKeys) {
            if (genRand() % 4 == 0) {
                work.delKeys(k);
            }
        }

        std::thread thd1([&master]() {
#ifndef _WIN32
            testAll(master);
#endif
        });

        std::thread thd2([&master]() {
#ifndef _WIN32
            std::this_thread::sleep_for(std::chrono::seconds(genRand() % 50));
            auto ctx1 = std::make_shared<asio::io_context>();
            auto sess1 = makeSession(master, ctx1);
            WorkLoad work(master, sess1);
            work.init();
            //work.flush();
#endif
        });

        std::this_thread::sleep_for(std::chrono::seconds(genRand() % 10 + 5));
        auto slave2 = makeAnotherSlave(slave2_dir, i, slave2_port);

        LOG(INFO) << "waiting thd1 to exited";
        thd1.join();
        thd2.join();

        waitSlaveCatchup(master, slave);
        compareData(master, slave);

        waitSlaveCatchup(master, slave1);
        compareData(master, slave1);

        waitSlaveCatchup(master, slave2);
        compareData(master, slave2);

#ifndef _WIN32
        master->stop();
        slave->stop();
        slave1->stop();
        slave2->stop();

        ASSERT_EQ(slave.use_count(), 1);
        //ASSERT_EQ(master.use_count(), 1);
        ASSERT_EQ(slave1.use_count(), 1);
        ASSERT_EQ(slave2.use_count(), 1);
#endif

        LOG(INFO) << ">>>>>> test store count:" << i << " end;";
    }
}

}  // namespace tendisplus
