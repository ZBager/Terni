// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// Terni for C#: a World runs one game on one device. Calls that fail throw a
// TerniException (DeviceLostException when the device was lost) with Terni's message.
// docs/c-api.md describes the calls; the names here follow it.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace Terni
{
    /// <summary>What a failed call returned (terni_status).</summary>
    public enum Status
    {
        Ok = 0,
        Error = 1,
        Invalid = 2,
        NotLoaded = 3,
        NotRunning = 4,
        Running = 5,
        DeviceLost = 6,
        TooSmall = 7,
        GameErrors = 8,
        NoCompiler = 9,
    }

    public class TerniException : Exception
    {
        public Status Status { get; }
        public TerniException(Status status, string message) : base(message) { Status = status; }
    }

    /// <summary>The device was lost: the world stopped. Start it again and load the last save.</summary>
    public class DeviceLostException : TerniException
    {
        public DeviceLostException(string message) : base(Status.DeviceLost, message) { }
    }

    public enum FieldType { Quantity = 0, Rate = 1, Agg = 2, Int = 3, Count = 4, Ref = 5, Money64 = 6 }
    public enum DevicePreference { Discrete = 0, Integrated = 1, Cpu = 2 }
    public enum DriverResult { Added = 0, NotShipped = 1, SystemHasOne = 2 }

    public sealed class Device
    {
        public int Index { get; internal set; }
        public string Name { get; internal set; } = "";
        public string Type { get; internal set; } = "";
        public string Driver { get; internal set; } = "";
        public string WhyNot { get; internal set; } = "";
        public bool Usable { get; internal set; }
        public bool Cpu { get; internal set; }
        public uint VendorId { get; internal set; }
        public byte[] Uuid { get; internal set; } = new byte[16];
        public byte[]? Luid { get; internal set; }  // null where the device has none (not Windows)
        public int Robustness { get; internal set; }
        public override string ToString() => $"{Index}: {Name} ({Type})";
    }

    /// <summary>What a renderer says about its GPU (Terni.FindDevice()).</summary>
    public sealed class RendererGpu
    {
        public byte[]? Uuid { get; set; }
        public byte[]? Luid { get; set; }
        public uint VendorId { get; set; }
        public string Name { get; set; } = "";
    }

    public struct FieldInfo
    {
        public FieldType Type;
        public int Size;      // values per row
        public int Words;     // 32-bit words per value
        public int RefEntity; // for Ref fields; else -1
    }

    public struct Modifier : IEquatable<Modifier>
    {
        public uint Slot;
        public uint Version;
        public static readonly Modifier None = new Modifier { Slot = 0xFFFFFFFFu };
        public bool IsNone => Slot == 0xFFFFFFFFu;
        public bool Equals(Modifier other) => Slot == other.Slot && Version == other.Version;
        public override bool Equals(object? obj) => obj is Modifier m && Equals(m);
        public override int GetHashCode() => (int)(Slot * 31 + Version);
    }

    public struct Contribution
    {
        public Modifier Modifier;
        public int Bundle, Entity, Row;
        public int Value;
    }

    public struct OrderIssue
    {
        public string Stage, Field;
        public bool Parallel;
        public ulong FirstRow, Rows;
    }

    public struct Hash
    {
        public string Name;
        public ulong Value;
    }

    public struct StageTime
    {
        public string Name;
        public bool Ran;
        public double LastMs, TotalMs;
        public ulong Runs;
    }

    public struct DeviceHealth
    {
        public double UsualMs, RecentMs;
        public ulong Ticks;
        public bool Slow;
    }

    public struct CompileStats
    {
        public int Compiled, Cached;
        public double Ms;
    }

    public sealed class StartOptions
    {
        public int Device = -1;
        public DevicePreference Preference = DevicePreference.Discrete;
        public byte[]? RendererUuid;
        public byte[]? RendererLuid;
        public string? CacheDir;
        public bool CacheOnly;
        public bool Validation;
        public string? ShaderLibraryDir;
    }

    internal static class Utf8
    {
        internal static byte[] Of(string s)
        {
            int n = Encoding.UTF8.GetByteCount(s);
            var bytes = new byte[n + 1];
            Encoding.UTF8.GetBytes(s, 0, s.Length, bytes, 0);
            return bytes;
        }
        internal static string FromNative(IntPtr p) => p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";
        internal static IntPtr Alloc(string? s)
        {
            if (s == null) return IntPtr.Zero;
            byte[] bytes = Of(s);
            IntPtr p = Marshal.AllocHGlobal(bytes.Length);
            Marshal.Copy(bytes, 0, p, bytes.Length);
            return p;
        }
    }

    /// <summary>Calls that need no world: devices, drivers, saves.</summary>
    public static unsafe class Library
    {
        static bool checkedAbi;

        internal static void Check(int status, World? world = null)
        {
            if (!checkedAbi)
            {
                uint abi = Native.terni_abi_version();
                if (abi != Native.AbiVersion)
                    throw new TerniException(Status.Error, $"the Terni library has ABI version {abi}; this wrapper is for {Native.AbiVersion}");
                checkedAbi = true;
            }
            if (status == 0) return;
            string message = Utf8.FromNative(Native.terni_last_error());
            if (status == (int)Status.DeviceLost) throw new DeviceLostException(message);
            throw new TerniException((Status)status, message);
        }

        public static string Version => Utf8.FromNative(Native.terni_version());
        public static bool CanCompileRules => Native.terni_can_compile_rules() != 0;

        public static Device[] Devices()
        {
            Check(Native.terni_device_count(out uint count));
            var list = new Device[count];
            for (uint i = 0; i < count; ++i)
            {
                Check(Native.terni_device_info(i, out Native.Device d));
                var device = new Device
                {
                    Index = (int)d.index,
                    Name = Utf8.FromNative(d.name),
                    Type = Utf8.FromNative(d.type),
                    Driver = Utf8.FromNative(d.driver),
                    WhyNot = Utf8.FromNative(d.why_not),
                    Usable = d.usable != 0,
                    Cpu = d.cpu != 0,
                    VendorId = d.vendor_id,
                    Robustness = d.robustness,
                };
                for (int b = 0; b < 16; ++b) device.Uuid[b] = d.uuid[b];
                if (d.luid_valid != 0)
                {
                    device.Luid = new byte[8];
                    for (int b = 0; b < 8; ++b) device.Luid[b] = d.luid[b];
                }
                list[i] = device;
            }
            return list;
        }

        public static void AddVulkanDriver(string libraryPath) => Check(Native.terni_add_vulkan_driver(Utf8.Of(libraryPath)));

        /// <summary>A usable device: the renderer's (by UUID or LUID) if given, else by preference; -1 if none.</summary>
        public static int PickDevice(DevicePreference preference = DevicePreference.Discrete, byte[]? uuid = null, byte[]? luid = null)
        {
            if (uuid != null && uuid.Length != 16) throw new ArgumentException("a UUID is 16 bytes", nameof(uuid));
            if (luid != null && luid.Length != 8) throw new ArgumentException("a LUID is 8 bytes", nameof(luid));
            fixed (byte* u = uuid)
            fixed (byte* l = luid)
            {
                Check(Native.terni_pick_device((int)preference, u, l, out int device));
                return device;
            }
        }

        static RendererGpu FromNative(in Native.RendererGpu g)
        {
            var gpu = new RendererGpu { VendorId = g.vendor_id };
            fixed (byte* name = g.name) gpu.Name = Utf8.FromNative((IntPtr)name);
            if (g.has_uuid != 0)
            {
                gpu.Uuid = new byte[16];
                for (int b = 0; b < 16; ++b) gpu.Uuid[b] = g.uuid[b];
            }
            if (g.has_luid != 0)
            {
                gpu.Luid = new byte[8];
                for (int b = 0; b < 8; ++b) gpu.Luid[b] = g.luid[b];
            }
            return gpu;
        }

        /// <summary>A Vulkan renderer's GPU, from its VkInstance, vkGetInstanceProcAddr (zero: the loader's) and VkPhysicalDevice.</summary>
        public static RendererGpu VulkanRendererGpu(IntPtr instance, IntPtr getInstanceProcAddr, IntPtr physicalDevice)
        {
            Check(Native.terni_vulkan_renderer_gpu(instance, getInstanceProcAddr, physicalDevice, out Native.RendererGpu g));
            return FromNative(g);
        }

        /// <summary>A DirectX renderer's GPU: DXGI adapter number `index` (Windows only).</summary>
        public static RendererGpu DxgiRendererGpu(uint adapterIndex)
        {
            Check(Native.terni_dxgi_renderer_gpu(adapterIndex, out Native.RendererGpu g));
            return FromNative(g);
        }

        /// <summary>A DirectX renderer's GPU from its IDXGIAdapter* (Windows only).</summary>
        public static RendererGpu DxgiAdapterGpu(IntPtr adapter)
        {
            Check(Native.terni_dxgi_adapter_gpu(adapter, out Native.RendererGpu g));
            return FromNative(g);
        }

        /// <summary>The usable device that is the renderer's GPU; -1 if none.</summary>
        public static int FindDevice(RendererGpu gpu)
        {
            var g = new Native.RendererGpu { vendor_id = gpu.VendorId };
            if (gpu.Uuid != null)
            {
                if (gpu.Uuid.Length != 16) throw new ArgumentException("a UUID is 16 bytes");
                g.has_uuid = 1;
                for (int b = 0; b < 16; ++b) g.uuid[b] = gpu.Uuid[b];
            }
            if (gpu.Luid != null)
            {
                if (gpu.Luid.Length != 8) throw new ArgumentException("a LUID is 8 bytes");
                g.has_luid = 1;
                for (int b = 0; b < 8; ++b) g.luid[b] = gpu.Luid[b];
            }
            byte[] name = Utf8.Of(gpu.Name);
            int n = Math.Min(name.Length, 255);
            for (int b = 0; b < n; ++b) g.name[b] = name[b];
            g.name[n] = 0;
            Check(Native.terni_find_device(g, out int device));
            return device;
        }

        public static string ShippedDriverName => Utf8.FromNative(Native.terni_shipped_driver_name());

        /// <summary>Adds the software Vulkan driver shipped in `folder` unless the system has one; once per process.</summary>
        public static DriverResult AddShippedDriver(string folder)
        {
            Check(Native.terni_add_shipped_driver(Utf8.Of(folder), out int result));
            return (DriverResult)result;
        }

        /// <summary>The checksum of the game a save was made with.</summary>
        public static ulong SaveChecksum(byte[] save)
        {
            fixed (byte* p = save)
            {
                Check(Native.terni_save_checksum(p, (UIntPtr)save.Length, out ulong checksum));
                return checksum;
            }
        }
    }

    /// <summary>One game and, once started, its simulation on one device. Use from one thread at a time.</summary>
    public sealed unsafe class World : IDisposable
    {
        IntPtr handle;

        public World()
        {
            Library.Check(Native.terni_world_create(out handle));
        }

        ~World() { Release(); }

        public void Dispose()
        {
            Release();
            GC.SuppressFinalize(this);
        }

        void Release()
        {
            if (handle == IntPtr.Zero) return;
            Native.terni_world_destroy(handle);
            handle = IntPtr.Zero;
        }

        IntPtr H
        {
            get
            {
                if (handle == IntPtr.Zero) throw new ObjectDisposedException(nameof(World));
                return handle;
            }
        }

        void Check(int status) => Library.Check(status, this);

        static string[] List(uint count, Func<uint, IntPtr> item)
        {
            var list = new string[count];
            for (uint i = 0; i < count; ++i) list[i] = Utf8.FromNative(item(i));
            return list;
        }

        // ---- loading ----

        /// <summary>The game's folder with mod folders on top, the game's first. Throws with Status.GameErrors if the game has errors (see Errors).</summary>
        public void Load(params string[] folders)
        {
            var pointers = new IntPtr[folders.Length];
            try
            {
                for (int i = 0; i < folders.Length; ++i) pointers[i] = Utf8.Alloc(folders[i]);
                Check(Native.terni_load(H, pointers, (uint)folders.Length));
            }
            finally
            {
                foreach (IntPtr p in pointers) if (p != IntPtr.Zero) Marshal.FreeHGlobal(p);
            }
        }

        public void Unload() => Native.terni_unload(H);
        public bool Loaded => Native.terni_loaded(H) != 0;
        public string[] Errors => List(Native.terni_diagnostic_count(H, 0), i => Native.terni_diagnostic(H, 0, i));
        public string[] Warnings => List(Native.terni_diagnostic_count(H, 1), i => Native.terni_diagnostic(H, 1, i));

        // ---- what the game declares ----

        public string[] EntityTypes => List(Native.terni_entity_count(H), i => Native.terni_entity_name(H, i));
        public int FindEntity(string name) => Native.terni_find_entity(H, Utf8.Of(name));
        public int Capacity(int entity) => (int)Native.terni_capacity(H, (uint)entity);
        public bool IsDynamic(int entity) => Native.terni_dynamic(H, (uint)entity) != 0;
        public string[] Fields(int entity) => List(Native.terni_field_count(H, (uint)entity), i => Native.terni_field_name(H, (uint)entity, i));
        public int FindField(int entity, string name) => Native.terni_find_field(H, (uint)entity, Utf8.Of(name));

        public FieldInfo FieldInfo(int entity, string field)
        {
            Check(Native.terni_field_info_of(H, (uint)entity, Utf8.Of(field), out Native.FieldInfo f));
            return new FieldInfo { Type = (FieldType)f.type, Size = (int)f.size, Words = (int)f.words, RefEntity = f.ref_entity };
        }

        /// <summary>The row history gave this id; -1 if none.</summary>
        public int FindRow(int entity, string id) => (int)Native.terni_find_row(H, (uint)entity, Utf8.Of(id));
        public string RowId(int entity, int row) => Utf8.FromNative(Native.terni_row_id(H, (uint)entity, (uint)row));
        public string[] Defines => List(Native.terni_define_count(H), i => Native.terni_define_name(H, i));
        public int FindDefine(string name) => Native.terni_find_define(H, Utf8.Of(name));

        /// <summary>A define's raw value, as rules read D.name.</summary>
        public int Define(string name)
        {
            int index = FindDefine(name);
            if (index < 0) throw new TerniException(Status.Invalid, $"the game has no define '{name}'");
            Check(Native.terni_define_value(H, (uint)index, out int raw));
            return raw;
        }

        public string[] Bundles => List(Native.terni_bundle_count(H), i => Native.terni_bundle_name(H, i));
        public int FindBundle(string name) => Native.terni_find_bundle(H, Utf8.Of(name));
        public string[] ModifierKeys => List(Native.terni_modifier_key_count(H), i => Native.terni_modifier_key_name(H, i));
        public int FindModifierKey(string name) => Native.terni_find_modifier_key(H, Utf8.Of(name));

        public ulong Checksum
        {
            get
            {
                Check(Native.terni_checksum(H, out ulong sum));
                return sum;
            }
        }

        // ---- before the start ----

        public void SetCapacity(int entity, int rows) => Check(Native.terni_set_capacity(H, (uint)entity, (uint)rows));
        public void SetModifierCapacity(int instances) => Check(Native.terni_set_modifier_capacity(H, (uint)instances));
        public void SetStartCount(int entity, int count) => Check(Native.terni_set_start_count(H, (uint)entity, (uint)count));
        public int StartCount(int entity) => (int)Native.terni_start_count(H, (uint)entity);

        public void WriteStart(int entity, string field, int row, ReadOnlySpan<int> words)
        {
            fixed (int* p = words) Check(Native.terni_write_start(H, (uint)entity, Utf8.Of(field), (uint)row, p, (UIntPtr)words.Length));
        }

        public int[] ReadStart(int entity, string field, int firstRow = 0, int rows = -1)
        {
            byte[] name = Utf8.Of(field);
            uint n = rows < 0 ? 0xFFFFFFFFu : (uint)rows;
            int status = Native.terni_read_start(H, (uint)entity, name, (uint)firstRow, n, null, UIntPtr.Zero, out UIntPtr needed);
            if (status != (int)Status.TooSmall) Check(status);
            var words = new int[(int)needed];
            fixed (int* p = words) Check(Native.terni_read_start(H, (uint)entity, name, (uint)firstRow, n, p, (UIntPtr)words.Length, out needed));
            return words;
        }

        public void AddStartModifier(int bundle, int entity, int row, int scale = 65536, int duration = 0) =>
            Check(Native.terni_add_start_modifier(H, (uint)bundle, (uint)entity, (uint)row, scale, (uint)duration));

        /// <summary>Compiles every rule into the SPIR-V cache, without a device (not in terni_runtime).</summary>
        public CompileStats CompileRules(string cacheDir)
        {
            Check(Native.terni_compile_rules(H, Utf8.Of(cacheDir), out Native.CompileStats s));
            return new CompileStats { Compiled = (int)s.compiled, Cached = (int)s.cached, Ms = s.ms };
        }

        // ---- starting and stopping ----

        public void Start(StartOptions? options = null)
        {
            options ??= new StartOptions();
            Native.terni_start_options_init(out Native.StartOptions o);
            o.struct_size = (uint)sizeof(Native.StartOptions);
            o.device = options.Device;
            o.preference = (int)options.Preference;
            if (options.RendererUuid != null)
            {
                if (options.RendererUuid.Length != 16) throw new ArgumentException("a UUID is 16 bytes");
                o.has_uuid = 1;
                for (int b = 0; b < 16; ++b) o.uuid[b] = options.RendererUuid[b];
            }
            if (options.RendererLuid != null)
            {
                if (options.RendererLuid.Length != 8) throw new ArgumentException("a LUID is 8 bytes");
                o.has_luid = 1;
                for (int b = 0; b < 8; ++b) o.luid[b] = options.RendererLuid[b];
            }
            o.cache_only = options.CacheOnly ? 1 : 0;
            o.validation = options.Validation ? 1 : 0;
            try
            {
                o.cache_dir = Utf8.Alloc(options.CacheDir);
                o.shader_library_dir = Utf8.Alloc(options.ShaderLibraryDir);
                Check(Native.terni_start(H, o));
            }
            finally
            {
                if (o.cache_dir != IntPtr.Zero) Marshal.FreeHGlobal(o.cache_dir);
                if (o.shader_library_dir != IntPtr.Zero) Marshal.FreeHGlobal(o.shader_library_dir);
            }
        }

        public void Stop() => Native.terni_stop(H);
        public bool Running => Native.terni_running(H) != 0;
        public bool DeviceLost => Native.terni_device_lost(H) != 0;
        /// <summary>For testing recovery: the next call that uses the device finds it lost.</summary>
        public void SimulateDeviceLost() => Native.terni_simulate_device_lost(H);

        // ---- ticks ----

        public void Tick() => Check(Native.terni_tick(H));
        public void BeginTick() => Check(Native.terni_begin_tick(H));

        public bool TickReady
        {
            get
            {
                Check(Native.terni_tick_ready(H, out int ready));
                return ready != 0;
            }
        }

        public void EndTick() => Check(Native.terni_end_tick(H));
        public bool TickInFlight => Native.terni_tick_in_flight(H) != 0;
        public ulong Ticks => Native.terni_ticks(H);

        /// <summary>A tick that also finds stages whose result depends on thread order. Slow: for tests.</summary>
        public OrderIssue[] TickChecked()
        {
            Check(Native.terni_tick_checked(H, out uint count));
            var issues = new OrderIssue[count];
            for (uint i = 0; i < count; ++i)
            {
                Check(Native.terni_order_issue_at(H, i, out Native.OrderIssue o));
                issues[i] = new OrderIssue
                {
                    Stage = Utf8.FromNative(o.stage),
                    Field = Utf8.FromNative(o.field),
                    Parallel = o.parallel != 0,
                    FirstRow = o.first_row,
                    Rows = o.rows,
                };
            }
            return issues;
        }

        // ---- state ----

        /// <summary>Raw words of `rows` rows from `firstRow` (-1: to the capacity), read back now (it waits for the GPU).</summary>
        public int[] ReadField(int entity, string field, int firstRow = 0, int rows = -1)
        {
            byte[] name = Utf8.Of(field);
            uint n = rows < 0 ? 0xFFFFFFFFu : (uint)rows;
            int status = Native.terni_read_field(H, (uint)entity, name, (uint)firstRow, n, null, UIntPtr.Zero, out UIntPtr needed);
            if (status != (int)Status.TooSmall) Check(status);
            var words = new int[(int)needed];
            fixed (int* p = words) Check(Native.terni_read_field(H, (uint)entity, name, (uint)firstRow, n, p, (UIntPtr)words.Length, out needed));
            return words;
        }

        /// <summary>Into `words`; returns the words written.</summary>
        public int ReadField(int entity, string field, int firstRow, int rows, Span<int> words)
        {
            fixed (int* p = words)
            {
                Check(Native.terni_read_field(H, (uint)entity, Utf8.Of(field), (uint)firstRow, rows < 0 ? 0xFFFFFFFFu : (uint)rows,
                                              p, (UIntPtr)words.Length, out UIntPtr written));
                return (int)written;
            }
        }

        /// <summary>Whole rows from `row` on; applied at the next tick.</summary>
        public void WriteField(int entity, string field, int row, ReadOnlySpan<int> words)
        {
            fixed (int* p = words) Check(Native.terni_write_field(H, (uint)entity, Utf8.Of(field), (uint)row, p, (UIntPtr)words.Length));
        }

        /// <summary>Values [index, index + n) of one row of a sized field; the rest of the row is kept.</summary>
        public void WriteValues(int entity, string field, int row, int index, ReadOnlySpan<int> words)
        {
            fixed (int* p = words)
                Check(Native.terni_write_values(H, (uint)entity, Utf8.Of(field), (uint)row, (uint)index, p, (UIntPtr)words.Length));
        }

        /// <summary>A field copied back at the end of every tick: the rows in use from `firstRow` (rows -1), or exactly `rows` rows.</summary>
        public int Subscribe(int entity, string field, int firstRow = 0, int rows = -1)
        {
            Check(Native.terni_subscribe(H, (uint)entity, Utf8.Of(field), (uint)firstRow, rows < 0 ? 0xFFFFFFFFu : (uint)rows, out uint id));
            return (int)id;
        }

        /// <summary>A subscription's words as of the last collected tick; valid until the tick after the next one begins.</summary>
        public ReadOnlySpan<int> Subscription(int id)
        {
            Check(Native.terni_subscription(H, (uint)id, out int* words, out UIntPtr count));
            return new ReadOnlySpan<int>(words, (int)count);
        }

        // ---- entities at run time ----

        /// <summary>A new row of a dynamic entity type, alive from the next tick; -1 if the type is full.</summary>
        public int CreateEntity(int entity)
        {
            Check(Native.terni_create_entity(H, (uint)entity, out uint row));
            return row == 0xFFFFFFFFu ? -1 : (int)row;
        }

        /// <summary>False if the row was not alive.</summary>
        public bool DestroyEntity(int entity, int row)
        {
            Check(Native.terni_destroy_entity(H, (uint)entity, (uint)row, out int wasAlive));
            return wasAlive != 0;
        }

        public bool IsAlive(int entity, int row) => Native.terni_alive(H, (uint)entity, (uint)row) != 0;
        public int Generation(int entity, int row) => (int)Native.terni_generation(H, (uint)entity, (uint)row);
        public int RowsInUse(int entity) => (int)Native.terni_rows_in_use(H, (uint)entity);
        public int RowsAlive(int entity) => (int)Native.terni_rows_alive(H, (uint)entity);

        /// <summary>One bit per row (row r is bit r % 32 of word r / 32); valid until the next create, destroy, tick or load.</summary>
        public ReadOnlySpan<uint> AliveBits(int entity)
        {
            Check(Native.terni_alive_bits(H, (uint)entity, out uint* bits, out UIntPtr count));
            return new ReadOnlySpan<uint>(bits, (int)count);
        }

        public int RowsFree(int entity)
        {
            Check(Native.terni_rows_free(H, (uint)entity, out uint free));
            return (int)free;
        }

        // ---- modifiers ----

        static Native.Modifier ToNative(Modifier m) => new Native.Modifier { slot = m.Slot, version = m.Version };
        static Modifier FromNative(Native.Modifier m) => new Modifier { Slot = m.slot, Version = m.version };

        /// <summary>`scale` 65536 is 1; `duration` in ticks, 0 forever.</summary>
        public Modifier AddModifier(int bundle, int entity, int row, int scale = 65536, int duration = 0)
        {
            Check(Native.terni_add_modifier(H, (uint)bundle, (uint)entity, (uint)row, scale, (uint)duration, out Native.Modifier m));
            return FromNative(m);
        }

        /// <summary>The same bundle on many rows in one call: all or nothing.</summary>
        public Modifier[] AddModifiers(int bundle, int entity, ReadOnlySpan<int> rows, int scale = 65536, int duration = 0)
        {
            var native = new Native.Modifier[rows.Length];
            fixed (int* r = rows)
            fixed (Native.Modifier* m = native)
                Check(Native.terni_add_modifiers(H, (uint)bundle, (uint)entity, (uint*)r, (UIntPtr)rows.Length, scale, (uint)duration, m));
            var result = new Modifier[rows.Length];
            for (int i = 0; i < result.Length; ++i) result[i] = FromNative(native[i]);
            return result;
        }

        /// <summary>Removes `modifier` if alive and adds the new one in its place; `modifier` becomes the new handle.</summary>
        public void ReplaceModifier(ref Modifier modifier, int bundle, int entity, int row, int scale = 65536, int duration = 0)
        {
            Native.Modifier m = ToNative(modifier);
            Check(Native.terni_replace_modifier(H, ref m, (uint)bundle, (uint)entity, (uint)row, scale, (uint)duration));
            modifier = FromNative(m);
        }

        /// <summary>False if the handle was stale.</summary>
        public bool SetModifierScale(Modifier modifier, int scale)
        {
            Check(Native.terni_set_modifier_scale(H, ToNative(modifier), scale, out int found));
            return found != 0;
        }

        /// <summary>False if the handle was stale.</summary>
        public bool RemoveModifier(Modifier modifier)
        {
            Check(Native.terni_remove_modifier(H, ToNative(modifier), out int found));
            return found != 0;
        }

        public bool IsModifierAlive(Modifier modifier) => Native.terni_modifier_alive(H, ToNative(modifier)) != 0;
        public int ModifierInstances => (int)Native.terni_modifier_instances(H);

        public int ModifierTotal(int key, int entity, int row)
        {
            Check(Native.terni_modifier_total(H, (uint)key, (uint)entity, (uint)row, out int total));
            return total;
        }

        /// <summary>The instances behind a total, nearest first.</summary>
        public Contribution[] ModifierBreakdown(int key, int entity, int row)
        {
            Check(Native.terni_modifier_breakdown(H, (uint)key, (uint)entity, (uint)row, out uint count));
            var list = new Contribution[count];
            for (uint i = 0; i < count; ++i)
            {
                Check(Native.terni_contribution_at(H, i, out Native.Contribution c));
                list[i] = new Contribution
                {
                    Modifier = FromNative(c.modifier),
                    Bundle = (int)c.bundle,
                    Entity = (int)c.entity,
                    Row = (int)c.row,
                    Value = c.value,
                };
            }
            return list;
        }

        // ---- saves ----

        public byte[] Save()
        {
            Check(Native.terni_save(H, out byte* data, out UIntPtr size));
            var bytes = new byte[(int)size];
            Marshal.Copy((IntPtr)data, bytes, 0, bytes.Length);
            return bytes;
        }

        /// <summary>byName: fit a save of another version of the game by name. Returns one line per thing fitted.</summary>
        public string[] LoadSave(byte[] save, bool byName = false)
        {
            uint count;
            fixed (byte* p = save) Check(Native.terni_load_save(H, p, (UIntPtr)save.Length, byName ? 1 : 0, out count));
            return List(count, i => Native.terni_fitted(H, i));
        }

        // ---- hashes ----

        Hash[] Hashes(uint count)
        {
            var list = new Hash[count];
            for (uint i = 0; i < count; ++i)
            {
                Check(Native.terni_hash_at(H, i, out IntPtr name, out ulong value));
                list[i] = new Hash { Name = Utf8.FromNative(name), Value = value };
            }
            return list;
        }

        /// <summary>One hash per field, computed on the device.</summary>
        public Hash[] FieldHashes()
        {
            Check(Native.terni_field_hashes(H, out uint count));
            return Hashes(count);
        }

        /// <summary>One hash per entity type, computed on the device.</summary>
        public Hash[] EntityHashes()
        {
            Check(Native.terni_entity_hashes(H, out uint count));
            return Hashes(count);
        }

        public ulong StateHash
        {
            get
            {
                Check(Native.terni_state_hash(H, out ulong hash));
                return hash;
            }
        }

        // ---- measurements ----

        public StageTime[] StageTimes()
        {
            Check(Native.terni_stage_times(H, out uint count));
            var list = new StageTime[count];
            for (uint i = 0; i < count; ++i)
            {
                Check(Native.terni_stage_time_at(H, i, out Native.StageTime t));
                list[i] = new StageTime { Name = Utf8.FromNative(t.name), Ran = t.ran != 0, LastMs = t.last_ms, Runs = t.runs, TotalMs = t.total_ms };
            }
            return list;
        }

        public DeviceHealth Health
        {
            get
            {
                Check(Native.terni_health(H, out Native.DeviceHealth h));
                return new DeviceHealth { UsualMs = h.usual_ms, RecentMs = h.recent_ms, Ticks = h.ticks, Slow = h.slow != 0 };
            }
        }

        public CompileStats CompileStats
        {
            get
            {
                Check(Native.terni_compile_stats_of(H, out Native.CompileStats s));
                return new CompileStats { Compiled = (int)s.compiled, Cached = (int)s.cached, Ms = s.ms };
            }
        }

        public double LastTickMs => Native.terni_last_tick_ms(H);
        public double LastTickGpuMs => Native.terni_last_tick_gpu_ms(H);
        public string DeviceName => Utf8.FromNative(Native.terni_device_name(H));
        public string Report => Utf8.FromNative(Native.terni_report(H));

        /// <summary>Continues on another device with the same state, bit for bit.</summary>
        public void MoveToDevice(int device) => Check(Native.terni_move_to_device(H, (uint)device));
    }
}
