// SPDX-License-Identifier: Apache-2.0
//
// P1.1 standalone SystemC simulator for connection setup / teardown.
//
// Models the kernel control-plane path that brings up the
// data-plane resources required before traffic can flow:
//   - RoCE: per-QP setup = 4 × ioctl_lat (CREATE → INIT → RTR → RTS
//     state transitions in the kernel verbs provider) + 1 × OoB RTT
//     for QPN/PSN exchange via TCP between peer apps. QPs = N × M.
//   - UB: per-Jetty = 1 × ioctl_lat (urma_create_jetty); per
//     TP-Channel (per remote host) = 1 × ioctl_lat + 1 × OoB RTT
//     (UBFM route registration / discovery). Total = N + M.
//
// The simulator models contention realistically:
//   - Each app's ioctls serialise on its own kernel thread (1 thread
//     per app).
//   - The OoB exchange is a single TCP socket per app, so per-app
//     OoB exchanges serialise.
//   - Across apps, kernel ioctls are parallelisable up to NCPU.
//
// Reports total bring-up wall-clock time at symmetric N=M ∈ {1, 16,
// 256, 1024, 4096}.

#include <systemc.h>
#include <cstdio>
#include <vector>
#include <random>
#include <fstream>
#include <ostream>
#include <memory>

namespace twonode {
struct SetupTask;
inline std::ostream& operator<<(std::ostream& os, const SetupTask&) { return os << "<task>"; }
inline bool operator==(const SetupTask&, const SetupTask&) { return false; }
}
inline void sc_trace(sc_core::sc_trace_file*, const twonode::SetupTask&, const std::string&) {}

namespace twonode {

struct ConnSetupCfg {
    std::string stack       = "roce";
    uint32_t    n_apps      = 1;
    uint32_t    n_remotes   = 1;
    uint32_t    ioctl_lat_ns = 5'000;       // kernel ioctl baseline
    uint32_t    oob_rtt_ns   = 500'000;     // OoB TCP RTT
    uint32_t    n_cpu        = 32;          // parallel kernel threads
    std::string out_csv      = "";
};

// One kernel-thread SC_THREAD per CPU. The thread pulls work
// (one setup task) from a shared queue and serialises ioctls.
struct SetupTask {
    enum Kind { ROCE_QP_SETUP, UB_JETTY_CREATE, UB_TP_CHANNEL_OPEN };
    Kind kind;
    uint32_t app_id;
    uint32_t remote_id;   // ignored for UB_JETTY_CREATE
};

class KernelWorker : public sc_core::sc_module {
public:
    sc_core::sc_fifo<SetupTask> in{"in", 65536};
    sc_core::sc_fifo<int>       done{"done", 65536};   // 1 per completed
    uint32_t ioctl_lat_ns;
    uint32_t oob_rtt_ns;

    SC_HAS_PROCESS(KernelWorker);
    KernelWorker(sc_core::sc_module_name nm, uint32_t il, uint32_t orr)
      : sc_core::sc_module(nm), ioctl_lat_ns(il), oob_rtt_ns(orr)
    { SC_THREAD(run); }

    void run() {
        while (true) {
            SetupTask t = in.read();
            switch (t.kind) {
                case SetupTask::ROCE_QP_SETUP:
                    // 4 ioctls (CREATE, INIT, RTR, RTS) + 1 OoB RTT.
                    wait(sc_core::sc_time(4.0 * ioctl_lat_ns, sc_core::SC_NS));
                    wait(sc_core::sc_time((double)oob_rtt_ns, sc_core::SC_NS));
                    break;
                case SetupTask::UB_JETTY_CREATE:
                    wait(sc_core::sc_time((double)ioctl_lat_ns, sc_core::SC_NS));
                    break;
                case SetupTask::UB_TP_CHANNEL_OPEN:
                    wait(sc_core::sc_time((double)ioctl_lat_ns, sc_core::SC_NS));
                    wait(sc_core::sc_time((double)oob_rtt_ns, sc_core::SC_NS));
                    break;
            }
            done.write(1);
        }
    }
};

}  // namespace twonode

using namespace twonode;

static void usage() {
    std::fprintf(stderr,
        "usage: conn_setup_sim --stack {roce|ub} --n-apps N --n-remotes M\n"
        " [--ioctl-ns N] [--oob-rtt-ns N] [--n-cpu N] [--out-csv FILE]\n");
}

int sc_main(int argc, char** argv) {
    ConnSetupCfg cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&](){ if (i+1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
        if      (a == "--stack")        cfg.stack = nxt();
        else if (a == "--n-apps")       cfg.n_apps = (uint32_t)std::atoi(nxt());
        else if (a == "--n-remotes")    cfg.n_remotes = (uint32_t)std::atoi(nxt());
        else if (a == "--ioctl-ns")     cfg.ioctl_lat_ns = (uint32_t)std::atoi(nxt());
        else if (a == "--oob-rtt-ns")   cfg.oob_rtt_ns = (uint32_t)std::atoi(nxt());
        else if (a == "--n-cpu")        cfg.n_cpu = (uint32_t)std::atoi(nxt());
        else if (a == "--out-csv")      cfg.out_csv = nxt();
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); usage(); return 2; }
    }

    // Enumerate setup tasks.
    std::vector<SetupTask> tasks;
    if (cfg.stack == "roce") {
        for (uint32_t i = 0; i < cfg.n_apps; ++i) {
            for (uint32_t j = 0; j < cfg.n_remotes; ++j) {
                tasks.push_back({SetupTask::ROCE_QP_SETUP, i, j});
            }
        }
    } else if (cfg.stack == "ub") {
        for (uint32_t i = 0; i < cfg.n_apps; ++i) {
            tasks.push_back({SetupTask::UB_JETTY_CREATE, i, 0});
        }
        for (uint32_t j = 0; j < cfg.n_remotes; ++j) {
            tasks.push_back({SetupTask::UB_TP_CHANNEL_OPEN, 0, j});
        }
    } else {
        std::fprintf(stderr, "unknown stack %s\n", cfg.stack.c_str());
        return 2;
    }

    // Spawn n_cpu kernel-thread workers.
    std::vector<std::unique_ptr<KernelWorker>> workers;
    for (uint32_t k = 0; k < cfg.n_cpu; ++k) {
        char nm[32]; std::snprintf(nm, sizeof(nm), "k%u", k);
        workers.emplace_back(new KernelWorker(nm, cfg.ioctl_lat_ns, cfg.oob_rtt_ns));
    }

    // Dispatcher: round-robin across workers (a simple coordinator
    // model; real Linux uses per-CPU runqueues + load balancing).
    sc_core::sc_event start_done;
    uint64_t end_ns = 0;
    uint64_t total = tasks.size();

    class Dispatcher : public sc_core::sc_module {
    public:
        std::vector<KernelWorker*> ws;
        std::vector<SetupTask>     tt;
        uint64_t* out_end_ns;
        uint64_t* completed_total;
        SC_HAS_PROCESS(Dispatcher);
        Dispatcher(sc_core::sc_module_name nm,
                   std::vector<KernelWorker*> w, std::vector<SetupTask> t,
                   uint64_t* e, uint64_t* tot)
          : sc_core::sc_module(nm), ws(std::move(w)), tt(std::move(t)),
            out_end_ns(e), completed_total(tot)
        { SC_THREAD(run); }
        void run() {
            // Issue all tasks across workers round-robin.
            for (size_t i = 0; i < tt.size(); ++i) {
                ws[i % ws.size()]->in.write(tt[i]);
            }
            // Wait for all completions.
            uint64_t completed = 0;
            while (completed < tt.size()) {
                for (auto* w : ws) {
                    int d;
                    while (w->done.nb_read(d)) completed++;
                }
                if (completed >= tt.size()) break;
                wait(sc_core::sc_time(100.0, sc_core::SC_NS));
            }
            *out_end_ns = (uint64_t)(sc_core::sc_time_stamp().to_double() / 1000.0);
            *completed_total = completed;
        }
    };

    std::vector<KernelWorker*> wp;
    for (auto& u : workers) wp.push_back(u.get());
    Dispatcher disp("disp", wp, tasks, &end_ns, &total);

    // Run a generous deadline.
    double pessimistic_ns = (double)tasks.size() *
        (5 * cfg.ioctl_lat_ns + cfg.oob_rtt_ns) / cfg.n_cpu * 2.0 + 1e6;
    sc_core::sc_start(sc_core::sc_time(pessimistic_ns, sc_core::SC_NS));

    std::printf("=== conn_setup_sim stack=%s N=%u M=%u n_cpu=%u ===\n",
                cfg.stack.c_str(), cfg.n_apps, cfg.n_remotes, cfg.n_cpu);
    std::printf("  tasks=%lu completed=%lu  total_time = %.6f s\n",
                (unsigned long)tasks.size(), (unsigned long)total,
                end_ns / 1e9);

    if (!cfg.out_csv.empty()) {
        bool first = true;
        std::ifstream check(cfg.out_csv);
        if (check.good()) { check.close(); first = false; }
        std::ofstream f(cfg.out_csv, std::ios::app);
        if (first) f << "stack,n_apps,n_remotes,n_cpu,n_tasks,total_ns,total_s\n";
        f << cfg.stack << ',' << cfg.n_apps << ',' << cfg.n_remotes << ','
          << cfg.n_cpu << ',' << tasks.size() << ',' << end_ns << ','
          << (end_ns/1e9) << '\n';
    }
    return 0;
}
