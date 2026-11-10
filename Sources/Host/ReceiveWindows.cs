// SPDX-License-Identifier: BSD-2-Clause
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

internal static class ReceiveWindows
{
    private const int Success = 0;
    private const int NoDevice = -4;
    private const int TimedOut = -7;
    private const int PipeStalled = -9;
    private const byte InputEndpoint = 0x81;
    private static volatile bool stopping;

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    private struct DeviceDescriptor
    {
        public byte length, type;
        public ushort usbVersion;
        public byte deviceClass, subclass, protocol, packetSize;
        public ushort vendor, product, revision;
        public byte manufacturer, productString, serial, configurations;
    }

    private static class USB
    {
        private const string Library = "libusb-1.0.dll";

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int libusb_init(out IntPtr context);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void libusb_exit(IntPtr context);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr libusb_get_device_list(IntPtr context, out IntPtr devices);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void libusb_free_device_list(IntPtr devices, int unref);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int libusb_get_device_descriptor(IntPtr device, out DeviceDescriptor descriptor);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int libusb_open(IntPtr device, out IntPtr handle);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void libusb_close(IntPtr handle);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int libusb_get_configuration(IntPtr handle, out int configuration);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int libusb_set_configuration(IntPtr handle, int configuration);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int libusb_claim_interface(IntPtr handle, int number);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int libusb_release_interface(IntPtr handle, int number);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int libusb_bulk_transfer(IntPtr handle, byte endpoint,
            [Out] byte[] bytes, int length, out int transferred, uint timeout);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int libusb_clear_halt(IntPtr handle, byte endpoint);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern IntPtr libusb_error_name(int error);
    }

    private static void Report(string operation, int error)
    {
        Console.Error.WriteLine("{0:O} {1}: {2} ({3})", DateTime.Now, operation,
            Marshal.PtrToStringAnsi(USB.libusb_error_name(error)), error);
    }

    private static int OpenDevice(IntPtr context, bool linux, out IntPtr handle)
    {
        handle = IntPtr.Zero;
        IntPtr devices;
        long count = USB.libusb_get_device_list(context, out devices).ToInt64();
        if (count < 0)
            return (int)count;

        try
        {
            for (long index = 0; index < count; ++index)
            {
                IntPtr device = Marshal.ReadIntPtr(devices, checked((int)(index * IntPtr.Size)));
                DeviceDescriptor descriptor;
                if (USB.libusb_get_device_descriptor(device, out descriptor) != Success)
                    continue;
                ushort vendor = linux ? (ushort)0x1D6B : (ushort)0xFFFF;
                ushort product = linux ? (ushort)0x0010 : (ushort)0x0001;
                if (descriptor.vendor == vendor && descriptor.product == product)
                    return USB.libusb_open(device, out handle);
            }
            return NoDevice;
        }
        finally
        {
            USB.libusb_free_device_list(devices, 1);
        }
    }

    private static int Capture(IntPtr context, bool linux, FileStream output)
    {
        Stream terminal = Console.OpenStandardOutput();
        long received = 0;
        int lastError = Success;
        byte[] bytes = new byte[1024];
        while (!stopping)
        {
            IntPtr handle;
            int result = OpenDevice(context, linux, out handle);
            if (result != Success)
            {
                if (result != NoDevice && result != lastError)
                    Report("Open debug device; check its WinUSB binding", result);
                lastError = result;
                Thread.Sleep(200);
                continue;
            }

            bool claimed = false;
            try
            {
                int configuration;
                result = USB.libusb_get_configuration(handle, out configuration);
                if (result == Success && configuration == 0)
                    result = USB.libusb_set_configuration(handle, 1);
                if (result == Success)
                    result = USB.libusb_claim_interface(handle, 0);
                if (result != Success)
                {
                    if (result != lastError)
                        Report("Configure debug interface", result);
                    lastError = result;
                    Thread.Sleep(500);
                    continue;
                }
                claimed = true;
                lastError = Success;
                Console.Error.WriteLine("{0:O} Connected; receiving console bytes.", DateTime.Now);

                while (!stopping)
                {
                    int transferred;
                    // DbC exposes Bulk IN at 0x81 with 1024-byte packets. Reading one
                    // packet also completes writes whose length is a packet multiple.
                    result = USB.libusb_bulk_transfer(handle, InputEndpoint, bytes,
                        bytes.Length, out transferred, 500);
                    // libusb reports completed bytes when a transfer times out.
                    if ((result == Success || result == TimedOut) && transferred > 0)
                    {
                        output.Write(bytes, 0, transferred);
                        output.Flush();
                        terminal.Write(bytes, 0, transferred);
                        terminal.Flush();
                        received += transferred;
                    }
                    if (result == Success || result == TimedOut)
                        continue;
                    if (result == PipeStalled && USB.libusb_clear_halt(handle, InputEndpoint) == Success)
                        continue;
                    Report("USB input ended; waiting for reconnection", result);
                    break;
                }
            }
            finally
            {
                if (claimed)
                    USB.libusb_release_interface(handle, 0);
                USB.libusb_close(handle);
            }
        }
        Console.Error.WriteLine("Saved {0} bytes.", received);
        return 0;
    }

    public static int Main(string[] arguments)
    {
        bool linux = false;
        string path = "DarwinDbC-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".log";
        for (int index = 0; index < arguments.Length; ++index)
        {
            if (arguments[index] == "--linux")
                linux = true;
            else if (arguments[index] == "--output" && index + 1 < arguments.Length)
                path = arguments[++index];
            else
            {
                Console.Error.WriteLine("Usage: darwindbc-receive.exe [--output capture.log] [--linux]");
                return arguments[index] == "--help" ? 0 : 2;
            }
        }

        Console.OutputEncoding = new UTF8Encoding(false);
        Console.CancelKeyPress += delegate(object sender, ConsoleCancelEventArgs eventArguments)
        {
            eventArguments.Cancel = true;
            stopping = true;
        };

        try
        {
            using (FileStream output = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read))
            {
                IntPtr context;
                int result = USB.libusb_init(out context);
                if (result != Success)
                {
                    Report("Initialize libusb", result);
                    return 1;
                }
                try
                {
                    Console.Error.WriteLine("Capture: {0}", Path.GetFullPath(path));
                    Console.Error.WriteLine("Waiting for {0}; Ctrl-C stops.",
                        linux ? "Linux USB Debug Target (1d6b:0010)" : "Kernel Console (ffff:0001)");
                    return Capture(context, linux, output);
                }
                finally
                {
                    USB.libusb_exit(context);
                }
            }
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error.Message);
            return 1;
        }
    }
}
