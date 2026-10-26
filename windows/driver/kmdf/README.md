# LibrePods KMDF Driver

KMDF Bluetooth profile driver for the LibrePods Windows port. Derived from the
KMDF driver in [ivLis-Studio/librepods-windows](https://github.com/ivLis-Studio/librepods-windows)
(GPLv3).

Windows has no user-mode L2CAP API, so the app cannot open the AirPods AACP
channel from user space. This driver binds to the AirPods' vendor-specific
`BTHENUM` service PDO and performs the outbound L2CAP channel open
(`BRB_L2CA_OPEN_CHANNEL`, PSM `0x1001`) in kernel mode, then forwards ACL data
through `BRB_L2CA_ACL_TRANSFER`. The Qt app talks to it through the shared
IOCTL ABI via `DriverL2CapSocket`.

What it does now:

- Exposes the LibrePods device interface GUID.
- Implements the shared IOCTL ABI from `driver/shared/librepods_driver_protocol.h`.
- Tracks in-memory session ids for AACP clients.
- Splits the actual Bluetooth transport behind `transport.h`.
- Reports `not implemented` from the default transport stub.
- Provides a signed `loopback` transport variant for end-to-end bridge testing.
- Carries a dedicated `BthL2Cap` build mode for the real Windows Bluetooth driver path.
- Submits real Bluetooth BRBs for `BthL2Cap` open, close, send, and read operations.
- Packages `BthL2Cap` as a Bluetooth profile driver that matches the AirPods vendor-specific `BTHENUM` service GUIDs `{4715650b-5e9d-4ac2-b898-a4fc0aa5df78}` and `{74ec2172-0bad-4d01-8f77-997b2be0722a}` instead of a root-enumerated test device.

What it does not do yet:

- Translate AACP packets to the AirPods transport.
- Validate the `BthL2Cap` path on a real AirPods-attached `BTHENUM` device stack.
- Handle long-running notification behavior beyond the current synchronous read path.

Build prerequisites:

1. Visual Studio 2022 (or Build Tools) with C++ desktop tools
2. The WDK is restored automatically from NuGet (`Microsoft.Windows.WDK.x64`
   10.0.26100.6584 + `Microsoft.Windows.SDK.CPP.x64`); no WDK VS extension is
   needed. The `../vctargets/` overlay supplies the
   `WindowsKernelModeDriver10.0` platform toolset MSBuild normally gets from a
   full WDK install.
3. A test-signing PFX at `certs\librepods-test.pfx` (password
   `librepods-test`). Create one in an elevated or normal PowerShell with:

   ```powershell
   New-SelfSignedCertificate -Type Custom -Subject "CN=LibrePods KMDF Test" `
     -KeyUsage DigitalSignature -CertStoreLocation Cert:\CurrentUser\My `
     -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3") `
     -KeyExportPolicy Exportable
   # then export the .pfx/.cer into certs\ (see driver-build.cmd for names)
   ```

   The build signs `librepods-kmdf.sys` and `librepods-kmdf.cat` with
   `signtool` + `inf2cat` directly (the MSBuild `SignTask` auto-certificate
   path requires rights a plain build shell does not have).

Project file:

- `LibrePodsKmdf.vcxproj`

Driver package files:

- `librepods-kmdf-root.inf`
- `librepods-kmdf-bthenum.inf`
- `driver.c`
- `queue.c`
- `driver.h`
- `transport.h`
- `transport_stub.c`

Helper scripts:

- `driver-build.cmd`
  Builds the KMDF project with `msbuild`, then runs `inf2cat` + `signtool`
  Pass `stub`, `loopback`, or `bthl2cap` to choose the transport variant
  Archives signed artifacts under `artifacts\<Mode>\`
- `install-driver.cmd`
  Stages the archived package with `pnputil`
  Uses local `devcon.exe` from the restored WDK package to create or update `Root\LibrePodsKmdf` for `stub` and `loopback`
  Uses `pnputil /install` against the paired AirPods `BTHENUM` service PDO for `bthl2cap`
  Pass `/trustcert` to import the generated test certificate into `Root` and `TrustedPublisher`
  Pass `/enabletestsigning` to enable Windows TESTSIGNING mode
  Automatically requests UAC elevation when needed
- `install-test-driver.cmd`
  Development-only convenience wrapper
  Imports the generated test certificate, enables TESTSIGNING, and installs the selected driver package
  Automatically requests UAC elevation when needed

Test-machine install:

1. Build the target driver variant:
   `driver-build.cmd bthl2cap`
2. Pair the AirPods in Windows and open the charging case once so Windows enumerates the vendor-specific `BTHENUM` service device.
3. Run:
   `install-test-driver.cmd bthl2cap`
3. If the script asks for a reboot, restart Windows and run the command once more.

Release-signing note:

- The current package is test-signed for development only.
- Public distribution of a kernel driver will still require a Microsoft-trusted release-signing path through the Windows Hardware Developer flow.

The driver builds standalone through `driver-build.cmd`; it is not part of the
Qt application build so app builds work on machines without the WDK.
