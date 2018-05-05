#include <iostream>

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Main

#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <regex>

#include <apr.h>
#include <sqlite3.h>

#include "akumuli.h"
#include "storage_engine/input_log.h"
#include "log_iface.h"
#include "status_util.h"
#include "util.h"


using namespace Akumuli;

void test_logger(aku_LogLevel tag, const char* msg) {
    AKU_UNUSED(tag);
    BOOST_TEST_MESSAGE(msg);
}

bool volume_filename_is_ok(std::string name) {
    static const char* exp = "inputlog\\d+_\\d+\\.ils";
    static const std::regex regex(exp);
    return std::regex_match(name, regex);
}

struct AprInitializer {
    AprInitializer() {
        apr_initialize();
        Logger::set_logger(&test_logger);
    }
};

static AprInitializer initializer;
static LogSequencer sequencer;

BOOST_AUTO_TEST_CASE(Test_input_roundtrip) {
    std::vector<std::tuple<u64, u64, double>> exp, act;
    std::vector<u64> stale_ids;
    {
        InputLog ilog(&sequencer, "./", 100, 4096, 0);
        for (int i = 0; i < 10000; i++) {
            double val = static_cast<double>(rand()) / RAND_MAX;
            aku_Status status = ilog.append(42, i, val, &stale_ids);
            exp.push_back(std::make_tuple(42, i, val));
            if (status == AKU_EOVERFLOW) {
                ilog.rotate();
            }
        }
    }
    BOOST_REQUIRE(stale_ids.empty());
    {
        InputLog ilog("./", 0);
        while(true) {
            u64 ids[1024];
            u64 tss[1024];
            double xss[1024];
            aku_Status status;
            u32 outsz;
            std::tie(status, outsz) = ilog.read_next(1024, ids, tss, xss);
            BOOST_REQUIRE_EQUAL(status, AKU_SUCCESS);
            for(u32 i = 0; i < outsz; i++) {
                act.push_back(std::make_tuple(ids[i], tss[i], xss[i]));
            }
            if (outsz == 0) {
                break;
            }
        }
        ilog.reopen();
        ilog.delete_files();
    }
    BOOST_REQUIRE_EQUAL(exp.size(), act.size());
    for (u32 i = 0; i < exp.size(); i++) {
        BOOST_REQUIRE_EQUAL(std::get<0>(exp.at(i)), std::get<0>(act.at(i)));
        BOOST_REQUIRE_EQUAL(std::get<1>(exp.at(i)), std::get<1>(act.at(i)));
        BOOST_REQUIRE_EQUAL(std::get<2>(exp.at(i)), std::get<2>(act.at(i)));
    }
}

BOOST_AUTO_TEST_CASE(Test_input_rotation) {
    u32 N = 10;
    InputLog ilog(&sequencer, "./", N, 4096, 0);

    // This amount of data should saturate the log (random data is not
    // very compressable).
    std::vector<u64> stale_ids;
    for (int i = 0; i < 10000; i++) {
        double val = static_cast<double>(rand()) / RAND_MAX;
        aku_Status status = ilog.append(42, i, val, &stale_ids);
        if (status == AKU_EOVERFLOW) {
            ilog.rotate();
        }
    }

    // Check number of files (should be N)
    std::vector<std::string> names;
    for (auto it = boost::filesystem::directory_iterator("./");
         it != boost::filesystem::directory_iterator(); it++) {
        boost::filesystem::path path = *it;
        if (!boost::starts_with(path.filename().string(), "inputlog")) {
            continue;
        }
        if (path.extension().string() != ".ils") {
            continue;
        }
        names.push_back(path.filename().string());
    }

    BOOST_REQUIRE_EQUAL(names.size(), N);

    for (auto name: names) {
        BOOST_REQUIRE(volume_filename_is_ok(name));
    }
}


BOOST_AUTO_TEST_CASE(Test_input_volume_read_next_frame) {
    std::vector<std::tuple<u64, u64, double>> exp, act;
    const char* filename = "./tmp_test_vol.ilog";
    {
        LZ4Volume volume(&sequencer, filename, 0x10000);
        for (int i = 0; i < 10000; i++) {
            double val = static_cast<double>(rand()) / RAND_MAX;
            aku_Status status = volume.append(42, i, val);
            exp.push_back(std::make_tuple(42, i, val));
            if (status == AKU_EOVERFLOW) {
                break;
            }
        }
    }
    {
        LZ4Volume volume(filename);
        while(true) {
            aku_Status status;
            const LZ4Volume::Frame* frame;
            std::tie(status, frame) = volume.read_next_frame();
            BOOST_REQUIRE_EQUAL(status, AKU_SUCCESS);
            if (frame == nullptr) {
                // Done iterating
                break;
            }
            for(u32 i = 0; i < frame->part.size; i++) {
                act.push_back(std::make_tuple(frame->part.ids[i],
                                              frame->part.tss[i],
                                              frame->part.xss[i]));
            }
        }
        volume.delete_file();
    }
    BOOST_REQUIRE_EQUAL(exp.size(), act.size());
    for (u32 i = 0; i < exp.size(); i++) {
        BOOST_REQUIRE_EQUAL(std::get<0>(exp.at(i)), std::get<0>(act.at(i)));
        BOOST_REQUIRE_EQUAL(std::get<1>(exp.at(i)), std::get<1>(act.at(i)));
        BOOST_REQUIRE_EQUAL(std::get<2>(exp.at(i)), std::get<2>(act.at(i)));
    }
}

BOOST_AUTO_TEST_CASE(Test_input_roundtrip_with_frames) {
    std::vector<std::tuple<u64, u64, double>> exp, act;
    std::vector<u64> stale_ids;
    {
        InputLog ilog(&sequencer, "./", 100, 4096, 0);
        for (int i = 0; i < 10000; i++) {
            double val = static_cast<double>(rand()) / RAND_MAX;
            aku_Status status = ilog.append(42, i, val, &stale_ids);
            exp.push_back(std::make_tuple(42, i, val));
            if (status == AKU_EOVERFLOW) {
                ilog.rotate();
            }
        }
    }
    BOOST_REQUIRE(stale_ids.empty());
    {
        InputLog ilog("./", 0);
        while(true) {
            aku_Status status;
            const LZ4Volume::Frame* frame;
            std::tie(status, frame) = ilog.read_next_frame();
            if (frame == nullptr) {
                BOOST_REQUIRE_EQUAL(status, AKU_ENO_DATA);
                break;
            }
            BOOST_REQUIRE_EQUAL(status, AKU_SUCCESS);
            for(u32 i = 0; i < frame->part.size; i++) {
                act.push_back(std::make_tuple(frame->part.ids[i],
                                              frame->part.tss[i],
                                              frame->part.xss[i]));
            }
        }
        ilog.reopen();
        ilog.delete_files();
    }
    BOOST_REQUIRE_EQUAL(exp.size(), act.size());
    for (u32 i = 0; i < exp.size(); i++) {
        BOOST_REQUIRE_EQUAL(std::get<0>(exp.at(i)), std::get<0>(act.at(i)));
        BOOST_REQUIRE_EQUAL(std::get<1>(exp.at(i)), std::get<1>(act.at(i)));
        BOOST_REQUIRE_EQUAL(std::get<2>(exp.at(i)), std::get<2>(act.at(i)));
    }
}

BOOST_AUTO_TEST_CASE(Test_input_roundtrip_with_shardedlog_no_conflicts) {
    std::vector<std::tuple<u64, u64, double>> exp, act;
    std::vector<u64> stale_ids;
    {
        ShardedInputLog slog(3, "./", 100, 4096);
        auto fill_data_in = [&](InputLog& ilog, aku_ParamId series) {
            for (int i = 0; i < 10000; i++) {
                double val = static_cast<double>(rand()) / RAND_MAX;
                aku_Status status = ilog.append(series, i, val, &stale_ids);
                exp.push_back(std::make_tuple(series, i, val));
                if (status == AKU_EOVERFLOW) {
                    ilog.rotate();
                }
            }
        };
        fill_data_in(slog.get_shard(0), 111);
        fill_data_in(slog.get_shard(1), 222);
        fill_data_in(slog.get_shard(2), 333);
    }
    {
        ShardedInputLog slog(0, "./");
        // Read by one
        while(true) {
            aku_ParamId id;
            aku_Timestamp ts;
            double xs;
            aku_Status status;
            u32 outsize;
            std::tie(status, outsize) = slog.read_next(1, &id, &ts, &xs);
            if (outsize == 1) {
                act.push_back(std::make_tuple(id, ts, xs));
            }
            if (status == AKU_ENO_DATA) {
                // EOF
                break;
            } else if (status != AKU_SUCCESS) {
                BOOST_ERROR("Read failed " + StatusUtil::str(status));
            }
        }
    }
    BOOST_REQUIRE_EQUAL(exp.size(), act.size());
    for (u32 i = 0; i < exp.size(); i++) {
        if (std::get<0>(exp.at(i)) != std::get<0>(act.at(i))) {
            BOOST_REQUIRE_EQUAL(std::get<0>(exp.at(i)), std::get<0>(act.at(i)));
        }
        if (std::get<1>(exp.at(i)) != std::get<1>(act.at(i))) {
            BOOST_REQUIRE_EQUAL(std::get<1>(exp.at(i)), std::get<1>(act.at(i)));
        }
        if (std::get<2>(exp.at(i)) != std::get<2>(act.at(i))) {
            BOOST_REQUIRE_EQUAL(std::get<2>(exp.at(i)), std::get<2>(act.at(i)));
        }
    }
}