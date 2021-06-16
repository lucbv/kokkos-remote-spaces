
#include <RDMA_Interface.hpp>

namespace RACERlib {

#ifdef RAW_CUDA

template <typename T, class Team>
__device__ void pack_response(T *local_values, RdmaScatterGatherWorker *sgw,
                              unsigned *completion_flag, Team &&team) {
  KOKKOS_REMOTE_SHARED unsigned completion;
  KOKKOS_REMOTE_SHARED uint64_t request;
  int my_thread = threadIdx.x * blockDim.y + threadIdx.y;
  int total_threads = blockDim.x * blockDim.y;
  uint32_t queue_size = RdmaScatterGatherEngine::queue_size;
  while (completion == 0) {
    uint64_t idx = sgw->rx_block_request_ctr % queue_size;
    uint64_t trip_number = sgw->rx_block_request_ctr / queue_size;
    if (my_thread == 0) {
      request = volatile_load(&sgw->rx_block_request_cmd_queue[idx]);
    }
    __syncthreads();

    if (GET_BLOCK_FLAG(request) == MAKE_READY_FLAG(trip_number)) {
      uint32_t num_requests = GET_BLOCK_SIZE(request);
      uint32_t pe = GET_BLOCK_PE(request);
      uint32_t window = GET_BLOCK_WINDOW(request);
      uint32_t reply_offset =
          pe * queue_size + sgw->tx_element_reply_ctrs[pe] % queue_size;
      uint32_t *offsets = sgw->rx_element_request_queue + window * queue_size;
      T *reply_tx_buffer_T = ((T *)sgw->tx_element_reply_queue) + reply_offset;

      uint32_t num_packed = 0;
      while (num_packed < num_requests) {
        uint32_t my_index = num_packed + my_thread;
        if (my_index < num_requests) {
          // this needs to be volatile to force visibility from the IB send
          uint32_t offset =
              GET_ELEMENT_OFFSET(volatile_load(&offsets[my_index]));
          reply_tx_buffer_T[my_index] = local_values[offset];
        }
        num_packed += total_threads;
      }
      if (my_thread == 0) {
        ++sgw->rx_block_request_ctr;
        sgw->tx_element_reply_ctrs[pe] += num_requests;
      }
      // force visibility
      __threadfence_system();
      if (my_thread == 0) {
        volatile_store(&sgw->tx_block_reply_cmd_queue[idx], request);
      }
    }

    if (my_thread == 0) {
      completion = volatile_load(completion_flag);
    }
    __syncthreads();
  }
  __syncthreads();
  if (my_thread == 0) {
    volatile_store(completion_flag, 0u);
  }
}

template <class Team>
__device__ void aggregate_requests(RdmaScatterGatherWorker *sgw, Team &&team,
                                   unsigned num_worker_teams) {
  int my_thread = threadIdx.x * blockDim.y + threadIdx.y;
  int total_threads = blockDim.x * blockDim.y;
  uint32_t queue_size = RdmaScatterGatherEngine::queue_size;
  static constexpr uint32_t mtu = 16384; // try to at least send 16K elements
  static constexpr uint32_t max_mtu_stalls = 4;
  KOKKOS_REMOTE_SHARED unsigned completion;
  KOKKOS_REMOTE_SHARED uint64_t total_requests;
  KOKKOS_REMOTE_SHARED int
      misses[32]; // TODO, make this an array, I'm too lazy right now
  for (int i = 0; i < 32; ++i)
    misses[i] = 0;
  completion = 0;
  __syncthreads();
  while (completion < num_worker_teams) {
    for (int pe = 0; pe < sgw->num_pes; ++pe) {
      uint64_t head = sgw->tx_element_aggregate_ctrs[pe];
      if (my_thread == 0) {
        uint64_t last_cleared_on_device = sgw->ack_ctrs_d[pe];
        if (head > last_cleared_on_device) {
          uint64_t last_cleared_on_host = volatile_load(&sgw->ack_ctrs_h[pe]);
          if (last_cleared_on_device < last_cleared_on_host) {
            volatile_store(&sgw->ack_ctrs_d[pe], last_cleared_on_host);
          }
        }
        uint64_t max_index =
            Kokkos::atomic_fetch_add(&sgw->tx_element_request_ctrs[pe], 0u);
        total_requests = max_index - head;
        if (total_requests < mtu && misses[pe] < max_mtu_stalls) {
          total_requests = 0;
          ++misses[pe];
        } else {
          misses[pe] = 0;
        }
      }
      __syncthreads();
      if (total_requests > 0) {
        unsigned requests_done = 0;
        while (requests_done < total_requests) {
          uint64_t my_offset = head + requests_done + my_thread;
          if (my_offset < total_requests) {
            uint64_t my_idx = my_offset % queue_size;
            uint64_t my_trip_number = my_offset / queue_size;
            uint32_t ready_flag = MAKE_READY_FLAG(my_trip_number);
            uint32_t req_slot = my_idx + pe * queue_size;
            uint32_t next_request =
                volatile_load(&sgw->tx_element_request_queue[req_slot]);
            while (GET_BLOCK_FLAG(next_request) != ready_flag) {
              next_request =
                  volatile_load(&sgw->tx_element_request_queue[req_slot]);
            }
            // this looks stupid, but is necessary to make visible to peer
            // devices
            sgw->tx_element_request_queue[req_slot] = next_request;
          }
          requests_done += total_threads;
        }
        // we have written the requests, now make them peer visible
        __threadfence_system();

        if (my_thread == 0) {
          uint64_t tail_idx = sgw->tx_block_request_ctr++;
          sgw->tx_element_aggregate_ctrs[pe] += total_requests;
          uint64_t queue_idx = tail_idx % queue_size;
          uint64_t trip_number = tail_idx / queue_size;
          uint64_t request =
              MAKE_BLOCK_GET_REQUEST(total_requests, pe, trip_number);
          volatile_store(&sgw->tx_block_request_cmd_queue[queue_idx], request);
        }
        __syncthreads();
      }
    }
    if (my_thread == 0) {
      completion = volatile_load(sgw->request_done_flag);
    }
    __syncthreads();
  }
  __syncthreads();
  if (my_thread == 0) {
    volatile_store(sgw->request_done_flag, 0u);
    volatile_store(sgw->response_done_flag, 1u);
  }
}

#else

template <typename T, class Team>
KOKKOS_FUNCTION void pack_response(T *local_values,
                                   RdmaScatterGatherWorker *sgw,
                                   unsigned *completion_flag, Team &&team) {
  KOKKOS_REMOTE_SHARED unsigned completion;
  KOKKOS_REMOTE_SHARED uint64_t request;
  completion = 0;
  uint32_t queue_size = RdmaScatterGatherEngine::queue_size;
  while (completion == 0) {
    uint64_t idx = sgw->rx_block_request_ctr % queue_size;
    uint64_t trip_number = sgw->rx_block_request_ctr / queue_size;
    Kokkos::single(Kokkos::PerTeam(team), [&]() {
      request = volatile_load(&sgw->rx_block_request_cmd_queue[idx]);
    });
    team.team_barrier();
    if (GET_BLOCK_FLAG(request) == MAKE_READY_FLAG(trip_number)) {
      uint32_t num_requests = GET_BLOCK_SIZE(request);
      uint32_t pe = GET_BLOCK_PE(request);
      uint32_t window = GET_BLOCK_WINDOW(request);
      uint32_t reply_offset =
          pe * queue_size + sgw->tx_element_reply_ctrs[pe] % queue_size;
      uint32_t *offsets = sgw->rx_element_request_queue + window * queue_size;
      T *reply_tx_buffer_T = ((T *)sgw->tx_element_reply_queue) + reply_offset;

      auto vec_length = team.vector_length();
      uint64_t num_passes = num_requests / vec_length;
      if (num_requests % vec_length)
        num_passes++;
      Kokkos::parallel_for(Kokkos::TeamThreadRange(team, 0, num_passes),
                           [&](const int64_t pass) {
                             uint64_t start = pass * vec_length;
                             uint64_t stop = start + vec_length;
                             if (stop > num_requests)
                               stop = num_requests;
                             Kokkos::parallel_for(
                                 Kokkos::ThreadVectorRange(team, start, stop),
                                 [=](uint64_t my_index) {
                                   // this needs to be volatile to force
                                   // visibility from the IB send
                                   uint32_t offset = GET_ELEMENT_OFFSET(
                                       volatile_load(&offsets[my_index]));
                                   reply_tx_buffer_T[my_index] =
                                       local_values[offset];
                                 });
                           });
      Kokkos::single(Kokkos::PerTeam(team), [&]() {
        ++sgw->rx_block_request_ctr;
        sgw->tx_element_reply_ctrs[pe] += num_requests;
      });

      KOKKOS_REMOTE_THREADFENCE_SYSTEM();
      Kokkos::single(Kokkos::PerTeam(team), [&]() {
        volatile_store(&sgw->tx_block_reply_cmd_queue[idx], request);
      });
    }
    Kokkos::single(Kokkos::PerTeam(team),
                   [&]() { completion = volatile_load(completion_flag); });
    team.team_barrier();
  }
  team.team_barrier();
  Kokkos::single(Kokkos::PerTeam(team),
                 [&]() { volatile_store(completion_flag, 0u); });
}

template <class Team>
KOKKOS_INLINE_FUNCTION void aggregate_requests(RdmaScatterGatherWorker *sgw,
                                               Team &&team,
                                               unsigned num_worker_teams) {
  uint32_t queue_size = RdmaScatterGatherEngine::queue_size;
  static constexpr uint32_t mtu = 16384; // try to at least send 16K elements
  static constexpr uint32_t max_mtu_stalls = 4;
  KOKKOS_REMOTE_SHARED unsigned completion;
  KOKKOS_REMOTE_SHARED uint64_t total_requests;
  KOKKOS_REMOTE_SHARED int
      misses[32]; // TODO, make this an array, I'm too lazy right now
  for (int i = 0; i < 32; ++i)
    misses[i] = 0;
  completion = 0;
  team.team_barrier();
  while (completion < num_worker_teams) {
    for (int pe = 0; pe < sgw->num_pes; ++pe) {
      uint64_t head = sgw->tx_element_aggregate_ctrs[pe];
      Kokkos::single(Kokkos::PerTeam(team), [&]() {
        total_requests = 0;
        uint64_t last_cleared_on_device = sgw->ack_ctrs_d[pe];
        if (head > last_cleared_on_device) {
          uint64_t last_cleared_on_host = volatile_load(&sgw->ack_ctrs_h[pe]);
          if (last_cleared_on_device < last_cleared_on_host) {
            volatile_store(&sgw->ack_ctrs_d[pe], last_cleared_on_host);
          }
        }
        uint64_t max_index =
            Kokkos::atomic_fetch_add(&sgw->tx_element_request_ctrs[pe], 0u);
        total_requests = max_index - head;
        if (total_requests < mtu && misses[pe] < max_mtu_stalls) {
          total_requests = 0;
          ++misses[pe];
        } else {
          misses[pe] = 0;
        }
      });
      team.team_barrier();
      if (total_requests > 0) {
        auto vec_length = team.vector_length();
        uint64_t num_passes = total_requests / vec_length;
        if (total_requests % vec_length)
          num_passes++;

        Kokkos::parallel_for(
            Kokkos::TeamThreadRange(team, 0, num_passes),
            [&](const int64_t pass) {
              uint64_t start = pass * vec_length;
              uint64_t stop = start + vec_length;
              if (stop > total_requests)
                stop = total_requests;
              Kokkos::parallel_for(
                  Kokkos::ThreadVectorRange(team, start, stop),
                  [=](uint64_t offset) {
                    uint64_t my_offset = head + offset;
                    uint64_t my_trip_number = my_offset / queue_size;
                    uint64_t my_idx = my_offset % queue_size;
                    uint64_t ready_flag = MAKE_READY_FLAG(my_trip_number);
                    uint64_t req_slot = my_idx + pe * queue_size;
                    uint32_t next_request =
                        volatile_load(&sgw->tx_element_request_queue[req_slot]);
                    while (GET_BLOCK_FLAG(next_request) != ready_flag) {
                      next_request = volatile_load(
                          &sgw->tx_element_request_queue[req_slot]);
                    }
                    // this looks stupid, but is necessary to make visible to
                    // peer devices
                    sgw->tx_element_request_queue[req_slot] = next_request;
                  });
            });
        // we have written the requests, now make them peer visible
        KOKKOS_REMOTE_THREADFENCE_SYSTEM();

        Kokkos::single(Kokkos::PerTeam(team), [&]() {
          uint64_t tail_idx = sgw->tx_block_request_ctr++;
          sgw->tx_element_aggregate_ctrs[pe] += total_requests;
          uint64_t queue_idx = tail_idx % queue_size;
          uint64_t trip_number = tail_idx / queue_size;
          uint64_t request =
              MAKE_BLOCK_GET_REQUEST(total_requests, pe, trip_number);
          volatile_store(&sgw->tx_block_request_cmd_queue[queue_idx], request);
        });
        team.team_barrier();
      }
    }

    Kokkos::single(Kokkos::PerTeam(team), [&]() {
      completion = volatile_load(sgw->request_done_flag);
    });
    team.team_barrier();
  }
  team.team_barrier();
  Kokkos::single(Kokkos::PerTeam(team), [&]() {
    volatile_store(sgw->request_done_flag, 0u);
    volatile_store(sgw->response_done_flag, 1u);
  });
}

#endif // RAW_CUDA

} // namespace RACERlib