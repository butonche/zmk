/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_combos

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

// static K_SEM_DEFINE(candidates_sem, 1, 1);

struct combo_cfg {
    int32_t key_positions[CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO];
    int32_t key_position_len;
    struct zmk_behavior_binding behavior;
    int32_t timeout_ms;
    // if slow release is set, the combo releases when the last key is released.
    // otherwise, the combo releases when the first key is released.
    bool slow_release;
    // the virtual key position is a key position outside the range used by the keyboard.
    // it is necessary so hold-taps can uniquely identify a behavior.
    int32_t virtual_key_position;
    int32_t layers_len;
    int8_t layers[];
};

struct active_combo {
    struct combo_cfg *combo;
    // key_positions_pressed is filled with key_positions when the combo is pressed.
    // The keys are removed from this array when they are released.
    // Once this array is empty, the behavior is released.
    const zmk_event_t *key_positions_pressed[CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO];
};

struct combo_candidate {
    struct combo_cfg *combo;
    // the time after which this behavior should be removed from candidates.
    // by keeping track of when the candidate should be cleared there is no
    // possibility of accidental releases.
    int64_t timeout_at;
};

// set of keys pressed split in sets for each potential combo
const zmk_event_t *pressed_keys[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS]
                               [CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO] = {NULL};
// the set of candidate combos based on the currently pressed_keys split in sets for each
struct combo_candidate candidates[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS]
                                 [CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY];
// the last candidate that was completely pressed
struct combo_cfg *fully_pressed_combo[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS] = {NULL};
// a lookup dict that maps a key position to all combos on that position
struct combo_cfg *combo_lookup[ZMK_KEYMAP_LEN][CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY] = {NULL};
// combos that have been activated and still have (some) keys pressed
// this array is always contiguous from 0.
struct active_combo active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS] = {NULL};
int active_combo_count = 0;
int position_to_candidate_set[ZMK_KEYMAP_LEN] = {[0 ... ZMK_KEYMAP_LEN - 1] = -1};
int pressed_key_to_candidate_set[ZMK_KEYMAP_LEN] = {[0 ... ZMK_KEYMAP_LEN - 1] = -1};
int last_used_candidate_set = -1;

struct k_work_delayable timeout_task[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS];
int64_t timeout_task_timeout_at[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS];

// Store the combo key pointer in the combos array, one pointer for each key position
// The combos are sorted shortest-first, then by virtual-key-position.
static int initialize_combo(struct combo_cfg *new_combo) {
    for (int i = 0; i < new_combo->key_position_len; i++) {
        int32_t position = new_combo->key_positions[i];
        if (position >= ZMK_KEYMAP_LEN) {
            LOG_ERR("Unable to initialize combo, key position %d does not exist", position);
            return -EINVAL;
        }

        struct combo_cfg *insert_combo = new_combo;
        bool set = false;
        for (int j = 0; j < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; j++) {
            struct combo_cfg *combo_at_j = combo_lookup[position][j];
            if (combo_at_j == NULL) {
                combo_lookup[position][j] = insert_combo;
                set = true;
                break;
            }
            if (combo_at_j->key_position_len < insert_combo->key_position_len ||
                (combo_at_j->key_position_len == insert_combo->key_position_len &&
                 combo_at_j->virtual_key_position < insert_combo->virtual_key_position)) {
                continue;
            }
            // put insert_combo in this spot, move all other combos up.
            combo_lookup[position][j] = insert_combo;
            insert_combo = combo_at_j;
        }
        if (!set) {
            LOG_ERR("Too many combos for key position %d, CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY %d.",
                    position, CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY);
            return -ENOMEM;
        }
    }
    return 0;
}

static bool combo_active_on_layer(struct combo_cfg *combo, uint8_t layer) {
    if (combo->layers[0] == -1) {
        // -1 in the first layer position is global layer scope
        return true;
    }
    for (int j = 0; j < combo->layers_len; j++) {
        if (combo->layers[j] == layer) {
            return true;
        }
    }
    return false;
}

static void update_possible_positions_for_candidates(int candidate_set) {
    for (int i = 0; i < ZMK_KEYMAP_LEN; i++) {
        if (position_to_candidate_set[i] == candidate_set) {
            position_to_candidate_set[i] = -1;
        }
    }
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; i++) {
        struct combo_cfg *combo = candidates[candidate_set][i].combo;
        if (combo == NULL) {
            break;
        } else {
            for (int j = 0; j < combo->key_position_len; j++) {
                position_to_candidate_set[combo->key_positions[j]] = candidate_set;
            }
        }
    }
}

static int select_candidate_set(int32_t position) {
    int candidate_set = position_to_candidate_set[position];
    if (candidate_set == -1) {
        struct combo_cfg *combo_at_j = combo_lookup[position][0];
        // check if this position is part of a combo, if not, add it to the last used candidate set
        if (combo_at_j == NULL) {
            candidate_set = last_used_candidate_set;
        }
        // else, we want to find an empty candidate set
        else {
            int used_sets[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS] = {0};
            for (int i = 0; i < ZMK_KEYMAP_LEN; i++) {
                if (position_to_candidate_set[i] != -1) {
                    used_sets[position_to_candidate_set[i]] = 1;
                }
            }
            for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
                if (used_sets[i] == 0) {
                    candidate_set = i;
                    break;
                }
            }
        }
    }
    if (candidate_set == -1) {
        LOG_ERR("combo: could not find an empty candidate set!");
    } else {
        // LOG_DBG("combo: selected candidate set %d.", candidate_set);
        last_used_candidate_set = candidate_set;
    }

    return candidate_set;
}

static int setup_candidates_for_first_keypress(int32_t position, int64_t timestamp,
                                               int candidate_set) {
    int number_of_combo_candidates = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();

    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; i++) {
        struct combo_cfg *combo = combo_lookup[position][i];
        if (combo == NULL) {
            break;
        }
        if (combo_active_on_layer(combo, highest_active_layer)) {
            candidates[candidate_set][number_of_combo_candidates].combo = combo;
            candidates[candidate_set][number_of_combo_candidates].timeout_at =
                timestamp + combo->timeout_ms;
            number_of_combo_candidates++;
        }
        // LOG_DBG("combo timeout %d %d %d", position, i, candidates[i].timeout_at);
    }
    update_possible_positions_for_candidates(candidate_set);
    return number_of_combo_candidates;
}

static int filter_candidates(int32_t position, int candidate_set) {
    // this code iterates over candidates and the lookup together to filter in O(n)
    // assuming they are both sorted on key_position_len, virtal_key_position
    int matches = 0, lookup_idx = 0, candidate_idx = 0;
    while (lookup_idx < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY &&
           candidate_idx < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY) {
        struct combo_cfg *candidate = candidates[candidate_set][candidate_idx].combo;
        struct combo_cfg *lookup = combo_lookup[position][lookup_idx];
        if (candidate == NULL || lookup == NULL) {
            break;
        }
        if (candidate->virtual_key_position == lookup->virtual_key_position) {
            candidates[candidate_set][matches] = candidates[candidate_set][candidate_idx];
            matches++;
            candidate_idx++;
            lookup_idx++;
        } else if (candidate->key_position_len > lookup->key_position_len) {
            lookup_idx++;
        } else if (candidate->key_position_len < lookup->key_position_len) {
            candidate_idx++;
        } else if (candidate->virtual_key_position > lookup->virtual_key_position) {
            lookup_idx++;
        } else if (candidate->virtual_key_position < lookup->virtual_key_position) {
            candidate_idx++;
        }
    }
    // clear unmatched candidates
    for (int i = matches; i < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; i++) {
        candidates[candidate_set][i].combo = NULL;
    }
    update_possible_positions_for_candidates(candidate_set);
    // LOG_DBG("combo matches after filter %d", matches);
    return matches;
}

static int64_t first_candidate_timeout(int candidate_set) {
    int64_t first_timeout = LONG_MAX;
    for (int j = 0; j < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; j++) {
        if (candidates[candidate_set][j].combo == NULL) {
            break;
        }
        if (candidates[candidate_set][j].timeout_at < first_timeout) {
            first_timeout = candidates[candidate_set][j].timeout_at;
        }
    }
    LOG_DBG("combo: timeout %lld", first_timeout);
    return first_timeout;
}

static inline bool candidate_is_completely_pressed(struct combo_cfg *candidate, int candidate_set) {
    // this code assumes set(pressed_keys) <= set(candidate->key_positions)
    // this invariant is enforced by filter_candidates
    // since events may have been reraised after clearing one or more slots at
    // the start of pressed_keys (see: release_pressed_keys), we have to check
    // that each key needed to trigger the combo was pressed, not just the last.
    for (int i = 0; i < candidate->key_position_len; i++) {
        if (pressed_keys[candidate_set][i] == NULL) {
            return false;
        }
    }
    return true;
}

static int cleanup(int candidate_set);

static int filter_timed_out_candidates(int64_t timestamp, int candidate_set) {
    int num_candidates = 0;
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; i++) {
        struct combo_candidate *candidate = &candidates[candidate_set][i];
        if (candidate->combo == NULL) {
            break;
        }
        if (candidate->timeout_at > timestamp) {
            // reorder candidates so they're contiguous
            candidates[candidate_set][num_candidates].combo = candidate->combo;
            candidates[candidate_set][num_candidates].timeout_at = candidate->timeout_at;
            num_candidates++;
        } else {
            candidate->combo = NULL;
        }
    }
    update_possible_positions_for_candidates(candidate_set);
    return num_candidates;
}

static void clear_candidates(int candidate_set) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; i++) {
        if (candidates[candidate_set][i].combo == NULL) {
            break;
        }
        candidates[candidate_set][i].combo = NULL;
    }
    update_possible_positions_for_candidates(candidate_set);
}

static int capture_pressed_key(const zmk_event_t *ev, int candidate_set) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO; i++) {
        if (pressed_keys[candidate_set][i] != NULL) {
            continue;
        }
        pressed_keys[candidate_set][i] = ev;
        return ZMK_EV_EVENT_CAPTURED;
    }
    return 0;
}

const struct zmk_listener zmk_listener_combo;

static int release_pressed_keys(int candidate_set) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO; i++) {
        const zmk_event_t *captured_event = pressed_keys[candidate_set][i];
        if (pressed_keys[candidate_set][i] == NULL) {
            return i;
        }
        pressed_keys[candidate_set][i] = NULL;
        if (i == 0) {
            LOG_DBG("combo: releasing position event %d",
                    as_zmk_position_state_changed(captured_event)->position);
            ZMK_EVENT_RELEASE(captured_event)
        } else {
            // reprocess events (see tests/combo/fully-overlapping-combos-3 for why this is
            // needed)
            LOG_DBG("combo: reraising position event %d",
                    as_zmk_position_state_changed(captured_event)->position);
            ZMK_EVENT_RAISE(captured_event);
        }
    }
    return CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO;
}

static inline int press_combo_behavior(struct combo_cfg *combo, int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = combo->virtual_key_position,
        .timestamp = timestamp,
    };

    return behavior_keymap_binding_pressed(&combo->behavior, event);
}

static inline int release_combo_behavior(struct combo_cfg *combo, int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = combo->virtual_key_position,
        .timestamp = timestamp,
    };

    return behavior_keymap_binding_released(&combo->behavior, event);
}

static void move_pressed_keys_to_active_combo(struct active_combo *active_combo,
                                              int candidate_set) {
    int combo_length = active_combo->combo->key_position_len;
    for (int i = 0; i < combo_length; i++) {
        active_combo->key_positions_pressed[i] = pressed_keys[candidate_set][i];
        pressed_keys[candidate_set][i] = NULL;
    }
    // move any other pressed keys up
    for (int i = 0; i + combo_length < CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO; i++) {
        if (pressed_keys[candidate_set][i + combo_length] == NULL) {
            return;
        }
        pressed_keys[candidate_set][i] = pressed_keys[candidate_set][i + combo_length];
        pressed_keys[candidate_set][i + combo_length] = NULL;
    }
}

static struct active_combo *store_active_combo(struct combo_cfg *combo) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        if (active_combos[i].combo == NULL) {
            active_combos[i].combo = combo;
            active_combo_count++;
            return &active_combos[i];
        }
    }
    LOG_ERR("Unable to store combo; already %d active. Increase "
            "CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS",
            CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS);
    return NULL;
}

static void activate_combo(struct combo_cfg *combo, int candidate_set) {
    struct active_combo *active_combo = store_active_combo(combo);
    if (active_combo == NULL) {
        // unable to store combo
        release_pressed_keys(candidate_set);
        return;
    }
    move_pressed_keys_to_active_combo(active_combo, candidate_set);
    press_combo_behavior(
        combo, as_zmk_position_state_changed(active_combo->key_positions_pressed[0])->timestamp);
}

static void deactivate_combo(int active_combo_index) {
    active_combo_count--;
    if (active_combo_index != active_combo_count) {
        memcpy(&active_combos[active_combo_index], &active_combos[active_combo_count],
               sizeof(struct active_combo));
    }
    active_combos[active_combo_count].combo = NULL;
    active_combos[active_combo_count] = (struct active_combo){0};
}

/* returns true if a key was released. */
static bool release_combo_key(int32_t position, int64_t timestamp) {
    for (int combo_idx = 0; combo_idx < active_combo_count; combo_idx++) {
        struct active_combo *active_combo = &active_combos[combo_idx];

        bool key_released = false;
        bool all_keys_pressed = true;
        bool all_keys_released = true;
        for (int i = 0; i < active_combo->combo->key_position_len; i++) {
            if (active_combo->key_positions_pressed[i] == NULL) {
                all_keys_pressed = false;
            } else if (as_zmk_position_state_changed(active_combo->key_positions_pressed[i])
                           ->position != position) {
                all_keys_released = false;
            } else { // not null and position matches
                ZMK_EVENT_FREE(active_combo->key_positions_pressed[i]);
                active_combo->key_positions_pressed[i] = NULL;
                key_released = true;
            }
        }

        if (key_released) {
            if ((active_combo->combo->slow_release && all_keys_released) ||
                (!active_combo->combo->slow_release && all_keys_pressed)) {
                release_combo_behavior(active_combo->combo, timestamp);
                LOG_DBG("combo: released combo at %lld", timestamp);
            }
            if (all_keys_released) {
                deactivate_combo(combo_idx);
            }
            return true;
        }
    }
    return false;
}

static int cleanup(int candidate_set) {
    int cancel_state = k_work_cancel_delayable(&timeout_task[candidate_set]);
    LOG_DBG("combo: cleanup cancel state %d for set %d", cancel_state, candidate_set);

    clear_candidates(candidate_set);
    if (fully_pressed_combo[candidate_set] != NULL) {
        activate_combo(fully_pressed_combo[candidate_set], candidate_set);
        fully_pressed_combo[candidate_set] = NULL;
    }
    return release_pressed_keys(candidate_set);
}

static void update_timeout_task(int candidate_set) {
    int64_t first_timeout = first_candidate_timeout(candidate_set);
    if (timeout_task_timeout_at[candidate_set] == first_timeout) {
        LOG_DBG("combo: timeout skipped %lld", k_uptime_get());
        return;
    }
    if (first_timeout == LONG_MAX) {
        timeout_task_timeout_at[candidate_set] = 0;
        k_work_cancel_delayable(&timeout_task[candidate_set]);
        return;
    }
    if (k_work_schedule(&timeout_task[candidate_set], K_MSEC(first_timeout - k_uptime_get())) >=
        0) {
        timeout_task_timeout_at[candidate_set] = first_timeout;
        LOG_DBG("combo: timeout re-scheduled");
    } else {
        LOG_DBG("combo: timeout was not re-scheduled %lld", k_uptime_get());
    }
}

static int position_state_down(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    LOG_DBG("combo: key down start");
    // if (k_sem_take(&candidates_sem, K_FOREVER) < 0) {
    //     LOG_DBG("combo: could not acquire semafore 1");
    //     return 0;
    // }
    int num_candidates;
    int candidate_set = select_candidate_set(data->position);
    pressed_key_to_candidate_set[data->position] = candidate_set;
    if (candidates[candidate_set][0].combo == NULL) {
        num_candidates =
            setup_candidates_for_first_keypress(data->position, data->timestamp, candidate_set);
        if (num_candidates == 0) {
            // k_sem_give(&candidates_sem);
            return 0;
        }
    } else {
        filter_timed_out_candidates(data->timestamp, candidate_set);
        num_candidates = filter_candidates(data->position, candidate_set);
    }
    update_timeout_task(candidate_set);

    struct combo_cfg *candidate_combo = candidates[candidate_set][0].combo;
    LOG_DBG("combo: capturing position event %d", data->position);
    int ret = capture_pressed_key(ev, candidate_set);
    switch (num_candidates) {
    case 0:
        cleanup(candidate_set);
        break;
    case 1:
        if (candidate_is_completely_pressed(candidate_combo, candidate_set)) {
            fully_pressed_combo[candidate_set] = candidate_combo;
            cleanup(candidate_set);
        }
        break;
    default:
        if (candidate_is_completely_pressed(candidate_combo, candidate_set)) {
            fully_pressed_combo[candidate_set] = candidate_combo;
        }
        break;
    }

    // k_sem_give(&candidates_sem);
    LOG_DBG("combo: key down end");
    return ret;
}

static int position_state_up(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    LOG_DBG("combo: key up start");
    // if (k_sem_take(&candidates_sem, K_FOREVER) < 0) {
    //     LOG_DBG("combo: could not acquire semafore 2");
    //     return 0;
    // }

    int ret = 0;
    int candidate_set = pressed_key_to_candidate_set[data->position];
    pressed_key_to_candidate_set[data->position] = -1;
    if (candidate_set == -1) {
        LOG_ERR("combo: could not determine candidate set when releasing position %d",
                data->position);
    } else {
        int released_keys = cleanup(candidate_set);
        if (release_combo_key(data->position, data->timestamp)) {
            ret = ZMK_EV_EVENT_HANDLED;
        } else if (released_keys > 1) {
            // The second and further key down events are re-raised. To preserve
            // correct order for e.g. hold-taps, reraise the key up event too.
            ZMK_EVENT_RAISE(ev);
            ret = ZMK_EV_EVENT_CAPTURED;
        }
    }
    // k_sem_give(&candidates_sem);
    LOG_DBG("combo: key up end");
    return ret;
}

static void combo_timeout_handler(struct k_work *item) {
    LOG_DBG("combo: timeout handler start");
    // if (k_sem_take(&candidates_sem, K_FOREVER) < 0) {
    //     LOG_DBG("combo: could not acquire semafore 3");
    //     return;
    // }
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        if ((k_work_delayable_busy_get(&timeout_task[i]) & 1) == 1) {
            LOG_DBG("combo: timeout handler for set %d", i);
            if (timeout_task_timeout_at[i] == 0 || k_uptime_get() < timeout_task_timeout_at[i]) {
                // timer was cancelled or rescheduled.
                break;
            }
            if (filter_timed_out_candidates(timeout_task_timeout_at[i], i) < 2) {
                cleanup(i);
            }
            update_timeout_task(i);
            break;
        }
    }
    // k_sem_give(&candidates_sem);
    LOG_DBG("combo: timeout handler end");
}

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
    if (data == NULL) {
        return 0;
    }

    if (data->state) { // keydown
        return position_state_down(ev, data);
    } else { // keyup
        return position_state_up(ev, data);
    }
}

ZMK_LISTENER(combo, position_state_changed_listener);
ZMK_SUBSCRIPTION(combo, zmk_position_state_changed);

#define COMBO_INST(n)                                                                              \
    static struct combo_cfg combo_config_##n = {                                                   \
        .timeout_ms = DT_PROP(n, timeout_ms),                                                      \
        .key_positions = DT_PROP(n, key_positions),                                                \
        .key_position_len = DT_PROP_LEN(n, key_positions),                                         \
        .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, n),                                              \
        .virtual_key_position = ZMK_VIRTUAL_KEY_POSITION_COMBO(__COUNTER__),                       \
        .slow_release = DT_PROP(n, slow_release),                                                  \
        .layers = DT_PROP(n, layers),                                                              \
        .layers_len = DT_PROP_LEN(n, layers),                                                      \
    };

#define INITIALIZE_COMBO(n) initialize_combo(&combo_config_##n);

DT_INST_FOREACH_CHILD(0, COMBO_INST)

static int combo_init() {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        k_work_init_delayable(&timeout_task[i], combo_timeout_handler);
    }
    DT_INST_FOREACH_CHILD(0, INITIALIZE_COMBO);
    return 0;
}

SYS_INIT(combo_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif
