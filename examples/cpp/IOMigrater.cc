/*
 * IOMigrater - Migrate I/O intensive thread to available vCPU to 
 *			    improve system performance, fairness and scalability.
 *
 * Weiwei Jia <harryxiyou@gmail.com> (C/C++) 2017
 *
 * Licensed under the Apache License, Version 2.0 (the "License")
 */

#include <unistd.h>
#include <stdlib.h>
#include <algorithm>
#include <iostream>
#include <sys/signalfd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sched.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/shm.h>
#include <sys/ipc.h

//#include "debug.h"
#include "glib-2.0/glib.h"
#include "BPF.h"

#define handle_error(msg) \
	do { perror(msg); exit(EXIT_FAILURE); } while (0)

const std::string BPF_PROGRAM = R"(
#include <uapi/linux/ptrace.h>
#include <linux/blkdev.h>

// for saving process info by request
struct who_t {
    u32 pid;
    char name[TASK_COMM_LEN];
};

// the key for the output summary
struct info_t {
    u32 pid;
    int rwflag;
    int major;
    int minor;
    char name[TASK_COMM_LEN];
};

// the value of the output summary
struct val_t {
    u64 bytes;
    u64 ns; //changed by Weiwei Jia
    u32 io;
};

BPF_HASH(start, struct request *);
BPF_HASH(whobyreq, struct request *, struct who_t);
BPF_HASH(counts, struct info_t, struct val_t);

// cache PID and comm by-req
int trace_pid_start(struct pt_regs *ctx, struct request *req)
{
    struct who_t who = {};

    if (bpf_get_current_comm(&who.name, sizeof(who.name)) == 0) {
        who.pid = bpf_get_current_pid_tgid();
        whobyreq.update(&req, &who);
    }

    return 0;
}

// time block I/O
int trace_req_start(struct pt_regs *ctx, struct request *req)
{
    u64 ts;

    ts = bpf_ktime_get_ns();
    start.update(&req, &ts);

    return 0;
}

// output
int trace_req_completion(struct pt_regs *ctx, struct request *req)
{
    u64 *tsp;

    // fetch timestamp and calculate delta
    tsp = start.lookup(&req);
    if (tsp == 0) {
        return 0;    // missed tracing issue
    }

    struct who_t *whop;
    struct val_t *valp, zero = {};
    u64 delta_ns = bpf_ktime_get_ns() - *tsp;

    // setup info_t key
    struct info_t info = {};
    info.major = req->rq_disk->major;
    info.minor = req->rq_disk->first_minor;
/*
 * The following deals with a kernel version change (in mainline 4.7, although
 * it may be backported to earlier kernels) with how block request write flags
 * are tested. We handle both pre- and post-change versions here. Please avoid
 * kernel version tests like this as much as possible: they inflate the code,
 * test, and maintenance burden.
 */
#ifdef REQ_WRITE
    info.rwflag = !!(req->cmd_flags & REQ_WRITE);
#elif defined(REQ_OP_SHIFT)
    info.rwflag = !!((req->cmd_flags >> REQ_OP_SHIFT) == REQ_OP_WRITE);
#else
    info.rwflag = !!((req->cmd_flags & REQ_OP_MASK) == REQ_OP_WRITE);
#endif

    whop = whobyreq.lookup(&req);
    if (whop == 0) {
        // missed pid who, save stats as pid 0
        valp = counts.lookup_or_init(&info, &zero);
    } else {
        info.pid = whop->pid;
        __builtin_memcpy(&info.name, whop->name, sizeof(info.name));
        valp = counts.lookup_or_init(&info, &zero);
    }

    // save stats
    //bpf_trace_printk("%lu\\n", valp->us);
    valp->ns += delta_ns;
    valp->bytes += req->__data_len;
    valp->io++;

    start.delete(&req);
    whobyreq.delete(&req);

    return 0;
}
)";

// Define the same struct to use in user space.
// for saving process info by request
struct who_t {
    u32 pid;
    char name[TASK_COMM_LEN];
};

// the key for the output summary
struct info_t {
    u32 pid;
    int rwflag;
    int major;
    int minor;
    char name[TASK_COMM_LEN];
};

// the value of the output summary
struct val_t {
    u64 bytes;
    u64 ns; //changed by Weiwei Jia
    u32 io;
};

void attach(void) {
  auto attach_res = bpf.attach_kprobe("blk_account_io_start", "trace_pid_start");
  if (attach_res.code() != 0) {
    std::cerr << attach_res.msg() << std::endl;
    return 1;
  }
  attach_res = bpf.attach_kprobe("blk_start_request", "trace_req_start");
  if (attach_res.code() != 0) {
    std::cerr << attach_res.msg() << std::endl;
    return 1;
  }
  attach_res = bpf.attach_kprobe("blk_mq_start_request", "trace_req_start");
  if (attach_res.code() != 0) {
    std::cerr << attach_res.msg() << std::endl;
    return 1;
  }
  attach_res = bpf.attach_kprobe("blk_account_io_completion", "trace_req_completion");
  if (attach_res.code() != 0) {
    std::cerr << attach_res.msg() << std::endl;
    return 1;
  }
  
  return ;
}

void detach(void) {
	auto detach_res = bpf.detach_kprobe("blk_account_io_start");
	if (detach_res.code() != 0) {
		std::cerr << detach_res.msg() << std::endl;
		return 1;
	}
	detach_res = bpf.detach_kprobe("blk_start_request");
	if (detach_res.code() != 0) {
		std::cerr << detach_res.msg() << std::endl;
		return 1;
	}
	detach_res = bpf.detach_kprobe("blk_mq_start_request");
	if (detach_res.code() != 0) {
		std::cerr << detach_res.msg() << std::endl;
		return 1;
	}
	detach_res = bpf.detach_kprobe("blk_account_io_completion");
	if (detach_res.code() != 0) {
		std::cerr << detach_res.msg() << std::endl;
		return 1;
	}
  
  return ;
}

void sig_handler(int signo) {
	if (signo == SIGINT) {
		printf("Detaching and free resources...");
		detach();
	} else {
		handle_error("Signal Error!\n");
	}
	
	exit(EXIT_SUCCESS);
}

int main(int argc, char** argv) {
  ebpf::BPF bpf;
  auto init_res = bpf.init(BPF_PROGRAM);
  if (init_res.code() != 0) {
    std::cerr << init_res.msg() << std::endl;
    return 1;
  }
  
  if (signal(SIGINT, sig_handler) == SIG_ERR) {
	handle_error("SIGINT error!\n");
  }
  attach();
  
  int probe_time = 10;  // 10 milliseconds in default
  if (argc == 2) {
    probe_time = atoi(argv[1]);
  }
  std::cout << "Filering I/O intensive threads for " << probe_time << " milliseconds" << std::endl;

  while (1) {
	  usleep(probe_time);
	  printf("It is still trying to find I/O intensive threads...\n");
#if 0
	  auto table =
		  bpf.get_hash_table<stack_key_t, uint64_t>("counts").get_table_offline();
	  std::sort(table.begin(), table.end(), [](std::pair<stack_key_t, uint64_t> a,
											   std::pair<stack_key_t, uint64_t> b) {
		return a.second < b.second;
	  });
	  auto stacks = bpf.get_stack_table("stack_traces");

	  for (auto it : table) {
		std::cout << "PID: " << it.first.pid << " (" << it.first.name << ") "
				  << "made " << it.second
				  << " TCP sends on following stack: " << std::endl;
		std::cout << "  Kernel Stack:" << std::endl;
		if (it.first.kernel_stack >= 0) {
		  auto syms = stacks.get_stack_symbol(it.first.kernel_stack, -1);
		  for (auto sym : syms)
			std::cout << "    " << sym << std::endl;
		} else
		  std::cout << "    " << it.first.kernel_stack << std::endl;
		std::cout << "  User Stack:" << std::endl;
		if (it.first.user_stack >= 0) {
		  auto syms = stacks.get_stack_symbol(it.first.user_stack, it.first.pid);
		  for (auto sym : syms)
			std::cout << "    " << sym << std::endl;
		} else
		  std::cout << "    " << it.first.user_stack << std::endl;
	  }
#endif
  }
  return 0;
}
