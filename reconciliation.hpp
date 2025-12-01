#ifndef RECONCILIATION_HPP
#define RECONCILIATION_HPP

#include <algorithm>
#include <functional>
#include <sstream>
#include <vector>

#include "sbpt_generated_includes.hpp"

/*
 * @brief Generic reconciliation system that hides internal bookkeeping.
 *
 * The point of reconciliation is you have some intermittent authorative state which drives some object being received
 * with some delay, so locally you use StateUpdateData to update that object so that it feels live, which introduces
 * some potential drift from the authorative state, but when you reconcile it keeps it a constant number of StateUpdates
 * away from the authorative state.
 *
 * User defines:
 *  - StateT:  game state (position, velocity, etc.)
 *  - StateUpdateDataT: user update data (input deltas, etc.)
 *
 * Facts:
 * 1. As you reconcile your current state is always equal to the authorative state last used in reconciling plus a few
 * state update datas applied on top of it (this is usually a constant number of updates)
 *
 */
template <typename StateT, typename StateUpdateDataT> class Reconciliation {
  public:
    struct IdTaggedState {
        StateT state;
        unsigned int last_sud_id_used_to_update = 0;
    };

    struct IdTaggedStateUpdateData {
        StateUpdateDataT update_data;
        unsigned int id = 0;
    };

    // NOTE: the reason this isn't const is due to the fact that sometimes the state update data contains an object that
    // has a non-const function that needs to be run during the update, and this wouldn't be possible if we passed in a
    // const.
    // NOTE: the bool indicates whether or not this is being called during reconciliation, note this distinction is
    // required so that sound effect or other non-related things don't get called again during reconciliation.
    using UpdateFunction = std::function<void(StateUpdateDataT &, bool)>;

    using GetStateFunction = std::function<StateT()>;
    using SetStateFunction = std::function<void(const StateT &)>;
    using StateToStringFn = std::function<std::string(const StateT &)>;
    using UpdateToStringFn = std::function<std::string(const StateUpdateDataT &)>;
    using DiffToStringFn = std::function<std::string(const StateT &, const StateT &)>;

    Reconciliation(
        UpdateFunction fn, GetStateFunction get_fn, SetStateFunction set_fn,
        StateToStringFn state_to_string_fn = [](const StateT &) { return ""; },
        UpdateToStringFn update_to_string_fn = [](const StateUpdateDataT &) { return ""; },
        DiffToStringFn diff_to_string_fn = [](const StateT &, const StateT &) { return ""; })
        : user_update_fn(std::move(fn)), get_state_fn(std::move(get_fn)), set_state_fn(std::move(set_fn)),
          state_to_string_fn(std::move(state_to_string_fn)), update_to_string_fn(std::move(update_to_string_fn)),
          diff_to_string_fn(std::move(diff_to_string_fn)) {}

    /**
     * @brief Applies an update and returns the update id assigned to it.
     *
     * @return the id that was assigned to this update data
     * this function is utilized for regular updates and reconciliation updates
     */
    unsigned int apply_update(const StateUpdateDataT &user_update) {
        GlobalLogSection _("apply_update");

        IdTaggedStateUpdateData wrapped{user_update, next_update_id++};
        global_logger->info("applying update: {}", to_string(wrapped));

        global_logger->info("state before applying: {}", state_to_string_fn(get_state_fn()));
        user_update_fn(wrapped.update_data, false);
        update_datas_applied_since_last_authorative_state.push_back(wrapped);
        global_logger->info("state after applying: {}", state_to_string_fn(get_state_fn()));

        return wrapped.id;
    }

    /// Called on client when receiving authoritative server state
    void reconcile(const IdTaggedState &authoritative_state) {
        GlobalLogSection _("reconcile");

        StateT predicted_state = get_state_fn();

        set_state_fn(authoritative_state.state);
        global_logger->info("just set state to: {}", to_string(authoritative_state));

        // Remove acknowledged updates
        auto it = std::remove_if(
            update_datas_applied_since_last_authorative_state.begin(),
            update_datas_applied_since_last_authorative_state.end(),
            [&](const IdTaggedStateUpdateData &u) { return u.id <= authoritative_state.last_sud_id_used_to_update; });

        if (it != update_datas_applied_since_last_authorative_state.end()) {
            global_logger->info("removing acknowledged updates up to id={}",
                                authoritative_state.last_sud_id_used_to_update);
        }
        update_datas_applied_since_last_authorative_state.erase(
            it, update_datas_applied_since_last_authorative_state.end());

        global_logger->info("we need to reapply {} update datas, specifically we need to apply:",
                            update_datas_applied_since_last_authorative_state.size());
        for (const auto &u : update_datas_applied_since_last_authorative_state)
            global_logger->info("  [id={}]", u.id);

        // reapply unacknowledged updates
        for (auto &u : update_datas_applied_since_last_authorative_state) {
            global_logger->info("reapplying state update data: {}", to_string(u));
            global_logger->info("state before re-applying: {}", state_to_string_fn(get_state_fn()));
            user_update_fn(u.update_data, true);
            global_logger->info("state after re-applying: {}", state_to_string_fn(get_state_fn()));
        }

        // compare final reconciled vs predicted
        global_logger->info("Finished reconciliation.");
        StateT reconciled_state = get_state_fn();
        global_logger->info("Predicted state was: {}", state_to_string_fn(predicted_state));
        global_logger->info("Reconciled state was: {}", state_to_string_fn(reconciled_state));
        global_logger->info("State difference (predicted vs reconciled):\n{}",
                            diff_to_string_fn(predicted_state, reconciled_state));
    }

    void server_apply_update(IdTaggedState &server_state, const IdTaggedStateUpdateData &update) {
        GlobalLogSection _("server_apply_update");

        global_logger->info("Applying server update id={}", update.id);
        user_update_fn(update.update_data, false);
        server_state.last_sud_id_used_to_update = update.id;

        global_logger->info("Server state after update id={}", update.id);
        global_logger->info("{}", to_string(server_state));
    }

    /// Helper to get the wrapped state for transmission
    IdTaggedState get_internal_state() const {
        GlobalLogSection _("get_internal_state");

        global_logger->info("Returning internal state -> next_id={}", next_update_id - 1);
        global_logger->info("{}", to_string(IdTaggedState{get_state_fn(), next_update_id - 1}));

        return {get_state_fn(), next_update_id - 1};
    }

    const std::vector<IdTaggedStateUpdateData> &get_update_datas_applied_since_last_authorative_state() const {
        return update_datas_applied_since_last_authorative_state;
    }

    std::vector<IdTaggedStateUpdateData> get_update_data_applied_since_last_authorative_state_copy() const {
        return update_datas_applied_since_last_authorative_state;
    }

  private:
    /// Internal helpers that define how to stringify wrapped data types
    std::string to_string(const IdTaggedState &s) const {
        std::ostringstream ss;
        ss << "[last_sud_id_used_to_update=" << s.last_sud_id_used_to_update << "] " << state_to_string_fn(s.state);
        return ss.str();
    }

    std::string to_string(const IdTaggedStateUpdateData &u) const {
        std::ostringstream ss;
        ss << "[id=" << u.id << "] " << update_to_string_fn(u.update_data);
        return ss.str();
    }

  private:
    std::vector<IdTaggedStateUpdateData> update_datas_applied_since_last_authorative_state;
    unsigned int next_update_id = 1;

    UpdateFunction user_update_fn;
    GetStateFunction get_state_fn;
    SetStateFunction set_state_fn;

    StateToStringFn state_to_string_fn;
    UpdateToStringFn update_to_string_fn;
    DiffToStringFn diff_to_string_fn;
};

#endif // RECONCILIATION_HPP
