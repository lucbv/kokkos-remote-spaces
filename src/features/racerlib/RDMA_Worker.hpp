#ifndef RACERLIB_RDMA_WORKER
#define RACERLIB_RDMA_WORKER

#include <RDMA_Access_Cache.hpp>
#include <RDMA_Engine.hpp>

namespace RACERlib {

struct RdmaScatterGatherWorker {
  // struct RemoteCacheHolder;

  static constexpr uint32_t queue_size = /*RdmaScatterGatherEngine::queue_size;*/ 1;

  template <class T> KOKKOS_FUNCTION T get(int pe, uint32_t offset);

  template <class T> KOKKOS_FUNCTION T request(int pe, uint32_t offset);

  Features::Cache::RemoteCache cache;
  /** Array of size num_pes, a running count of element requests
   * generated by worker threads */
  uint64_t *tx_element_request_ctrs;
  /** Array of size num_pes, a running count of element requests
   * completed and acked by the host. This is not read by the
   * worker threads to limit host-device contention */
  uint64_t *ack_ctrs_h;
  /** Array of size num_pes, a running count of element requests
   * completed and acked to worker threads. This is a mirror of
   * ack_ctrs_h that guarantees ack_ctrs_d[i] <= ack_cstr_h[i]
   * and that ack_ctrs_d will eventually equal ack_ctrs_h
   */
  uint64_t *ack_ctrs_d;
  /** Array of size num_pes, a running count of element replies
   * back to each PE */
  uint64_t *tx_element_reply_ctrs;
  /** Array of size num_pes, a running count of element requests
   * aggregated and processed by the request threads */
  uint64_t *tx_element_aggregate_ctrs;
  /** Array of size num_pes*queue_length */
  uint32_t *tx_element_request_queue;
  /** Array of size num_pes, the number of times we have wrapped around
   *  the circular buffer queue for each PE */
  uint32_t *tx_element_request_trip_counts;
  /** Arraqy of size num_pes*queue_length, requests from remote PEs
   *  are received here */
  uint32_t *rx_element_request_queue;
  /** Array of size num_pes*queue_length, data is gathered here
   *  and sent back to requesting PEs */
  void *tx_element_reply_queue;
  /** Array of size num_pes*queue_length, gathered data from the remote PE
   *  is received here */
  void *rx_element_reply_queue;
  /** Array of size num_pes, a pointer that can be directly read
   * to access peer data, nullptr if no peer pointer exists
   */
  void **direct_ptrs;
  int rank;
  int num_pes;

  /** Array of size queue_length
   *  Request threads on device write commands into this queue
   *  Progress threads on the CPU read command from this queue
   *  Indicates host should send a block of element requests to remote PE
   *  Combined queue for all PEs */
  uint64_t *tx_block_request_cmd_queue;
  /** Array of size queue_length
   *  Response threads on device read command from this queue
   *  Progress threads on the CPU write commands into this queue after receiving
   * request Indicates GPU should gather data from scattered offsets into a
   * contiguous block Combined queue for all PEs */
  uint64_t *rx_block_request_cmd_queue;
  /** Array of size queue_length
   *  Response threads on device write commands into this queue
   *  Progress threads on the CPU read command from this queue
   *  Indicates host a block of element replies is ready to send back to
   * requesting PE Combined queue for all PEs */
  uint64_t *tx_block_reply_cmd_queue;
  /** A running count of the number of block requests sent to all PEs */
  uint64_t tx_block_request_ctr;
  /** A running count of the number of block requests received from all PEs */
  uint64_t rx_block_request_ctr;

  unsigned *request_done_flag;
  unsigned *response_done_flag;
  unsigned *fence_done_flag;
};
} // namespace RACERlib

#endif // RACERLIB_RDMA_WORKER