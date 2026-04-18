#pragma once

#include <grpcpp/grpcpp.h>
#include "../generated/orion.grpc.pb.h"
#include <fstream>
#include <filesystem>

namespace orion::distributed {

namespace fs = std::filesystem;

class CasClient {
public:
    explicit CasClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(orion::CasService::NewStub(channel)) {}

    bool fetch_blob(const std::string& hash, const fs::path& dest_path) {
        orion::BlobRequest req;
        req.set_hash(hash);

        grpc::ClientContext context;
        auto reader = stub_->FetchBlob(&context, req);

        orion::BlobChunk chunk;
        std::ofstream out(dest_path, std::ios::binary);
        if (!out) return false;

        while (reader->Read(&chunk)) {
            out.write(chunk.data().data(), chunk.data().size());
        }

        grpc::Status status = reader->Finish();
        return status.ok();
    }

    std::string upload_blob(const fs::path& source_path) {
        if (!fs::exists(source_path)) return "";

        grpc::ClientContext context;
        orion::BlobReply reply;
        auto writer = stub_->UploadBlob(&context, &reply);

        std::ifstream in(source_path, std::ios::binary);
        if (!in) return "";

        const size_t chunk_size = 64 * 1024;
        char buffer[chunk_size];
        
        // We'll compute the hash locally first to set it in the chunks
        // But the server also computes it. 
        // For simplicity, let's just send.
        
        while (in.read(buffer, chunk_size) || in.gcount() > 0) {
            orion::BlobChunk chunk;
            chunk.set_data(buffer, in.gcount());
            if (!writer->Write(chunk)) break;
        }

        writer->WritesDone();
        grpc::Status status = writer->Finish();
        
        if (status.ok() && reply.success()) {
            return reply.hash();
        }
        return "";
    }

private:
    std::unique_ptr<orion::CasService::Stub> stub_;
};

} // namespace orion::distributed
