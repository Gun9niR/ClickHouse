#include "S3QueueOrderedFileMetadata.h"
#include <Common/SipHash.h>
#include <Common/getRandomASCIIString.h>
#include <Common/logger_useful.h>
#include <Interpreters/Context.h>
#include <Poco/JSON/JSON.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace
{
    S3QueueOrderedFileMetadata::Bucket getBucketForPathImpl(const std::string & path, size_t buckets_num)
    {
        return sipHash64(path) % buckets_num;
    }

    std::string getProcessedPathForBucket(const std::filesystem::path & zk_path, size_t bucket)
    {
        return zk_path / "buckets" / toString(bucket) / "processed";
    }

    std::string getProcessedPath(const std::filesystem::path & zk_path, const std::string & path, size_t buckets_num)
    {
        if (buckets_num > 1)
            return getProcessedPathForBucket(zk_path, getBucketForPathImpl(path, buckets_num));
        else
            return zk_path / "processed";
    }

    zkutil::ZooKeeperPtr getZooKeeper()
    {
        return Context::getGlobalContextInstance()->getZooKeeper();
    }
}

S3QueueOrderedFileMetadata::S3QueueOrderedFileMetadata(
    const std::filesystem::path & zk_path_,
    const std::string & path_,
    FileStatusPtr file_status_,
    size_t buckets_num_,
    size_t max_loading_retries_,
    LoggerPtr log_)
    : S3QueueIFileMetadata(
        path_,
        /* processing_node_path */zk_path_ / "processing" / getNodeName(path_),
        /* processed_node_path */getProcessedPath(zk_path_, path_, buckets_num_),
        /* failed_node_path */zk_path_ / "failed" / getNodeName(path_),
        file_status_,
        max_loading_retries_,
        log_)
    , buckets_num(buckets_num_)
    , zk_path(zk_path_)
{
}

std::vector<std::string> S3QueueOrderedFileMetadata::getMetadataPaths(size_t buckets_num)
{
    if (buckets_num > 1)
    {
        std::vector<std::string> paths{"buckets", "failed", "processing"};
        for (size_t i = 0; i < buckets_num; ++i)
            paths.push_back("buckets/" + toString(i));
        return paths;
    }
    else
        return {"failed", "processing"};
}

S3QueueOrderedFileMetadata::Bucket S3QueueOrderedFileMetadata::getBucketForPath(const std::string & path_, size_t buckets_num)
{
    return getBucketForPathImpl(path_, buckets_num);
}

S3QueueOrderedFileMetadata::BucketHolderPtr S3QueueOrderedFileMetadata::tryAcquireBucket(
    const std::filesystem::path & zk_path,
    const Bucket & bucket,
    const Processor & processor)
{
    const auto zk_client = getZooKeeper();
    const auto bucket_lock_path = zk_path / "buckets" / toString(bucket) / "lock";
    const auto processor_info = getProcessorInfo(processor);

    auto code = zk_client->tryCreate(bucket_lock_path, processor_info, zkutil::CreateMode::Ephemeral);
    if (code == Coordination::Error::ZOK)
    {
        LOG_TEST(
            getLogger("S3QueueOrderedFileMetadata"),
            "Processor {} acquired bucket {} for processing", processor, bucket);

        return std::make_shared<BucketHolder>(bucket, bucket_lock_path, zk_client);
    }

    if (code == Coordination::Error::ZNODEEXISTS)
        return nullptr;

    if (Coordination::isHardwareError(code))
        return nullptr;

    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected error: {}", magic_enum::enum_name(code));
}

std::pair<bool, S3QueueIFileMetadata::FileStatus::State> S3QueueOrderedFileMetadata::setProcessingImpl()
{
    /// In one zookeeper transaction do the following:
    enum RequestType
    {
        /// node_name is not within failed persistent nodes
        FAILED_PATH_DOESNT_EXIST = 0,
        /// node_name ephemeral processing node was successfully created
        CREATED_PROCESSING_PATH = 2,
        /// update processing id
        SET_PROCESSING_ID = 4,
        /// max_processed_node version did not change
        CHECKED_MAX_PROCESSED_PATH = 5,
    };

    const auto zk_client = getZooKeeper();
    processing_id = node_metadata.processing_id = getRandomASCIIString(10);
    auto processor_info = getProcessorInfo(processing_id.value());

    while (true)
    {
        NodeMetadata processed_node;
        Coordination::Stat processed_node_stat;
        bool has_processed_node = getMaxProcessedFile(processed_node, &processed_node_stat, zk_client);
        if (has_processed_node)
        {
            LOG_TEST(log, "Current max processed file {} from path: {}",
                        processed_node.file_path, processed_node_path);

            if (!processed_node.file_path.empty() && path <= processed_node.file_path)
            {
                return {false, FileStatus::State::Processed};
            }
        }

        Coordination::Requests requests;
        requests.push_back(zkutil::makeCreateRequest(failed_node_path, "", zkutil::CreateMode::Persistent));
        requests.push_back(zkutil::makeRemoveRequest(failed_node_path, -1));
        requests.push_back(zkutil::makeCreateRequest(processing_node_path, node_metadata.toString(), zkutil::CreateMode::Ephemeral));

        requests.push_back(
            zkutil::makeCreateRequest(
                processing_node_id_path, processor_info, zkutil::CreateMode::Persistent, /* ignore_if_exists */true));
        requests.push_back(zkutil::makeSetRequest(processing_node_id_path, processor_info, -1));

        if (has_processed_node)
        {
            requests.push_back(zkutil::makeCheckRequest(processed_node_path, processed_node_stat.version));
        }
        else
        {
            requests.push_back(zkutil::makeCreateRequest(processed_node_path, "", zkutil::CreateMode::Persistent));
            requests.push_back(zkutil::makeRemoveRequest(processed_node_path, -1));
        }

        Coordination::Responses responses;
        const auto code = zk_client->tryMulti(requests, responses);
        auto is_request_failed = [&](RequestType type) { return responses[type]->error != Coordination::Error::ZOK; };

        if (code == Coordination::Error::ZOK)
        {
            const auto * set_response = dynamic_cast<const Coordination::SetResponse *>(responses[SET_PROCESSING_ID].get());
            processing_id_version = set_response->stat.version;
            return {true, FileStatus::State::None};
        }

        if (is_request_failed(FAILED_PATH_DOESNT_EXIST))
            return {false, FileStatus::State::Failed};

        if (is_request_failed(CREATED_PROCESSING_PATH))
            return {false, FileStatus::State::Processing};

        if (is_request_failed(CHECKED_MAX_PROCESSED_PATH))
        {
            LOG_TEST(log, "Version of max processed file changed: {}. Will retry for file `{}`", code, path);
            continue;
        }

        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected response state: {}", code);
    }
}

void S3QueueOrderedFileMetadata::setProcessedAtStartRequests(
    Coordination::Requests & requests,
    const zkutil::ZooKeeperPtr & zk_client)
{
    if (buckets_num > 1)
    {
        for (size_t i = 0; i < buckets_num; ++i)
        {
            auto path = getProcessedPathForBucket(zk_path, i);
            setProcessedRequests(requests, zk_client, path, /* ignore_if_exists */true);
        }
    }
    else
    {
        setProcessedRequests(requests, zk_client, processed_node_path, /* ignore_if_exists */true);
    }
}

void S3QueueOrderedFileMetadata::setProcessedRequests(
    Coordination::Requests & requests,
    const zkutil::ZooKeeperPtr & zk_client,
    const std::string & processed_node_path_,
    bool ignore_if_exists)
{
    NodeMetadata processed_node;
    Coordination::Stat processed_node_stat;
    if (getMaxProcessedFile(processed_node, &processed_node_stat, processed_node_path_, zk_client))
    {
        LOG_TEST(log, "Current max processed file: {}, condition less: {}",
                 processed_node.file_path, bool(path <= processed_node.file_path));

        if (!processed_node.file_path.empty() && path <= processed_node.file_path)
        {
            LOG_TRACE(log, "File {} is already processed, current max processed file: {}", path, processed_node.file_path);

            if (ignore_if_exists)
                return;

            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "File ({}) is already processed, while expected it not to be (path: {})",
                path, processed_node_path_);
        }
        requests.push_back(zkutil::makeSetRequest(processed_node_path_, node_metadata.toString(), processed_node_stat.version));
    }
    else
    {
        LOG_TEST(log, "Max processed file does not exist, creating at: {}", processed_node_path_);
        requests.push_back(zkutil::makeCreateRequest(processed_node_path_, node_metadata.toString(), zkutil::CreateMode::Persistent));
    }

    if (processing_id_version.has_value())
    {
        requests.push_back(zkutil::makeCheckRequest(processing_node_id_path, processing_id_version.value()));
        requests.push_back(zkutil::makeRemoveRequest(processing_node_id_path, processing_id_version.value()));
        requests.push_back(zkutil::makeRemoveRequest(processing_node_path, -1));
    }
}

void S3QueueOrderedFileMetadata::setProcessedImpl()
{
    LOG_TRACE(log, "Setting file `{}` as processed (at {})", path, processed_node_path);

    /// In one zookeeper transaction do the following:
    enum RequestType
    {
        SET_MAX_PROCESSED_PATH = 0,
        CHECK_PROCESSING_ID_PATH = 1, /// Optional.
        REMOVE_PROCESSING_ID_PATH = 2, /// Optional.
        REMOVE_PROCESSING_PATH = 3, /// Optional.
    };

    const auto zk_client = getZooKeeper();
    const auto node_metadata_str = node_metadata.toString();

    while (true)
    {
        Coordination::Requests requests;
        setProcessedRequests(requests, zk_client, processed_node_path, /* ignore_if_exists */false);

        Coordination::Responses responses;
        auto is_request_failed = [&](RequestType type) { return responses[type]->error != Coordination::Error::ZOK; };

        auto code = zk_client->tryMulti(requests, responses);
        if (code == Coordination::Error::ZOK)
        {
            if (max_loading_retries)
                zk_client->tryRemove(failed_node_path + ".retriable", -1);

            LOG_TRACE(log, "Moved file `{}` to processed", path);
            return;
        }

        if (Coordination::isHardwareError(code))
        {
            LOG_WARNING(log, "Cannot set file {} as processed. Lost connection to keeper: {}", path, code);
            return;
        }

        if (is_request_failed(SET_MAX_PROCESSED_PATH))
        {
            LOG_TRACE(log, "Failed to update processed node for path {}: {}. "
                      "Will retry.", path, code);
            continue;
        }

        if (is_request_failed(CHECK_PROCESSING_ID_PATH))
        {
            LOG_WARNING(log, "Cannot set file as processed. "
                        "Version of processing id node changed: {}", code);
            return;
        }

        if (is_request_failed(REMOVE_PROCESSING_PATH))
        {
            LOG_WARNING(log, "Failed to remove processing path: {}", code);
            return;
        }

        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected state of zookeeper transaction: {}", code);
    }
}

}
