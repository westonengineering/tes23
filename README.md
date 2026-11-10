# IcdLink

FT4232H (D2XX) and COM-port link with deterministic periodic traffic, generated
C++/C# message code and a generated test GUI. Builds with MSVC, x64.

Recipes for every common task, including EEPROM pauses, bit tests, MVVM binding and
fault handling, are in [docs/HOWTO.md](docs/HOWTO.md).

## Requirements

- Visual Studio 2022 or newer with the C++ desktop and .NET desktop workloads
- .NET Framework 4.8 targeting pack and the .NET 10 SDK
- Python 2.7 or 3.x on the PATH (standard library only)
- The FTDI D2XX driver to talk to real hardware. The simulator does not need it.

## Build

    build.bat Release

This regenerates from `icd\project.json`, then builds `bin\Release\IcdLink.dll`,
`IcdLinkCli.exe`, the tests and the GUI. `IcdLink.sln` opens in Visual Studio.
Run `build.bat` after any ICD change: the C# side refuses to load a DLL built
from a different ICD.

## Describing an ICD

`icd\project.json` lists the sources and holds the protocol settings (baud,
period, window, CRC, byte order). A source can be:

| Source | Notes |
|---|---|
| `.docx` | Every table with Byte, Bits and Desc columns is a message. |
| `.txt` / `.tsv` | The table selected in Word and pasted into Notepad. Use it if a Word file parses badly. |
| `.json` | Written by hand. See `icd\DemoChannelD.json`. |
| `"tx"` + `"rx"` files | Separate send and receive ICDs whose layouts, lengths and IDs differ. See `docs\HOWTO.md`. |

Word and text sources give the message ID, name and channel in the paragraph
above the table (`Message 0x90 Status (Channel C)`) or in the project entry:

    { "file": "icd_export.txt", "channel": "C", "id": "0x90", "name": "FakeIcd" }

Optional table columns: Type, Scale, Offset, Unit, Default, Dir, Enum.
To drop fields from the response, use a Dir column with `to`, or add
`"omit_from": ["Test3"]` to the source entry. The response ends at the last
returned field, and fields keep the same byte positions in both directions.

`icd\DemoChannelD.json` is an invented placeholder. Replace it with the real
channel D ICD.

Frame on the wire: message type, payload, CRC-32, optional 0xFF.

## C#

Reference `IcdLink.Net` and put `IcdLink.dll` next to the program.

    var device = new IcdDevice();
    device.OpenFtdi(IcdChannel.C);
    device.OpenFtdi(IcdChannel.D);
    device.StartCyclic(IcdChannel.C);

    device.Atomic(() =>
    {
        device.ToC.FakeIcd.Power = true;       // takes effect together with Test1
        device.ToC.FakeIcd.Test1 = 3;
        device.Sync();                         // commit and wait for the device's answer
        if (device.FromC.FakeIcd.Test1 == 3)   // reading of that answer, frozen
            device.ToC.FakeIcd.Test2 = 1;
    });                                        // commits the rest

Inside `Atomic`, no other thread can change To values, periodic traffic keeps
sending the last committed values, and From values only move at `Sync` or
`Refresh`. A single property set outside a block is its own small block.
An exception discards writes that have not been committed. Blocks can wait
for the device, so run long ones with `Task.Run` rather than on the UI thread.

Long device work such as EEPROM access:

    using (device.BeginLongOperation(IcdChannel.D))
    {
        device.ToD.EepromWrite.Address = 0x10;
        device.ToD.EepromWrite.Send(5000);
    }   // periodic traffic resumes here, or after the watchdog if the program hangs

Binding: every message implements `INotifyPropertyChanged`, so MVVM and the
WinForms designer can bind to `device.ToC.FakeIcd`. Call
`device.StartNotifications()` from the UI thread.

## Command line

    IcdLinkCli help
    IcdLinkCli open sim then cyclic C on then set ToC.FakeIcd.Test1 7 then sync then expect FromC.FakeIcd.Test1 == 7

Each run of `IcdLinkCli` closes the link when it exits. For a .bat file with
several steps, start `IcdLinkCli serve` once and send steps with `IcdLinkCli call`,
as in `tests\demo_test.bat`. Exit codes: 0 pass, 1 expect failed, 2 bad command,
3 timeout, 4 device error.

## Test GUI

`bin\Release\gui\net48\IcdTestGui.exe`. Connect to the simulator, FTDI or COM
ports, pick a channel and message, edit values or click bits, then Apply or Send.
Red outlines show bits that came back different from what was sent.

## Tests

    bin\Release\IcdLinkTests.exe
    bin\Release\tests\net48\IcdLink.Tests.exe
    tests\demo_test.bat
    tests\run_matrix.bat

`run_matrix.bat` covers channels C and D, periodic and manual sending, with and
without half-duplex echo, on the normal ICD and on a fake ICD that sends 375
bytes and gets 275 back. `tests\check_wire.py` then checks the raw frames using
only Python's own CRC.

These run against the simulated device in `native\SimDevice.cpp`.

## Hardware notes

Channel C needs the FT4232H EEPROM to have RI-as-TXDEN turned on for RS-485. The
program warns at connect if it is off. If the RS-485 receiver stays enabled while
transmitting, set `"rs485_echo": true` for that channel.
