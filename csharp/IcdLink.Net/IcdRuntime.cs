// IcdRuntime.cs - C# side of the link. Generated code in IcdMessages.g.cs derives
// from the types here.
//
//   device.Atomic(() =>
//   {
//       device.ToC.FakeIcd.Power = true;     // nothing leaves until the block commits
//       device.ToC.FakeIcd.Test1 = 3;
//       device.Sync();                       // commit, wait for the device's answer
//       if (device.FromC.FakeIcd.Test1 == 3) // frozen reading of that answer
//           device.ToC.FakeIcd.Test2 = 1;
//   });                                      // commits the rest
//
// While a block runs no other thread can change anything, periodic traffic keeps
// sending the last committed values, and FromC/FromD do not move under the block.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace IcdLink
{
    public enum IcdChannel { C = 0, D = 1 }
    public enum IcdSide { To = 0, From = 1 }
    public enum IcdFieldKind : byte { Bool = 0, UInt = 1, Int = 2, Float32 = 3, Float64 = 4, Enum = 5 }

    public sealed class IcdFieldInfo
    {
        public readonly string Name, Unit, EnumText;
        public readonly int ByteOffset, ByteCount, BitLow, BitWidth, Direction;
        public readonly IcdFieldKind Kind;
        public readonly bool BigEndian;
        public readonly double Scale, ValueOffset;
        public IcdFieldInfo(string name, int byteOffset, int byteCount, int bitLow, int bitWidth, IcdFieldKind kind,
                            int direction, bool bigEndian, double scale, double valueOffset, string unit, string enumText)
        {
            Name = name; ByteOffset = byteOffset; ByteCount = byteCount; BitLow = bitLow; BitWidth = bitWidth;
            Kind = kind; Direction = direction; BigEndian = bigEndian; Scale = scale; ValueOffset = valueOffset;
            Unit = unit; EnumText = enumText;
        }
        public bool ExistsOn(IcdSide side) { return (Direction & (side == IcdSide.To ? 1 : 2)) != 0; }
        public bool IsScaled { get { return Scale != 1.0 || ValueOffset != 0.0; } }

        public KeyValuePair<long, string>[] EnumItems()
        {
            var list = new List<KeyValuePair<long, string>>();
            foreach (var part in EnumText.Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries))
            {
                var bits = part.Split('=');
                list.Add(new KeyValuePair<long, string>(long.Parse(bits[0], CultureInfo.InvariantCulture), bits[1]));
            }
            return list.ToArray();
        }
    }

    public sealed class IcdMsgInfo
    {
        public readonly string Name;
        public readonly char Channel;
        public readonly byte Id, RxId;
        public readonly int TxLen, RxLen;
        public readonly bool HasResponse, CyclicByDefault;
        public readonly IcdFieldInfo[] Fields;
        public int Index { get; internal set; }
        public IcdMsgInfo(string name, char channel, byte id, byte rxId, int txLen, int rxLen, bool hasResponse, bool cyclic, IcdFieldInfo[] fields)
        {
            Name = name; Channel = channel; Id = id; RxId = rxId; TxLen = txLen; RxLen = rxLen;
            HasResponse = hasResponse; CyclicByDefault = cyclic; Fields = fields;
        }
        public int Length(IcdSide side) { return side == IcdSide.To ? TxLen : RxLen; }
    }

    public sealed class IcdCrcInfo
    {
        private readonly uint[] _table = new uint[256];
        private readonly byte[] _reflect = new byte[256];
        private readonly uint _init, _xorOut;
        private readonly bool _refIn, _refOut;
        public readonly bool BigEndianOnWire, CoversType;

        public IcdCrcInfo(uint poly, uint init, uint xorOut, bool reflectIn, bool reflectOut, bool bigEndianOnWire, bool coversType)
        {
            _init = init; _xorOut = xorOut; _refIn = reflectIn; _refOut = reflectOut;
            BigEndianOnWire = bigEndianOnWire; CoversType = coversType;
            for (uint i = 0; i < 256; i++)
            {
                uint c = i << 24;
                for (int b = 0; b < 8; b++) c = (c & 0x80000000u) != 0 ? (c << 1) ^ poly : c << 1;
                _table[i] = c;
                byte r = 0;
                for (int b = 0; b < 8; b++) if ((i & (1u << b)) != 0) r |= (byte)(1 << (7 - b));
                _reflect[i] = r;
            }
        }

        /// <summary>The bytes that follow an image on the wire.</summary>
        public byte[] CrcBytes(byte[] image, int length)
        {
            uint c = _init;
            for (int i = CoversType ? 0 : 1; i < length; i++)
            {
                byte b = _refIn ? _reflect[image[i]] : image[i];
                c = (c << 8) ^ _table[((c >> 24) ^ b) & 0xFF];
            }
            if (_refOut)
                c = ((uint)_reflect[c & 0xFF] << 24) | ((uint)_reflect[(c >> 8) & 0xFF] << 16) |
                    ((uint)_reflect[(c >> 16) & 0xFF] << 8) | _reflect[(c >> 24) & 0xFF];
            c ^= _xorOut;
            var o = new byte[4];
            for (int i = 0; i < 4; i++) o[BigEndianOnWire ? 3 - i : i] = (byte)(c >> (8 * i));
            return o;
        }
    }

    public sealed class IcdSchemaInfo
    {
        public readonly uint Hash;
        public readonly IcdCrcInfo Crc;
        public readonly IcdMsgInfo[] Messages;
        public readonly int TxTotal, RxTotal;
        internal readonly int[] TxOffsets, RxOffsets;

        public IcdSchemaInfo(uint hash, IcdCrcInfo crc, IcdMsgInfo[] messages)
        {
            Hash = hash; Crc = crc; Messages = messages;
            TxOffsets = new int[messages.Length]; RxOffsets = new int[messages.Length];
            for (int i = 0; i < messages.Length; i++)
            {
                messages[i].Index = i;
                TxOffsets[i] = TxTotal; RxOffsets[i] = RxTotal;
                TxTotal += messages[i].TxLen; RxTotal += messages[i].RxLen;
            }
        }
    }

    public class IcdException : Exception
    {
        public readonly int Status;
        public IcdException(int status, string message) : base(message) { Status = status; }
    }
    public sealed class IcdTimeoutException : IcdException
    {
        public IcdTimeoutException(int status, string message) : base(status, message) { }
    }

    public enum IcdLinkEventKind { RxTimeout, CrcError, WindowMiss, MissedSlot, IoError, EchoError, LongOpWatchdog }

    /// <summary>Faults counted since the previous report for one channel and kind.</summary>
    public sealed class IcdLinkEventArgs : EventArgs
    {
        public readonly IcdChannel Channel;
        public readonly IcdLinkEventKind Kind;
        public readonly ulong NewCount, Total;
        public IcdLinkEventArgs(IcdChannel channel, IcdLinkEventKind kind, ulong newCount, ulong total)
        {
            Channel = channel; Kind = kind; NewCount = newCount; Total = total;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct IcdChannelConfig
    {
        public uint Baud, PeriodUs, WindowUs, RxTimeoutUs, SpinUs, TrailerFf, Rs485Echo, Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct IcdChannelStats
    {
        public ulong Slots, TxFrames, RxFrames, CrcErrors, RxTimeouts;
        public ulong WindowMisses, MissedSlots, StrayBytes, IoErrors, EchoErrors;
        public ulong OneShots, LongOps, LongOpWatchdogs;
        public ulong LastLatenessUs, MaxLatenessUs, LastExchangeUs;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct IcdMsgMeta
    {
        public ulong RxSeq, RxTag, LastCommit, SentTag;
        public long RxAgeUs;
        public uint RxValid, Reserved;
    }

    internal static class Native
    {
        private const string Dll = "IcdLink.dll";
        private const CallingConvention Cc = CallingConvention.Cdecl;
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_Create(out IntPtr h);
        [DllImport(Dll, CallingConvention = Cc)] public static extern void IL_Destroy(IntPtr h);
        [DllImport(Dll, CallingConvention = Cc)] public static extern uint IL_SchemaHash();
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_DefaultConfig(int ch, out IcdChannelConfig c);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)] public static extern int IL_ListFtdi(StringBuilder b, int cap);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)] public static extern int IL_ListCom(StringBuilder b, int cap);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)] public static extern int IL_LastError(IntPtr h, StringBuilder b, int cap);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)] public static extern int IL_LastWarning(IntPtr h, StringBuilder b, int cap);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)] public static extern int IL_OpenFtdi(IntPtr h, int ch, string serial, ref IcdChannelConfig c);
        [DllImport(Dll, CallingConvention = Cc, CharSet = CharSet.Ansi)] public static extern int IL_OpenCom(IntPtr h, int ch, string port, ref IcdChannelConfig c);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_OpenSim(IntPtr h, int ch, ref IcdChannelConfig c);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_SimSetFaults(IntPtr h, int ch, uint drop, uint corrupt, uint delay);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_Close(IntPtr h, int ch);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_IsOpen(IntPtr h, int ch);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_SetCyclic(IntPtr h, int ch, int e);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_SetCyclicMessage(IntPtr h, int m, int e);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_SetPeriod(IntPtr h, int ch, uint us);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_TxnBegin(IntPtr h, uint ms, out ulong t);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_TxnEnd(IntPtr h, ulong t);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_Commit(IntPtr h, ulong t, int[] m, int n, byte[] flat, out ulong seq);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_WaitApplied(IntPtr h, ulong seq, uint ms);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_Snapshot(IntPtr h, [Out] byte[] tx, [Out] byte[] rx, [Out] IcdMsgMeta[] meta, out ulong seq);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_ReadMsg(IntPtr h, int m, int side, [Out] byte[] o, out IcdMsgMeta meta);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_Exchange(IntPtr h, int m, uint ms, [Out] byte[] rx);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_LongOpBegin(IntPtr h, int ch, uint w, uint ms);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_LongOpEnd(IntPtr h, int ch);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_GetStats(IntPtr h, int ch, out IcdChannelStats s);
        [DllImport(Dll, CallingConvention = Cc)] public static extern int IL_ResetStats(IntPtr h, int ch);
    }

    /// <summary>Bit-level conversion between an image and field values.</summary>
    public static class IcdBits
    {
        private static ulong Mask(int width) { return width >= 64 ? ulong.MaxValue : (1UL << width) - 1UL; }

        private static ulong Assemble(byte[] image, IcdFieldInfo f)
        {
            ulong v = 0;
            for (int i = 0; i < f.ByteCount; i++)
                v = (v << 8) | image[f.ByteOffset + (f.BigEndian ? i : f.ByteCount - 1 - i)];
            return v;
        }

        public static ulong GetRaw(byte[] image, IcdFieldInfo f) { return (Assemble(image, f) >> f.BitLow) & Mask(f.BitWidth); }

        public static void SetRaw(byte[] image, IcdFieldInfo f, ulong raw)
        {
            ulong mask = Mask(f.BitWidth) << f.BitLow;
            ulong v = (Assemble(image, f) & ~mask) | ((raw << f.BitLow) & mask);
            for (int i = 0; i < f.ByteCount; i++)
            {
                image[f.ByteOffset + (f.BigEndian ? f.ByteCount - 1 - i : i)] = (byte)v;
                v >>= 8;
            }
        }

        public static long SignExtend(ulong raw, int width)
        {
            if (width >= 64) return unchecked((long)raw);
            ulong sign = 1UL << (width - 1);
            return unchecked((long)((raw ^ sign) - sign));
        }

        /// <summary>The field in engineering units. Bool is 0 or 1.</summary>
        public static double GetValue(byte[] image, IcdFieldInfo f)
        {
            ulong raw = GetRaw(image, f);
            switch (f.Kind)
            {
                case IcdFieldKind.Bool: return raw != 0 ? 1 : 0;
                case IcdFieldKind.Float32: return BitConverter.ToSingle(BitConverter.GetBytes((uint)raw), 0) * f.Scale + f.ValueOffset;
                case IcdFieldKind.Float64: return BitConverter.ToDouble(BitConverter.GetBytes(raw), 0) * f.Scale + f.ValueOffset;
                case IcdFieldKind.Int: return SignExtend(raw, f.BitWidth) * f.Scale + f.ValueOffset;
                default: return raw * f.Scale + f.ValueOffset;
            }
        }

        /// <summary>Stores a value in engineering units. Throws if it does not fit.</summary>
        public static void SetValue(byte[] image, IcdFieldInfo f, double value)
        {
            if (double.IsNaN(value)) throw new ArgumentOutOfRangeException(f.Name, "value is not a number");
            double scaled = (value - f.ValueOffset) / f.Scale;
            switch (f.Kind)
            {
                case IcdFieldKind.Bool:
                    if (value != 0 && value != 1) throw new ArgumentOutOfRangeException(f.Name, value, "must be 0 or 1");
                    SetRaw(image, f, value != 0 ? 1UL : 0UL);
                    return;
                case IcdFieldKind.Float32:
                    SetRaw(image, f, BitConverter.ToUInt32(BitConverter.GetBytes((float)scaled), 0));
                    return;
                case IcdFieldKind.Float64:
                    SetRaw(image, f, BitConverter.ToUInt64(BitConverter.GetBytes(scaled), 0));
                    return;
                case IcdFieldKind.Int:
                {
                    double r = Math.Round(scaled, MidpointRounding.AwayFromZero);
                    double hi = Math.Pow(2, f.BitWidth - 1);
                    if (r < -hi || r >= hi) throw new ArgumentOutOfRangeException(f.Name, value, "does not fit " + f.BitWidth + " bits");
                    SetRaw(image, f, unchecked((ulong)(long)r));
                    return;
                }
                default:
                {
                    double r = Math.Round(scaled, MidpointRounding.AwayFromZero);
                    if (r < 0 || r >= Math.Pow(2, f.BitWidth)) throw new ArgumentOutOfRangeException(f.Name, value, "does not fit " + f.BitWidth + " bits");
                    SetRaw(image, f, (ulong)r);
                    return;
                }
            }
        }

        public static string Format(byte[] image, IcdFieldInfo f)
        {
            double v = GetValue(image, f);
            if (f.Kind == IcdFieldKind.Bool) return v != 0 ? "True" : "False";
            if (f.Kind == IcdFieldKind.Enum)
                foreach (var e in f.EnumItems())
                    if (e.Key == (long)v) return e.Value;
            return v.ToString("G9", CultureInfo.InvariantCulture);
        }

        public static bool TryParse(string text, IcdFieldInfo f, out double value)
        {
            text = (text ?? "").Trim();
            if (string.Equals(text, "true", StringComparison.OrdinalIgnoreCase)) { value = 1; return true; }
            if (string.Equals(text, "false", StringComparison.OrdinalIgnoreCase)) { value = 0; return true; }
            if (f.Kind == IcdFieldKind.Enum)
                foreach (var e in f.EnumItems())
                    if (string.Equals(e.Value, text, StringComparison.OrdinalIgnoreCase)) { value = e.Key; return true; }
            if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                ulong hex;
                if (ulong.TryParse(text.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out hex)) { value = hex; return true; }
                value = 0;
                return false;
            }
            return double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out value);
        }
    }

    /// <summary>Typed accessors for one message in one direction. Base of the generated classes.</summary>
    public abstract class IcdMessageView : INotifyPropertyChanged
    {
        protected readonly IcdDeviceBase Device;
        public readonly IcdMsgInfo Info;
        public readonly IcdSide Side;
        public event PropertyChangedEventHandler PropertyChanged;

        protected IcdMessageView(IcdDeviceBase device, int msgIndex, IcdSide side)
        {
            Device = device; Info = device.Schema.Messages[msgIndex]; Side = side;
        }

        /// <summary>True once a good response to this message has been received.</summary>
        public bool IsValid { get { return Device.GetMeta(Info.Index).RxValid != 0; } }
        /// <summary>Number of good responses received.</summary>
        public ulong Sequence { get { return Device.GetMeta(Info.Index).RxSeq; } }
        /// <summary>Age of the newest response in milliseconds, or -1 if none has arrived.</summary>
        public double AgeMs { get { long a = Device.GetMeta(Info.Index).RxAgeUs; return a < 0 ? -1 : a / 1000.0; } }

        /// <summary>Sends this message once with its committed values. Waits for the response.</summary>
        public void Send(int timeoutMs = IcdDeviceBase.DefaultSyncTimeoutMs) { Device.Send(Info.Index, timeoutMs); }

        internal void Raise(string name)
        {
            var handler = PropertyChanged;
            if (handler != null) handler(this, new PropertyChangedEventArgs(name));
        }

        private byte[] Image() { return Device.CurrentImage(Info.Index, Side); }

        protected bool GetBool(int f) { return IcdBits.GetRaw(Image(), Info.Fields[f]) != 0; }
        protected ulong GetU(int f) { return IcdBits.GetRaw(Image(), Info.Fields[f]); }
        protected long GetI(int f) { var d = Info.Fields[f]; return IcdBits.SignExtend(IcdBits.GetRaw(Image(), d), d.BitWidth); }
        protected float GetF32(int f) { return BitConverter.ToSingle(BitConverter.GetBytes((uint)IcdBits.GetRaw(Image(), Info.Fields[f])), 0); }
        protected double GetF64(int f) { return BitConverter.ToDouble(BitConverter.GetBytes(IcdBits.GetRaw(Image(), Info.Fields[f])), 0); }
        protected double GetScaled(int f) { return IcdBits.GetValue(Image(), Info.Fields[f]); }

        protected void SetBool(int f, bool value) { Device.Mutate(Info.Index, img => IcdBits.SetRaw(img, Info.Fields[f], value ? 1UL : 0UL)); }

        protected void SetU(int f, ulong value)
        {
            var d = Info.Fields[f];
            if (d.BitWidth < 64 && value >= (1UL << d.BitWidth))
                throw new ArgumentOutOfRangeException(d.Name, value, "does not fit " + d.BitWidth + " bits");
            Device.Mutate(Info.Index, img => IcdBits.SetRaw(img, d, value));
        }

        protected void SetI(int f, long value)
        {
            var d = Info.Fields[f];
            if (d.BitWidth < 64)
            {
                long hi = 1L << (d.BitWidth - 1);
                if (value < -hi || value >= hi) throw new ArgumentOutOfRangeException(d.Name, value, "does not fit " + d.BitWidth + " bits");
            }
            Device.Mutate(Info.Index, img => IcdBits.SetRaw(img, d, unchecked((ulong)value)));
        }

        protected void SetF32(int f, float value) { Device.Mutate(Info.Index, img => IcdBits.SetRaw(img, Info.Fields[f], BitConverter.ToUInt32(BitConverter.GetBytes(value), 0))); }
        protected void SetF64(int f, double value) { Device.Mutate(Info.Index, img => IcdBits.SetRaw(img, Info.Fields[f], BitConverter.ToUInt64(BitConverter.GetBytes(value), 0))); }
        protected void SetScaled(int f, double value) { Device.Mutate(Info.Index, img => IcdBits.SetValue(img, Info.Fields[f], value)); }
    }

    /// <summary>A pause of periodic traffic on one channel. Dispose ends it.</summary>
    public sealed class IcdLongOperation : IDisposable
    {
        private readonly IcdDeviceBase _device;
        private readonly IcdChannel _channel;
        private readonly ulong _watchdogsAtStart;
        private int _ended;

        internal IcdLongOperation(IcdDeviceBase device, IcdChannel channel, ulong watchdogsAtStart)
        {
            _device = device; _channel = channel; _watchdogsAtStart = watchdogsAtStart;
        }

        /// <summary>True if the watchdog ended this operation and periodic traffic has already restarted.</summary>
        public bool WatchdogFired { get { return _device.GetStats(_channel).LongOpWatchdogs != _watchdogsAtStart; } }

        /// <summary>Throws if the watchdog ended this operation. Call it between steps.</summary>
        public void ThrowIfWatchdogFired()
        {
            if (WatchdogFired)
                throw new IcdException(-7, "The long operation on channel " + _channel + " was ended by its watchdog. Periodic traffic has restarted.");
        }

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _ended, 1) != 0) return;
            if (!WatchdogFired) _device.EndLongOperation(_channel); // once expired, the channel may belong to a newer operation
        }
    }

    public abstract class IcdDeviceBase : IDisposable
    {
        public const int DefaultGateTimeoutMs = 5000;
        public const int DefaultSyncTimeoutMs = 1000;

        private sealed class Txn
        {
            public ulong Token;
            public byte[][] Tx, Rx;
            public IcdMsgMeta[] Meta;
            public bool[] Dirty;
        }

        public readonly IcdSchemaInfo Schema;
        private IntPtr _h;
        private readonly ThreadLocal<Txn> _txn = new ThreadLocal<Txn>();
        private readonly IcdMessageView[,] _views;

        private Timer _notifyTimer;
        private SynchronizationContext _notifyContext;
        private int _notifyBusy;
        private byte[] _lastTx, _lastRx;
        private IcdMsgMeta[] _lastMeta;
        private IcdChannelStats[] _lastStats;

        /// <summary>Link faults. Raised on the context that called StartNotifications, so
        /// StartNotifications must be running for this to fire.</summary>
        public event EventHandler<IcdLinkEventArgs> LinkEvent;

        protected IcdDeviceBase(IcdSchemaInfo schema)
        {
            Schema = schema;
            if (Native.IL_SchemaHash() != schema.Hash)
                throw new InvalidOperationException("IcdLink.dll was built from a different ICD than this program. Regenerate and rebuild both.");
            Check(Native.IL_Create(out _h), "create");
            _views = new IcdMessageView[schema.Messages.Length, 2];
        }

        protected void RegisterViews(IcdMessageView[] views)
        {
            foreach (var v in views) _views[v.Info.Index, (int)v.Side] = v;
        }

        public void Dispose()
        {
            StopNotifications();
            if (_h != IntPtr.Zero) { Native.IL_Destroy(_h); _h = IntPtr.Zero; }
            _txn.Dispose();
        }

        private void Check(int status, string what)
        {
            if (status == 0) return;
            var text = new StringBuilder(512);
            if (_h != IntPtr.Zero) Native.IL_LastError(_h, text, text.Capacity);
            string message = what + " failed (" + StatusName(status) + ")" + (text.Length > 0 ? ": " + text : "");
            if (status == -3 || status == -4) throw new IcdTimeoutException(status, message);
            throw new IcdException(status, message);
        }

        private static string StatusName(int s)
        {
            switch (s)
            {
                case -1: return "bad argument";
                case -2: return "channel not open";
                case -3: return "timeout";
                case -4: return "busy";
                case -5: return "I/O error";
                case -6: return "not the transaction owner";
                case -7: return "long operation active";
                case -8: return "schedule does not fit";
                case -9: return "closed";
                case -10: return "periodic traffic is off";
                default: return "status " + s;
            }
        }

        // ------------------------------------------------------ connection

        public static string[] ListFtdi()
        {
            var b = new StringBuilder(8192);
            return Native.IL_ListFtdi(b, b.Capacity) < 0 ? new string[0] : b.ToString().Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries);
        }

        public static string[] ListCom()
        {
            var b = new StringBuilder(8192);
            Native.IL_ListCom(b, b.Capacity);
            return b.ToString().Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries);
        }

        public IcdChannelConfig DefaultConfig(IcdChannel ch)
        {
            IcdChannelConfig c;
            Check(Native.IL_DefaultConfig((int)ch, out c), "DefaultConfig");
            return c;
        }

        public void OpenFtdi(IcdChannel ch, string serial = null, IcdChannelConfig? config = null)
        {
            var c = config ?? DefaultConfig(ch);
            Check(Native.IL_OpenFtdi(_h, (int)ch, string.IsNullOrEmpty(serial) ? null : serial, ref c), "OpenFtdi " + ch);
        }
        public void OpenCom(IcdChannel ch, string port, IcdChannelConfig? config = null)
        {
            var c = config ?? DefaultConfig(ch);
            Check(Native.IL_OpenCom(_h, (int)ch, port, ref c), "OpenCom " + ch);
        }
        public void OpenSim(IcdChannel ch, IcdChannelConfig? config = null)
        {
            var c = config ?? DefaultConfig(ch);
            Check(Native.IL_OpenSim(_h, (int)ch, ref c), "OpenSim " + ch);
        }
        public void SetSimFaults(IcdChannel ch, uint dropEvery, uint corruptEvery, uint delayUs = 200) { Check(Native.IL_SimSetFaults(_h, (int)ch, dropEvery, corruptEvery, delayUs), "SetSimFaults"); }
        public void Close(IcdChannel ch) { Check(Native.IL_Close(_h, (int)ch), "Close"); }
        public bool IsOpen(IcdChannel ch) { return Native.IL_IsOpen(_h, (int)ch) == 1; }

        /// <summary>A non-fatal note from the last open, or an empty string.</summary>
        public string TakeWarning()
        {
            var b = new StringBuilder(512);
            Native.IL_LastWarning(_h, b, b.Capacity);
            return b.ToString();
        }

        public void StartCyclic(IcdChannel ch) { Check(Native.IL_SetCyclic(_h, (int)ch, 1), "StartCyclic"); }
        public void StopCyclic(IcdChannel ch) { Check(Native.IL_SetCyclic(_h, (int)ch, 0), "StopCyclic"); }
        public void SetCyclicMessage(int msg, bool enable) { Check(Native.IL_SetCyclicMessage(_h, msg, enable ? 1 : 0), "SetCyclicMessage"); }
        public void SetPeriod(IcdChannel ch, uint periodUs) { Check(Native.IL_SetPeriod(_h, (int)ch, periodUs), "SetPeriod"); }

        public IcdChannelStats GetStats(IcdChannel ch)
        {
            IcdChannelStats s;
            Check(Native.IL_GetStats(_h, (int)ch, out s), "GetStats");
            return s;
        }
        public void ResetStats(IcdChannel ch) { Check(Native.IL_ResetStats(_h, (int)ch), "ResetStats"); }

        // ---------------------------------------------------- transactions

        /// <summary>Runs a block with exclusive control of every To value. All writes take
        /// effect together when the block ends or at Commit/Sync. An exception discards
        /// writes not yet committed. Nested calls on one thread join the outer block.</summary>
        public void Atomic(Action body, int gateTimeoutMs = DefaultGateTimeoutMs)
        {
            if (body == null) throw new ArgumentNullException("body");
            if (_txn.Value != null) { body(); return; }
            var t = Begin(gateTimeoutMs);
            _txn.Value = t;
            try
            {
                body();
                Publish(t);
            }
            finally
            {
                _txn.Value = null;
                Native.IL_TxnEnd(_h, t.Token);
            }
        }

        public T Atomic<T>(Func<T> body, int gateTimeoutMs = DefaultGateTimeoutMs)
        {
            T result = default(T);
            Atomic(() => { result = body(); }, gateTimeoutMs);
            return result;
        }

        private Txn Begin(int gateTimeoutMs)
        {
            ulong token;
            Check(Native.IL_TxnBegin(_h, (uint)gateTimeoutMs, out token), "Atomic");
            var t = new Txn { Token = token };
            int n = Schema.Messages.Length;
            t.Tx = new byte[n][]; t.Rx = new byte[n][]; t.Dirty = new bool[n]; t.Meta = new IcdMsgMeta[n];
            for (int i = 0; i < n; i++) { t.Tx[i] = new byte[Schema.Messages[i].TxLen]; t.Rx[i] = new byte[Schema.Messages[i].RxLen]; }
            try { Load(t, true); }
            catch { Native.IL_TxnEnd(_h, token); throw; }
            return t;
        }

        private void Load(Txn t, bool includeTx)
        {
            var tx = includeTx ? new byte[Schema.TxTotal] : null;
            var rx = new byte[Schema.RxTotal];
            ulong seq;
            Check(Native.IL_Snapshot(_h, tx, rx, t.Meta, out seq), "Snapshot");
            for (int i = 0; i < Schema.Messages.Length; i++)
            {
                if (includeTx) Buffer.BlockCopy(tx, Schema.TxOffsets[i], t.Tx[i], 0, Schema.Messages[i].TxLen);
                Buffer.BlockCopy(rx, Schema.RxOffsets[i], t.Rx[i], 0, Schema.Messages[i].RxLen);
            }
        }

        private void Publish(Txn t)
        {
            var ids = new List<int>();
            int bytes = 0;
            for (int i = 0; i < t.Dirty.Length; i++)
                if (t.Dirty[i]) { ids.Add(i); bytes += Schema.Messages[i].TxLen; }
            if (ids.Count == 0) return;
            var flat = new byte[bytes];
            int offset = 0;
            foreach (int i in ids)
            {
                Buffer.BlockCopy(t.Tx[i], 0, flat, offset, Schema.Messages[i].TxLen);
                offset += Schema.Messages[i].TxLen;
            }
            ulong seq;
            Check(Native.IL_Commit(_h, t.Token, ids.ToArray(), ids.Count, flat, out seq), "Commit");
            Array.Clear(t.Dirty, 0, t.Dirty.Length);
        }

        private Txn Require(string what)
        {
            var t = _txn.Value;
            if (t == null) throw new InvalidOperationException(what + " can only be used inside Atomic");
            return t;
        }

        /// <summary>Publishes this block's writes now, as one update. The block stays open.</summary>
        public void Commit() { Publish(Require("Commit")); }

        /// <summary>Updates FromC and FromD inside a block with the newest readings.</summary>
        public void Refresh() { Load(Require("Refresh"), false); }

        /// <summary>Commits, then waits for a response to a frame carrying that commit, then
        /// freezes FromC/FromD at the newest readings. Outside a block, waits for everything
        /// already committed.</summary>
        public void Sync(int timeoutMs = DefaultSyncTimeoutMs)
        {
            var t = _txn.Value;
            if (t != null) Publish(t);
            ulong seq;                  // newest commit, including any made before this block
            Check(Native.IL_Snapshot(_h, null, null, null, out seq), "Snapshot");
            Check(Native.IL_WaitApplied(_h, seq, (uint)timeoutMs), "Sync");
            if (t != null) Load(t, false);
        }

        /// <summary>Sends a message once with its committed values and waits for the response.
        /// With periodic traffic running it takes the next slot.</summary>
        public byte[] Send(int msg, int timeoutMs = DefaultSyncTimeoutMs)
        {
            var t = _txn.Value;
            if (t != null) Publish(t);
            var rx = new byte[Schema.Messages[msg].RxLen];
            Check(Native.IL_Exchange(_h, msg, (uint)timeoutMs, rx), "Send " + Schema.Messages[msg].Name);
            if (t != null) Load(t, false);
            return rx;
        }

        /// <summary>Stops periodic traffic on a channel for slow device work such as EEPROM
        /// access. Send still works. If nothing happens for watchdogMs, traffic restarts.</summary>
        public IcdLongOperation BeginLongOperation(IcdChannel ch, int watchdogMs = 30000, int timeoutMs = DefaultSyncTimeoutMs)
        {
            ulong watchdogs = GetStats(ch).LongOpWatchdogs;
            Check(Native.IL_LongOpBegin(_h, (int)ch, (uint)watchdogMs, (uint)timeoutMs), "BeginLongOperation");
            return new IcdLongOperation(this, ch, watchdogs);
        }
        internal void EndLongOperation(IcdChannel ch) { Check(Native.IL_LongOpEnd(_h, (int)ch), "EndLongOperation"); }

        // ----------------------------------------------------- image access

        internal byte[] CurrentImage(int msg, IcdSide side)
        {
            var t = _txn.Value;
            if (t != null) return side == IcdSide.To ? t.Tx[msg] : t.Rx[msg];
            var image = new byte[Schema.Messages[msg].Length(side)];
            IcdMsgMeta meta;
            Check(Native.IL_ReadMsg(_h, msg, (int)side, image, out meta), "Read");
            return image;
        }

        internal void Mutate(int msg, Action<byte[]> change)
        {
            var t = _txn.Value;
            if (t != null)
            {
                change(t.Tx[msg]);
                t.Dirty[msg] = true;
                return;
            }
            Atomic(() => Mutate(msg, change));
        }

        public IcdMsgMeta GetMeta(int msg)
        {
            var t = _txn.Value;
            if (t != null) return t.Meta[msg];
            IcdMsgMeta meta;
            Check(Native.IL_ReadMsg(_h, msg, 0, null, out meta), "Read");
            return meta;
        }

        /// <summary>Returns a copy of a whole image.</summary>
        public byte[] ReadImage(int msg, IcdSide side) { return (byte[])CurrentImage(msg, side).Clone(); }

        /// <summary>Replaces a whole To image. The message-type byte is kept.</summary>
        public void WriteImage(int msg, byte[] image)
        {
            if (image == null || image.Length != Schema.Messages[msg].TxLen) throw new ArgumentException("wrong image length");
            Mutate(msg, img => { byte id = img[0]; Buffer.BlockCopy(image, 0, img, 0, img.Length); img[0] = id; });
        }

        // ----------------------------------------------------- notifications

        /// <summary>Raises PropertyChanged on the calling thread's context whenever a value
        /// changes, for data binding from MVVM or the WinForms designer. Call from the UI thread.</summary>
        public void StartNotifications(int intervalMs = 50)
        {
            StopNotifications();
            _notifyContext = SynchronizationContext.Current;
            _lastStats = new[] { GetStats(IcdChannel.C), GetStats(IcdChannel.D) };
            _notifyTimer = new Timer(OnNotify, null, intervalMs, intervalMs);
        }

        public void StopNotifications()
        {
            var timer = _notifyTimer;
            _notifyTimer = null;
            if (timer != null) timer.Dispose();
        }

        private void OnNotify(object state)
        {
            if (Interlocked.Exchange(ref _notifyBusy, 1) != 0) return;
            try
            {
                ReportLinkEvents();
                var tx = new byte[Schema.TxTotal];
                var rx = new byte[Schema.RxTotal];
                var meta = new IcdMsgMeta[Schema.Messages.Length];
                ulong seq;
                if (Native.IL_Snapshot(_h, tx, rx, meta, out seq) != 0) return;
                var changes = new List<KeyValuePair<IcdMessageView, string>>();
                if (_lastTx != null)
                {
                    foreach (var m in Schema.Messages)
                    {
                        Diff(m, IcdSide.To, tx, _lastTx, Schema.TxOffsets[m.Index], changes);
                        Diff(m, IcdSide.From, rx, _lastRx, Schema.RxOffsets[m.Index], changes);
                        var view = _views[m.Index, (int)IcdSide.From];
                        if (view != null && meta[m.Index].RxSeq != _lastMeta[m.Index].RxSeq)
                        {
                            changes.Add(new KeyValuePair<IcdMessageView, string>(view, "Sequence"));
                            changes.Add(new KeyValuePair<IcdMessageView, string>(view, "IsValid"));
                            changes.Add(new KeyValuePair<IcdMessageView, string>(view, "AgeMs"));
                        }
                    }
                }
                _lastTx = tx; _lastRx = rx; _lastMeta = meta;
                if (changes.Count == 0) return;
                SendOrPostCallback raise = _ => { foreach (var c in changes) c.Key.Raise(c.Value); };
                if (_notifyContext != null) _notifyContext.Post(raise, null);
                else raise(null);
            }
            finally
            {
                Volatile.Write(ref _notifyBusy, 0);
            }
        }

        private void ReportLinkEvents()
        {
            var found = new List<IcdLinkEventArgs>();
            foreach (IcdChannel ch in new[] { IcdChannel.C, IcdChannel.D })
            {
                IcdChannelStats now;
                if (Native.IL_GetStats(_h, (int)ch, out now) != 0) continue;
                var before = _lastStats[(int)ch];
                _lastStats[(int)ch] = now;
                AddEvent(found, ch, IcdLinkEventKind.RxTimeout, before.RxTimeouts, now.RxTimeouts);
                AddEvent(found, ch, IcdLinkEventKind.CrcError, before.CrcErrors, now.CrcErrors);
                AddEvent(found, ch, IcdLinkEventKind.WindowMiss, before.WindowMisses, now.WindowMisses);
                AddEvent(found, ch, IcdLinkEventKind.MissedSlot, before.MissedSlots, now.MissedSlots);
                AddEvent(found, ch, IcdLinkEventKind.IoError, before.IoErrors, now.IoErrors);
                AddEvent(found, ch, IcdLinkEventKind.EchoError, before.EchoErrors, now.EchoErrors);
                AddEvent(found, ch, IcdLinkEventKind.LongOpWatchdog, before.LongOpWatchdogs, now.LongOpWatchdogs);
            }
            if (found.Count == 0) return;
            SendOrPostCallback raise = _ =>
            {
                var handler = LinkEvent;
                if (handler != null) foreach (var e in found) handler(this, e);
            };
            if (_notifyContext != null) _notifyContext.Post(raise, null);
            else raise(null);
        }

        private static void AddEvent(List<IcdLinkEventArgs> list, IcdChannel ch, IcdLinkEventKind kind, ulong before, ulong now)
        {
            if (now > before) list.Add(new IcdLinkEventArgs(ch, kind, now - before, now)); // lower means the counters were reset
        }

        private void Diff(IcdMsgInfo m, IcdSide side, byte[] now, byte[] before, int offset, List<KeyValuePair<IcdMessageView, string>> changes)
        {
            var view = _views[m.Index, (int)side];
            if (view == null) return;
            int len = m.Length(side);
            var a = new byte[len];
            var b = new byte[len];
            Buffer.BlockCopy(now, offset, a, 0, len);
            Buffer.BlockCopy(before, offset, b, 0, len);
            foreach (var f in m.Fields)
                if (f.ExistsOn(side) && IcdBits.GetRaw(a, f) != IcdBits.GetRaw(b, f))
                    changes.Add(new KeyValuePair<IcdMessageView, string>(view, f.Name));
        }
    }
}
