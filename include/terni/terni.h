// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
#ifndef TERNI_H
#define TERNI_H
/*
 * Terni's C API: the simulation behind plain C functions, for engines and languages that
 * cannot use the C++ API (C#, other languages through their foreign function interface,
 * engines whose headers are not C++20). Link the shared library `terni` (with the rule
 * compiler) or `terni_runtime` (without it: rules come from a SPIR-V cache); both export
 * exactly these functions. docs/c-api.md explains it with examples.
 *
 * Conventions:
 * - A terni_world holds one game and, once started, its simulation on one device. Use a
 *   world from one thread at a time; different worlds may run on different threads.
 * - Calls that can fail return a terni_status. TERNI_OK is 0; on anything else
 *   terni_last_error() says what went wrong. Out parameters are written only on success.
 *   No call throws, aborts or exits.
 * - Lookups by name return -1 when there is no such thing; they never fail otherwise.
 * - Strings are NUL-terminated UTF-8. Strings the API returns belong to Terni: each
 *   call says how long they stay valid. Paths are UTF-8 too, on Windows as well.
 * - Lists (devices, diagnostics, hashes, stage times, ...) are kept in the world, or for
 *   devices in the library: a call fills one and returns its length, and an accessor reads
 *   an item by index. A list stays valid until the call that fills it is made again, or
 *   the world loads, starts, stops or is destroyed.
 * - Truth values are int32_t: 0 is false, anything else true.
 * - Entity types, fields of an entity type, bundles, modifier keys and defines are
 *   numbered from 0 in the order the game declares them.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(TERNI_C_BUILD)
#    define TERNI_API __declspec(dllexport)
#  else
#    define TERNI_API __declspec(dllimport)
#  endif
#else
#  define TERNI_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The version of this interface. Compare terni_abi_version() with TERNI_ABI_VERSION at
 * startup: a library built from another version may lay out its structs differently. */
#define TERNI_ABI_VERSION 1

typedef int32_t terni_status;
#define TERNI_OK 0
#define TERNI_ERROR 1            /* anything else; terni_last_error() says what */
#define TERNI_INVALID 2          /* a bad argument: no such entity, row, field, id, ... */
#define TERNI_NOT_LOADED 3       /* no game is loaded */
#define TERNI_NOT_RUNNING 4      /* the world has not started (or has stopped) */
#define TERNI_RUNNING 5          /* the call is for before the start */
#define TERNI_DEVICE_LOST 6      /* the device was lost: the world stopped; start again and load the last save */
#define TERNI_TOO_SMALL 7        /* the buffer is too small; the needed size was written */
#define TERNI_GAME_ERRORS 8      /* the game has errors (terni_diagnostic()); it is not loaded */
#define TERNI_NO_COMPILER 9      /* this library has no rule compiler (terni_runtime) */

#define TERNI_ALL_ROWS 0xFFFFFFFFu /* as `rows`: every row from `first_row` on (see each call) */
#define TERNI_NO_ROW 0xFFFFFFFFu   /* terni_create_entity(): the entity type is full */

/* Field and define types (terni::FieldType). */
#define TERNI_QUANTITY 0 /* Q24.8 fixed point */
#define TERNI_RATE 1     /* Q16.16 fixed point */
#define TERNI_AGG 2      /* a sum through integer atomics, Q24.8 */
#define TERNI_INT 3
#define TERNI_COUNT 4
#define TERNI_REF 5      /* a row of another entity type */
#define TERNI_MONEY64 6  /* 64-bit, two words per value (low word first) */

/* Device preferences (terni::DevicePreference). */
#define TERNI_PREFER_DISCRETE 0   /* dedicated GPU, then integrated, then anything, then the CPU */
#define TERNI_PREFER_INTEGRATED 1 /* integrated first, e.g. to leave a dedicated GPU to the renderer */
#define TERNI_PREFER_CPU 2        /* a software driver (lavapipe) first */

typedef struct terni_world terni_world;

/* ---- the library ---- */

TERNI_API uint32_t terni_abi_version(void);
TERNI_API const char* terni_version(void); /* "0.1.0" */
/* What the last failed call on this thread did wrong; "" if none. Valid until the next
 * call on this thread. */
TERNI_API const char* terni_last_error(void);
/* 1 for `terni` (compiles rules), 0 for `terni_runtime`. */
TERNI_API int32_t terni_can_compile_rules(void);

/* ---- devices ---- */

typedef struct terni_device {
    uint32_t index;
    const char* name;
    const char* type;    /* "discrete", "integrated", "virtual", "cpu (software)", "other" */
    const char* driver;  /* "radv Mesa 25.2.4", ...; "" if the device does not say */
    const char* why_not; /* why it is not usable; "" if it is */
    int32_t usable;
    int32_t cpu;         /* a software driver: the rules run on the CPU */
    uint32_t vendor_id;
    uint8_t uuid[16];    /* VkPhysicalDeviceIDProperties::deviceUUID */
    uint8_t luid[8];     /* the adapter LUID that DirectX also reports; Windows only */
    int32_t luid_valid;
    int32_t robustness;  /* past a field's end: 0 undefined, 1 stays in the buffer, 2 reads 0 and writes nothing */
} terni_device;

/* Lists the Vulkan devices; 0 where there is no Vulkan driver. The list stays valid until
 * the next call. */
TERNI_API terni_status terni_device_count(uint32_t* count);
/* Its strings are valid until the next terni_device_count(). */
TERNI_API terni_status terni_device_info(uint32_t index, terni_device* device);
/* Adds a Vulkan driver shipped with the game (a software driver such as lavapipe:
 * vulkan_lvp.dll, libvulkan_lvp.so) to the system's, for the rest of the process. Call it
 * before listing or picking devices: their indices change. */
TERNI_API terni_status terni_add_vulkan_driver(const char* library_path);
/* A usable device: the one whose UUID or LUID matches (either may be NULL) to share the
 * renderer's GPU, else by `preference`. *device = -1 if no usable device exists. */
TERNI_API terni_status terni_pick_device(int32_t preference, const uint8_t* uuid16, const uint8_t* luid8,
                                         int32_t* device);

/* ---- the renderer's GPU, and a software driver shipped with the game ---- */

/* What a renderer says about its GPU, to run the simulation on the same one. */
typedef struct terni_renderer_gpu {
    int32_t has_uuid;  /* a Vulkan physical device UUID */
    uint8_t uuid[16];
    int32_t has_luid;  /* an adapter LUID (DirectX, or Vulkan on Windows) */
    uint8_t luid[8];
    uint32_t vendor_id; /* PCI vendor ID, and the name: the last resort */
    char name[256];     /* UTF-8 */
} terni_renderer_gpu;

/* From a Vulkan renderer's own objects: its VkInstance, the vkGetInstanceProcAddr it
 * uses (NULL: the Vulkan loader's) and the VkPhysicalDevice it renders with. */
TERNI_API terni_status terni_vulkan_renderer_gpu(void* instance, void* get_instance_proc_addr,
                                                 void* physical_device, terni_renderer_gpu* gpu);
/* From a DirectX renderer: DXGI adapter number `adapter_index`, or its adapter object
 * (an IDXGIAdapter*). Windows only: elsewhere they return TERNI_INVALID. */
TERNI_API terni_status terni_dxgi_renderer_gpu(uint32_t adapter_index, terni_renderer_gpu* gpu);
TERNI_API terni_status terni_dxgi_adapter_gpu(void* adapter, terni_renderer_gpu* gpu);
/* The usable device that is the renderer's GPU: by UUID, then LUID, then by vendor and
 * name if exactly one usable device has them. *device = -1 if none. Pass the index to
 * terni_start_options.device. */
TERNI_API terni_status terni_find_device(const terni_renderer_gpu* gpu, int32_t* device);

#define TERNI_DRIVER_ADDED 0          /* the shipped driver was added to the system's */
#define TERNI_DRIVER_NOT_SHIPPED 1    /* there is none in the folder */
#define TERNI_DRIVER_SYSTEM_HAS_ONE 2 /* the system has a usable software device: not needed */
/* The file name of a software Vulkan driver on this system: "vulkan_lvp.dll" or
 * "libvulkan_lvp.so". */
TERNI_API const char* terni_shipped_driver_name(void);
/* Adds the software Vulkan driver shipped in `folder` (terni_shipped_driver_name()) to
 * the system's, unless the system has a usable software device already. A machine whose
 * GPUs cannot run the simulation then still has a device, and TERNI_PREFER_DISCRETE
 * falls back to it. Works once per process: call it before devices are listed; later
 * calls give the first call's result. *result gets TERNI_DRIVER_ADDED, ... */
TERNI_API terni_status terni_add_shipped_driver(const char* folder, int32_t* result);

/* ---- a world ---- */

TERNI_API terni_status terni_world_create(terni_world** world);
TERNI_API void terni_world_destroy(terni_world* world); /* NULL is fine */

/* Loads the game's folder with mod folders on top, in order: the game's first (the same
 * as terni::load_game()). Stops and forgets whatever the world had. Returns
 * TERNI_GAME_ERRORS if the game has errors; either way terni_diagnostic() lists them and
 * the warnings. */
TERNI_API terni_status terni_load(terni_world* world, const char* const* folders, uint32_t folder_count);
TERNI_API void terni_unload(terni_world* world); /* stop, and forget the game */
TERNI_API int32_t terni_loaded(const terni_world* world);

#define TERNI_ERRORS 0
#define TERNI_WARNINGS 1
/* The errors or warnings of the last terni_load(), as "file:line: message". */
TERNI_API uint32_t terni_diagnostic_count(const terni_world* world, int32_t kind);
TERNI_API const char* terni_diagnostic(const terni_world* world, int32_t kind, uint32_t index); /* NULL past the end */

/* ---- what the game declares (valid while the game is loaded) ---- */

TERNI_API uint32_t terni_entity_count(const terni_world* world); /* entity types */
TERNI_API const char* terni_entity_name(const terni_world* world, uint32_t entity);
TERNI_API int32_t terni_find_entity(const terni_world* world, const char* name);
TERNI_API uint32_t terni_capacity(const terni_world* world, uint32_t entity);
TERNI_API int32_t terni_dynamic(const terni_world* world, uint32_t entity);

typedef struct terni_field_info {
    int32_t type;   /* TERNI_QUANTITY, ... */
    uint32_t size;  /* values per row */
    uint32_t words; /* 32-bit words per value: 2 for money64, else 1 */
    int32_t ref_entity; /* for TERNI_REF: the entity type it refers to; else -1 */
} terni_field_info;

TERNI_API uint32_t terni_field_count(const terni_world* world, uint32_t entity);
TERNI_API const char* terni_field_name(const terni_world* world, uint32_t entity, uint32_t field);
TERNI_API int32_t terni_find_field(const terni_world* world, uint32_t entity, const char* name);
TERNI_API terni_status terni_field_info_of(const terni_world* world, uint32_t entity, const char* field,
                                           terni_field_info* info);
/* The row a history file gave this id; -1 if none. */
TERNI_API int64_t terni_find_row(const terni_world* world, uint32_t entity, const char* id);
/* The id of a row from history; "" if it has none. Valid while the game is loaded. */
TERNI_API const char* terni_row_id(const terni_world* world, uint32_t entity, uint32_t row);

TERNI_API uint32_t terni_define_count(const terni_world* world);
TERNI_API const char* terni_define_name(const terni_world* world, uint32_t define);
TERNI_API int32_t terni_find_define(const terni_world* world, const char* name);
/* The raw word rules read as D.<name> (a rate of 0.02 is 1310). */
TERNI_API terni_status terni_define_value(const terni_world* world, uint32_t define, int32_t* raw);

TERNI_API uint32_t terni_bundle_count(const terni_world* world);
TERNI_API const char* terni_bundle_name(const terni_world* world, uint32_t bundle);
TERNI_API int32_t terni_find_bundle(const terni_world* world, const char* name);
TERNI_API uint32_t terni_modifier_key_count(const terni_world* world);
TERNI_API const char* terni_modifier_key_name(const terni_world* world, uint32_t key);
TERNI_API int32_t terni_find_modifier_key(const terni_world* world, const char* name);

/* terni::game_checksum(): what lockstep peers compare before playing together. */
TERNI_API terni_status terni_checksum(const terni_world* world, uint64_t* checksum);

/* ---- before the start: sizes and a generated starting state ---- */

/* Room for `rows` rows (the map size a match picks). Part of the checksum. */
TERNI_API terni_status terni_set_capacity(terni_world* world, uint32_t entity, uint32_t rows);
/* Room for this many modifier instances at once. Part of the checksum. */
TERNI_API terni_status terni_set_modifier_capacity(terni_world* world, uint32_t instances);
/* Rows in use at the start; of a dynamic entity type, the rows below it are alive. New
 * rows get the fields' defaults. */
TERNI_API terni_status terni_set_start_count(terni_world* world, uint32_t entity, uint32_t count);
TERNI_API uint32_t terni_start_count(const terni_world* world, uint32_t entity);
/* The starting value of rows [row, row + n) of a field, where n is word_count divided by
 * the field's words per row (size x words). */
TERNI_API terni_status terni_write_start(terni_world* world, uint32_t entity, const char* field, uint32_t row,
                                         const int32_t* words, size_t word_count);
/* The starting value of `rows` rows from `first_row` (TERNI_ALL_ROWS: to the capacity)
 * into `words`. TERNI_TOO_SMALL if `capacity` (in words) is short; *word_count gets the
 * words needed either way. */
TERNI_API terni_status terni_read_start(const terni_world* world, uint32_t entity, const char* field,
                                        uint32_t first_row, uint32_t rows, int32_t* words, size_t capacity,
                                        size_t* word_count);
/* A modifier instance present at the start, as `add_modifier` in history does.
 * `scale` 65536 is 1; `duration` in ticks, 0 forever. */
TERNI_API terni_status terni_add_start_modifier(terni_world* world, uint32_t bundle, uint32_t entity,
                                                uint32_t row, int32_t scale, uint32_t duration);

/* Compiles every rule into the SPIR-V cache in `cache_dir` without a device (for a build
 * step, or before a cache-only start). TERNI_NO_COMPILER in terni_runtime. */
typedef struct terni_compile_stats {
    uint32_t compiled; /* stages compiled */
    uint32_t cached;   /* stages taken from the cache */
    double ms;         /* time spent getting SPIR-V */
} terni_compile_stats;
TERNI_API terni_status terni_compile_rules(const terni_world* world, const char* cache_dir,
                                           terni_compile_stats* stats); /* stats may be NULL */

/* ---- starting and stopping ---- */

typedef struct terni_start_options {
    uint32_t struct_size;     /* the bytes that count; set by terni_start_options_init() */
    int32_t device;           /* a device index; -1: choose (renderer IDs, then preference) */
    int32_t preference;       /* TERNI_PREFER_DISCRETE, ... */
    int32_t has_uuid;         /* share the renderer's GPU: its Vulkan physical device UUID */
    uint8_t uuid[16];
    int32_t has_luid;         /* ... or its DirectX adapter LUID (matched after the UUID) */
    uint8_t luid[8];
    const char* cache_dir;    /* the SPIR-V cache; NULL or "" for none */
    int32_t cache_only;       /* never compile: every rule must be in the cache (always so in terni_runtime) */
    int32_t validation;       /* Vulkan validation layers */
    const char* shader_library_dir; /* NULL or "": the GLSL library inside Terni */
} terni_start_options;

/* Fills in the defaults. Call it first, then change what you need. The struct may grow at
 * its end in later versions, so the call carries the size the caller was built with: a
 * newer library never writes past an older caller's struct. Languages that call by name
 * call terni_start_options_init_sized() with the size of their own struct; the function
 * terni_start_options_init() fills the struct as ABI version 1 began, up to
 * `shader_library_dir`, and always will. */
TERNI_API void terni_start_options_init_sized(terni_start_options* options, uint32_t struct_size);
TERNI_API void terni_start_options_init(terni_start_options* options);
#define terni_start_options_init(options) \
    terni_start_options_init_sized((options), (uint32_t)sizeof(terni_start_options))
/* Starts the simulation on a device (stops a running one first). Options may be NULL for
 * the defaults. */
TERNI_API terni_status terni_start(terni_world* world, const terni_start_options* options);
/* How long waits for a tick (terni_tick(), terni_end_tick(), and calls that collect a
 * tick) spin before they sleep, in microseconds; 0, as a new world has it, sleeps at once.
 * Holds from the next terni_start() on. Saves the 10 us or so a sleeping thread takes to
 * wake: worth it only for ticks well under a millisecond. It keeps a CPU core busy
 * meanwhile, and does nothing on software devices. */
TERNI_API terni_status terni_set_tick_spin(terni_world* world, uint32_t microseconds);
TERNI_API void terni_stop(terni_world* world);
TERNI_API int32_t terni_running(const terni_world* world);
/* The device was lost (a rule ran too long and the driver reset the GPU, or the GPU went
 * away): the call that found it returned TERNI_DEVICE_LOST and the world stopped. Start
 * again, on this device or another, and load the last save. */
TERNI_API int32_t terni_device_lost(const terni_world* world);
/* For testing how a game recovers: the next call that uses the device finds it lost. */
TERNI_API void terni_simulate_device_lost(terni_world* world);

/* ---- ticks ---- */

TERNI_API terni_status terni_tick(terni_world* world);
/* A tick that does not block: begin, keep rendering, collect later. Writes and commands
 * made in between are for the next tick, exactly as if made after terni_end_tick(). */
TERNI_API terni_status terni_begin_tick(terni_world* world);
TERNI_API terni_status terni_tick_ready(terni_world* world, int32_t* ready); /* never blocks */
TERNI_API terni_status terni_end_tick(terni_world* world);                   /* blocks until done */
TERNI_API int32_t terni_tick_in_flight(const terni_world* world);
TERNI_API uint64_t terni_ticks(const terni_world* world); /* completed ticks */

typedef struct terni_order_issue {
    const char* stage;
    const char* field;   /* "entity.field" that came out different */
    int32_t parallel;    /* 0: forwards and backwards differ; 1: the parallel run differs */
    uint64_t first_row;
    uint64_t rows;
} terni_order_issue;
/* A tick like terni_tick() that also finds stages whose result depends on the order of
 * their threads (terni::GameSim::tick_checked()). Slow: for tests and parity runs. */
TERNI_API terni_status terni_tick_checked(terni_world* world, uint32_t* issue_count);
TERNI_API terni_status terni_order_issue_at(const terni_world* world, uint32_t index, terni_order_issue* issue);

/* ---- reading and writing state (writes take effect at the next tick) ---- */

/* Words of `rows` rows from `first_row` (TERNI_ALL_ROWS: to the capacity), read back from
 * the device now (it waits). TERNI_TOO_SMALL if `capacity` (in words) is short;
 * *word_count gets the words needed either way. */
TERNI_API terni_status terni_read_field(terni_world* world, uint32_t entity, const char* field, uint32_t first_row,
                                        uint32_t rows, int32_t* words, size_t capacity, size_t* word_count);
/* Whole rows from `row` on: word_count must be a multiple of the words per row. */
TERNI_API terni_status terni_write_field(terni_world* world, uint32_t entity, const char* field, uint32_t row,
                                         const int32_t* words, size_t word_count);
/* Values [index, index + n) of one row of a sized field; the rest of the row is kept. */
TERNI_API terni_status terni_write_values(terni_world* world, uint32_t entity, const char* field, uint32_t row,
                                          uint32_t index, const int32_t* words, size_t word_count);
/* A field copied back at the end of every tick, in the same submission: the rows in use
 * from `first_row` (rows = TERNI_ALL_ROWS), or exactly `rows` rows. */
TERNI_API terni_status terni_subscribe(terni_world* world, uint32_t entity, const char* field, uint32_t first_row,
                                       uint32_t rows, uint32_t* id);
/* A subscription's words as of the last collected tick. Valid until the tick after the
 * next one begins. */
TERNI_API terni_status terni_subscription(const terni_world* world, uint32_t id, const int32_t** words,
                                          size_t* word_count);

/* ---- entities at run time (entity types marked dynamic) ---- */

/* The lowest free row, alive from the next tick; *row = TERNI_NO_ROW if none is free. */
TERNI_API terni_status terni_create_entity(terni_world* world, uint32_t entity, uint32_t* row);
/* *was_alive (may be NULL) is 0 if the row was not alive. */
TERNI_API terni_status terni_destroy_entity(terni_world* world, uint32_t entity, uint32_t row, int32_t* was_alive);
TERNI_API int32_t terni_alive(const terni_world* world, uint32_t entity, uint32_t row);
TERNI_API uint32_t terni_generation(const terni_world* world, uint32_t entity, uint32_t row);
TERNI_API uint32_t terni_rows_in_use(const terni_world* world, uint32_t entity); /* one past the highest row ever alive */
TERNI_API uint32_t terni_rows_alive(const terni_world* world, uint32_t entity);
/* One bit per row (row r is bit r % 32 of word r / 32). Valid until the next call that
 * creates, destroys, ticks or loads. */
TERNI_API terni_status terni_alive_bits(const terni_world* world, uint32_t entity, const uint32_t** bits,
                                        size_t* word_count);
/* How many rows terni_create_entity() would find now: refuse a command before applying it. */
TERNI_API terni_status terni_rows_free(terni_world* world, uint32_t entity, uint32_t* free_rows);

/* ---- modifiers ---- */

/* A modifier instance. A stale handle (removed, expired, slot reused) matches nothing. */
typedef struct terni_modifier {
    uint32_t slot;
    uint32_t version;
} terni_modifier;
#define TERNI_NO_MODIFIER_SLOT 0xFFFFFFFFu

typedef struct terni_contribution {
    terni_modifier modifier;
    uint32_t bundle;
    uint32_t entity; /* where the instance sits: the row asked about, or one it inherits from */
    uint32_t row;
    int32_t value;   /* raw, already scaled */
} terni_contribution;

/* `scale` 65536 is 1; `duration` in ticks, 0 forever. */
TERNI_API terni_status terni_add_modifier(terni_world* world, uint32_t bundle, uint32_t entity, uint32_t row,
                                          int32_t scale, uint32_t duration, terni_modifier* modifier);
/* The same bundle on many rows in one call; `modifiers` (may be NULL) gets one handle per row. */
TERNI_API terni_status terni_add_modifiers(terni_world* world, uint32_t bundle, uint32_t entity, const uint32_t* rows,
                                           size_t row_count, int32_t scale, uint32_t duration,
                                           terni_modifier* modifiers);
/* Removes *modifier if it is still alive and adds the new one in its place: *modifier
 * becomes the new handle. For a modifier a game keeps one handle for (a policy, a focus). */
TERNI_API terni_status terni_replace_modifier(terni_world* world, terni_modifier* modifier, uint32_t bundle,
                                              uint32_t entity, uint32_t row, int32_t scale, uint32_t duration);
/* *found (may be NULL) is 0 if the handle was stale. */
TERNI_API terni_status terni_set_modifier_scale(terni_world* world, terni_modifier modifier, int32_t scale,
                                                int32_t* found);
TERNI_API terni_status terni_remove_modifier(terni_world* world, terni_modifier modifier, int32_t* found);
TERNI_API int32_t terni_modifier_alive(const terni_world* world, terni_modifier modifier);
TERNI_API uint32_t terni_modifier_instances(const terni_world* world);
/* Own and inherited total of `key` on a row. */
TERNI_API terni_status terni_modifier_total(terni_world* world, uint32_t key, uint32_t entity, uint32_t row,
                                            int32_t* total);
/* The instances behind the total, nearest first (for tooltips). */
TERNI_API terni_status terni_modifier_breakdown(terni_world* world, uint32_t key, uint32_t entity, uint32_t row,
                                                uint32_t* count);
TERNI_API terni_status terni_contribution_at(const terni_world* world, uint32_t index, terni_contribution* item);

/* ---- saves ---- */

/* The complete state. *data belongs to the world and is valid until the next
 * terni_save(), or the world loads, starts, stops or is destroyed. */
TERNI_API terni_status terni_save(terni_world* world, const uint8_t** data, size_t* size);
/* by_name 0: the save must come from this very game (same checksum). by_name 1: a save of
 * another version of the game, or other mods, is fitted to this one by name;
 * *fitted_count (may be NULL) gets the number of things fitted, read with terni_fitted(). */
TERNI_API terni_status terni_load_save(terni_world* world, const uint8_t* data, size_t size, int32_t by_name,
                                       uint32_t* fitted_count);
TERNI_API const char* terni_fitted(const terni_world* world, uint32_t index); /* NULL past the end */
/* The checksum of the game a save was made with, without loading it. */
TERNI_API terni_status terni_save_checksum(const uint8_t* data, size_t size, uint64_t* checksum);

/* ---- hashes, for finding a desync ---- */

/* Computed on the device: cheap enough to compare every few ticks. One hash per field
 * ("planet.pop"), or per entity type ("planet"), read with terni_hash_at(). */
TERNI_API terni_status terni_field_hashes(terni_world* world, uint32_t* count);
TERNI_API terni_status terni_entity_hashes(terni_world* world, uint32_t* count);
TERNI_API terni_status terni_hash_at(const terni_world* world, uint32_t index, const char** name, uint64_t* hash);
/* All field hashes combined into one. */
TERNI_API terni_status terni_state_hash(terni_world* world, uint64_t* hash);

/* ---- measurements ---- */

typedef struct terni_stage_time {
    const char* name;  /* the game's stages, then Terni's own (terni_...) */
    int32_t ran;       /* in the last tick */
    double last_ms;    /* GPU time in the last tick */
    uint64_t runs;     /* ticks it ran in, since the start */
    double total_ms;   /* GPU time over those ticks */
} terni_stage_time;
TERNI_API terni_status terni_stage_times(terni_world* world, uint32_t* count);
TERNI_API terni_status terni_stage_time_at(const terni_world* world, uint32_t index, terni_stage_time* time);

typedef struct terni_device_health {
    double usual_ms;  /* a tick when the device was at its best */
    double recent_ms; /* median of the last 32 ticks (0 until there are 32) */
    uint64_t ticks;   /* ticks measured */
    int32_t slow;     /* the device has become slow: warn, or terni_move_to_device() */
} terni_device_health;
TERNI_API terni_status terni_health(const terni_world* world, terni_device_health* health);
TERNI_API terni_status terni_compile_stats_of(const terni_world* world, terni_compile_stats* stats);
TERNI_API double terni_last_tick_ms(const terni_world* world);     /* wall time, transfers included */
TERNI_API double terni_last_tick_gpu_ms(const terni_world* world); /* the stages alone */
/* Valid until the next call on this world. "" if not running. */
TERNI_API const char* terni_device_name(terni_world* world);
TERNI_API const char* terni_report(terni_world* world);
/* Continues on another device with the same state, bit for bit. */
TERNI_API terni_status terni_move_to_device(terni_world* world, uint32_t device);

#ifdef __cplusplus
}
#endif

#endif /* TERNI_H */
