# DarwinDbC

Capture macOS kernel console output and panic diagnostics on another computer
over a USB 3 debug cable. DarwinDbC uses the xHCI Debug Capability (DbC) and
provides receivers for Windows and macOS.

**0.1.11 alpha** has been tested on Intel xHCI with Darwin 25.6 and a Windows 11
receiver. Hardware checks cover startup capture, queued output across reconnects,
panic reasons and backtraces, and panic takeover while a writer holds the
transport locks. The macOS receiver builds for Intel and Apple Silicon;
hardware reception checks remain pending.

## Connect and capture

Use an xHCI controller with DbC and a USB 3 debug cable connected to USB 3 ports
on both computers. Follow the cable requirements in
[xHCI 1.2c, section 7.6.1](https://cdrdv2.intel.com/v1/dl/getContent/868295).
The target port connected to the debug cable is assigned to DbC while enabled.

Add `DarwinDbC.kext` to the target's OpenCore configuration after Lilu.
Select the controller with `dbc-pci=BB:DD.F`; all three fields are hexadecimal.
For example:

```text
dbc-pci=00:14.0
```

Confirm that the selected PCI device has DbC. Without a selector, only the
bootstrap service runs. Use `-dbcoff` to disable DarwinDbC. Keep a separate
bootable configuration for recovery, and start the receiver before booting
the target.

### Windows receiver

1. Place the x64 `libusb-1.0.dll` beside `darwindbc-receive.exe`.
2. Use [Zadig](https://zadig.akeo.ie/) to bind WinUSB to the debug device.
   Load `KernelConsole.cfg` and confirm **FFFF:0001** before applying the driver.
3. Run `capture.cmd`. Each run creates a new log, status file and console file.
   `current-capture.txt` identifies the current log.

To choose an output file directly:

```bat
darwindbc-receive.exe --output startup.log
```

The receiver waits for the device and reconnects after a disconnect. Press
Ctrl-C to stop. Output files are created exclusively, preserving existing logs.

### macOS receiver

Run `receive.command`, or use the terminal:

```sh
./darwindbc-receive --output startup.log
```

The receiver opens the Bulk IN endpoint of FFFF:0001 and writes raw bytes to
both the terminal and the output file. `receive.command` creates a separate
file under `captures` for each run.

## Check the target

```sh
ioreg -r -c DarwinDbCService -l -w 0
```

A connected target should report `TransportState=running`,
`ConsoleRouting=ready` and `ConfigurationVerified=Yes`. Without a receiver,
the transport waits in `waiting`.

Key properties:

- `BytesWritten`: bytes acknowledged by transport completion events.
- `ConfirmedBytes`: acknowledged bytes from the ordinary console queue.
- `QueuedBytes`: ordinary console bytes awaiting acknowledgement.
- `UnconfirmedBytes`: cumulative bytes whose submitted transfers timed out.
  Late completions can subsequently increase `ConfirmedBytes`.
- `DiscardedBytes`: input bytes rejected by the ordinary queue.
- `HistoryBytes`: historical bytes captured from the XNU message buffer.
- `PanicReady`: panic and console routes are ready.
- `PanicBufferReady`: the panic buffer reading interface is available.

`Hardware` describes PCI, BAR and DMA mappings. `Registers` contains the current
DbC register sample and is removed while power is unavailable. `LastError`, its
timestamp and register values help locate resets and disconnects. See
[hardware checks](Documentation/HardwareChecks.md) for startup and power checks.

## Output and completion

DarwinDbC routes `console_write` and `console_write_unbuffered` while preserving
the original console calls. A preallocated queue retains disconnected and
concurrent output, then sends it in reservation order. Timed-out transfers keep
their pending data. Queue overflow emits `queue.overflow` with cumulative loss
and the affected input sequence window. Each ordinary console call has a 1 ms
transport wait budget; timer polling continues transmission.

An XNU message-buffer snapshot supplies retained startup history, framed by
`xnu.msgbuf.begin/end`. History and live output are distinct records and can
overlap. The historical range depends on the content still retained by XNU.

Panic capture uses a separate resident 2 MiB buffer, reads existing diagnostics
and captures subsequent output. Once other CPUs have stopped, takeover confirms
that DbC DMA has stopped before reclaiming abandoned locks and transfer ownership.
Short completions preserve the remaining suffix. Final synchronous polling has
bounded time and iteration budgets. `panic.end` reports `captured`, `confirmed`,
`remaining` and `discarded` immediately before that marker is generated.

After panic diagnostics, DarwinDbC completes USB output before the interactive
KDP network wait and remains in a local terminal loop. Ordinary debugger and
stackshot entries retain their existing flow. Interactive KDP bridging is a
future feature.

## Optional held-writer panic check

Save your work and keep the receiver recording. Confirm `QueuedBytes=0` and
`PanicReady=Yes` on the target. This command interrupts the target system after
a ten-second countdown and requires a manual restart:

```sh
sudo ./darwindbc-panic-held --trigger
```

The tool asks the driver to submit test data and trigger a panic while the
writer holds the transport locks. Check for the first panic reason, backtrace,
`panic.transport takeovers=1` and `panic.end`. If the request returns Busy or
NotReady, let ordinary output drain before retrying.

The tested configuration includes VirtualSMC handling for
`IOPlatformPanicAction` diagnostic actions 10 and 11. VirtualSMC 1.3.8 enters an
AppleSMC MMIO path for these actions and can cause a nested fault; apply the
corresponding action handling before using that version for panic checks.

## Build

The kext requires Xcode Command Line Tools, MacKernelSDK and the Lilu 1.7.2
DEBUG development bundle:

```sh
make kext \
    KERNEL_SDK=/path/to/MacKernelSDK \
    LILU_RESOURCES=/path/to/Lilu.kext/Contents/Resources
make receiver
make panic-held
```

`Build/DarwinDbC.kext` targets x86_64 with a macOS 13 minimum deployment version.
The macOS receiver contains arm64 and x86_64 code; the manual panic tool is
x86_64. Builds check the property list and apply an ad-hoc signature. Set
`BUILD` to a new directory when changing dependencies.

Build the Windows receiver with the .NET Framework x64 C# compiler:

```bat
mkdir Build
"%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:exe /platform:x64 /optimize+ /out:Build\darwindbc-receive.exe Sources\Host\ReceiveWindows.cs
```

Run transport and console checks on Linux:

```sh
make check
make check-format
```

Checks cover ring wrap and cycle bits, short and late completions, reset and
reconnect handling, queue contention and overflow, panic buffer boundaries,
nested faults and held-writer takeover.

## License and references

DarwinDbC uses BSD-2-Clause. The Lilu entry component uses BSD-3-Clause; see
[third-party notices](Documentation/ThirdPartyNotices.txt). The kext includes
the APSL-2.0 text for MacKernelSDK's `libkmod`. The Windows receiver dynamically
loads [libusb](https://github.com/libusb/libusb), licensed under LGPL-2.1-or-later.

- [Intel xHCI 1.2c](https://cdrdv2.intel.com/v1/dl/getContent/868295): section 7.6, DbC registers, rings and ownership.
- [EDK II USB3 debug library](https://github.com/tianocore/edk2/tree/37b18eca7958215fcf81b297e313adf60c0a70cb/SourceLevelDebugPkg/Library/DebugCommunicationLibUsb3): initialization and transfer behavior.
- [Apple XNU](https://github.com/apple-oss-distributions/xnu): console, message buffers, panic handling and IOKit DMA lifecycle.
- [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) and [Lilu](https://github.com/acidanthera/Lilu): build dependencies and kernel routing.
