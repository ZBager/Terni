// SPDX-License-Identifier: LGPL-2.1-only WITH AdditionRef-Terni-linking-exception
// Copyright (C) 2026 Rafał Zając
// The C# wrapper over the shared library: what a game, a dedicated server or a test tool
// in plain .NET does, with the same state on every device.
using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using Terni;

static class Program
{
    static int failures;

    static void Check(bool ok, string what)
    {
        if (ok) return;
        Console.WriteLine($"FAIL: {what}");
        ++failures;
    }

    static Status Throws(Action action)
    {
        try { action(); }
        catch (TerniException e) { return e.Status; }
        return Status.Ok;
    }

    static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.WriteLine("usage: Terni.Tests <terni library> <Terni source folder>");
            return 2;
        }
        string library = Path.GetFullPath(args[0]);
        NativeLibrary.SetDllImportResolver(typeof(World).Assembly,
            (name, assembly, path) => name == "terni" ? NativeLibrary.Load(library) : IntPtr.Zero);
        string galaxy = Path.Combine(args[1], "examples", "mini_galaxy");
        string armies = Path.Combine(args[1], "tests", "data", "dynamic_game");

        Check(Library.CanCompileRules, "terni compiles rules");
        Check(Library.Version.Length >= 5, "version");

        // a broken game
        using (var w = new World())
        {
            Check(Throws(() => w.Load(Path.Combine(args[1], "tests", "data", "bad_game"))) == Status.GameErrors, "bad game");
            Check(w.Errors.Length > 0 && w.Errors[0].Contains(".txt:"), "errors with file:line");
            Check(Throws(() => w.Tick()) == Status.NotRunning, "tick before start");
        }

        // every usable device: one state hash, and saves alike byte for byte
        Device[] devices = Library.Devices();
        Check(devices.Any(d => d.Usable), "a usable device");
        ulong first = 0;
        byte[]? firstSave = null;
        foreach (Device d in devices.Where(d => d.Usable))
        {
            using var w = new World();
            w.Load(galaxy);
            w.Start(new StartOptions { Device = d.Index, SpinMicroseconds = 100 });
            Check(w.DeviceName == d.Name, "device name");
            for (int t = 0; t < 200; ++t) w.Tick();
            ulong hash = w.StateHash;
            byte[] save = w.Save();
            Console.WriteLine($"  {d.Name,-45} hash {hash:x16}");
            if (firstSave == null) { first = hash; firstSave = save; }
            else Check(hash == first && save.SequenceEqual(firstSave), $"the same state on {d.Name}");
        }
        Check(Library.PickDevice(DevicePreference.Cpu) >= 0, "pick a device");
        Device some = devices.First(d => d.Usable);
        Check(Library.FindDevice(new RendererGpu { Uuid = some.Uuid }) == some.Index, "find a device by UUID");

        // what the game declares, state, modifiers, saves, hashes
        using (var w = new World())
        {
            w.Load(galaxy);
            int planet = w.FindEntity("planet"), empire = w.FindEntity("empire");
            Check(w.EntityTypes.SequenceEqual(new[] { "empire", "planet" }), "entity types");
            Check(w.Fields(planet).Contains("pop") && w.Capacity(planet) == 4096, "fields, capacity");
            Check(w.FieldInfo(empire, "treasury").Words == 2 && w.FieldInfo(planet, "owner").RefEntity == empire, "field info");
            Check(w.FindRow(planet, "carthago") == 2 && w.RowId(planet, 0) == "latium", "row ids");
            Check(w.Define("growth_rate") != 0 && w.Defines.Length == 4, "defines");
            int harvest = w.FindBundle("good_harvest"), popGrowth = w.FindModifierKey("pop_growth");

            // a starting state from code
            w.SetCapacity(planet, 8192);
            w.SetStartCount(planet, 100);
            int[] pops = Enumerable.Range(0, 100).Select(i => (1000 + i) * 256).ToArray();
            w.WriteStart(planet, "pop", 0, pops);
            Check(w.ReadStart(planet, "pop", 0, 100).SequenceEqual(pops), "starting state read back");
            w.AddStartModifier(harvest, planet, 50);

            w.Start();
            Check(Throws(() => w.SetCapacity(planet, 4096)) == Status.Running, "capacity after the start");
            Check(w.ReadField(planet, "pop", 0, 100).SequenceEqual(pops), "the starting state on the device");
            Check(w.ModifierTotal(popGrowth, planet, 50) == 655, "a starting modifier");
            int sub = w.Subscribe(planet, "housing");
            w.WriteField(planet, "housing", 3, new[] { 4242 * 256 });
            w.Tick();
            Check(w.Subscription(sub)[3] == 4242 * 256 && w.Subscription(sub).Length == 100, "subscription");

            Modifier[] many = w.AddModifiers(harvest, planet, new[] { 60, 61, 62 });
            Check(many.All(m => w.IsModifierAlive(m)), "modifiers on many rows");
            Modifier policy = Modifier.None;
            w.ReplaceModifier(ref policy, harvest, planet, 70);
            Modifier old = policy;
            w.ReplaceModifier(ref policy, harvest, planet, 70, 2 * 65536);
            Check(!w.IsModifierAlive(old) && w.ModifierTotal(popGrowth, planet, 70) == 1310, "a replaced modifier");
            Check(w.ModifierBreakdown(popGrowth, planet, 70).Single().Value == 1310, "breakdown");
            Check(w.RemoveModifier(policy) && !w.RemoveModifier(policy), "remove");

            byte[] save = w.Save();
            Check(Library.SaveChecksum(save) == w.Checksum, "save checksum");
            for (int t = 0; t < 20; ++t) w.Tick();
            ulong later = w.StateHash;
            Check(w.LoadSave(save).Length == 0, "load a save");
            for (int t = 0; t < 20; ++t) w.Tick();
            Check(w.StateHash == later, "a save continues exactly");
            Check(w.EntityHashes().Any(h => h.Name == "planet") && w.FieldHashes().Length > 10, "hashes");

            w.BeginTick();
            while (!w.TickReady) { }
            w.EndTick();
            Check(w.TickChecked().Length == 0, "order check");
            Check(w.StageTimes()[0].Name == "planet_growth" && w.Health.Ticks > 0, "measurements");

            w.SimulateDeviceLost();
            Check(Throws(() => w.Tick()) == Status.DeviceLost && w.DeviceLost && !w.Running, "a lost device");
            w.Start();
            w.LoadSave(save);
            w.Tick();
        }

        // entities that come and go
        using (var w = new World())
        {
            w.Load(armies);
            int army = w.FindEntity("army");
            w.Start();
            Check(w.RowsFree(army) == 15, "rows free");
            int row = w.CreateEntity(army);
            Check(row == 1 && w.IsAlive(army, row) && w.AliveBits(army)[0] == 3u, "create");
            Check(w.DestroyEntity(army, row) && !w.DestroyEntity(army, row), "destroy");
        }

        Console.WriteLine($"Terni.Tests: {(failures == 0 ? "PASS" : "FAIL")}");
        return failures == 0 ? 0 : 1;
    }
}
