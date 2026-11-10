# IcdLink how-to

Practical recipes for using IcdLink from a C# application, the test GUI and batch
files. Field names in the examples (`Telemetry`, `EepromWrite`, `FakeIcd`) come
from the sample ICDs. Yours will match your own ICD.

> **Hardware status:** everything here has been tested against the built-in
> simulated device. It has not yet been run on a physical FT4232H.

## Contents

1. [Add or update an ICD](#1-add-or-update-an-icd)
2. [Connect from your application](#2-connect-from-your-application)
3. [Read and write values](#3-read-and-write-values)
4. [Change several values at once](#4-change-several-values-at-once)
5. [Set, check, then change](#5-set-check-then-change)
6. [Sync or Send](#6-sync-or-send)
7. [Send a message on demand](#7-send-a-message-on-demand)
8. [Pause for EEPROM and other slow work](#8-pause-for-eeprom-and-other-slow-work)
9. [Handle a watchdog expiry](#9-handle-a-watchdog-expiry)
10. [Keep the UI busy during a bit test](#10-keep-the-ui-busy-during-a-bit-test)
11. [Bind to MVVM or WinForms](#11-bind-to-mvvm-or-winforms)
12. [Handle errors and link faults](#12-handle-errors-and-link-faults)
13. [Automate with batch files](#13-automate-with-batch-files)
14. [Use the test GUI](#14-use-the-test-gui)
15. [Troubleshooting](#15-troubleshooting)

---

## 1. Add or update an ICD

The ICD is compiled into the code, so every change needs a rebuild.

1. In Word, select the table and paste it into a `.txt` file in `icd/`. A `.docx`
   also works directly. The text file is there for documents that parse badly.
2. Add a line above the table giving the ID and channel:

   ```text
   Message 0x90 Status (Channel C)
   Byte	Bits	Desc
   2	0	Power
   ```

3. List the file in `icd/project.json`:

   ```json
   "sources": [ { "file": "myicd.txt" } ]
   ```

   If you would rather not edit the text file, give the details here instead:

   ```json
   { "file": "myicd.txt", "channel": "C", "id": "0x90", "name": "Status" }
   ```

4. Set the real baud, period, window, RX timeout and CRC settings in the same file.
5. Run `build.bat Release`.

**Replies that are shorter than the send.** A Byte/Bits/Desc table says nothing
about what comes back, so the generator expects the full length. Mark send-only
fields with a `Dir` column containing `to`, or list them in the source entry:

```json
{ "file": "myicd.txt", "omit_from": ["Test3"] }
```

The reply ends at the last field it returns. Fields keep the same byte positions
in both directions.

### Separate send and receive ICDs

Use this when the reply doesn't line up with what was sent: different fields,
positions, lengths or message ID. Give one file for each direction:

```json
"sources": [ { "tx": "send.txt", "rx": "receive.txt" } ]
```

The table format is the same in both files, and either can be `.txt` or `.docx`.
`Reserved` rows in the receive file count toward the reply length.

**One message.** A single table in each file always pairs up, whatever its heading
says. If the files have no heading lines, set the details in the source entry:

```json
{ "tx": "send.txt", "rx": "receive.txt", "channel": "C", "id": "0x90", "rx_id": "0x91", "name": "Status" }
```

`rx_id` is optional and defaults to the same value as `id`.

**Several messages.** Put each message in its own table, with a heading such as
`Message 0x90 Status (Channel C)` above every table in both files. Each send table
is matched with a receive table by the same ID, and then by the same name. Words like
Tx, Rx, Send and Reply are ignored in names, so `Status Reply` matches `Status`.

When IDs and names don't match, say which goes with which. List any message
the device never answers under `no_response`:

```json
{ "tx": "send.txt", "rx": "receive.txt",
  "pairs": { "0x10": "0x20", "0x12": "0x22" },
  "no_response": [ "0x30" ] }
```

If any table is left without a partner, generation stops and lists them.

In code nothing changes, and a field can have the same name in both directions at
different positions:

```csharp
d.ToC.Status.Level = 0x5A;               // byte 3 of the send layout
byte level = d.FromC.Status.Level;       // byte 2 of the receive layout
```

If an enum has the same name in both directions, its types are named
`C_Status_Mode_To` and `C_Status_Mode_From`, because their values can differ.

Every send must have exactly one expected reply, or none. Messages the device
sends on its own, without being asked, aren't supported.

**Other optional table columns:** `Type` (bool, uint8..64, int8..64, float, double,
enum), `Scale`, `Offset`, `Unit`, `Default`, `Enum` (`0=Off,1=On`).

**Other per-message options** in the source entry: `"cyclic": false` for messages
sent only on demand, and `"has_response": false` for messages the device never
answers.

**Byte numbering.** `"byte_base": 1` means Byte 1 is the message type and Byte 2
is the first payload byte. Use `0` if the ICD counts from zero.

## 2. Connect from your application

Reference `IcdLink.Net`, build for x64 and put `IcdLink.dll` beside the exe.

```csharp
using IcdLink;
using IcdLink.Generated;

public static class Link
{
    public static readonly IcdDevice Device = new IcdDevice();
}

Link.Device.OpenFtdi(IcdChannel.C);     // the serial argument picks a chip if several are attached
Link.Device.OpenFtdi(IcdChannel.D);

string warning = Link.Device.TakeWarning();   // for example, RS-485 TXDEN turned off

Link.Device.StartCyclic(IcdChannel.C);
Link.Device.StartCyclic(IcdChannel.D);
```

To develop without hardware, use `Link.Device.OpenSim(IcdChannel.C)`. To override
settings at connect time:

```csharp
var config = Link.Device.DefaultConfig(IcdChannel.D);
config.Baud = 921600;
config.PeriodUs = 20000;
config.RxTimeoutUs = 18000;
Link.Device.OpenFtdi(IcdChannel.D, null, config);
```

Call `Link.Device.Dispose()` when the application exits.

## 3. Read and write values

`To` values are what you send. `From` values are what the device returned, and
they are read-only.

```csharp
Link.Device.ToD.Telemetry.Mode = D_Telemetry_Mode.Run;
double temperature = Link.Device.FromD.Telemetry.TempC;
```

- Any thread can do this at any time.
- A value that doesn't fit its bits throws `ArgumentOutOfRangeException`.
- Setting a value only changes what gets sent. It reaches the device with the next
  periodic frame or `Send`. `FromD` keeps its old value until a reply arrives.
- `Link.Device.FromD.Telemetry.IsValid` is false until the first good response.
  `Sequence` counts responses and `AgeMs` is the age of the newest one.

## 4. Change several values at once

Each plain set is a separate update. To make several land in the same frame, use
a block:

```csharp
Link.Device.Atomic(() =>
{
    Link.Device.ToC.FakeIcd.Power = true;
    Link.Device.ToC.FakeIcd.Test1 = 3;
    Link.Device.ToC.FakeIcd.Test2 = 5;
});
```

Nothing is sent until the block ends, and then every frame has all of the new
values or none. If the block throws, its uncommitted changes are discarded.
One block can change messages on both channels.

## 5. Set, check, then change

```csharp
var d = Link.Device;
d.Atomic(() =>
{
    d.ToC.FakeIcd.Test1 = 3;
    d.Sync();                       // commit, wait for the device's answer

    if (d.FromC.FakeIcd.Test1 == 3)
        d.ToC.FakeIcd.Test2 = 1;    // sent when the block ends
});
```

While a block runs:

- No other thread can change any `To` value. Their sets wait for the block.
- Periodic traffic keeps running with the last committed values.
- `From` values stay frozen, and only move at `Sync`, `Send` or `Refresh`.

Do not use `await` inside `Atomic`. A block belongs to one thread, and code after
an `await` can resume outside it. Use `Thread.Sleep` for delays, and run the whole
block with `Task.Run`.

## 6. Sync or Send

| | `Sync()` | `Send()` |
|---|---|---|
| Transmits anything itself | No | Yes, one frame |
| What it waits for | Periodic traffic to carry your changes and the device to answer | The reply to that one frame |
| Periodic running | Required | Takes the next slot |
| Periodic off | Throws | Sends immediately |
| During a long operation | Throws for that channel | Sends immediately |
| On-demand messages | Returns without waiting | The way to send them |

Both commit a block's changes first and update `From` values afterward when used
inside `Atomic`.

Only use the values read after `Sync` if the call returned normally. It throws
`IcdTimeoutException` if the device doesn't answer in time.

## 7. Send a message on demand

Mark the message `"cyclic": false` in the ICD, then:

```csharp
var d = Link.Device;
d.Atomic(() =>
{
    d.ToD.EepromWrite.Address = 0x10;
    d.ToD.EepromWrite.Value = 0x5A;
});
d.ToD.EepromWrite.Send(1000);
ushort echoed = d.FromD.EepromWrite.Address;
```

To change which messages rotate through the period at runtime:

```csharp
d.SetCyclicMessage(d.ToD.Telemetry.Info.Index, false);
```

## 8. Pause for EEPROM and other slow work

```csharp
var d = Link.Device;
await Task.Run(() =>
{
    using (var op = d.BeginLongOperation(IcdChannel.D, watchdogMs: 30000))
    {
        d.ToD.EepromWrite.Address = 0x10;
        d.ToD.EepromWrite.Value = 0x5A;
        d.ToD.EepromWrite.Send(5000);       // timeout for this slow reply
    }                                       // periodic traffic resumes here
});
```

- Only that channel pauses. The other keeps its timing.
- `Send` goes out immediately while paused, and its timeout is the time allowed
  for that reply.
- Use `Send` while paused, because `Sync` throws.
- If the device doesn't reply to the message, set `"has_response": false`.
- Periodic traffic restarts by itself when the `using` block ends, including when
  an exception leaves it.

## 9. Handle a watchdog expiry

The watchdog protects the device if your program hangs mid-operation. If nothing
is sent for `watchdogMs`, periodic traffic restarts by itself. Time spent inside
a `Send` doesn't count, and each `Send` restarts the countdown.

Once it fires, your EEPROM sequence is no longer protected, so check for it between
steps:

```csharp
var d = Link.Device;
try
{
    await Task.Run(() =>
    {
        using (var op = d.BeginLongOperation(IcdChannel.D, watchdogMs: 30000))
        {
            for (ushort address = 0; address < 16; address++)
            {
                op.ThrowIfWatchdogFired();
                d.ToD.EepromWrite.Address = address;
                d.ToD.EepromWrite.Value = 0;
                d.ToD.EepromWrite.Send(5000);
            }
            op.ThrowIfWatchdogFired();
        }
    });
}
catch (IcdException ex)
{
    // Periodic traffic is already running again. The EEPROM may be partly written,
    // so verify it or redo the whole sequence.
    Status = ex.Message;
}
```

`op.WatchdogFired` gives the same answer as a bool. To be told even when no
operation code is running, watch for `LongOpWatchdog` in
[`LinkEvent`](#12-handle-errors-and-link-faults).

Choose `watchdogMs` longer than the longest gap between your `Send` calls, including
any `Thread.Sleep` while waiting for the device.

## 10. Keep the UI busy during a bit test

Use one flag for the whole page, and bind `IsEnabled` or your commands' `CanExecute`
to it.

```csharp
public async Task RunBitTestAsync()
{
    if (IsBusy) return;
    IsBusy = true;
    var progress = new Progress<string>(text => Status = text);
    try
    {
        await Task.Run(() => BitTest(progress));
        Status = "Passed";
    }
    catch (IcdException ex)
    {
        Status = "Failed: " + ex.Message;
    }
    finally
    {
        IsBusy = false;
    }
}

private void BitTest(IProgress<string> progress)
{
    var d = Link.Device;
    d.Atomic(() =>
    {
        d.ToD.Telemetry.Enable = true;
        d.Sync();
        progress.Report("Step 1 done");
        Thread.Sleep(500);

        if (!d.FromD.Telemetry.Enable)
            throw new IcdException(0, "Enable did not latch");

        d.ToD.Telemetry.Mode = D_Telemetry_Mode.Run;
        d.Sync();
        progress.Report("Step 2 done");
    });
}
```

**Keep a strict test in one `Atomic`.** `IsBusy` only disables the buttons. It doesn't
stop timers or background code. Between separate `await`ed steps, nothing holds
the values, so something else could change them mid-test.

While a long block runs, other threads' sets wait and throw `IcdTimeoutException`
after 5 seconds. Don't let them run on the UI thread during a test.

A bit test can include a long operation. Put the `using` inside the block, use `Send`
inside it, and call `Sync` only after the `using` ends.

## 11. Bind to MVVM or WinForms

Every message implements `INotifyPropertyChanged`. Start notifications once from
the UI thread after connecting:

```csharp
Link.Device.StartNotifications();
```

WPF example, with `DataContext` set to `Link.Device`:

```xml
<CheckBox IsChecked="{Binding ToD.Telemetry.Enable, Mode=TwoWay}" />
<TextBox  Text="{Binding ToD.Telemetry.TempC, Mode=TwoWay, ValidatesOnExceptions=True}" />
<TextBlock Text="{Binding FromD.Telemetry.TempC, Mode=OneWay}" />
```

- Use `ValidatesOnExceptions` so an out-of-range entry shows as a validation error.
- Bind `FromD` and `FromC` with `OneWay`, since they have no setters.
- In WinForms, use a `BindingSource` whose data source is the message object, such
  as `Link.Device.ToD.Telemetry`.

## 12. Handle errors and link faults

### Exceptions from your calls

| Exception or message | Meaning | What to do |
|---|---|---|
| `IcdTimeoutException` from `Send` or `Sync` | No good reply in time | Check wiring, baud and CRC settings. Retry or abort the test. |
| `IcdTimeoutException` from a set or `Atomic` | Another block held the values too long | Shorten blocks or move the work off the UI thread. |
| `periodic traffic is off` | `Sync` with periodic stopped | Call `StartCyclic`, or use `Send`. |
| `long operation active` | `Sync` on a paused channel | Use `Send`, or `Sync` after the operation ends. |
| `channel not open` | Not connected | Open the channel. |
| `schedule does not fit` | The frames can't fit the period at this baud | Raise the baud or period, or the RX timeout. |
| `ArgumentOutOfRangeException` | Value doesn't fit the field | Fix the value. |
| `InvalidOperationException` at startup | DLL and C# built from different ICDs | Run `build.bat` and use the matching DLL. |

An exception inside `Atomic` discards changes not yet committed. Anything already
committed by `Commit`, `Sync` or `Send` stays.

### Faults on the link

Faults in periodic traffic happen with no call of yours to throw from, so subscribe
to `LinkEvent`. It needs `StartNotifications` running, and it arrives on the UI
thread.

```csharp
Link.Device.LinkEvent += (sender, e) =>
{
    switch (e.Kind)
    {
        case IcdLinkEventKind.LongOpWatchdog:
            Status = "Channel " + e.Channel + " long operation expired";
            break;
        case IcdLinkEventKind.IoError:
            Status = "Channel " + e.Channel + " I/O error, reconnect needed";
            break;
        default:
            Log(e.Channel + " " + e.Kind + ": " + e.NewCount + " new, " + e.Total + " total");
            break;
    }
};
Link.Device.StartNotifications();
```

| Kind | Meaning | Usual cause |
|---|---|---|
| `RxTimeout` | No complete good reply for a frame | Device silent, wrong baud, RX timeout too short |
| `CrcError` | A candidate reply failed its CRC | CRC settings wrong, line noise |
| `WindowMiss` | A send was later than the window | PC overloaded |
| `MissedSlot` | A whole slot was skipped | PC overloaded, period too tight |
| `EchoError` | The heard-back transmit didn't match | Bus collision, `rs485_echo` set wrongly |
| `IoError` | A driver read or write failed | Cable unplugged, device reset |
| `LongOpWatchdog` | A long operation expired | Program stalled, watchdog too short |

For an `IoError` that keeps repeating, call `Close` on the channel, then open it
again and restart periodic traffic.

You can also read the counters at any time with `Link.Device.GetStats(IcdChannel.C)`.

## 13. Automate with batch files

One command line, in one process:

```bat
IcdLinkCli open sim then cyclic C on then set ToC.FakeIcd.Test1 7 then sync then expect FromC.FakeIcd.Test1 == 7
```

The link closes when that process exits. For a `.bat` with several steps or
branching, keep one server running. See `tests/demo_test.bat`:

```bat
start "" /b IcdLinkCli serve
IcdLinkCli call open ftdi
IcdLinkCli call cyclic C on
IcdLinkCli call begin then set ToC.FakeIcd.Power 1 then sync
IcdLinkCli call expect FromC.FakeIcd.Power == 1
if errorlevel 1 goto failed
IcdLinkCli call end
IcdLinkCli call shutdown
```

| Exit code | Meaning |
|---|---|
| 0 | Pass |
| 1 | `expect` failed |
| 2 | Bad command |
| 3 | Timeout |
| 4 | Device error |

Long operations from a batch file:

```bat
IcdLinkCli call longop begin D 30000
IcdLinkCli call send ToD.EepromWrite 5000
IcdLinkCli call longop end D
```

Run `IcdLinkCli help` for the full command list.

## 14. Use the test GUI

Run `bin\Release\gui\net48\IcdTestGui.exe`.

1. Pick Simulated, FTDI or COM ports and click **Connect**.
2. Pick the channel and message.
3. Edit values in the table or click bits in the grid.
4. Click **Apply edits** to commit them together, or tick **Apply on edit**.
5. Click **Send once**, or tick **Periodic** for continuous traffic.

The right side shows the last good response. Red outlines mark bits that differ
from what was sent. **Begin long op** pauses periodic traffic on the channel.

## 15. Troubleshooting

| Symptom | Check |
|---|---|
| CRC errors climb and no responses appear | The CRC settings in `project.json`, or baud rate and parity |
| Every frame times out | A reply shorter than the ICD says (see [section 1](#1-add-or-update-an-icd)), device power, wiring |
| Connect says the schedule doesn't fit | Time on the wire is about (send + reply bytes, including CRC) x 11 / baud |
| Warning that TXDEN is off | Turn on RI-as-TXDEN for channel C in the FT4232H EEPROM, using FT_Prog |
| Replies have your own bytes first | Set `"rs485_echo": true` for that channel |
| Interface already open or not found | Close other programs, and confirm the D2XX driver is installed |
| `DllNotFoundException` for `IcdLink.dll` | Copy it beside the exe, and build the app as x64 |
| Values never reach the device | Periodic isn't running and nothing called `Send` |
