// Tests of the C# API against the simulated device. Exit code is the failure count.
using System;
using System.Threading;
using System.Threading.Tasks;
using IcdLink;
using IcdLink.Generated;

internal static class Program
{
    private static int _failures, _checks;

    private static void Check(bool ok, string what)
    {
        Interlocked.Increment(ref _checks);
        if (ok) return;
        Interlocked.Increment(ref _failures);
        Console.WriteLine("  FAIL " + what);
    }

    private static void Throws<T>(Action a, string what) where T : Exception
    {
        try { a(); Check(false, what + " did not throw"); }
        catch (T) { Check(true, what); }
    }

    private static IcdDevice OpenFast()
    {
        var d = new IcdDevice();
        foreach (IcdChannel ch in new[] { IcdChannel.C, IcdChannel.D })
        {
            var c = d.DefaultConfig(ch);
            c.Baud = 921600; c.PeriodUs = 2000; c.RxTimeoutUs = 1000;
            d.OpenSim(ch, c);
        }
        return d;
    }

    private static void Accessors()
    {
        Console.WriteLine("typed accessors");
        using (var d = OpenFast())
        {
            d.ToC.FakeIcd.Power = true;
            d.ToC.FakeIcd.Test4 = 10;
            d.ToC.FakeIcd.Test1 = 3;
            d.ToC.FakeIcd.Test2 = 5;
            d.ToC.FakeIcd.Test3 = 31;
            var img = d.ReadImage(d.ToC.FakeIcd.Info.Index, IcdSide.To);
            Check(BitConverter.ToString(img) == "90-15-03-50-1F", "image bytes " + BitConverter.ToString(img));
            var crc = d.Schema.Crc.CrcBytes(img, img.Length);
            Check(BitConverter.ToString(crc) == "58-99-16-45", "crc bytes " + BitConverter.ToString(crc));

            Throws<ArgumentOutOfRangeException>(() => d.ToC.FakeIcd.Test1 = 16, "4-bit field rejects 16");
            Throws<ArgumentOutOfRangeException>(() => d.ToC.FakeIcd.Test3 = 32, "5-bit field rejects 32");

            d.ToD.Telemetry.Enable = true;
            d.ToD.Telemetry.Mode = D_Telemetry_Mode.Fault;
            d.ToD.Telemetry.TempC = -12.3;
            d.ToD.Telemetry.Gain = 2.5f;
            d.ToD.Telemetry.CommandOnly = 200;
            d.ToD.Telemetry.Send();
            Check(d.FromD.Telemetry.IsValid, "response is valid");
            Check(d.FromD.Telemetry.Enable, "bool came back");
            Check(d.FromD.Telemetry.Mode == D_Telemetry_Mode.Fault, "enum came back");
            Check(Math.Abs(d.FromD.Telemetry.TempC + 12.3) < 0.051, "scaled int came back");
            Check(d.FromD.Telemetry.Gain == 2.5f, "float came back");
            Check(d.Schema.Messages[d.FromD.Telemetry.Info.Index].RxLen == 8, "From is one byte shorter");
        }
    }

    private static void BlockIsAllOrNothing()
    {
        Console.WriteLine("a block lands as one update (2 s stress)");
        using (var d = OpenFast())
        {
            d.StartCyclic(IcdChannel.C);
            int stop = 0, writes = 0, reads = 0, torn = 0;
            var tasks = new Task[4];
            for (int w = 0; w < 3; w++)
            {
                byte seed = (byte)w;
                tasks[w] = Task.Run(() =>
                {
                    byte v = seed;
                    while (Volatile.Read(ref stop) == 0)
                    {
                        v = (byte)((v + 1) & 0xF);
                        byte value = v;
                        d.Atomic(() =>
                        {
                            d.ToC.FakeIcd.Test1 = value;
                            d.ToC.FakeIcd.Test2 = value;
                            d.ToC.FakeIcd.Test4 = value;
                        });
                        Interlocked.Increment(ref writes);
                    }
                });
            }
            tasks[3] = Task.Run(() =>
            {
                while (Volatile.Read(ref stop) == 0)
                {
                    d.Atomic(() =>
                    {
                        byte a = d.FromC.FakeIcd.Test1, b = d.FromC.FakeIcd.Test2, c = d.FromC.FakeIcd.Test4;
                        if (a != b || a != c) Interlocked.Increment(ref torn);
                    });
                    Interlocked.Increment(ref reads);
                }
            });
            Thread.Sleep(2000);
            Volatile.Write(ref stop, 1);
            Task.WaitAll(tasks);
            Console.WriteLine("  " + writes + " blocks written, " + reads + " blocks read");
            Check(writes > 100 && reads > 100, "stress ran");
            Check(torn == 0, "torn readings: " + torn);
        }
    }

    private static void SetCheckChange()
    {
        Console.WriteLine("set, check, change is not interleaved by another thread (2 s stress)");
        using (var d = OpenFast())
        {
            d.StartCyclic(IcdChannel.C);
            int stop = 0, wrong = 0, cycles = 0, intruderWrites = 0;
            var intruder = Task.Run(() =>
            {
                while (Volatile.Read(ref stop) == 0)
                {
                    d.ToC.FakeIcd.Test3 = 31;
                    Interlocked.Increment(ref intruderWrites);
                }
            });
            byte k = 0;
            var end = DateTime.UtcNow.AddSeconds(2);
            while (DateTime.UtcNow < end)
            {
                k = (byte)((k + 1) % 30);
                byte value = k;
                d.Atomic(() =>
                {
                    d.ToC.FakeIcd.Test3 = value;
                    d.Sync();
                    if (d.FromC.FakeIcd.Test3 != value) Interlocked.Increment(ref wrong);
                    Thread.Sleep(1);                                      // the intruder is free to try here
                    if (d.ToC.FakeIcd.Test3 != value) Interlocked.Increment(ref wrong);
                    d.Sync();
                    if (d.FromC.FakeIcd.Test3 != value) Interlocked.Increment(ref wrong);
                });
                cycles++;
            }
            Volatile.Write(ref stop, 1);
            intruder.Wait();
            Console.WriteLine("  " + cycles + " cycles, " + intruderWrites + " competing writes");
            Check(cycles > 20, "stress ran");
            Check(intruderWrites > 5, "the other thread really competed");
            Check(wrong == 0, "values changed under the block: " + wrong);
        }
    }

    private static void RollbackAndNesting()
    {
        Console.WriteLine("exceptions discard uncommitted writes; nested blocks join");
        using (var d = OpenFast())
        {
            d.ToC.FakeIcd.Test1 = 1;
            try
            {
                d.Atomic(() =>
                {
                    d.ToC.FakeIcd.Test1 = 9;
                    Check(d.ToC.FakeIcd.Test1 == 9, "block sees its own write");
                    throw new InvalidTimeoutTestException();
                });
            }
            catch (InvalidTimeoutTestException) { }
            Check(d.ToC.FakeIcd.Test1 == 1, "rolled back");

            byte outside = 0;
            d.Atomic(() =>
            {
                d.Atomic(() => d.ToC.FakeIcd.Test1 = 7);
                var other = Task.Run(() => { outside = d.ToC.FakeIcd.Test1; });
                other.Wait();
            });
            Check(outside == 1, "other threads do not see an uncommitted write");
            Check(d.ToC.FakeIcd.Test1 == 7, "nested write committed with the outer block");

            Throws<InvalidOperationException>(() => d.Commit(), "Commit outside a block");
            Throws<IcdException>(() => d.Atomic(() => { d.ToC.FakeIcd.Test1 = 2; d.Sync(); }), "Sync with periodic traffic off");
        }
    }

    private static void GateTimeout()
    {
        Console.WriteLine("a stuck block times out instead of deadlocking");
        using (var d = OpenFast())
        {
            using (var hold = new ManualResetEventSlim())
            using (var inBlock = new ManualResetEventSlim())
            {
                var owner = Task.Run(() => d.Atomic(() => { inBlock.Set(); hold.Wait(); }));
                inBlock.Wait();
                Throws<IcdTimeoutException>(() => d.Atomic(() => d.ToC.FakeIcd.Test1 = 1, 50), "gate timeout");
                hold.Set();
                owner.Wait();
            }
        }
    }

    private static void LongOperation()
    {
        Console.WriteLine("long operation");
        using (var d = OpenFast())
        {
            d.StartCyclic(IcdChannel.D);
            Thread.Sleep(50);
            using (d.BeginLongOperation(IcdChannel.D))
            {
                ulong before = d.GetStats(IcdChannel.D).TxFrames;
                Thread.Sleep(50);
                Check(d.GetStats(IcdChannel.D).TxFrames == before, "periodic traffic stopped");
                d.ToD.EepromWrite.Address = 0x1234;
                d.ToD.EepromWrite.Value = 0x5A;
                d.ToD.EepromWrite.Send(500);
                Check(d.FromD.EepromWrite.Address == 0x1234, "exchange during long op");
                Check(d.GetStats(IcdChannel.D).TxFrames == before + 1, "exactly one frame sent");
            }
            ulong resumed = d.GetStats(IcdChannel.D).TxFrames;
            Thread.Sleep(50);
            Check(d.GetStats(IcdChannel.D).TxFrames > resumed + 5, "periodic traffic resumed");
        }
    }

    private static void Notifications()
    {
        Console.WriteLine("property change notifications");
        using (var d = OpenFast())
        {
            int test1 = 0, test2 = 0;
            d.ToC.FakeIcd.PropertyChanged += (s, e) =>
            {
                if (e.PropertyName == "Test1") Interlocked.Increment(ref test1);
                if (e.PropertyName == "Test2") Interlocked.Increment(ref test2);
            };
            d.StartNotifications(10);
            Thread.Sleep(50);
            d.ToC.FakeIcd.Test1 = 4;
            Thread.Sleep(100);
            Check(test1 == 1, "changed field raised once, got " + test1);
            Check(test2 == 0, "unchanged field did not raise");
            d.StopNotifications();
        }
    }

    private static void WatchdogAndEvents()
    {
        Console.WriteLine("watchdog expiry and link events reach the application");
        using (var d = OpenFast())
        {
            int watchdogEvents = 0, timeoutEvents = 0;
            d.LinkEvent += (s, e) =>
            {
                if (e.Channel == IcdChannel.D && e.Kind == IcdLinkEventKind.LongOpWatchdog) Interlocked.Add(ref watchdogEvents, (int)e.NewCount);
                if (e.Channel == IcdChannel.C && e.Kind == IcdLinkEventKind.RxTimeout) Interlocked.Add(ref timeoutEvents, (int)e.NewCount);
            };
            d.StartNotifications(10);

            d.StartCyclic(IcdChannel.D);
            var healthy = d.BeginLongOperation(IcdChannel.D, 5000);
            Check(!healthy.WatchdogFired, "fresh operation is not expired");
            healthy.ThrowIfWatchdogFired();
            healthy.Dispose();

            var expired = d.BeginLongOperation(IcdChannel.D, 100);
            ulong paused = d.GetStats(IcdChannel.D).TxFrames;
            Thread.Sleep(300);
            Check(expired.WatchdogFired, "expiry is visible to the owner");
            Throws<IcdException>(() => expired.ThrowIfWatchdogFired(), "ThrowIfWatchdogFired after expiry");
            Check(d.GetStats(IcdChannel.D).TxFrames > paused + 5, "periodic traffic restarted");

            var newer = d.BeginLongOperation(IcdChannel.D, 5000);
            expired.Dispose();                                     // must not end the newer operation
            ulong held = d.GetStats(IcdChannel.D).TxFrames;
            Thread.Sleep(50);
            Check(d.GetStats(IcdChannel.D).TxFrames == held, "disposing an expired operation leaves a newer one alone");
            newer.Dispose();

            d.SetSimFaults(IcdChannel.C, 5, 0);
            d.StartCyclic(IcdChannel.C);
            Thread.Sleep(150);
            d.StopCyclic(IcdChannel.C);
            Thread.Sleep(50);
            d.StopNotifications();
            Check(watchdogEvents == 1, "one watchdog event, got " + watchdogEvents);
            Check(timeoutEvents > 0 && (ulong)timeoutEvents == d.GetStats(IcdChannel.C).RxTimeouts, "timeout events match the counter, got " + timeoutEvents);
        }
    }

    private static int Main()
    {
        Accessors();
        BlockIsAllOrNothing();
        SetCheckChange();
        RollbackAndNesting();
        GateTimeout();
        LongOperation();
        Notifications();
        WatchdogAndEvents();
        Console.WriteLine();
        Console.WriteLine(_checks + " checks, " + _failures + " failed (" + System.Runtime.InteropServices.RuntimeInformation.FrameworkDescription + ")");
        return _failures;
    }

    private sealed class InvalidTimeoutTestException : Exception { }
}
