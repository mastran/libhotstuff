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

#include "hotstuff/hotstuff.h"
#include "hotstuff/client.h"

using salticidae::static_pointer_cast;

#define LOG_INFO HOTSTUFF_LOG_INFO
#define LOG_DEBUG HOTSTUFF_LOG_DEBUG
#define LOG_WARN HOTSTUFF_LOG_WARN

namespace hotstuff {

const opcode_t MsgPropose::opcode;
MsgPropose::MsgPropose(const Proposal &proposal) { serialized << proposal; }
void MsgPropose::postponed_parse(HotStuffCore *hsc) {
    proposal.hsc = hsc;
    serialized >> proposal;
}

const opcode_t MsgVote::opcode;
MsgVote::MsgVote(const Vote &vote) { serialized << vote; }
void MsgVote::postponed_parse(HotStuffCore *hsc) {
    vote.hsc = hsc;
    serialized >> vote;
}

const opcode_t MsgStatus::opcode;
MsgStatus::MsgStatus(const Status &status) { serialized << status; }
void MsgStatus::postponed_parse(HotStuffCore *hsc) {
    status.hsc = hsc;
    serialized >> status;
}

const opcode_t MsgBlame::opcode;
MsgBlame::MsgBlame(const Blame &blame) { serialized << blame; }
void MsgBlame::postponed_parse(HotStuffCore *hsc) {
    blame.hsc = hsc;
    serialized >> blame;
}

const opcode_t MsgBlameNotify::opcode;
MsgBlameNotify::MsgBlameNotify(const BlameNotify &bn) { serialized << bn; }
void MsgBlameNotify::postponed_parse(HotStuffCore *hsc) {
    bn.hsc = hsc;
    serialized >> bn;
}

const opcode_t MsgNotify::opcode;
MsgNotify::MsgNotify(const hotstuff::Notify &notify) {serialized << notify;}
void MsgNotify::postponed_parse(HotStuffCore *hsc) {
    notify.hsc = hsc;
    serialized >> notify;
}

const opcode_t MsgNewView::opcode;
MsgNewView::MsgNewView(const Status &status) { serialized << status; }
void MsgNewView::postponed_parse(HotStuffCore *hsc) {
    status.hsc = hsc;
    serialized >> status;
}

const opcode_t MsgReqBlock::opcode;
MsgReqBlock::MsgReqBlock(const std::vector<uint256_t> &blk_hashes) {
    serialized << htole((uint32_t)blk_hashes.size());
    for (const auto &h: blk_hashes)
        serialized << h;
}

MsgReqBlock::MsgReqBlock(DataStream &&s) {
    uint32_t size;
    s >> size;
    size = letoh(size);
    blk_hashes.resize(size);
    for (auto &h: blk_hashes) s >> h;
}

const opcode_t MsgRespBlock::opcode;
MsgRespBlock::MsgRespBlock(const std::vector<block_t> &blks) {
    serialized << htole((uint32_t)blks.size());
    for (auto blk: blks) serialized << *blk;
}

void MsgRespBlock::postponed_parse(HotStuffCore *hsc) {
    uint32_t size;
    serialized >> size;
    size = letoh(size);
    blks.resize(size);
    for (auto &blk: blks)
    {
        Block _blk;
        _blk.unserialize(serialized, hsc);
        blk = hsc->storage->add_blk(std::move(_blk), hsc->get_config());
    }
}

// TODO: improve this function
void HotStuffBase::exec_command(uint256_t cmd_hash, commit_cb_t callback) {
    cmd_pending.enqueue(std::make_pair(cmd_hash, callback));
}

void HotStuffBase::on_fetch_blk(const block_t &blk) {
#ifdef HOTSTUFF_BLK_PROFILE
    blk_profiler.get_tx(blk->get_hash());
#endif
    LOG_DEBUG("fetched %.10s", get_hex(blk->get_hash()).c_str());
    part_fetched++;
    fetched++;
    //for (auto cmd: blk->get_cmds()) on_fetch_cmd(cmd);
    const uint256_t &blk_hash = blk->get_hash();
    auto it = blk_fetch_waiting.find(blk_hash);
    if (it != blk_fetch_waiting.end())
    {
        it->second.resolve(blk);
        blk_fetch_waiting.erase(it);
    }
}

void HotStuffBase::on_deliver_blk(const block_t &blk) {
    const uint256_t &blk_hash = blk->get_hash();
    bool valid;
    /* sanity check: all parents must be delivered */
    for (const auto &p: blk->get_parent_hashes())
        assert(storage->is_blk_delivered(p));
    if ((valid = HotStuffCore::on_deliver_blk(blk)))
    {
        LOG_DEBUG("block %.10s delivered",
                get_hex(blk_hash).c_str());
        part_parent_size += blk->get_parent_hashes().size();
        part_delivered++;
        delivered++;
    }
    else
    {
        LOG_WARN("dropping invalid block");
    }

    auto it = blk_delivery_waiting.find(blk_hash);
    if (it != blk_delivery_waiting.end())
    {
        auto &pm = it->second;
        if (valid)
        {
            pm.elapsed.stop(false);
            auto sec = pm.elapsed.elapsed_sec;
            part_delivery_time += sec;
            part_delivery_time_min = std::min(part_delivery_time_min, sec);
            part_delivery_time_max = std::max(part_delivery_time_max, sec);

            pm.resolve(blk);
        }
        else
        {
            pm.reject(blk);
            // TODO: do we need to also free it from storage?
        }
        blk_delivery_waiting.erase(it);
    }
}

promise_t HotStuffBase::async_fetch_blk(const uint256_t &blk_hash,
                                        const NetAddr *replica_id,
                                        bool fetch_now) {
    if (storage->is_blk_fetched(blk_hash))
        return promise_t([this, &blk_hash](promise_t pm){
            pm.resolve(storage->find_blk(blk_hash));
        });
    auto it = blk_fetch_waiting.find(blk_hash);
    if (it == blk_fetch_waiting.end())
    {
#ifdef HOTSTUFF_BLK_PROFILE
        blk_profiler.rec_tx(blk_hash, false);
#endif
        it = blk_fetch_waiting.insert(
            std::make_pair(
                blk_hash,
                BlockFetchContext(blk_hash, this))).first;
    }
    if (replica_id != nullptr)
        it->second.add_replica(*replica_id, fetch_now);
    return static_cast<promise_t &>(it->second);
}

promise_t HotStuffBase::async_deliver_blk(const uint256_t &blk_hash,
                                        const NetAddr &replica_id) {
    if (storage->is_blk_delivered(blk_hash))
        return promise_t([this, &blk_hash](promise_t pm) {
            pm.resolve(storage->find_blk(blk_hash));
        });
    auto it = blk_delivery_waiting.find(blk_hash);
    if (it != blk_delivery_waiting.end())
        return static_cast<promise_t &>(it->second);
    BlockDeliveryContext pm{[](promise_t){}};
    it = blk_delivery_waiting.insert(std::make_pair(blk_hash, pm)).first;
    /* otherwise the on_deliver_batch will resolve */
    async_fetch_blk(blk_hash, &replica_id).then([this, replica_id](block_t blk) {
        /* qc_ref should be fetched */
        std::vector<promise_t> pms;
        const auto &qc = blk->get_qc();
        if (qc)
            pms.push_back(async_fetch_blk(blk->get_qc_ref_hash(), &replica_id));
        /* the parents should be delivered */
        for (const auto &phash: blk->get_parent_hashes())
            pms.push_back(async_deliver_blk(phash, replica_id));
        if (blk != get_genesis())
            pms.push_back(blk->verify(get_config(), vpool));
        promise::all(pms).then([this, blk]() {
            on_deliver_blk(blk);
        });
    });
    return static_cast<promise_t &>(pm);
}

void HotStuffBase::propose_handler(MsgPropose &&msg, const Net::conn_t &conn) {
    const NetAddr &peer = conn->get_peer();
    msg.postponed_parse(this);
    auto &prop = msg.proposal;
    block_t blk = prop.blk;
    if (!blk) return;
    promise::all(std::vector<promise_t>{
        async_deliver_blk(blk->get_hash(), peer)
    }).then([this, prop = std::move(prop)]() {
        on_receive_proposal(prop);
    });
}

void HotStuffBase::vote_handler(MsgVote &&msg, const Net::conn_t &conn) {
    const NetAddr &peer = conn->get_peer();
    msg.postponed_parse(this);
    //auto &vote = msg.vote;
    RcObj<Vote> v(new Vote(std::move(msg.vote)));
    promise::all(std::vector<promise_t>{
        async_deliver_blk(v->blk_hash, peer),
        v->verify(vpool),
    }).then([this, v=std::move(v)](const promise::values_t values) {
        if (!promise::any_cast<bool>(values[1]))
            LOG_WARN("invalid vote from %d", v->voter);
        else
            on_receive_vote(*v);
    });
}

promise_t HotStuffBase::verify_notify(Notify &notify){
    block_t blk = storage->find_blk(notify.blk_hash);
    if(blk->get_decision() == 1){
        promise_t pm;
        return pm.then([]{ return true;});
    }
    return notify.verify(vpool);
}

void HotStuffBase::notify_handler(MsgNotify &&msg, const Net::conn_t &conn){
    const NetAddr &peer = conn->get_peer();
    if (peer.is_null()) return;
    msg.postponed_parse(this);

    RcObj<Notify> n(new Notify(std::move(msg.notify)));
        promise::all(std::vector<promise_t>{
                async_deliver_blk(n->blk_hash, peer),
                verify_notify(*n)
        }).then([this, n, peer](const promise::values_t values) {
            if (!promise::any_cast<bool>(values[1]))
                LOG_WARN("invalid notify from %s", std::string(peer).c_str());
            else
                on_receive_notify(*n);
        });
}

void HotStuffBase::status_handler(MsgStatus &&msg, const Net::conn_t &conn) {
    const NetAddr &peer = conn->get_peer();
    if (peer.is_null()) return;
    msg.postponed_parse(this);
    RcObj<Status> s(new Status(std::move(msg.status)));
    promise::all(std::vector<promise_t>{
        async_deliver_blk(s->hqc_blk_hash, peer),
        s->verify(vpool)
    }).then([this, s, peer](const promise::values_t values) {
        if (!promise::any_cast<bool>(values[1]))
            LOG_WARN("invalid status message from %s", std::string(peer).c_str());
        else
            on_receive_status(*s);
    });
}

void HotStuffBase::blame_handler(MsgBlame &&msg, const Net::conn_t &conn) {
    const NetAddr &peer = conn->get_peer();
    if (peer.is_null()) return;
    msg.postponed_parse(this);
    RcObj<Blame> b(new Blame(std::move(msg.blame)));
    b->verify(vpool).then([this, b, peer](bool result) {
        if (!result)
            LOG_WARN("invalid blame message from %s", std::string(peer).c_str());
        else
            on_receive_blame(*b);
    });
}

void HotStuffBase::blamenotify_handler(MsgBlameNotify &&msg, const Net::conn_t &conn) {
    const NetAddr &peer = conn->get_peer();
    if (peer.is_null()) return;
    msg.postponed_parse(this);
    RcObj<BlameNotify> bn(new BlameNotify(std::move(msg.bn)));
    promise::all(std::vector<promise_t>{
        async_deliver_blk(bn->hqc_hash, peer),
        bn->verify(vpool)
    }).then([this, bn, peer](promise::values_t values) {
        auto result = promise::any_cast<bool>(values[1]);
        if (!result)
            LOG_WARN("invalid blamenotify message from %s", std::string(peer).c_str());
        else
            on_receive_blamenotify(*bn);
    });
}

void HotStuffBase::new_view_handler(hotstuff::MsgNewView &&msg, const Net::conn_t &conn) {
    const NetAddr &peer = conn->get_peer();
    if (peer.is_null()) return;
    msg.postponed_parse(this);
    RcObj<Status> s(new Status(std::move(msg.status)));
    promise::all(std::vector<promise_t>{
            async_deliver_blk(s->hqc_blk_hash, peer),
            s->verify(vpool)
    }).then([this, s, peer](const promise::values_t values) {
        if (!promise::any_cast<bool>(values[1]))
            LOG_WARN("invalid status message from %s", std::string(peer).c_str());
        else
            on_receive_new_view(*s);
    });

}


void HotStuffBase::set_commit_timer(const block_t &blk, double t_sec) {
#ifdef SYNCHS_NOTIMER
    on_commit_timeout(blk);
#else
    auto height = blk->get_height();
    auto &timer = commit_timers[height] =
        TimerEvent(ec, [this, blk=std::move(blk), height](TimerEvent &) {
            on_commit_timeout(blk);
            stop_commit_timer(height);
        });
    timer.add(t_sec);
#endif
}

void HotStuffBase::stop_commit_timer(uint32_t height) {
    commit_timers.erase(height);
}

void HotStuffBase::stop_commit_timer_all() {
    commit_timers.clear();
}

void HotStuffBase::set_blame_timer(double t_sec) {
    blame_timer = TimerEvent(ec, [this](TimerEvent &) {
        on_blame_timeout();
        stop_blame_timer();
    });
    blame_timer.add(t_sec);
}

void HotStuffBase::stop_blame_timer() {
    blame_timer.clear();
}

void HotStuffBase::reset_blame_timer(double t_sec){
    stop_blame_timer();
    set_blame_timer(t_sec);
}

void HotStuffBase::set_viewtrans_timer(double t_sec) {
    viewtrans_timer = TimerEvent(ec, [this](TimerEvent &) {
        on_viewtrans_timeout();
        stop_viewtrans_timer();
    });
    viewtrans_timer.add(t_sec);
}

void HotStuffBase::stop_viewtrans_timer() {
    viewtrans_timer.clear();
}

void HotStuffBase::set_status_timer(double t_sec) {
    status_timer = TimerEvent(ec, [this](TimerEvent &){
        on_status_timeout();
        stop_status_timer();
    });
    status_timer.add(t_sec);
}

void HotStuffBase::stop_status_timer() {
    status_timer.clear();
}


void HotStuffBase::req_blk_handler(MsgReqBlock &&msg, const Net::conn_t &conn) {
    const NetAddr replica = conn->get_peer();
    auto &blk_hashes = msg.blk_hashes;
    std::vector<promise_t> pms;
    for (const auto &h: blk_hashes)
        pms.push_back(async_fetch_blk(h, nullptr));
    promise::all(pms).then([replica, this](const promise::values_t values) {
        std::vector<block_t> blks;
        for (auto &v: values)
        {
            auto blk = promise::any_cast<block_t>(v);
            blks.push_back(blk);
        }
        pn.send_msg(MsgRespBlock(blks), replica);
    });
}

void HotStuffBase::resp_blk_handler(MsgRespBlock &&msg, const Net::conn_t &) {
    msg.postponed_parse(this);
    for (const auto &blk: msg.blks)
        if (blk) on_fetch_blk(blk);
}

void HotStuffBase::print_stat() const {
    LOG_INFO("===== begin stats =====");
    LOG_INFO("-------- queues -------");
    LOG_INFO("blk_fetch_waiting: %lu", blk_fetch_waiting.size());
    LOG_INFO("blk_delivery_waiting: %lu", blk_delivery_waiting.size());
    LOG_INFO("decision_waiting: %lu", decision_waiting.size());
    LOG_INFO("commit_timers: %lu", commit_timers.size());
    LOG_INFO("-------- misc ---------");
    LOG_INFO("fetched: %lu", fetched);
    LOG_INFO("delivered: %lu", delivered);
    LOG_INFO("cmd_cache: %lu", storage->get_cmd_cache_size());
    LOG_INFO("blk_cache: %lu", storage->get_blk_cache_size());
    LOG_INFO("------ misc (10s) -----");
    LOG_INFO("fetched: %lu", part_fetched);
    LOG_INFO("delivered: %lu", part_delivered);
    LOG_INFO("decided: %lu", part_decided);
    LOG_INFO("gened: %lu", part_gened);
    LOG_INFO("avg. parent_size: %.3f",
            part_delivered ? part_parent_size / double(part_delivered) : 0);
    LOG_INFO("delivery time: %.3f avg, %.3f min, %.3f max",
            part_delivered ? part_delivery_time / double(part_delivered) : 0,
            part_delivery_time_min == double_inf ? 0 : part_delivery_time_min,
            part_delivery_time_max);

    part_parent_size = 0;
    part_fetched = 0;
    part_delivered = 0;
    part_decided = 0;
    part_gened = 0;
    part_delivery_time = 0;
    part_delivery_time_min = double_inf;
    part_delivery_time_max = 0;
#ifdef HOTSTUFF_MSG_STAT
    LOG_INFO("--- replica msg. (10s) ---");
    size_t _nsent = 0;
    size_t _nrecv = 0;
    for (const auto &replica: peers)
    {
        auto conn = pn.get_peer_conn(replica);
        if (conn == nullptr) continue;
        size_t ns = conn->get_nsent();
        size_t nr = conn->get_nrecv();
        size_t nsb = conn->get_nsentb();
        size_t nrb = conn->get_nrecvb();
        conn->clear_msgstat();
        LOG_INFO("%s: %u(%u), %u(%u), %u",
            std::string(replica).c_str(), ns, nsb, nr, nrb, part_fetched_replica[replica]);
        _nsent += ns;
        _nrecv += nr;
        part_fetched_replica[replica] = 0;
    }
    nsent += _nsent;
    nrecv += _nrecv;
    LOG_INFO("sent: %lu", _nsent);
    LOG_INFO("recv: %lu", _nrecv);
    LOG_INFO("--- replica msg. total ---");
    LOG_INFO("sent: %lu", nsent);
    LOG_INFO("recv: %lu", nrecv);
#endif
    LOG_INFO("====== end stats ======");
}

HotStuffBase::HotStuffBase(uint32_t blk_size,
                    ReplicaID rid,
                    privkey_bt &&priv_key,
                    NetAddr listen_addr,
                    pacemaker_bt pmaker,
                    EventContext ec,
                    size_t nworker,
                    const Net::Config &netconfig):
        HotStuffCore(rid, std::move(priv_key)),
        listen_addr(listen_addr),
        blk_size(blk_size),
        ec(ec),
        tcall(ec),
        vpool(ec, nworker),
        pn(ec, netconfig),
        pmaker(std::move(pmaker)),

        fetched(0), delivered(0),
        nsent(0), nrecv(0),
        part_parent_size(0),
        part_fetched(0),
        part_delivered(0),
        part_decided(0),
        part_gened(0),
        part_delivery_time(0),
        part_delivery_time_min(double_inf),
        part_delivery_time_max(0)
{
    /* register the handlers for msg from replicas */
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::propose_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::vote_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::notify_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::status_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::blame_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::blamenotify_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::req_blk_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::resp_blk_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::new_view_handler, this, _1, _2));
    pn.start();
    pn.listen(listen_addr);
}

void HotStuffBase::do_consensus(const block_t &blk){
//    pmaker->on_consensus(blk);
}

void HotStuffBase::do_broadcast_proposal(const Proposal &prop) {
    MsgPropose prop_msg(prop);
    pn.multicast_msg(prop_msg, peers);
    //for (const auto &replica: peers)
    //    pn.send_msg(prop_msg, replica);
}

void HotStuffBase::do_decide(Finality &&fin) {
    part_decided++;
    state_machine_execute(fin);
    auto it = decision_waiting.find(fin.cmd_hash);
    if (it != decision_waiting.end())
    {
        it->second(std::move(fin));
        decision_waiting.erase(it);
    }
}

void HotStuffBase::do_status(const Status &status) {
    MsgStatus m(status);
    ReplicaID next_proposer = pmaker->get_proposer();

    if (next_proposer != get_id())
        pn.send_msg(m, get_config().get_addr(next_proposer));
    else
        on_receive_status(status);
}

HotStuffBase::~HotStuffBase() {}

void HotStuffBase::start(
        std::vector<std::pair<NetAddr, pubkey_bt>> &&replicas,
        double delta, bool ec_loop) {
    for (size_t i = 0; i < replicas.size(); i++)
    {
        auto &addr = replicas[i].first;
        HotStuffCore::add_replica(i, addr, std::move(replicas[i].second));
        if (addr != listen_addr)
        {
            peers.push_back(addr);
            pn.add_peer(addr);
        }
    }

    /* ((n - 1) + 1 - 1) / 3 */
    uint32_t nfaulty = peers.size() / 2;
    if (nfaulty == 0)
        LOG_WARN("too few replicas in the system to tolerate any failure");
    on_init(nfaulty, delta);
    pmaker->init(this);
    if (ec_loop)
        ec.dispatch();

    cmd_pending.reg_handler(ec, [this](cmd_queue_t &q) {
        std::pair<uint256_t, commit_cb_t> e;
        while (q.try_dequeue(e))
        {
            ReplicaID proposer = pmaker->get_proposer();
            if (proposer != get_id()) continue;

            const auto &cmd_hash = e.first;
            cmd_pending_buffer.push(cmd_hash);

            auto it = decision_waiting.find(cmd_hash);
            if (it == decision_waiting.end())
            {
                it = decision_waiting.insert(std::make_pair(cmd_hash, e.second)).first;
            }
            else
            {
                // TODO: duplicate commands
            }
            if (cmd_pending_buffer.size() >= blk_size)
            {
                std::vector<uint256_t> cmds;
                for (uint32_t i = 0; i < blk_size; i++)
                {
                    cmds.push_back(cmd_pending_buffer.front());
                    cmd_pending_buffer.pop();
                }
                pmaker->beat().then([this, cmds = std::move(cmds)](ReplicaID proposer) {
                    if (proposer == get_id())
                        on_propose(cmds, pmaker->get_parents());
                });
                return true;
            }
        }
        return false;
    });
}

}
