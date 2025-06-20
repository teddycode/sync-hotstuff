/**
 * Copyright 2018 VMware
 * Copyright 2018 Ted Yin
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

#include <iostream>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <random>
#include <unistd.h>
#include <signal.h>

#include "salticidae/stream.h"
#include "salticidae/util.h"
#include "salticidae/network.h"
#include "salticidae/msg.h"

#include "hotstuff/promise.hpp"
#include "hotstuff/type.h"
#include "hotstuff/entity.h"
#include "hotstuff/util.h"
#include "hotstuff/client.h"
#include "hotstuff/hotstuff.h"
#include "hotstuff/liveness.h"

using salticidae::MsgNetwork;
using salticidae::ClientNetwork;
using salticidae::ElapsedTime;
using salticidae::Config;
using salticidae::_1;
using salticidae::_2;
using salticidae::static_pointer_cast;
using salticidae::trim_all;
using salticidae::split;

using hotstuff::TimerEvent;
using hotstuff::EventContext;
using hotstuff::NetAddr;
using hotstuff::HotStuffError;
using hotstuff::CommandDummy;
using hotstuff::Finality;
using hotstuff::command_t;
using hotstuff::uint256_t;
using hotstuff::opcode_t;
using hotstuff::bytearray_t;
using hotstuff::DataStream;
using hotstuff::ReplicaID;
using hotstuff::MsgReqCmd;
using hotstuff::MsgRespCmd;
using hotstuff::get_hash;
using hotstuff::promise_t;

using HotStuff = hotstuff::HotStuffSecp256k1;

/** 根据节点id判断该副本是否是失效节点，only for test */
bool is_faulty_replica(ReplicaID rid, uint16_t faulty_size, uint16_t total_replicas) {
    if(rid > total_replicas) {
        throw std::invalid_argument("the faulty replica id exceeds total replicas");
    }
    uint16_t faulty_limit = total_replicas / 2; 
    if(faulty_size > faulty_limit) {
        throw std::invalid_argument("the number of faulty replicas exceeds the limit");
    }
    // alway consider the last `faulty_size` replicas as faulty 
    if (rid >= total_replicas - faulty_size) {
        return true; // This replica is considered faulty
    }
    return false; 
}

std::vector<ReplicaID> get_faulty_list(std::string faulty_list_str){

    std::vector<ReplicaID> faulty_replicas = {};

      if (!faulty_list_str.empty()) {
        // 移除可能的方括号
        faulty_list_str.erase(std::remove(faulty_list_str.begin(), faulty_list_str.end(), '['), faulty_list_str.end());
        faulty_list_str.erase(std::remove(faulty_list_str.begin(), faulty_list_str.end(), ']'), faulty_list_str.end());
        
        std::vector<std::string> faulty_ids = split(faulty_list_str, ",");
        for (const auto &id_str : faulty_ids) {
            try {
                ReplicaID id = std::stoi(id_str);
                faulty_replicas.push_back(id);
                HOTSTUFF_LOG_INFO("Added replica %d to faulty list", id);
            } catch (std::exception &e) {
                HOTSTUFF_LOG_WARN("Invalid replica ID in faulty-list: %s", id_str.c_str());
            }
        }
    }
    return faulty_replicas;
}

bool is_faulty_replica(uint16_t idx, std::vector<ReplicaID> faulty_replicas){
    if (faulty_replicas.empty()) {
        return false; // 如果没有故障副本，则返回false
    }
    // 检查idx是否在故障副本列表中
    for (const auto &faulty_id : faulty_replicas) {
        if (idx == faulty_id) {
            return true; // idx在故障副本列表中
        }
    }
    return false; // idx不在故障副本列表中
}

class HotStuffApp: public HotStuff {
    double stat_period;
    double impeach_timeout;
    EventContext ec;
    EventContext req_ec;
    EventContext resp_ec;
    /** Network messaging between a replica and its client. */
    ClientNetwork<opcode_t> cn;
    /** Timer object to schedule a periodic printing of system statistics */
    TimerEvent ev_stat_timer;
    /** Timer object to monitor the progress for simple impeachment */
    TimerEvent impeach_timer;
    /** The listen address for client RPC */
    NetAddr clisten_addr;

    /** enable leader fault mode */
    bool enable_leader_fault;

    // 新增领导者崩溃模拟计时器和参数
    TimerEvent leader_crash_timer;
    double leader_tenure; // 领导者任期时间（秒）

    std::unordered_map<const uint256_t, promise_t> unconfirmed;

    using conn_t = ClientNetwork<opcode_t>::conn_t;
    using resp_queue_t = salticidae::MPSCQueueEventDriven<std::pair<Finality, NetAddr>>;

    /* for the dedicated thread sending responses to the clients */
    std::thread req_thread;
    std::thread resp_thread;
    resp_queue_t resp_queue;
    salticidae::BoxObj<salticidae::ThreadCall> resp_tcall;
    salticidae::BoxObj<salticidae::ThreadCall> req_tcall;

    void client_request_cmd_handler(MsgReqCmd &&, const conn_t &);
    void simulate_leader_crash();

    static command_t parse_cmd(DataStream &s) {
        auto cmd = new CommandDummy();
        s >> *cmd;
        return cmd;
    }

    void reset_imp_timer() {
        impeach_timer.del();
        impeach_timer.add(impeach_timeout);
    }

    void state_machine_execute(const Finality &fin) override {
        reset_imp_timer();
#ifndef HOTSTUFF_ENABLE_BENCHMARK
        HOTSTUFF_LOG_INFO("replicated %s", std::string(fin).c_str());
#endif
    }

#ifdef SYNCHS_AUTOCLI
    void do_demand_commands(size_t blk_size) override {
        size_t ncli = client_conns.size();
        size_t bsize = (blk_size + ncli - 1) / ncli;
        hotstuff::MsgDemandCmd mdc{bsize};
        for(const auto &conn: client_conns)
            cn.send_msg(mdc, conn);
    }
#endif

#ifdef HOTSTUFF_MSG_STAT
    std::unordered_set<conn_t> client_conns;
    void print_stat() const;
#endif

    public:
    HotStuffApp(uint32_t blk_size,
                double stat_period,
                double impeach_timeout,
                ReplicaID idx,
                const bytearray_t &raw_privkey,
                NetAddr plisten_addr,
                NetAddr clisten_addr,
                hotstuff::pacemaker_bt pmaker,
                const EventContext &ec,
                size_t nworker,
                const Net::Config &repnet_config,
                const ClientNetwork<opcode_t>::Config &clinet_config,
                bool enable_leader_fault,
                double leader_tenure);

    void start(const std::vector<std::tuple<NetAddr, bytearray_t, bytearray_t>> &reps, double delta);
    void stop();
};

std::pair<std::string, std::string> split_ip_port_cport(const std::string &s) {
    auto ret = trim_all(split(s, ";"));
    if (ret.size() != 2)
        throw std::invalid_argument("invalid cport format");
    return std::make_pair(ret[0], ret[1]);
}

salticidae::BoxObj<HotStuffApp> papp = nullptr;

int main(int argc, char **argv) {
    Config config("./configs/hotstuff.conf");

    ElapsedTime elapsed;
    elapsed.start();

    auto opt_blk_size = Config::OptValInt::create(1);
    auto opt_parent_limit = Config::OptValInt::create(-1);
    auto opt_stat_period = Config::OptValDouble::create(10);
    auto opt_replicas = Config::OptValStrVec::create();
    auto opt_idx = Config::OptValInt::create(0);
    auto opt_client_port = Config::OptValInt::create(-1);
    auto opt_privkey = Config::OptValStr::create();
    auto opt_tls_privkey = Config::OptValStr::create();
    auto opt_tls_cert = Config::OptValStr::create();
    auto opt_help = Config::OptValFlag::create(false);
    auto opt_pace_maker = Config::OptValStr::create("dummy");
    auto opt_fixed_proposer = Config::OptValInt::create(1);
    auto opt_base_timeout = Config::OptValDouble::create(1);
    auto opt_prop_delay = Config::OptValDouble::create(1);
    auto opt_imp_timeout = Config::OptValDouble::create(12);
    auto opt_nworker = Config::OptValInt::create(1);
    auto opt_repnworker = Config::OptValInt::create(1);
    auto opt_repburst = Config::OptValInt::create(100);
    auto opt_clinworker = Config::OptValInt::create(8);
    auto opt_cliburst = Config::OptValInt::create(1000);
    auto opt_notls = Config::OptValFlag::create(false);
    auto opt_delta = Config::OptValDouble::create(1);
    auto opt_leader_fault = Config::OptValFlag::create(false); // add this option to enable the faulty leaders
    auto opt_faulty_list = Config::OptValStr::create(""); // add this option for specific faulty replica list
    double leader_tenure = 10.0; // default leader tenure time

    config.add_opt("block-size", opt_blk_size, Config::SET_VAL);
    config.add_opt("parent-limit", opt_parent_limit, Config::SET_VAL);
    config.add_opt("stat-period", opt_stat_period, Config::SET_VAL);
    config.add_opt("replica", opt_replicas, Config::APPEND, 'a', "add an replica to the list");
    config.add_opt("idx", opt_idx, Config::SET_VAL, 'i', "specify the index in the replica list");
    config.add_opt("cport", opt_client_port, Config::SET_VAL, 'c', "specify the port listening for clients");
    config.add_opt("privkey", opt_privkey, Config::SET_VAL);
    config.add_opt("tls-privkey", opt_tls_privkey, Config::SET_VAL);
    config.add_opt("tls-cert", opt_tls_cert, Config::SET_VAL);
    config.add_opt("pace-maker", opt_pace_maker, Config::SET_VAL, 'p', "specify pace maker (dummy, rr)");
    config.add_opt("proposer", opt_fixed_proposer, Config::SET_VAL, 'l', "set the fixed proposer (for dummy)");
    config.add_opt("base-timeout", opt_base_timeout, Config::SET_VAL, 't', "set the initial timeout for the Round-Robin Pacemaker");
    config.add_opt("prop-delay", opt_prop_delay, Config::SET_VAL, 't', "set the delay that follows the timeout for the Round-Robin Pacemaker");
    config.add_opt("imp-timeout", opt_imp_timeout, Config::SET_VAL, 'u', "set impeachment timeout (for sticky)");
    config.add_opt("nworker", opt_nworker, Config::SET_VAL, 'n', "the number of threads for verification");
    config.add_opt("repnworker", opt_repnworker, Config::SET_VAL, 'm', "the number of threads for replica network");
    config.add_opt("repburst", opt_repburst, Config::SET_VAL, 'b', "");
    config.add_opt("clinworker", opt_clinworker, Config::SET_VAL, 'M', "the number of threads for client network");
    config.add_opt("cliburst", opt_cliburst, Config::SET_VAL, 'B', "");
    config.add_opt("notls", opt_notls, Config::SWITCH_ON, 's', "disable TLS");
    config.add_opt("delta", opt_delta, Config::SET_VAL, 'd', "maximum network delay");
    config.add_opt("help", opt_help, Config::SWITCH_ON, 'h', "show this help info");
    config.add_opt("leader-fault", opt_leader_fault, Config::SWITCH_ON, 'F', "enable faulty leaders (default: false)");
    config.add_opt("faulty-list", opt_faulty_list, Config::SET_VAL, '\0', "specify list of faulty replicas, e.g., \"0,2,4,6,8\"");
    config.add_opt("leader-tenure", Config::OptValDouble::create(leader_tenure), Config::SET_VAL, '\0', "set the leader tenure time in seconds (default: 10.0)");

    EventContext ec;
    config.parse(argc, argv);
    if (opt_help->get())
    {
        config.print_help();
        exit(0);
    }
    auto idx = opt_idx->get();
    auto client_port = opt_client_port->get();
    std::vector<std::tuple<std::string, std::string, std::string>> replicas;
    for (const auto &s: opt_replicas->get())
    {
        auto res = trim_all(split(s, ","));
        if (res.size() != 3)
            throw HotStuffError("invalid replica info");
        replicas.push_back(std::make_tuple(res[0], res[1], res[2]));
    }

    if (!(0 <= idx && (size_t)idx < replicas.size()))
        throw HotStuffError("replica idx out of range");
    std::string binding_addr = std::get<0>(replicas[idx]);
    if (client_port == -1)
    {
        auto p = split_ip_port_cport(binding_addr);
        size_t idx;
        try {
            client_port = stoi(p.second, &idx);
        } catch (std::invalid_argument &) {
            throw HotStuffError("client port not specified");
        }
    }

    NetAddr plisten_addr{split_ip_port_cport(binding_addr).first};

    auto parent_limit = opt_parent_limit->get();
    hotstuff::pacemaker_bt pmaker;
    if (opt_pace_maker->get() == "dummy")
        pmaker = new hotstuff::PaceMakerDummyFixed(opt_fixed_proposer->get(), parent_limit);
    else
        pmaker = new hotstuff::PaceMakerRR(ec, parent_limit, opt_base_timeout->get(), opt_prop_delay->get());

    HotStuffApp::Net::Config repnet_config;
    ClientNetwork<opcode_t>::Config clinet_config;
    if (!opt_tls_privkey->get().empty() && !opt_notls->get())
    {
        auto tls_priv_key = new salticidae::PKey(
                salticidae::PKey::create_privkey_from_der(
                    hotstuff::from_hex(opt_tls_privkey->get())));
        auto tls_cert = new salticidae::X509(
                salticidae::X509::create_from_der(
                    hotstuff::from_hex(opt_tls_cert->get())));
        repnet_config
            .enable_tls(true)
            .tls_key(tls_priv_key)
            .tls_cert(tls_cert);
    }
    repnet_config
        .burst_size(opt_repburst->get())
        .nworker(opt_repnworker->get());
    clinet_config
        .burst_size(opt_cliburst->get())
        .nworker(opt_clinworker->get());

        // 读取并处理faulty-list配置
    std::vector<ReplicaID> faulty_replicas = get_faulty_list(opt_faulty_list->get());

    // check if faulty 
    if(is_faulty_replica(idx,faulty_replicas)) {
        HOTSTUFF_LOG_INFO("replica %d is considered faulty, existing", idx);
        exit(0);
    }
  
    // 读取leader-fault配置
    bool enable_leader_fault = opt_leader_fault->get();
    HOTSTUFF_LOG_INFO("leader fault mode: %s", enable_leader_fault ? "enabled" : "disabled");
    HOTSTUFF_LOG_DEBUG("leader tenure time: %.2f ms", leader_tenure);

    papp = new HotStuffApp(opt_blk_size->get(),
                        opt_stat_period->get(),
                        opt_imp_timeout->get(),
                        idx,
                        hotstuff::from_hex(opt_privkey->get()),
                        plisten_addr,
                        NetAddr("0.0.0.0", client_port),
                        std::move(pmaker),
                        ec,
                        opt_nworker->get(),
                        repnet_config,
                        clinet_config,
                        enable_leader_fault,
                        leader_tenure);
    std::vector<std::tuple<NetAddr, bytearray_t, bytearray_t>> reps;
    for (auto &r: replicas)
    {
        auto p = split_ip_port_cport(std::get<0>(r));
        reps.push_back(std::make_tuple(
            NetAddr(p.first),
            hotstuff::from_hex(std::get<1>(r)),
            hotstuff::from_hex(std::get<2>(r))));
    }
    auto shutdown = [&](int) { papp->stop(); };
    salticidae::SigEvent ev_sigint(ec, shutdown);
    salticidae::SigEvent ev_sigterm(ec, shutdown);
    ev_sigint.add(SIGINT);
    ev_sigterm.add(SIGTERM);

    papp->start(reps, opt_delta->get());
    elapsed.stop(true);
    return 0;
}

HotStuffApp::HotStuffApp(uint32_t blk_size,
                        double stat_period,
                        double impeach_timeout,
                        ReplicaID idx,
                        const bytearray_t &raw_privkey,
                        NetAddr plisten_addr,
                        NetAddr clisten_addr,
                        hotstuff::pacemaker_bt pmaker,
                        const EventContext &ec,
                        size_t nworker,
                        const Net::Config &repnet_config,
                        const ClientNetwork<opcode_t>::Config &clinet_config,
                        bool enable_leader_fault,
                        double leader_tenure):
    HotStuff(blk_size, idx, raw_privkey,
            plisten_addr, std::move(pmaker), ec, nworker, repnet_config),
    stat_period(stat_period),
    impeach_timeout(impeach_timeout),
    ec(ec),leader_tenure(leader_tenure),
    enable_leader_fault(enable_leader_fault),
    cn(req_ec, clinet_config),
    clisten_addr(clisten_addr) {
    /* prepare the thread used for sending back confirmations */
    resp_tcall = new salticidae::ThreadCall(resp_ec);
    req_tcall = new salticidae::ThreadCall(req_ec);
    resp_queue.reg_handler(resp_ec, [this](resp_queue_t &q) {
        std::pair<Finality, NetAddr> p;
        while (q.try_dequeue(p))
        {
            try {
                cn.send_msg(MsgRespCmd(std::move(p.first)), p.second);
            } catch (std::exception &err) {
                HOTSTUFF_LOG_WARN("unable to send to the client: %s", err.what());
            }
        }
        return false;
    });

    /* register the handlers for msg from clients */
    cn.reg_handler(salticidae::generic_bind(&HotStuffApp::client_request_cmd_handler, this, _1, _2));
    cn.start();
    cn.listen(clisten_addr);
}

void HotStuffApp::client_request_cmd_handler(MsgReqCmd &&msg, const conn_t &conn) {
    const NetAddr addr = conn->get_addr();
    auto cmd = parse_cmd(msg.serialized);
    const auto &cmd_hash = cmd->get_hash();
    // HOTSTUFF_LOG_DEBUG("processing %s", std::string(*cmd).c_str());
    exec_command(cmd_hash, [this, addr](Finality fin) {
        resp_queue.enqueue(std::make_pair(fin, addr));
    });
}

 // 模拟领导者崩溃的方法
void HotStuffApp::simulate_leader_crash() {
    if (!enable_leader_fault) return;
    
    ReplicaID current_proposer = get_pace_maker()->get_proposer();
    ReplicaID my_id = get_id();
    
    HOTSTUFF_LOG_INFO("检查领导者崩溃条件: 当前领导者=%d, 我的ID=%d", current_proposer, my_id);
    
    // 如果我是当前领导者，则模拟崩溃（触发视图变更）
    if (current_proposer == my_id) {
        HOTSTUFF_LOG_INFO("我是当前领导者，模拟崩溃...");
        // 强制触发视图变更
        get_pace_maker()->impeach();
    }
    
    // 重置计时器，继续监控下一个领导者
    leader_crash_timer.add(leader_tenure);
}

void HotStuffApp::start(const std::vector<std::tuple<NetAddr, bytearray_t, bytearray_t>> &reps,
                        double delta) {
    ev_stat_timer = TimerEvent(ec, [this](TimerEvent &) {
        HotStuff::print_stat();
        HotStuffApp::print_stat();
        //HotStuffCore::prune(100);
        ev_stat_timer.add(stat_period);
    });
    ev_stat_timer.add(stat_period);
    impeach_timer = TimerEvent(ec, [this](TimerEvent &) {
        if (get_decision_waiting().size()){
            HOTSTUFF_LOG_DEBUG("impeaching the pace maker");
            get_pace_maker()->impeach();
        }
        reset_imp_timer();
    });
    impeach_timer.add(impeach_timeout);

    HOTSTUFF_LOG_INFO("** initializing the system %d**",enable_leader_fault);
    // 添加领导者崩溃计时器
    if (enable_leader_fault) {
        leader_crash_timer = TimerEvent(ec, [this](TimerEvent &) {
            simulate_leader_crash();
        });
        leader_crash_timer.add(leader_tenure);
        HOTSTUFF_LOG_INFO("已启用领导者崩溃模拟，每 %.2f 秒触发一次", leader_tenure);
    }

    HOTSTUFF_LOG_INFO("** starting the system with parameters **");
    HOTSTUFF_LOG_INFO("blk_size = %lu", blk_size);
    HOTSTUFF_LOG_INFO("conns = %lu", HotStuff::size());
    HOTSTUFF_LOG_INFO("delta = %.4f", delta);
    HOTSTUFF_LOG_INFO("** starting the event loop...");
    HotStuff::start(reps, delta);
    cn.reg_conn_handler([this](const salticidae::ConnPool::conn_t &_conn, bool connected) {
        auto conn = salticidae::static_pointer_cast<conn_t::type>(_conn);
        if (connected)
            client_conns.insert(conn);
        else
            client_conns.erase(conn);
        return true;
    });
    req_thread = std::thread([this]() { req_ec.dispatch(); });
    resp_thread = std::thread([this]() { resp_ec.dispatch(); });
    /* enter the event main loop */
    ec.dispatch();
}

void HotStuffApp::stop() {
    papp->req_tcall->async_call([this](salticidae::ThreadCall::Handle &) {
        req_ec.stop();
    });
    papp->resp_tcall->async_call([this](salticidae::ThreadCall::Handle &) {
        resp_ec.stop();
    });

    req_thread.join();
    resp_thread.join();
    ec.stop();
}

void HotStuffApp::print_stat() const {
#ifdef HOTSTUFF_MSG_STAT
    HOTSTUFF_LOG_INFO("--- client msg. (10s) ---");
    size_t _nsent = 0;
    size_t _nrecv = 0;
    for (const auto &conn: client_conns)
    {
        if (conn == nullptr) continue;
        size_t ns = conn->get_nsent();
        size_t nr = conn->get_nrecv();
        size_t nsb = conn->get_nsentb();
        size_t nrb = conn->get_nrecvb();
        conn->clear_msgstat();
        HOTSTUFF_LOG_INFO("%s: %u(%u), %u(%u)",
            std::string(conn->get_addr()).c_str(), ns, nsb, nr, nrb);
        _nsent += ns;
        _nrecv += nr;
    }
    HOTSTUFF_LOG_INFO("--- end client msg. ---");
#endif
}
