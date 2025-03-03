/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include "velox/dwio/common/DataSink.h"

#include "velox/common/base/Fs.h"
#include "velox/common/file/FileSystems.h"
#include "velox/dwio/common/exception/Exception.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace facebook::velox::dwio::common {

void WriteFileDataSink::write(std::vector<DataBuffer<char>>& buffers) {
  writeImpl(buffers, [&](auto& buffer) {
    const uint64_t size = buffer.size();
    writeFile_->append({buffer.data(), size});
    return size;
  });
}

void WriteFileDataSink::doClose() {
  LOG(INFO) << "closing file: " << getName() << ",  total size: " << size_;
  if (writeFile_ != nullptr) {
    writeFile_->close();
  }
}

std::unique_ptr<DataSink> localWriteFileSink(
    const std::string& filename,
    memory::MemoryPool* /*unused*/,
    MetricsLogPtr metricsLog,
    IoStatistics* stats = nullptr) {
  if (strncmp(filename.c_str(), "file:", 5) == 0) {
    auto pathSuffix = filename.substr(5);
    return std::make_unique<WriteFileDataSink>(
        std::make_unique<LocalWriteFile>(pathSuffix, true, false),
        pathSuffix,
        metricsLog,
        stats);
  }
  return nullptr;
}

void WriteFileDataSink::registerLocalFileFactory() {
  DataSink::registerFactory(localWriteFileSink);
}

LocalFileSink::LocalFileSink(
    const std::string& name,
    const MetricsLogPtr& metricLogger,
    IoStatistics* stats)
    : DataSink{name, metricLogger, stats} {
  auto dir = fs::path(name).parent_path();
  if (!fs::exists(dir)) {
    DWIO_ENSURE(velox::common::generateFileDirectory(dir.c_str()));
  }
  file_ = open(name_.c_str(), O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
  if (file_ == -1) {
    markClosed();
    DWIO_RAISE("Can't open ", name_, " ErrorNo ", errno, ": ", strerror(errno));
  }
}

void LocalFileSink::write(std::vector<DataBuffer<char>>& buffers) {
  writeImpl(buffers, [&](auto& buffer) {
    size_t size = buffer.size();
    size_t offset = 0;
    while (offset < size) {
      // Write system call can write fewer bytes than requested.
      auto bytesWritten = ::write(file_, buffer.data() + offset, size - offset);

      // errno should only be accessed when the return value is -1.
      DWIO_ENSURE_NE(
          bytesWritten,
          -1,
          "Bad write of ",
          name_,
          " ErrorNo ",
          errno,
          " Remaining ",
          size - offset);

      // ensure the file is making some forward progress in each loop.
      DWIO_ENSURE_GT(
          bytesWritten,
          0,
          "No bytes transferred ",
          name_,
          " Size: ",
          size,
          " Offset: ",
          offset);

      offset += bytesWritten;
    }
    return size;
  });
}

static std::vector<DataSink::Factory>& factories() {
  static std::vector<DataSink::Factory> factories;
  return factories;
}

// static
bool DataSink::registerFactory(const DataSink::Factory& factory) {
  factories().push_back(factory);
  return true;
}

std::unique_ptr<DataSink> DataSink::create(
    const std::string& filePath,
    memory::MemoryPool* pool,
    const MetricsLogPtr& metricsLog,
    IoStatistics* stats) {
  DWIO_ENSURE_NOT_NULL(metricsLog.get());
  for (auto& factory : factories()) {
    auto result = factory(filePath, pool, metricsLog, stats);
    if (result) {
      return result;
    }
  }
  // TODO: remove this fallback once file data sink all switch to use velox
  // filesystem for io operation.
  return std::make_unique<LocalFileSink>(filePath, metricsLog, stats);
}

static std::unique_ptr<DataSink> localFileSink(
    const std::string& filename,
    memory::MemoryPool* /*unused*/,
    const MetricsLogPtr& metricsLog,
    IoStatistics* stats = nullptr) {
  if (strncmp(filename.c_str(), "file:", 5) == 0) {
    return std::make_unique<LocalFileSink>(
        filename.substr(5), metricsLog, stats);
  }
  return nullptr;
}

VELOX_REGISTER_DATA_SINK_METHOD_DEFINITION(LocalFileSink, localFileSink);

void registerDataSinks() {
  dwio::common::LocalFileSink::registerFactory();
}

} // namespace facebook::velox::dwio::common
