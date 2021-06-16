#ifndef RACERLIB_RDMA_ENGINE
#define RACERLIB_RDMA_ENGINE

#define RAW_CUDA

#include <Kokkos_Atomic.hpp>
#include <Kokkos_View.hpp>

#include <RACERlib_Config.hpp>
#include <RDMA_Access_Cache.hpp>
#include <RDMA_Transport.hpp>
#include <RDMA_Worker.hpp>

#include <infiniband/verbs.h>
#include <iostream>
#include <map>
#include <mpi.h>
#include <pthread.h>
#include <queue>
#include <set>
#include <vector>

namespace RACERlib {

#define NEW_REQUEST_BIT 0
#define NEW_REQUEST_MASK 1

template <class T> struct SPSC_LockFree_Pool {
  uint64_t read_head;
  uint64_t write_head;
  uint32_t queue_size;
  T *queue;
  bool allocated;

  SPSC_LockFree_Pool()
      : read_head(0), write_head(0), queue_size(0), allocated(false),
        queue(nullptr) {}

  uint32_t size() const { return queue_size; }

  ~SPSC_LockFree_Pool() {
    if (allocated) {
      delete[] queue;
    }
  }

  void fill_empty(uint32_t size) {
    queue = new T[size];
    queue_size = size;
    allocated = true;
  }

  void fill_append(T t) {
    queue[write_head] = t;
    ++write_head;
  }

  template <class U = T>
  typename std::enable_if<std::is_pointer<U>::value>::type
  fill_from_storage(uint32_t size, U u) {
    queue = new T[size];
    queue_size = size;
    allocated = true;
    for (uint32_t i = 0; i < size; ++i) {
      queue[i] = &u[i];
    }
    write_head = size;
  }

  void fill(uint32_t size, T *t) {
    queue = t;
    write_head = size;
    queue_size = size;
  }

  void fill_iota(uint32_t size) {
    queue = new T[size];
    for (uint32_t i = 0; i < size; ++i) {
      queue[i] = i;
    }
    write_head = size;
    queue_size = size;
    allocated = true;
  }

  void append(T t) {
    // we guarantee to only put in what was there at the beginning
    // we therefore don't need to check for overruns
    auto idx = write_head % queue_size;
    queue[idx] = t;
    atomic_add(&write_head, uint64_t(1));
  }

  T pop() {
    while (read_head == volatile_load(&write_head))
      ;
    auto idx = read_head % queue_size;
    T t = queue[idx];
    atomic_add(&read_head, uint64_t(1));
    return t;
  }
};

struct RemoteWindowConfig {
  void *reply_tx_buf;
  uint32_t reply_tx_key;
  void *reply_rx_buf;
  uint32_t reply_rx_key;
};

struct RemoteWindow {
  RemoteWindowConfig cfg;
  /** The following are fixed for the entire run */
  uint32_t local_key;
  uint32_t elem_size;
  uint32_t requester;

  /** The following change with each new request */
  uint32_t epoch;
  uint32_t reply_token;
  uint32_t offset;
  uint32_t num_entries;
};

struct RdmaWorkRequest {
  enum Type {
    SEND_SG_REQUEST,
    RECV_SG_REQUEST,
    SEND_SG_RESPONSE,
    RECV_SG_RESPONSE
  };

  Type type;
  ibv_sge sge[3];
  ibv_srq *srq;
  ibv_qp *qp;

  union {
    ibv_send_wr sr;
    ibv_recv_wr rr;
  } req;

  union {
    ibv_send_wr *sr;
    ibv_recv_wr *rr;
  } bad_req;

  void *buf;
};

struct RdmaScatterGatherEngine;

struct RdmaScatterGatherBuffer {
#ifdef KOKKOS_ENABLE_CUDA
  CUipcMemHandle handle;
#endif
  void *reply_tx_buffer;
  uint32_t reply_tx_key;
  char hostname[64];
};

struct PendingRdmaRequest {
  uint64_t start_idx;
  uint32_t num_entries;
  int pe;
  uint32_t token;
  RdmaScatterGatherEngine *sge;
};

struct RdmaScatterGatherEngine {

  // for optimization purposes, we have a statically sized queue
  constexpr static uint32_t queue_size = 1 << 20;

  RemoteWindow *get_rx_remote_window(int idx) const {
    RemoteWindow *windows = (RemoteWindow *)rx_remote_windows_mr->addr;
    return &windows[idx];
  }

  void ack_scatter_gather(PendingRdmaRequest &req);

  void ack_completed(int pe, uint64_t num_completed);

  RdmaScatterGatherEngine(MPI_Comm comm, void *buf, size_t elem_size,
                          size_t header_size);

  ~RdmaScatterGatherEngine();

  bool is_running() {
    return Kokkos::atomic_fetch_add(&terminate_signal, 0) == 0;
  }

  void stop_running() { Kokkos::atomic_add(&terminate_signal, uint32_t(1)); }

  void fence();

  void poll_requests();
  void poll_responses();
  void generate_requests();

  /** The data structures used on host and device */
public: // these are public, safe to use on device
  uint64_t *tx_element_request_ctrs;
  uint64_t *tx_element_reply_ctrs;
  uint32_t *tx_element_request_trip_counts;
  uint64_t *tx_element_aggregate_ctrs;

  uint64_t *ack_ctrs_h;
  uint64_t *ack_ctrs_d;

  void **direct_ptrs_d;

  ibv_mr *tx_element_request_queue_mr;
  ibv_mr *tx_element_reply_queue_mr;
  ibv_mr *rx_element_request_queue_mr;
  ibv_mr *rx_element_reply_queue_mr;

  unsigned *request_done_flag;
  unsigned *response_done_flag;
  unsigned *fence_done_flag;
  MPI_Comm comm;
  int num_pes;
  int rank;
  Features::Cache::RemoteCache cache;

  uint64_t *tx_block_request_cmd_queue;
  uint64_t *rx_block_request_cmd_queue;
  uint64_t *tx_block_reply_cmd_queue;
  uint64_t tx_block_request_ctr;
  uint64_t rx_block_request_ctr;
  uint64_t tx_block_reply_ctr;

  uint32_t epoch;

  /** The data structures only used on the host */
private:
  ibv_mr *rx_remote_windows_mr;
  ibv_mr *tx_remote_windows_mr;
  ibv_mr *all_request_mr;
#ifdef KOKKOS_ENABLE_CUDA
  CUipcMemHandle ipc_handle;
#endif

  pthread_t request_thread;
  pthread_t response_thread;
  pthread_t ack_thread;
  uint32_t terminate_signal;

  void **direct_ptrs_h;

  /** An array of size num_pes, contains a running count of the number
   *  of element requests actually sent each remote PE */
  uint64_t *tx_element_request_sent_ctrs;
  /** An array of size num_pes, contains a running count of the number
   *  of element requests received back from each remote PE
   *  and acked to the device */
  uint64_t *tx_element_request_acked_ctrs;

  SPSC_LockFree_Pool<RdmaWorkRequest *> available_send_request_wrs;
  SPSC_LockFree_Pool<RdmaWorkRequest *> available_send_response_wrs;
  SPSC_LockFree_Pool<RdmaWorkRequest *> available_recv_request_wrs;
  SPSC_LockFree_Pool<RdmaWorkRequest *> available_recv_response_wrs;
  std::vector<RemoteWindowConfig> tx_remote_window_configs;
  std::queue<RemoteWindow *> pending_replies;

#ifdef KOKKOS_ENABLE_CUDA
  cudaStream_t response_stream;
#endif

  struct SortAcks {
    bool operator()(PendingRdmaRequest *l, PendingRdmaRequest *r) const {
      return l->start_idx < r->start_idx;
    }
  };

  std::set<PendingRdmaRequest *, SortAcks> misordered_acks;

  SPSC_LockFree_Pool<RemoteWindow *> available_tx_windows;

  void remote_window_start_reply(RemoteWindow *win);
  void remote_window_finish_reply();
  void request_received(RdmaWorkRequest *req);
  void response_received(RdmaWorkRequest *req, uint32_t token);
  void send_remote_window(Transport *tport, int pe, uint32_t num_elements);
  void poll(Transport *tport);
  void check_for_new_block_requests();
  void ack_response(PendingRdmaRequest &req);
};

RdmaScatterGatherEngine *allocate_rdma_scatter_gather_engine(void *buf,
                                                             size_t elem_size,
                                                             size_t header_size,
                                                             MPI_Comm comm);
void free_rdma_scatter_gather_engine(RdmaScatterGatherEngine *sge);


#define heisenbug                                                              \
  printf("%s:%d\n", __FILE__, __LINE__);                                       \
  fflush(stdout)

} // namespace RACERlib

#endif // RACERLIB_RDMA_ENGINE_H