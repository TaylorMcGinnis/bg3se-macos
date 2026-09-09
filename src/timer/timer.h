/**
 * timer.h - Timer system for scheduling delayed and repeating callbacks
 *
 * Features:
 * - One-shot timers (fire once after delay)
 * - Repeating timers (fire at regular intervals)
 * - Pause/resume support
 * - High-resolution monotonic timing via mach_absolute_time
 */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include <stdbool.h>
#include <lua.h>

// Initial timer pool / free-list capacity; the pool grows on demand
// (upstream uses an unbounded SaltedPool, and a large load order schedules
// hundreds of timers in a single LevelGameplayStarted burst).
#define TIMER_INITIAL_CAPACITY 256

// Timer handle type (returned to Lua)
typedef uint64_t TimerHandle;

// Initialize the timer system
void timer_init(void);

// Shutdown and cleanup all timers
void timer_shutdown(lua_State *L);

// Create a one-shot timer
// Returns handle, or 0 on failure
TimerHandle timer_create(lua_State *L, double delay_ms, int callback_ref, double repeat_ms);

// Cancel a timer
// Returns true if timer was found and cancelled
bool timer_cancel(lua_State *L, TimerHandle handle);

// Pause a timer
bool timer_pause(TimerHandle handle);

// Resume a paused timer
bool timer_resume(TimerHandle handle);

// Check if timer is paused
bool timer_is_paused(TimerHandle handle);

// Update timers - call from game tick
// Fires any expired timers
void timer_update(lua_State *L);

// Get monotonic time in milliseconds since process start
double timer_get_monotonic_ms(void);

// Get monotonic time in microseconds since process start
double timer_get_microsec_time(void);

// Get Unix timestamp (seconds since epoch)
int64_t timer_get_epoch_seconds(void);

// Get formatted clock time "YYYY-MM-DD HH:MM:SS"
// Note: Returns pointer to static buffer, not thread-safe
const char* timer_get_clock_time(void);

// Clear all timers (call on Lua state reset)
void timer_clear_all(lua_State *L);

// ============================================================================
// Game Time Tracking
// ============================================================================

// Called on each game tick with the delta time
// This updates the internal game time counter
void timer_tick(double delta_ms);

// Pause game time tracking (game is paused)
void timer_pause_game_time(void);

// Resume game time tracking (game is unpaused)
void timer_resume_game_time(void);

// Check if game time is paused
bool timer_is_game_paused(void);

// Get current game time in seconds (pauses when game pauses)
double timer_get_game_time(void);

// Get last frame's delta time in seconds
double timer_get_delta_time(void);

// Get game tick count
int64_t timer_get_tick_count(void);

// ============================================================================
// Persistent Timers (survive save/load)
// ============================================================================

// Maximum callback name length
#define TIMER_CALLBACK_NAME_MAX 64

// Maximum args JSON length
#define TIMER_ARGS_JSON_MAX 1024

// Maximum persistent timers
#define PERSISTENT_TIMER_MAX 64

// Register a named persistent handler
// Returns true on success
bool timer_register_persistent_handler(lua_State *L, const char *name, int callback_ref);

// Unregister a persistent handler
bool timer_unregister_persistent_handler(lua_State *L, const char *name);

// Create a persistent timer (uses named handler, serializable args)
// Returns handle, or 0 on failure
TimerHandle timer_create_persistent(lua_State *L, double delay_ms, const char *handler_name,
                                     const char *args_json, double repeat_ms);

// Cancel a persistent timer
bool timer_cancel_persistent(lua_State *L, TimerHandle handle);

// Export all persistent timers to JSON (for save)
// Returns malloc'd string, caller must free
char* timer_export_persistent(void);

// Import persistent timers from JSON (for load)
// Returns number of timers restored
int timer_import_persistent(lua_State *L, const char *json);

// Clear all persistent timers and handlers
void timer_clear_persistent(lua_State *L);

// Update persistent timers
void timer_update_persistent(lua_State *L);

#endif // TIMER_H
