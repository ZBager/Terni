// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// The functions and structs of include/terni/terni.h, one to one. Strings go in as
// NUL-terminated UTF-8 byte arrays and come back as pointers (Utf8.FromNative), so
// nothing here depends on how a runtime marshals strings.
using System;
using System.Runtime.InteropServices;

namespace Terni
{
    internal static unsafe class Native
    {
#if TERNI_RUNTIME
        internal const string Library = "terni_runtime";
#else
        internal const string Library = "terni";
#endif
        internal const int AbiVersion = 1;  // TERNI_ABI_VERSION of the header this file follows

        [StructLayout(LayoutKind.Sequential)]
        internal struct Device
        {
            public uint index;
            public IntPtr name, type, driver, why_not;
            public int usable, cpu;
            public uint vendor_id;
            public fixed byte uuid[16];
            public fixed byte luid[8];
            public int luid_valid, robustness;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct RendererGpu
        {
            public int has_uuid;
            public fixed byte uuid[16];
            public int has_luid;
            public fixed byte luid[8];
            public uint vendor_id;
            public fixed byte name[256];
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct FieldInfo
        {
            public int type;
            public uint size, words;
            public int ref_entity;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct CompileStats
        {
            public uint compiled, cached;
            public double ms;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct StartOptions
        {
            public uint struct_size;
            public int device, preference, has_uuid;
            public fixed byte uuid[16];
            public int has_luid;
            public fixed byte luid[8];
            public IntPtr cache_dir;
            public int cache_only, validation;
            public IntPtr shader_library_dir;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct OrderIssue
        {
            public IntPtr stage, field;
            public int parallel;
            public ulong first_row, rows;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct Modifier
        {
            public uint slot, version;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct Contribution
        {
            public Modifier modifier;
            public uint bundle, entity, row;
            public int value;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct StageTime
        {
            public IntPtr name;
            public int ran;
            public double last_ms;
            public ulong runs;
            public double total_ms;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct DeviceHealth
        {
            public double usual_ms, recent_ms;
            public ulong ticks;
            public int slow;
        }

        // ---- the library ----
        [DllImport(Library)] internal static extern uint terni_abi_version();
        [DllImport(Library)] internal static extern IntPtr terni_version();
        [DllImport(Library)] internal static extern IntPtr terni_last_error();
        [DllImport(Library)] internal static extern int terni_can_compile_rules();

        // ---- devices ----
        [DllImport(Library)] internal static extern int terni_device_count(out uint count);
        [DllImport(Library)] internal static extern int terni_device_info(uint index, out Device device);
        [DllImport(Library)] internal static extern int terni_add_vulkan_driver(byte[] library_path);
        [DllImport(Library)] internal static extern int terni_pick_device(int preference, byte* uuid16, byte* luid8, out int device);

        // ---- the renderer's GPU, a shipped driver ----
        [DllImport(Library)] internal static extern int terni_vulkan_renderer_gpu(IntPtr instance, IntPtr get_instance_proc_addr, IntPtr physical_device, out RendererGpu gpu);
        [DllImport(Library)] internal static extern int terni_dxgi_renderer_gpu(uint adapter_index, out RendererGpu gpu);
        [DllImport(Library)] internal static extern int terni_dxgi_adapter_gpu(IntPtr adapter, out RendererGpu gpu);
        [DllImport(Library)] internal static extern int terni_find_device(in RendererGpu gpu, out int device);
        [DllImport(Library)] internal static extern IntPtr terni_shipped_driver_name();
        [DllImport(Library)] internal static extern int terni_add_shipped_driver(byte[] folder, out int result);

        // ---- a world ----
        [DllImport(Library)] internal static extern int terni_world_create(out IntPtr world);
        [DllImport(Library)] internal static extern void terni_world_destroy(IntPtr world);
        [DllImport(Library)] internal static extern int terni_load(IntPtr world, IntPtr[] folders, uint folder_count);
        [DllImport(Library)] internal static extern void terni_unload(IntPtr world);
        [DllImport(Library)] internal static extern int terni_loaded(IntPtr world);
        [DllImport(Library)] internal static extern uint terni_diagnostic_count(IntPtr world, int kind);
        [DllImport(Library)] internal static extern IntPtr terni_diagnostic(IntPtr world, int kind, uint index);

        // ---- what the game declares ----
        [DllImport(Library)] internal static extern uint terni_entity_count(IntPtr world);
        [DllImport(Library)] internal static extern IntPtr terni_entity_name(IntPtr world, uint entity);
        [DllImport(Library)] internal static extern int terni_find_entity(IntPtr world, byte[] name);
        [DllImport(Library)] internal static extern uint terni_capacity(IntPtr world, uint entity);
        [DllImport(Library)] internal static extern int terni_dynamic(IntPtr world, uint entity);
        [DllImport(Library)] internal static extern uint terni_field_count(IntPtr world, uint entity);
        [DllImport(Library)] internal static extern IntPtr terni_field_name(IntPtr world, uint entity, uint field);
        [DllImport(Library)] internal static extern int terni_find_field(IntPtr world, uint entity, byte[] name);
        [DllImport(Library)] internal static extern int terni_field_info_of(IntPtr world, uint entity, byte[] field, out FieldInfo info);
        [DllImport(Library)] internal static extern long terni_find_row(IntPtr world, uint entity, byte[] id);
        [DllImport(Library)] internal static extern IntPtr terni_row_id(IntPtr world, uint entity, uint row);
        [DllImport(Library)] internal static extern uint terni_define_count(IntPtr world);
        [DllImport(Library)] internal static extern IntPtr terni_define_name(IntPtr world, uint define);
        [DllImport(Library)] internal static extern int terni_find_define(IntPtr world, byte[] name);
        [DllImport(Library)] internal static extern int terni_define_value(IntPtr world, uint define, out int raw);
        [DllImport(Library)] internal static extern uint terni_bundle_count(IntPtr world);
        [DllImport(Library)] internal static extern IntPtr terni_bundle_name(IntPtr world, uint bundle);
        [DllImport(Library)] internal static extern int terni_find_bundle(IntPtr world, byte[] name);
        [DllImport(Library)] internal static extern uint terni_modifier_key_count(IntPtr world);
        [DllImport(Library)] internal static extern IntPtr terni_modifier_key_name(IntPtr world, uint key);
        [DllImport(Library)] internal static extern int terni_find_modifier_key(IntPtr world, byte[] name);
        [DllImport(Library)] internal static extern int terni_checksum(IntPtr world, out ulong checksum);

        // ---- before the start ----
        [DllImport(Library)] internal static extern int terni_set_capacity(IntPtr world, uint entity, uint rows);
        [DllImport(Library)] internal static extern int terni_set_modifier_capacity(IntPtr world, uint instances);
        [DllImport(Library)] internal static extern int terni_set_start_count(IntPtr world, uint entity, uint count);
        [DllImport(Library)] internal static extern uint terni_start_count(IntPtr world, uint entity);
        [DllImport(Library)] internal static extern int terni_write_start(IntPtr world, uint entity, byte[] field, uint row, int* words, UIntPtr word_count);
        [DllImport(Library)] internal static extern int terni_read_start(IntPtr world, uint entity, byte[] field, uint first_row, uint rows, int* words, UIntPtr capacity, out UIntPtr word_count);
        [DllImport(Library)] internal static extern int terni_add_start_modifier(IntPtr world, uint bundle, uint entity, uint row, int scale, uint duration);
        [DllImport(Library)] internal static extern int terni_compile_rules(IntPtr world, byte[] cache_dir, out CompileStats stats);

        // ---- starting and stopping ----
        [DllImport(Library)] internal static extern void terni_start_options_init(out StartOptions options);
        [DllImport(Library)] internal static extern int terni_start(IntPtr world, in StartOptions options);
        [DllImport(Library)] internal static extern void terni_stop(IntPtr world);
        [DllImport(Library)] internal static extern int terni_running(IntPtr world);
        [DllImport(Library)] internal static extern int terni_device_lost(IntPtr world);
        [DllImport(Library)] internal static extern void terni_simulate_device_lost(IntPtr world);

        // ---- ticks ----
        [DllImport(Library)] internal static extern int terni_tick(IntPtr world);
        [DllImport(Library)] internal static extern int terni_begin_tick(IntPtr world);
        [DllImport(Library)] internal static extern int terni_tick_ready(IntPtr world, out int ready);
        [DllImport(Library)] internal static extern int terni_end_tick(IntPtr world);
        [DllImport(Library)] internal static extern int terni_tick_in_flight(IntPtr world);
        [DllImport(Library)] internal static extern ulong terni_ticks(IntPtr world);
        [DllImport(Library)] internal static extern int terni_tick_checked(IntPtr world, out uint issue_count);
        [DllImport(Library)] internal static extern int terni_order_issue_at(IntPtr world, uint index, out OrderIssue issue);

        // ---- state ----
        [DllImport(Library)] internal static extern int terni_read_field(IntPtr world, uint entity, byte[] field, uint first_row, uint rows, int* words, UIntPtr capacity, out UIntPtr word_count);
        [DllImport(Library)] internal static extern int terni_write_field(IntPtr world, uint entity, byte[] field, uint row, int* words, UIntPtr word_count);
        [DllImport(Library)] internal static extern int terni_write_values(IntPtr world, uint entity, byte[] field, uint row, uint index, int* words, UIntPtr word_count);
        [DllImport(Library)] internal static extern int terni_subscribe(IntPtr world, uint entity, byte[] field, uint first_row, uint rows, out uint id);
        [DllImport(Library)] internal static extern int terni_subscription(IntPtr world, uint id, out int* words, out UIntPtr word_count);

        // ---- entities at run time ----
        [DllImport(Library)] internal static extern int terni_create_entity(IntPtr world, uint entity, out uint row);
        [DllImport(Library)] internal static extern int terni_destroy_entity(IntPtr world, uint entity, uint row, out int was_alive);
        [DllImport(Library)] internal static extern int terni_alive(IntPtr world, uint entity, uint row);
        [DllImport(Library)] internal static extern uint terni_generation(IntPtr world, uint entity, uint row);
        [DllImport(Library)] internal static extern uint terni_rows_in_use(IntPtr world, uint entity);
        [DllImport(Library)] internal static extern uint terni_rows_alive(IntPtr world, uint entity);
        [DllImport(Library)] internal static extern int terni_alive_bits(IntPtr world, uint entity, out uint* bits, out UIntPtr word_count);
        [DllImport(Library)] internal static extern int terni_rows_free(IntPtr world, uint entity, out uint free_rows);

        // ---- modifiers ----
        [DllImport(Library)] internal static extern int terni_add_modifier(IntPtr world, uint bundle, uint entity, uint row, int scale, uint duration, out Modifier modifier);
        [DllImport(Library)] internal static extern int terni_add_modifiers(IntPtr world, uint bundle, uint entity, uint* rows, UIntPtr row_count, int scale, uint duration, Modifier* modifiers);
        [DllImport(Library)] internal static extern int terni_replace_modifier(IntPtr world, ref Modifier modifier, uint bundle, uint entity, uint row, int scale, uint duration);
        [DllImport(Library)] internal static extern int terni_set_modifier_scale(IntPtr world, Modifier modifier, int scale, out int found);
        [DllImport(Library)] internal static extern int terni_remove_modifier(IntPtr world, Modifier modifier, out int found);
        [DllImport(Library)] internal static extern int terni_modifier_alive(IntPtr world, Modifier modifier);
        [DllImport(Library)] internal static extern uint terni_modifier_instances(IntPtr world);
        [DllImport(Library)] internal static extern int terni_modifier_total(IntPtr world, uint key, uint entity, uint row, out int total);
        [DllImport(Library)] internal static extern int terni_modifier_breakdown(IntPtr world, uint key, uint entity, uint row, out uint count);
        [DllImport(Library)] internal static extern int terni_contribution_at(IntPtr world, uint index, out Contribution item);

        // ---- saves ----
        [DllImport(Library)] internal static extern int terni_save(IntPtr world, out byte* data, out UIntPtr size);
        [DllImport(Library)] internal static extern int terni_load_save(IntPtr world, byte* data, UIntPtr size, int by_name, out uint fitted_count);
        [DllImport(Library)] internal static extern IntPtr terni_fitted(IntPtr world, uint index);
        [DllImport(Library)] internal static extern int terni_save_checksum(byte* data, UIntPtr size, out ulong checksum);

        // ---- hashes ----
        [DllImport(Library)] internal static extern int terni_field_hashes(IntPtr world, out uint count);
        [DllImport(Library)] internal static extern int terni_entity_hashes(IntPtr world, out uint count);
        [DllImport(Library)] internal static extern int terni_hash_at(IntPtr world, uint index, out IntPtr name, out ulong hash);
        [DllImport(Library)] internal static extern int terni_state_hash(IntPtr world, out ulong hash);

        // ---- measurements ----
        [DllImport(Library)] internal static extern int terni_stage_times(IntPtr world, out uint count);
        [DllImport(Library)] internal static extern int terni_stage_time_at(IntPtr world, uint index, out StageTime time);
        [DllImport(Library)] internal static extern int terni_health(IntPtr world, out DeviceHealth health);
        [DllImport(Library)] internal static extern int terni_compile_stats_of(IntPtr world, out CompileStats stats);
        [DllImport(Library)] internal static extern double terni_last_tick_ms(IntPtr world);
        [DllImport(Library)] internal static extern double terni_last_tick_gpu_ms(IntPtr world);
        [DllImport(Library)] internal static extern IntPtr terni_device_name(IntPtr world);
        [DllImport(Library)] internal static extern IntPtr terni_report(IntPtr world);
        [DllImport(Library)] internal static extern int terni_move_to_device(IntPtr world, uint device);
    }
}
