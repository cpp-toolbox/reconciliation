#ifndef RECONCILIATION_HPP
#define RECONCILIATION_HPP

#include <algorithm>
#include <functional>
#include <sstream>
#include <vector>

#include "sbpt_generated_includes.hpp"

///
/*
 * @brief Generic reconciliation system that hides internal bookkeeping.
 *
 * User defines:
 *  - StateT:  game state (position, velocity, etc.)
 *  - StateUpdateDataT: user update data (input deltas, etc.)
 *
 * Facts:
 * 1. As you reconcile your current state is always equal to the authorative state last used in reconciling plus a few
 * state update datas applied on top of it
 *
 *
 */
template <typename StateT, typename StateUpdateDataT> class Reconciliation {
  public:
    struct IdTaggedState {
        StateT state;
        unsigned int last_sud_id_used_to_update = 0;
    };

    struct IdTaggedStateUpdateData {
        StateUpdateDataT update;
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
     * this function is utilized for regular updates and reconciliation updates
     */
    unsigned int apply_update(const StateUpdateDataT &user_update) {
        LogSection _(*global_logger, "apply_update");

        IdTaggedStateUpdateData wrapped{user_update, next_update_id++};
        global_logger->info("Applying update: {}", to_string(wrapped));

        global_logger->info("State before updating: {}", state_to_string_fn(get_state_fn()));

        user_update_fn(wrapped.update, false);
        update_data_applied_since_last_authorative_state.push_back(wrapped);

        global_logger->info("State after updating: {}", state_to_string_fn(get_state_fn()));

        return wrapped.id;
    }

    /// Called on client when receiving authoritative server state
    void reconcile(const IdTaggedState &authoritative_state) {
        LogSection _(*global_logger, "reconcile");

        StateT predicted_state = get_state_fn();

        set_state_fn(authoritative_state.state);
        global_logger->info("just set state to: {}", to_string(authoritative_state));

        // Remove acknowledged updates
        auto it = std::remove_if(
            update_data_applied_since_last_authorative_state.begin(),
            update_data_applied_since_last_authorative_state.end(),
            [&](const IdTaggedStateUpdateData &u) { return u.id <= authoritative_state.last_sud_id_used_to_update; });

        if (it != update_data_applied_since_last_authorative_state.end()) {
            global_logger->info("Removing acknowledged updates up to id={}",
                                authoritative_state.last_sud_id_used_to_update);
        }
        update_data_applied_since_last_authorative_state.erase(it,
                                                               update_data_applied_since_last_authorative_state.end());

        global_logger->info("We need to reapply {} update datas, specifically we need to apply:",
                            update_data_applied_since_last_authorative_state.size());
        for (const auto &u : update_data_applied_since_last_authorative_state)
            global_logger->info("  [id={}]", u.id);

        // Reapply unacknowledged updates
        for (auto &u : update_data_applied_since_last_authorative_state) {
            global_logger->info("Reapplying state update data: {}", to_string(u));
            user_update_fn(u.update, true);
            global_logger->info("State after: {}", state_to_string_fn(get_state_fn()));
        }

        // Compare final reconciled vs predicted
        global_logger->info("Finished reconciliation.");
        StateT reconciled_state = get_state_fn();
        global_logger->info("Predicted state was: {}", state_to_string_fn(predicted_state));
        global_logger->info("Reconciled state was: {}", state_to_string_fn(reconciled_state));
        global_logger->info("State difference (predicted vs reconciled):\n{}",
                            diff_to_string_fn(predicted_state, reconciled_state));
    }

    /// Called on server when applying client updates
    void server_apply_update(IdTaggedState &server_state, const IdTaggedStateUpdateData &update) {
        LogSection _(*global_logger, "server_apply_update");

        global_logger->info("Applying server update id={}", update.id);
        user_update_fn(update.update, false);
        server_state.last_sud_id_used_to_update = update.id;

        global_logger->info("Server state after update id={}", update.id);
        global_logger->info("{}", to_string(server_state));
    }

    /// Helper to get the wrapped state for transmission
    IdTaggedState get_internal_state() const {
        LogSection _(*global_logger, "get_internal_state");

        global_logger->info("Returning internal state -> next_id={}", next_update_id - 1);
        global_logger->info("{}", to_string(IdTaggedState{get_state_fn(), next_update_id - 1}));

        return {get_state_fn(), next_update_id - 1};
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
        ss << "[id=" << u.id << "] " << update_to_string_fn(u.update);
        return ss.str();
    }

  private:
    std::vector<IdTaggedStateUpdateData> update_data_applied_since_last_authorative_state;
    unsigned int next_update_id = 1;

    UpdateFunction user_update_fn;
    GetStateFunction get_state_fn;
    SetStateFunction set_state_fn;

    StateToStringFn state_to_string_fn;
    UpdateToStringFn update_to_string_fn;
    DiffToStringFn diff_to_string_fn;
};

#endif // RECONCILIATION_HPP
