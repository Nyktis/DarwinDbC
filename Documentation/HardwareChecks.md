# Hardware checks

Run these commands on the target Mac. Start with initialization, then check
ordinary USB use and sleep/wake behavior. Keep an independently bootable
configuration available and save your work before the first target boot.

## Initialization without a receiver

Load `DarwinDbC.kext` after Lilu and select a controller with
`dbc-pci=BB:DD.F`. All fields are hexadecimal. Use `-dbcoff` to disable the
module when returning to the previous configuration.

After reaching the desktop:

```sh
ioreg -r -c DarwinDbCService -l -w 0
```

Check these properties with no receiver attached:

- `Hardware.PCIAddress` matches the selected controller.
- `Hardware.DMABase` meets `DMAAlignment`; `DMALength=49152`.
- `ConfigurationVerified=Yes` and `RegistersValid=Yes`.
- `ConsoleRouting` reports `pending`, `ready` or `failed` independently.
- `TransportState=waiting`; DCCTRL has DCE set and DCR clear.
- `BytesRead`, `BytesWritten` and `ConfirmedBytes` are zero.
  `QueuedBytes` can increase as startup output is captured.
- `InvalidEvents`, `ControllerResets`, `Disconnects` and
  `ConfigurationErrors` remain zero during stable waiting.

`TransportStatus` can initially report `timeout`, then `disconnected` as
polling waits for host configuration. Use `TransportState` to distinguish
waiting from a transport fault. Initial enable can increment `Timeouts`.

`ConfigurationVerified` checks the DMA allocation and device identity before
setting DCE: DCID, DCERSTSZ, DCERSTBA, DCERDP, DCCP, DCDDI1 and DCDDI2.
`Registers` contains all eleven DbC register values. Status sampling reads
registers; the event path acknowledges port change bits.

## Ordinary USB use

Save a starting sample:

```sh
ioreg -a -r -c DarwinDbCService > "$HOME/Desktop/dbc-before.plist"
```

Use the keyboard, mouse and storage devices on the same controller, then leave
the system idle for about ten minutes. Check input response, storage I/O, CPU
use and kernel memory. `SampleTimeMicroseconds` should advance while transport
state and error counters remain stable.

## Sleep and wake

Sleep and wake the system three times. After each wake, wait about two seconds
and inspect the service:

- `PowerTransitions` increases with controller power notifications.
- Without a receiver, state returns to `waiting`.
- `ConfigurationVerified` and `RegistersValid` return to `Yes`.
- `Hardware.DMABase` stays unchanged during the service's lifetime.
- Re-enable can increment `Timeouts`; `InvalidEvents` should remain zero.
- Inspect `LastError` and its timestamp for resets or preparation failures.

While power is unavailable, state is `suspended`, `RegistersValid=No` and the
register sample is removed. Closing confirms DCE is clear before releasing DMA.

Save the final sample:

```sh
ioreg -a -r -c DarwinDbCService > "$HOME/Desktop/dbc-after.plist"
```

## Locate a startup failure

If the transport service is missing:

```sh
ioreg -r -c AppleUSBXHCI -l -w 0
```

The selected provider's `DarwinDbCStart` contains:

- `Stage`: `claim`, `platform`, `controller`, `console`, `work loop`, `enable` or `ready`.
- `Status`: the stage result; `ready/success` marks completed startup.
- `DMAState`: `mapped` after startup, `released` after cleanup, or `retained`
  when shutdown confirmation failed.
- `Hardware` and `Registers`: information available before the failure.

The `platform` stage covers PCI, BAR, capability and DMA mapping checks.
`enable/configuration error` identifies a register readback mismatch.
For `DMAState=retained`, return to the previous boot configuration before
continuing investigation.

`DarwinDbCProbe` records `Selected`, `BootstrapReady` and `ConsoleRouting` on
each discovered controller. Inspect the separate bootstrap service with:

```sh
ioreg -r -c DarwinDbC -l -w 0
```

`ControllerNotification=Yes` confirms registration; `ControllersSeen` counts
notifications and `TransportAttached=Yes` marks creation of the transport
service. Bootstrap and transport use separate matching categories. Discovery
uses an `IOProviderClass` notification for existing and subsequent controllers.

If provider records are absent, check Lilu, the PCI selector and kernel output:

```sh
log show --last 1h --style compact \
    --predicate 'process == "kernel" AND eventMessage CONTAINS "DarwinDbC"'
```

## Capture over the debug cable

Start the receiver and connect the cable. Check for `TransportState=running`,
increasing `BytesWritten` and `ConfirmedBytes`, and a draining `QueuedBytes`.
Inspect `xnu.msgbuf.begin/end` for historical output and `queue.overflow` for
capacity loss. Disconnect and reconnect; the receiver should continue the same
capture file and pending output should resume.

Run panic checks separately using the explicit manual command in the README.
Check the first reason, backtrace, takeover marker and final counters.

## References

- [xHCI 1.2c](https://cdrdv2.intel.com/v1/dl/getContent/868295): sections 7.6.4.1 and 7.6.8.
- [XNU IOServicePM.cpp](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/iokit/Kernel/IOServicePM.cpp): power notifications and acknowledgement ordering.
- [IOKit matching](https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/Matching/Matching.html): matching categories and IOResources providers.
