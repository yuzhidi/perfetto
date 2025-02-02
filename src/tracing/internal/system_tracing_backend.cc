/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "perfetto/tracing/internal/system_tracing_backend.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/ext/tracing/ipc/consumer_ipc_client.h"
#include "perfetto/ext/tracing/ipc/default_socket.h"
#include "perfetto/ext/tracing/ipc/producer_ipc_client.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include "src/tracing/ipc/shared_memory_windows.h"
#else
#include "src/tracing/ipc/posix_shared_memory.h"
#endif

namespace perfetto {
namespace internal {

// static
TracingBackend* SystemTracingBackend::GetInstance() {
  static auto* instance = new SystemTracingBackend();
  return instance;
}

SystemTracingBackend::SystemTracingBackend() {}

std::unique_ptr<ProducerEndpoint> SystemTracingBackend::ConnectProducer(
    const ConnectProducerArgs& args) {
  PERFETTO_DCHECK(args.task_runner->RunsTasksOnCurrentThread());

  std::unique_ptr<SharedMemory> shm;
  std::unique_ptr<SharedMemoryArbiter> arbiter;
  uint32_t shmem_size_hint = args.shmem_size_hint_bytes;
  uint32_t shmem_page_size_hint = args.shmem_page_size_hint_bytes;
  if (args.use_producer_provided_smb) {
    if (shmem_size_hint == 0)
      shmem_size_hint = TracingService::kDefaultShmSize;
    if (shmem_page_size_hint == 0)
      shmem_page_size_hint = TracingService::kDefaultShmPageSize;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    shm = SharedMemoryWindows::Create(shmem_size_hint);
#else
    shm = PosixSharedMemory::Create(shmem_size_hint);
#endif
    arbiter = SharedMemoryArbiter::CreateUnboundInstance(shm.get(),
                                                         shmem_page_size_hint);
  }

  auto endpoint = ProducerIPCClient::Connect(
      GetProducerSocket(), args.producer, args.producer_name, args.task_runner,
      TracingService::ProducerSMBScrapingMode::kEnabled, shmem_size_hint,
      shmem_page_size_hint, std::move(shm), std::move(arbiter),
      ProducerIPCClient::ConnectionFlags::kRetryIfUnreachable);
  PERFETTO_CHECK(endpoint);
  return endpoint;
}

std::unique_ptr<ConsumerEndpoint> SystemTracingBackend::ConnectConsumer(
    const ConnectConsumerArgs& args) {
  auto endpoint = ConsumerIPCClient::Connect(GetConsumerSocket(), args.consumer,
                                             args.task_runner);
  PERFETTO_CHECK(endpoint);
  return endpoint;
}

}  // namespace internal
}  // namespace perfetto
