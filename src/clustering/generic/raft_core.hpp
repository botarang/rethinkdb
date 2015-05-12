// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CLUSTERING_GENERIC_RAFT_CORE_HPP_
#define CLUSTERING_GENERIC_RAFT_CORE_HPP_

#include <deque>
#include <set>
#include <map>

#include "errors.hpp"
#include <boost/optional.hpp>
#include <boost/variant.hpp>

#include "concurrency/auto_drainer.hpp"
#include "concurrency/new_mutex.hpp"
#include "concurrency/promise.hpp"
#include "concurrency/signal.hpp"
#include "concurrency/watchable.hpp"
#include "concurrency/watchable_map.hpp"
#include "containers/archive/boost_types.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/uuid.hpp"
#include "time.hpp"

/* This file implements the Raft consensus algorithm, as described in the paper "In
Search of an Understandable Consensus Algorithm (Extended Version)" (2014) by Diego
Ongaro and John Ousterhout. Because of the complexity and subtlety of the Raft algorithm,
we follow the paper closely and refer back to it regularly. You are advised to have a
copy of the paper on hand when reading or modifying this file.

This file only contains the basic Raft algorithm itself; it doesn't contain any
networking or storage logic. Instead, it uses abstract interfaces to send and receive
network messages and write data to persistent storage. This both keeps this file as
as simple as possible and makes it easy to test the Raft algorithm using mocked-up
network and storage systems.

We support both log compaction and configuration changes.

The classes in this file are templatized on a types called `state_t`, which represents
the state machine that the Raft cluster manages. Operations on the state machine are
represented by a member type `state_t::change_t`. So `state_t::change_t` is the type that
is stored in the Raft log, and `state_t` is stored when taking a snapshot.

`state_t` and `state_t::change_t` must satisfy the following requirements:
  * Both must be default-constructable, destructable, copy- and move-constructable, copy-
    and move-assignable.
  * Both must support the `==` and `!=` operators.
  * `state_t` must have a method `void apply_change(const change_t &)` which applies the
    change to the `state_t`, mutating it in place. */

/* `raft_term_t` and `raft_log_index_t` are typedefs to improve the readability of the
code, by making it clearer what the meaning of a particular number is. */
typedef uint64_t raft_term_t;
typedef uint64_t raft_log_index_t;

/* Every member of the Raft cluster is identified by a `raft_member_id_t`. The Raft paper
uses integers for this purpose, but we use UUIDs because we have no reliable distributed
way of assigning integers. Note that `raft_member_id_t` is not a `server_id_t` or a
`peer_id_t`. If a single server leaves a Raft cluter and then joins again, it will use a
different `raft_member_id_t` the second time. */
class raft_member_id_t {
public:
    raft_member_id_t() : uuid(nil_uuid()) { }
    explicit raft_member_id_t(uuid_u u) : uuid(u) { }
    bool is_nil() const { return uuid.is_nil(); }
    bool operator==(const raft_member_id_t &other) const { return uuid == other.uuid; }
    bool operator!=(const raft_member_id_t &other) const { return uuid != other.uuid; }
    bool operator<(const raft_member_id_t &other) const { return uuid < other.uuid; }
    uuid_u uuid;
};
RDB_DECLARE_SERIALIZABLE(raft_member_id_t);

/* `raft_config_t` describes the set of members that are involved in the Raft cluster. */
class raft_config_t {
public:
    /* Regular members of the Raft cluster go in `voting_members`. `non_voting_members`
    is for members that should receive updates, but that don't count for voting purposes.
    */
    std::set<raft_member_id_t> voting_members, non_voting_members;

    /* Returns a list of all members, voting and non-voting. */
    std::set<raft_member_id_t> get_all_members() const {
        std::set<raft_member_id_t> members;
        members.insert(voting_members.begin(), voting_members.end());
        members.insert(non_voting_members.begin(), non_voting_members.end());
        return members;
    }

    /* Returns `true` if `member` is a voting or non-voting member. */
    bool is_member(const raft_member_id_t &member) const {
        return voting_members.count(member) == 1 ||
            non_voting_members.count(member) == 1;
    }

    /* Returns `true` if `members` constitutes a majority. */
    bool is_quorum(const std::set<raft_member_id_t> &members) const {
        size_t votes = 0;
        for (const raft_member_id_t &m : members) {
            votes += voting_members.count(m);
        }
        return (votes * 2 > voting_members.size());
    }

    /* Returns `true` if the given member can act as a leader. (Mostly this exists for
    consistency with `raft_complex_config_t`.) */
    bool is_valid_leader(const raft_member_id_t &member) const {
        return voting_members.count(member) == 1;
    }

    /* The equality and inequality operators are mostly for debugging */
    bool operator==(const raft_config_t &other) const {
        return voting_members == other.voting_members &&
            non_voting_members == other.non_voting_members;
    }
    bool operator!=(const raft_config_t &other) const {
        return !(*this == other);
    }
};
RDB_DECLARE_SERIALIZABLE(raft_config_t);

/* `raft_complex_config_t` can represent either a `raft_config_t` or a joint consensus of
an old and a new `raft_config_t`. */
class raft_complex_config_t {
public:
    /* For a regular configuration, `config` holds the configuration and `new_config` is
    empty. For a joint consensus configuration, `config` holds the old configuration and
    `new_config` holds the new configuration. */
    raft_config_t config;
    boost::optional<raft_config_t> new_config;

    bool is_joint_consensus() const {
        return static_cast<bool>(new_config);
    }

    std::set<raft_member_id_t> get_all_members() const {
        std::set<raft_member_id_t> members = config.get_all_members();
        if (is_joint_consensus()) {
            /* Raft paper, Section 6: "Log entries are replicated to all servers in both
            configurations." */
            std::set<raft_member_id_t> members2 = new_config->get_all_members();
            members.insert(members2.begin(), members2.end());
        }
        return members;
    }

    bool is_member(const raft_member_id_t &member) const {
        return config.is_member(member) ||
            (is_joint_consensus() && new_config->is_member(member));
    }

    bool is_quorum(const std::set<raft_member_id_t> &members) const {
        /* Raft paper, Section 6: "Agreement (for elections and entry commitment)
        requires separate majorities from both the old and new configurations." */
        if (is_joint_consensus()) {
            return config.is_quorum(members) && new_config->is_quorum(members);
        } else {
            return config.is_quorum(members);
        }
    }

    bool is_valid_leader(const raft_member_id_t &member) const {
        /* Raft paper, Section 6: "Any server from either configuration may serve as
        leader." */
        return config.is_valid_leader(member) ||
            (is_joint_consensus() && new_config->is_valid_leader(member));
    }

    /* The equality and inequality operators are mostly for debugging */
    bool operator==(const raft_complex_config_t &other) const {
        return config == other.config && new_config == other.new_config;
    }
    bool operator!=(const raft_complex_config_t &other) const {
        return !(*this == other);
    }
};
RDB_DECLARE_SERIALIZABLE(raft_complex_config_t);

enum class raft_log_entry_type_t {
    /* A `regular` log entry is one with a `change_t`. So if `type` is `regular`,
    then `change` has a value but `config` is empty. */
    regular,
    /* A `config` log entry has a `raft_complex_config_t`. They are used to change
    the cluster configuration. See Section 6 of the Raft paper. So if `type` is
    `config`, then `config` has a value but `change` is empty. */
    config,
    /* A `noop` log entry does nothing and carries niether a `change_t` nor a
    `raft_complex_config_t`. See Section 8 of the Raft paper. */
    noop
};
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(raft_log_entry_type_t, int8_t,
    raft_log_entry_type_t::regular, raft_log_entry_type_t::noop);

/* `raft_log_entry_t` describes an entry in the Raft log. */
template<class state_t>
class raft_log_entry_t {
public:
    raft_log_entry_type_t type;
    raft_term_t term;
    /* Whether `change` and `config` are empty or not depends on the value of `type`. */
    boost::optional<typename state_t::change_t> change;
    boost::optional<raft_complex_config_t> config;

    RDB_MAKE_ME_SERIALIZABLE_4(raft_log_entry_t, type, term, change, config);
};

/* `raft_log_t` stores a slice of the Raft log. There are two situations where this shows
up in Raft: in an "AppendEntries RPC", and in each server's local state. The Raft paper
represents this as three separate variables, but grouping them together makes the
code clearer. */
template<class state_t>
class raft_log_t {
public:
    /* In an append-entries message, `prev_index` and `prev_term` correspond to the
    parameters that Figure 2 of the Raft paper calls `prevLogIndex` and `prevLogTerm`,
    and `entries` corresponds to the parameter that the Raft paper calls `entries`.

    In a server's local state, `prev_index` and `prev_term` correspond to the "last
    included index" and "last included term" variables as described in Section 7.
    `entries` corresponds to the `log` variable described in Figure 2. */

    raft_log_index_t prev_index;
    raft_term_t prev_term;
    std::deque<raft_log_entry_t<state_t> > entries;

    /* Return the latest index that is present in the log. If the log is empty, returns
    the index on which the log is based. */
    raft_log_index_t get_latest_index() const {
        return prev_index + entries.size();
    }

    /* Returns the term of the log entry at the given index. The index must either be
    present in the log or the last index before the log. */
    raft_term_t get_entry_term(raft_log_index_t index) const {
        guarantee(index >= prev_index, "the log doesn't go back this far");
        guarantee(index <= get_latest_index(), "the log doesn't go forward this far");
        if (index == prev_index) {
            return prev_term;
        } else {
            return get_entry_ref(index).term;
        }
    }

    /* Returns the entry in the log at the given index. */
    const raft_log_entry_t<state_t> &get_entry_ref(raft_log_index_t index) const {
        guarantee(index > prev_index, "the log doesn't go back this far");
        guarantee(index <= get_latest_index(), "the log doesn't go forward this far");
        return entries[index - prev_index - 1];
    }

    /* Deletes the log entry at the given index and all entries after it. */
    void delete_entries_from(raft_log_index_t index) {
        guarantee(index > prev_index, "the log doesn't go back this far");
        guarantee(index <= get_latest_index(), "the log doesn't go forward this far");
        entries.erase(entries.begin() + (index - prev_index - 1), entries.end());
    }

    /* Deletes the log entry at the given index and all entries before it. */
    void delete_entries_to(raft_log_index_t index) {
        guarantee(index > prev_index, "the log doesn't go back this far");
        guarantee(index <= get_latest_index(), "the log doesn't go forward this far");
        raft_term_t index_term = get_entry_term(index);
        entries.erase(entries.begin(), entries.begin() + (index - prev_index));
        prev_index = index;
        prev_term = index_term;
    }

    /* Appends the given entry ot the log. */
    void append(const raft_log_entry_t<state_t> &entry) {
        entries.push_back(entry);
    }

    RDB_MAKE_ME_SERIALIZABLE_3(raft_log_t, prev_index, prev_term, entries);
};

/* `raft_persistent_state_t` describes the information that each member of the Raft
cluster persists to stable storage. */
template<class state_t>
class raft_persistent_state_t {
public:
    /* `make_initial()` returns a `raft_persistent_state_t` for a member of a new Raft
    instance with starting state `initial_state` and configuration `initial_config`. The
    caller must ensure that every member of the new Raft cluster starts with the same
    values for these variables. */
    static raft_persistent_state_t make_initial(
        const state_t &initial_state,
        const raft_config_t &initial_config);

private:
    template<class state2_t> friend class raft_member_t;

    /* `current_term` and `voted_for` correspond to the variables with the same names in
    Figure 2 of the Raft paper. */
    raft_term_t current_term;
    raft_member_id_t voted_for;

    /* `snapshot_state` corresponds to the stored snapshotted state, as described in
    Section 7. */
    state_t snapshot_state;

    /* `snapshot_config` corresponds to the stored snapshotted configuration, as
    described in Section 7. */
    raft_complex_config_t snapshot_config;

    /* `log.prev_index` and `log.prev_term` correspond to the "last included index" and
    "last included term" as described in Section 7. `log.entries` corresponds to the
    `log` variable in Figure 2. */
    raft_log_t<state_t> log;

    RDB_MAKE_ME_SERIALIZABLE_5(raft_persistent_state_t, current_term, voted_for,
        snapshot_state, snapshot_config, log);
};

/* `raft_storage_interface_t` is an abstract class that `raft_member_t` uses to store
data on disk. */
template<class state_t>
class raft_storage_interface_t {
public:
    /* `write_persistent_state()` writes the state of the Raft member to stable storage.
    It does not return until the state is safely stored. The values stored with
    `write_state()` will be passed to the `raft_member_t` constructor when the Raft
    member is restarted. */
    virtual void write_persistent_state(
        const raft_persistent_state_t<state_t> &persistent_state,
        signal_t *interruptor) = 0;

    /* If writing the state becomes a performance bottleneck, we could implement a
    variant that only rewrites part of the state. In particular, we often need to append
    a few entries to the log but don't need to make any other changes. */

protected:
    virtual ~raft_storage_interface_t() { }
};

/* `raft_rpc_request_t` describes a request that one Raft member sends over the network
to another Raft member. It actually can describe one of three request types,
corresponding to the three RPCs in the Raft paper, but they're bundled together into one
type for the convenience of code that uses Raft. */
template<class state_t>
class raft_rpc_request_t {
private:
    template<class state2_t> friend class raft_member_t;

    /* `request_vote_t` describes the parameters to the "RequestVote RPC" described
    in Figure 2 of the Raft paper. */
    class request_vote_t {
    public:
        /* `term`, `candidate_id`, `last_log_index`, and `last_log_term` correspond to
        the parameters with the same names in the Raft paper. */
        raft_term_t term;
        raft_member_id_t candidate_id;
        raft_log_index_t last_log_index;
        raft_term_t last_log_term;
        RDB_MAKE_ME_SERIALIZABLE_4(request_vote_t,
            term, candidate_id, last_log_index, last_log_term);
    };

    /* `install_snapshot_t` describes the parameters of the "InstallSnapshot RPC"
    described in Figure 13 of the Raft paper. */
    class install_snapshot_t {
    public:
        /* `term`, `leader_id`, `last_included_index`, and `last_included_term`
        correspond to the parameters with the same names in the Raft paper. In the Raft
        paper, the content of the snapshot is sent as a series of binary blobs, but we
        don't want to do that; instead, we send the `state_t` and `raft_complex_config_t`
        directly. So our `snapshot_state` and `snapshot_config` parameters replace the
        `offset`, `data`, and `done` parameters of the Raft paper. */
        raft_term_t term;
        raft_member_id_t leader_id;
        raft_log_index_t last_included_index;
        raft_term_t last_included_term;
        state_t snapshot_state;
        raft_complex_config_t snapshot_config;
        RDB_MAKE_ME_SERIALIZABLE_6(install_snapshot_t,
            term, leader_id, last_included_index, last_included_term, snapshot_state,
            snapshot_config);
    };

    /* `append_entries_t` describes the parameters of the "AppendEntries RPC" described
    in Figure 2 of the Raft paper. */
    class append_entries_t {
    public:
        /* `term`, `leader_id`, and `leader_commit` correspond to the parameters with the
        same names in the Raft paper. `entries` corresponds to three of the paper's
        variables: `prevLogIndex`, `prevLogTerm`, and `entries`. */
        raft_term_t term;
        raft_member_id_t leader_id;
        raft_log_t<state_t> entries;
        raft_log_index_t leader_commit;
        RDB_MAKE_ME_SERIALIZABLE_4(append_entries_t,
            term, leader_id, entries, leader_commit);
    };

    /* This implementation deviates from the Raft paper in that we use the RPC layer's
    connection timeouts to detect failed leaders instead of using heartbeats. However,
    sometimes a leader will stop being leader without losing the connection to the other
    nodes. In this case, we need some other way to tell the other nodes that the leader
    is no longer active. The solution is a new type of RPC that doesn't appear in the
    Raft paper: the "StepDown RPC". Any time a leader ceases to be leader, it will send a
    StepDown RPC to every member of the Raft cluster. */
    class step_down_t {
    public:
        /* `leader_id` is the ID of the node that is stepping down, and `term` is the
        term that it was acting as leader for. */
        raft_term_t term;
        raft_member_id_t leader_id;
        RDB_MAKE_ME_SERIALIZABLE_2(step_down_t,
            term, leader_id);
    };

    boost::variant<request_vote_t, install_snapshot_t, append_entries_t, step_down_t>
        request;

    RDB_MAKE_ME_SERIALIZABLE_1(raft_rpc_request_t, request);
};

/* `raft_rpc_reply_t` describes the reply to a `raft_rpc_request_t`. */
class raft_rpc_reply_t {
private:
    template<class state2_t> friend class raft_member_t;

    /* `request_vote_t` describes the information returned from the "RequestVote
    RPC" described in Figure 2 of the Raft paper. */
    class request_vote_t {
    public:
        raft_term_t term;
        bool vote_granted;
        RDB_MAKE_ME_SERIALIZABLE_2(request_vote_t, term, vote_granted);
    };

    /* `install_snapshot_t` describes in the information returned from the
    "InstallSnapshot RPC" described in Figure 13 of the Raft paper. */
    class install_snapshot_t {
    public:
        raft_term_t term;
        RDB_MAKE_ME_SERIALIZABLE_1(install_snapshot_t, term);
    };

    /* `append_entries_t` describes the information returned from the
    "AppendEntries RPC" described in Figure 2 of the Raft paper. */
    class append_entries_t {
    public:
        raft_term_t term;
        bool success;
        RDB_MAKE_ME_SERIALIZABLE_2(append_entries_t, term, success);
    };

    /* `step_down_t` is the reply to a StepDown RPC; it doesn't appear in the Raft paper.
    See the note for `raft_rpc_request_t::step_down_t`. */
    class step_down_t {
    public:
        RDB_MAKE_ME_SERIALIZABLE_0(step_down_t);
    };

    boost::variant<request_vote_t, install_snapshot_t, append_entries_t, step_down_t>
        reply;

    RDB_MAKE_ME_SERIALIZABLE_1(raft_rpc_reply_t, reply);
};

/* `raft_network_interface_t` is the abstract class that `raft_member_t` uses to send
messages over the network. */
template<class state_t>
class raft_network_interface_t {
public:
    /* `send_rpc()` sends a message to the Raft member indicated in the `dest` field. The
    message will be delivered by calling the `on_rpc()` method on the `raft_member_t` in
    question.
      * If the RPC is delivered successfully, `send_rpc()` will return `true`, and
        the reply will be stored in `*reply_out`.
      * If something goes wrong, `send_rpc()` will return `false`. The RPC may or may not
        have been delivered in this case. The caller should wait until the Raft member is
        present in `get_connected_members()` before trying again.
      * If the interruptor is pulsed, it throws `interrupted_exc_t`. The RPC may or may
        not have been delivered. */
    virtual bool send_rpc(
        const raft_member_id_t &dest,
        const raft_rpc_request_t<state_t> &request,
        signal_t *interruptor,
        raft_rpc_reply_t *reply_out) = 0;

    /* `get_connected_members()` returns the set of all Raft members for which an RPC is
    likely to succeed. The values in the map should always be `nullptr`; the only reason
    it's a map at all is that we don't have a `watchable_set_t` type. */
    virtual watchable_map_t<raft_member_id_t, empty_value_t>
        *get_connected_members() = 0;

protected:
    virtual ~raft_network_interface_t() { }
};

/* `raft_member_t` is responsible for managing the activity of a single member of the
Raft cluster. */
template<class state_t>
class raft_member_t : public home_thread_mixin_debug_only_t
{
public:
    raft_member_t(
        const raft_member_id_t &this_member_id,
        raft_storage_interface_t<state_t> *storage,
        raft_network_interface_t<state_t> *network,
        const raft_persistent_state_t<state_t> &persistent_state,
        /* We'll print log messages of the form `<log_prefix>: <message>`. If
        `log_prefix` is empty, we won't print any messages. */
        const std::string &log_prefix);

    ~raft_member_t();

    /* Note that if any public method on `raft_member_t` is interrupted, the
    `raft_member_t` will be left in an undefined internal state. Therefore, the
    destructor should be called after the interruptor has been pulsed. (However, even
    though the internal state is undefined, the interrupted method call will not make
    invalid RPC calls or write invalid data to persistent storage.) */

    /* `state_and_config_t` describes the Raft cluster's current state, configuration,
    and log index all in the same struct. The reason for putting them in the same struct
    is so that they can be stored in a `watchable_t` and kept in sync. */
    class state_and_config_t {
    public:
        state_and_config_t(raft_log_index_t _log_index, const state_t &_state,
                           const raft_complex_config_t &_config) :
            log_index(_log_index), state(_state), config(_config) { }
        raft_log_index_t log_index;
        state_t state;
        raft_complex_config_t config;
    };

    /* `get_committed_state()` describes the state of the Raft cluster after all
    committed log entries have been applied. */
    clone_ptr_t<watchable_t<state_and_config_t> > get_committed_state() {
        assert_thread();
        return committed_state.get_watchable();
    }

    /* `get_latest_state()` describes the state of the Raft cluster if every log entry,
    including uncommitted entries, has been applied. */
    clone_ptr_t<watchable_t<state_and_config_t> > get_latest_state() {
        assert_thread();
        return latest_state.get_watchable();
    }

    /* `get_state_for_init()` returns a `raft_persistent_state_t` that could be used to
    initialize a new member joining the Raft cluster. */
    raft_persistent_state_t<state_t> get_state_for_init();

    /* Here's how to perform a Raft transaction:

    1. Find a `raft_member_t` in the cluster for which `get_readiness_for_change()`
    returns true. (For a config transaction, use `get_readiness_for_config_change()`
    instead.)

    2. Construct a `change_lock_t` on that `raft_member_t`.

    3. Call `propose_[config_]change()`. You can make multiple calls to
    `propose_change()` with the same `change_lock_t`, but no more than one call to
    `propose_config_change()`.

    4. Destroy the `change_lock_t` so the Raft cluster can process your transaction.

    5. If you need to be notified of whether your transaction succeeds or not, wait on
    the `change_token_t` returned by `propose_[config_]change()`. */

    /* These watchables indicate whether this Raft member is ready to accept changes. In
    general, if these watchables are true, then `propose_[config_]change()` will probably
    succeed. (However, this is not guaranteed.) If these watchables are false, don't
    bother trying `propose_[config_]change()`.

    Under the hood, these are true if:
    - This member is currently the leader
    - This member is in contact with a quorum of followers
    - We are not currently in a reconfiguration (for `get_readiness_for_config_change()`)
   */
    clone_ptr_t<watchable_t<bool> > get_readiness_for_change() {
        return readiness_for_change.get_watchable();
    }
    clone_ptr_t<watchable_t<bool> > get_readiness_for_config_change() {
        return readiness_for_config_change.get_watchable();
    }

    /* `change_lock_t` freezes the Raft member state in preparation for calling
    `propose_[config_]change()`. Only one `change_lock_t` can exist at a time, and while
    it exists, the Raft member will not process normal traffic; so don't keep the
    `change_lock_t` around longer than necessary. However, it is safe to block while
    holding the `change_lock_t` if you need to.

    The point of `change_lock_t` is that `get_latest_state()` will not change while the
    `change_lock_t` exists, unless the lock owner calls `propose_[config_]change()`. The
    state reported by `get_latest_state()` is guaranteed to be the state that the
    proposed change will be applied to. This makes it possible to atomically read the
    state and issue a change conditional on the state. */
    class change_lock_t {
    public:
        change_lock_t(raft_member_t *parent, signal_t *interruptor);
    private:
        friend class raft_member_t;
        new_mutex_acq_t mutex_acq;
    };

    /* `change_token_t` is a way to track the progress of a change to the Raft cluster.
    It's a promise that will be `true` if the change has been committed, and `false` if
    something went wrong. If it returns `false`, the change may or may not eventually be
    committed anyway. */
    class change_token_t : public promise_t<bool> {
    private:
        friend class raft_member_t;
        change_token_t(raft_member_t *parent, raft_log_index_t index, bool is_config);
        promise_t<bool> promise;
        bool is_config;
        multimap_insertion_sentry_t<raft_log_index_t, change_token_t *> sentry;
    };

    /* `propose_change()` tries to apply a `change_t` to the cluster.
    `propose_config_change()` tries to change the cluster's configuration. 

    `propose_[config_]change()` will block while the change is being initiated; this
    should be a relatively quick process. If you pulse the interruptor, the
    `raft_member_t` may be left in an undefined internal state.

    If the change is successfully initiated, `propose_[config_]change()` will return a
    `change_token_t` that you can use to monitor the progress of the change. If it is not
    successful, it will return `nullptr`. See `get_readiness_for_[config_]change()` for
    an explanation of when and why it will return `nullptr`. */
    scoped_ptr_t<change_token_t> propose_change(
        change_lock_t *change_lock,
        const typename state_t::change_t &change,
        signal_t *interruptor);
    scoped_ptr_t<change_token_t> propose_config_change(
        change_lock_t *change_lock,
        const raft_config_t &new_config,
        signal_t *interruptor);

    /* When a Raft member calls `send_rpc()` on its `raft_network_interface_t`, the RPC
    is sent across the network and delivered by calling `on_rpc()` at its destination. */
    void on_rpc(
        const raft_rpc_request_t<state_t> &request,
        signal_t *interruptor,
        raft_rpc_reply_t *reply_out);

#ifndef NDEBUG
    /* `check_invariants()` asserts that the given collection of Raft cluster members are
    in a valid, consistent state. This may block, because it needs to acquire each
    member's mutex, but it will not modify anything. Since this requires direct access to
    each member of the Raft cluster, it's only useful for testing. */
    static void check_invariants(
        const std::set<raft_member_t<state_t> *> &members);
#endif /* NDEBUG */

private:
    /* The Raft paper describes three states: "follower", "candidate", and "leader". We
    split the "follower" state into two sub-states depending on whether we believe that a
    leader exists or not. */
    enum class mode_t {
        follower_led,
        follower_unled,
        candidate,
        leader
    };

    /* These are the minimum and maximum election timeouts. In section 5.6, the Raft
    paper suggests that a typical election timeout should be somewhere between 10ms and
    500ms. We choose relatively long timeouts because immediate availability is not
    important, and we want to avoid a cycle of repeated failed elections. (This
    implementation deviates from the Raft paper in that we use the RPC layer's
    connectivity detection to determine if we need to start a new election, so these are
    actually the timeouts after the leader is determined to be dead.) */
    static const int32_t election_timeout_min_ms = 1000,
                         election_timeout_max_ms = 2000;

    /* Note: Methods prefixed with `follower_`, `candidate_`, or `leader_` are methods
    that are only used when in that state. This convention will hopefully make the code
    slightly clearer. */

    /* `on_rpc()` calls one of these three methods depending on what type of RPC it
    received. */
    void on_request_vote_rpc(
        const typename raft_rpc_request_t<state_t>::request_vote_t &rpc,
        signal_t *interruptor,
        raft_rpc_reply_t::request_vote_t *reply_out);
    void on_install_snapshot_rpc(
        const typename raft_rpc_request_t<state_t>::install_snapshot_t &rpc,
        signal_t *interruptor,
        raft_rpc_reply_t::install_snapshot_t *reply_out);
    void on_append_entries_rpc(
        const typename raft_rpc_request_t<state_t>::append_entries_t &rpc,
        signal_t *interruptor,
        raft_rpc_reply_t::append_entries_t *reply_out);
    void on_step_down_rpc(
        const typename raft_rpc_request_t<state_t>::step_down_t &rpc,
        signal_t *interruptor,
        raft_rpc_reply_t::step_down_t *reply_out);

#ifndef NDEBUG
    /* Asserts that all of the invariants that can be checked locally hold true. This
    doesn't block or modify anything. It should be safe to call it at any time (except
    when in between modifying two variables that should remain consistent with each
    other, of course). In general we call it whenever we acquire or release the mutex,
    because we know that the variables should be consistent at those times. */
    void check_invariants(const new_mutex_acq_t *mutex_acq);
#endif /* NDEBUG */

    /* `on_connected_members_change()` is called when a member connects or disconnects.
    */
    void on_connected_members_change(
        const raft_member_id_t &other_member_id, const empty_value_t *value);

    /* `apply_log_entries()` updates `state_and_config` with the entries from `log` with
    indexes `first <= index <= last`. */
    static void apply_log_entries(
        state_and_config_t *state_and_config,
        const raft_log_t<state_t> &log,
        raft_log_index_t first,
        raft_log_index_t last);

    /* `update_term()` sets the term to `new_term` and resets all per-term variables. It
    assumes that its caller will flush persistent state to stable storage eventually
    after it returns. */
    void update_term(raft_term_t new_term,
                     const new_mutex_acq_t *mutex_acq);

    /* When we change the commit index we have to also apply changes to the state
    machine. `update_commit_index()` handles that automatically. It assumes that its
    caller will flush persistent state to stable storage eventually after it returns. */
    void update_commit_index(raft_log_index_t new_commit_index,
                             const new_mutex_acq_t *mutex_acq);

    /* When we change `match_index` we might have to update `commit_index` as well.
    `leader_update_match_index()` handles that automatically. It may flush persistent
    state to stable storage before it returns. */
    void leader_update_match_index(
        raft_member_id_t key,
        raft_log_index_t new_value,
        const new_mutex_acq_t *mutex_acq,
        signal_t *interruptor);

    /* `update_readiness_for_change()` should be called whenever any of the variables
    that are used to compute `readiness_for_change` or `readiness_for_config_change` are
    modified. */
    void update_readiness_for_change();

    /* `start_election_and_leader_coro()` spawns `election_and_leader_coro()`, which will
    start a new election after a random timeout. The caller must have already put us into
    the `follower_unled` state and set up `last_heard_from_candidate` if appropriate. */
    void start_election_and_leader_coro(
        const new_mutex_acq_t *mutex_acq);

    /* `stop_election_and_leader_coro()` stops `election_and_leader_coro()`
    and blocks until the coro exits. If we were in the `leader` state before, it also
    sends out StepDown RPCs asynchronously so the other nodes know we're no longer acting
    as leader. It leaves `mode` set to `follower_unled`, so the caller must either change
    `mode` to `follower_led` or call `start_election_and_leader_coro()` in order to
    regain the invariant that `election_and_leader_coro()` is running unless we are in
    the `follower_led` state. */
    void stop_election_and_leader_coro(
        const new_mutex_acq_t *mutex_acq);

    /* `update_term_and_reset_election_and_leader_coro()` is equivalent to calling
    `stop_election_and_leader_coro()`, `update_term()`, and
    `start_election_and_leader_coro()` all in a row, except that if we were in the
    `follower_unled` or `candidate` state before it doesn't modify
    `last_heard_from_candidate`. */
    void update_term_and_reset_election_and_leader_coro(
        raft_term_t new_term,
        const new_mutex_acq_t *mutex_acq,
        signal_t *interruptor);

    /* `election_and_leader_coro()` contains most of the candidate- and leader-specific
    logic. When it begins, we are in the `follower_unled` state; it waits for a short
    timeout, then begins an election and transitions us to the `candidate` state. If it
    wins the elction, it transitions us to the `leader` state. If we are ever not in the
    `follower_led` state, there must be an instance of `election_and_leader_coro()`
    running. */
    void election_and_leader_coro(
        /* To make sure that `election_and_leader_coro` stops before the `raft_member_t`
        is destroyed. This is also used by `candidate_or_leader_become_follower()` to
        interrupt `candidate_and_leader_coro()`. */
        auto_drainer_t::lock_t leader_keepalive);

    /* `candidate_run_election()` is a helper function for `candidate_and_leader_coro()`.
    It sends out request-vote RPCs and wait for us to get enough votes. It blocks until
    we are elected or `cancel_signal` is pulsed. The caller is responsible for detecting
    the case where another leader is elected and also for detecting the case where the
    election times out, and pulsing `cancel_signal`. It returns `true` if we were
    elected. */
    bool candidate_run_election(
        /* Note that `candidate_run_election()` may temporarily release `mutex_acq`, but
        it will always be holding the lock when `run_election()` returns. But if
        `interruptor` is pulsed it will throw `interrupted_exc_t` and not reacquire the
        lock. */
        scoped_ptr_t<new_mutex_acq_t> *mutex_acq,
        signal_t *cancel_signal,
        signal_t *interruptor);

    /* `leader_spawn_update_coros()` is a helper function for
    `candidate_and_leader_coro()` that spawns or kills instances of `run_updates()` as
    necessary to ensure that there is always one for each cluster member. */
    void leader_spawn_update_coros(
        /* The value of `nextIndex` to use for each newly connected peer. */
        raft_log_index_t initial_next_index,
        /* A map containing an `auto_drainer_t` for each running update coroutine. */
        std::map<raft_member_id_t, scoped_ptr_t<auto_drainer_t> > *update_drainers,
        const new_mutex_acq_t *mutex_acq);

    /* `leader_send_updates()` is a helper function for `candidate_and_leader_coro()`;
    `leader_spawn_update_coros()` spawns one in a new coroutine for each peer. It pushes
    install-snapshot RPCs and/or append-entry RPCs out to the given peer until
    `update_keepalive.get_drain_signal()` is pulsed. */
    void leader_send_updates(
        const raft_member_id_t &peer,
        raft_log_index_t initial_next_index,
        auto_drainer_t::lock_t update_keepalive);

    /* `leader_continue_reconfiguration()` is a helper function for
    `candidate_and_leader_coro()`. It checks if we have completed the first phase of a
    reconfiguration (by committing a joint consensus configuration) and if so, it starts
    the second phase by committing the new configuration. It also checks if we have
    completed the second phase and if so, it makes us step down. */
    void leader_continue_reconfiguration(
        const new_mutex_acq_t *mutex_acq,
        signal_t *interruptor);

    /* `candidate_or_leader_note_term()` is a helper function for
    `candidate_run_election()` and `leader_send_updates()`. If the given term is greater
    than the current term, it updates the current term and interrupts
    `candidate_and_leader_coro()`. It returns `true` if the term was changed. */
    bool candidate_or_leader_note_term(
        raft_term_t term,
        const new_mutex_acq_t *mutex_acq);

    /* `leader_append_log_entry()` is a helper for `propose_change_if_leader()` and
    `propose_config_change_if_leader()`. It adds an entry to the log but doesn't wait for
    the entry to be committed. It flushes persistent state to stable storage. */
    void leader_append_log_entry(
        const raft_log_entry_t<state_t> &log_entry,
        const new_mutex_acq_t *mutex_acq,
        signal_t *interruptor);

    /* The member ID of the member of the Raft cluster represented by this
    `raft_member_t`. */
    const raft_member_id_t this_member_id;

    raft_storage_interface_t<state_t> *const storage;
    raft_network_interface_t<state_t> *const network;

    const std::string log_prefix;

    /* This stores all of the state variables of the Raft member that need to be written
    to stable storage when they change. We end up writing `ps.*` a lot, which is why the
    name is so abbreviated. */
    raft_persistent_state_t<state_t> ps;

    /* `committed_state` describes the state after all committed log entries have been
    applied. The `state` field of `committed_state` is equivalent to the "state machine"
    in the Raft paper. The `log_index` field is equal to the `lastApplied` and
    `commitIndex` variables in Figure 2 of the Raft paper. This implementation deviates
    from the Raft paper in that the paper allows for a delay between when changes are
    committed and when they are applied to the state machine, so `lastApplied` may lag
    behind `commitIndex`. But we always apply changes to the state machine as soon as
    they are committed, so `lastApplied` and `commitIndex` are equivalent for us. */
    watchable_variable_t<state_and_config_t> committed_state;

    /* `latest_state` describes the state after all log entries, not only committed ones,
    have been applied. This is publicly exposed to the user, and it's also useful because
    "a server always uses the latest configuration in its log, regardless of whether the
    entry is committed" (Raft paper, Section 6). Whenever `ps.log` is modified,
    `latest_state` must be updated to keep in sync. */
    watchable_variable_t<state_and_config_t> latest_state;

    /* Only `candidate_and_leader_coro()` should ever change `mode` */
    mode_t mode;

    /* `current_term_leader_id` is the ID of the member that is leader during this term.
    If we haven't seen any node acting as leader this term, it's `nil_uuid()`. When a
    member disconnects, we compare it to `current_term_leader_id` to decide if we should
    transition from `follower_led` to `follower_unled`. */
    raft_member_id_t current_term_leader_id;

    /* `current_term_leader_invalid` is `true` if we received a StepDown RPC or
    disconnection event for the member mentioned in `current_term_leader_id` during this
    term. If this is `true`, we won't interpret further AppendEntries or InstallSnapshot
    RPCs this term as evidence of a living leader. (However, we will still process the
    RPCs normally.) */
    bool current_term_leader_invalid;

    /* `last_leader_time` is the time at which we last received a valid RPC from a
    candidate or last believed a leader existed. If we are in the `follower_unled` state
    and an election timeout has elapsed since `last_leader_time`, we will transition to
    candidate state and start an election. If we are in the `follower_led` or `leader`
    state, we belive a candidate to currently exist, and `last_leader_time` will be
    set to the special value `LEADER_EXISTS_NOW`. When we transition into the
    `follower_unled` state we will set it to the current time. */
    static const boost::optional<microtime_t> LEADER_EXISTS_NOW;
    boost::optional<microtime_t> last_leader_time;

    /* `match_indexes` corresponds to the `matchIndex` array described in Figure 2 of the
    Raft paper. Note that it is only used if we are the leader; if we are not the leader,
    then it must be empty. */
    std::map<raft_member_id_t, raft_log_index_t> match_indexes;

    /* `readiness_for_change` and `readiness_for_config_change` track whether this member
    is ready to accept changes. A member is ready for changes if it is leader and in
    contact with a quorum of followers; it is ready for config changes if those
    conditions are met and it is also not currently in a reconfiguration. Whenever any of
    those variables changes, `update_readiness_for_change()` must be called. */
    watchable_variable_t<bool> readiness_for_change;
    watchable_variable_t<bool> readiness_for_config_change;

    /* `propose_[config_]change()` inserts a `change_token_t *` into `change_tokens`. If
    we stop being leader or lose contact with a majority of the cluster nodes, then all
    of the change tokens will be notified that the changes they were waiting on have
    failed. Whenever we commit a transaction, we also notify change tokens for success if
    appropriate. If we are not leader, `change_tokens` will be empty. */
    std::multimap<raft_log_index_t, change_token_t *> change_tokens;

    /* This mutex ensures that operations don't interleave in confusing ways. Each RPC
    acquires this mutex when it begins and releases it when it returns. Also, if
    `candidate_and_leader_coro()` is running, it holds this mutex when actively
    manipulating state and releases it when waiting. In general we don't hold the mutex
    when responding to an interruptor. */
    new_mutex_t mutex;

    /* This mutex assertion controls writes to the Raft log and associated state.
    Specifically, anything writing to `ps.log`, `ps.snapshot`, `committed_state`,
    `committed_config`, `commit_index`, or `last_applied` should hold this mutex
    assertion.
    If we are follower, `on_append_entries_rpc()` and `on_install_snapshot_rpc()` acquire
    this temporarily; if we are candidate or leader, `candidate_and_leader_coro()` holds
    this at all times. */
    mutex_assertion_t log_mutex;

    /* This makes sure that `election_and_leader_coro()` stops when the `raft_member_t`
    is destroyed. It's in a `scoped_ptr_t` so that `become_follower_led()` can destroy it
    to kill `election_and_leader_coro()`. */
    scoped_ptr_t<auto_drainer_t> election_and_leader_drainer;

    /* Occasionally we have to spawn miscellaneous coroutines. This makes sure that they
    all get stopped before the `raft_member_t` is destroyed. It's in a `scoped_ptr_t` so
    that the destructor can destroy it early. */
    scoped_ptr_t<auto_drainer_t> drainer;

    /* This calls `update_readiness_for_change()` whenever a peer connects or
    disconnects. */
    scoped_ptr_t<watchable_map_t<raft_member_id_t, empty_value_t>::all_subs_t>
        connected_members_subs;
};

#endif /* CLUSTERING_GENERIC_RAFT_CORE_HPP_ */

