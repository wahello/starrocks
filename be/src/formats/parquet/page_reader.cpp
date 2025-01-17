// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "formats/parquet/page_reader.h"

#include "common/config.h"
// #include "fs/fs.h"
#include "gutil/strings/substitute.h"
#include "util/thrift_util.h"

namespace starrocks::parquet {

static constexpr size_t kHeaderInitSize = 1024;

PageReader::PageReader(IBufferedInputStream* stream, uint64_t start_offset, uint64_t length)
        : _stream(stream), _start_offset(start_offset), _finish_offset(start_offset + length) {}

Status PageReader::next_header() {
    if (_offset != _next_header_pos) {
        return Status::InternalError(
                strings::Substitute("Try to parse parquet column header in wrong position, offset=$0 vs expect=$1",
                                    _offset, _next_header_pos));
    }
    if (_offset >= _finish_offset) {
        return Status::EndOfFile("");
    }

    const uint8_t* page_buf = nullptr;

    uint32_t header_length = 0;
    size_t nbytes = kHeaderInitSize;
    size_t remaining = _finish_offset - _offset;
    do {
        nbytes = std::min(nbytes, remaining);
        RETURN_IF_ERROR(_stream->get_bytes(&page_buf, _offset, &nbytes));

        header_length = nbytes;
        auto st = deserialize_thrift_msg(page_buf, &header_length, TProtocolType::COMPACT, &_cur_header);
        if (st.ok()) {
            break;
        }

        if ((nbytes > config::parquet_header_max_size) || (_offset + nbytes) >= _finish_offset) {
            return Status::Corruption("Failed to decode parquet page header");
        }
        nbytes <<= 2;
    } while (true);

    _offset += header_length;
    _next_header_pos = _offset + _cur_header.compressed_page_size;
    return Status::OK();
}

Status PageReader::read_bytes(const uint8_t** buffer, size_t size) {
    if (_offset + size > _next_header_pos) {
        return Status::InternalError("Size to read exceed page size");
    }
    uint64_t nbytes = size;
    RETURN_IF_ERROR(_stream->get_bytes(buffer, _offset, &nbytes));
    DCHECK_EQ(nbytes, size);
    _offset += nbytes;
    return Status::OK();
}

Status PageReader::skip_bytes(size_t size) {
    if (_offset + size > _next_header_pos) {
        return Status::InternalError("Size to skip exceed page size");
    }
    _offset += size;
    return Status::OK();
}

} // namespace starrocks::parquet
