#pragma once

#include <grpcpp/grpcpp.h>
#include "../generated/orion.grpc.pb.h"
#include "cas_store.h"
#include "../observability/logger.h"

namespace orion::distributed {

class CasServiceImpl final : public orion::CasService::Service {
public:
    explicit CasServiceImpl(CasStore& store) : store_(store) {}

    grpc::Status FetchBlob(grpc::ServerContext* context,
                           const orion::BlobRequest* req,
                           grpc::ServerWriter<orion::BlobChunk>* writer) override {
        
        LOG_INFO("CasService", "fetch_blob_request", {{"hash", req->hash()}});
        
        std::string data = store_.get_blob(req->hash());
        if (data.empty()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Blob not found");
        }

        const size_t chunk_size = 64 * 1024; // 64KB chunks
        size_t offset = 0;
        while (offset < data.size()) {
            orion::BlobChunk chunk;
            chunk.set_hash(req->hash());
            chunk.set_total_size(data.size());
            chunk.set_offset(offset);
            
            size_t len = std::min(chunk_size, data.size() - offset);
            chunk.set_data(data.substr(offset, len));
            
            if (!writer->Write(chunk)) {
                return grpc::Status::CANCELLED;
            }
            offset += len;
        }

        return grpc::Status::OK;
    }

    grpc::Status UploadBlob(grpc::ServerContext* context,
                            grpc::ServerReader<orion::BlobChunk>* reader,
                            orion::BlobReply* reply) override {
        
        orion::BlobChunk chunk;
        std::string buffer;
        std::string expected_hash;
        
        while (reader->Read(&chunk)) {
            if (expected_hash.empty()) {
                expected_hash = chunk.hash();
            }
            buffer.append(chunk.data());
        }

        if (buffer.empty()) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Empty blob");
        }

        // Verify hash before storing
        std::string actual_hash = store_.put_blob(buffer);
        
        if (!expected_hash.empty() && actual_hash != expected_hash) {
            reply->set_success(false);
            reply->set_error_message("Hash mismatch during upload");
            return grpc::Status::OK;
        }

        reply->set_hash(actual_hash);
        reply->set_success(true);
        LOG_INFO("CasService", "blob_uploaded", {{"hash", actual_hash}, {"size", std::to_string(buffer.size())}});
        
        return grpc::Status::OK;
    }

private:
    CasStore& store_;
};

} // namespace orion::distributed
