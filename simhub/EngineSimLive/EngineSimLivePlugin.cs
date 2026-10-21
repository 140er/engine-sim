using System;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using GameReaderCommon;
using SimHub.Plugins;

namespace EngineSimLive
{
    [PluginDescription("Sends RPM, throttle, gear, speed, and engine load to engine-sim-live")]
    [PluginAuthor("engine-sim-live")]
    [PluginName("Engine Sim Live")]
    public class EngineSimLivePlugin : IPlugin, IDataPlugin
    {
        public PluginManager PluginManager { get; set; }

        public System.Windows.Media.ImageSource PictureIcon => null;

        public string LeftMenuTitle => "Engine Sim Live";

        private UdpClient socket;
        private IPEndPoint destination;

        public void Init(PluginManager pluginManager)
        {
            PluginManager = pluginManager;
            socket = new UdpClient();
            destination = new IPEndPoint(IPAddress.Loopback, 21377);
        }

        public void DataUpdate(PluginManager pluginManager, ref GameData data)
        {
            if (data.NewData == null || socket == null) return;

            var packet = new byte[24];
            packet[0] = (byte)'E';
            packet[1] = (byte)'S';
            packet[2] = (byte)'T';
            packet[3] = (byte)'1';
            packet[4] = 1;

            double rpm = data.NewData.Rpms;
            byte flags = 0;
            if (rpm > 1.0) flags |= 1;

            double load = 0;
            if (TryDouble(data.NewData, "EngineLoad", out load))
            {
                if (load > 1.0) load /= 100.0;
                if (load < 0) load = 0;
                if (load > 1) load = 1;
                flags |= 2;
            }
            packet[5] = flags;

            short gear = GearNumber(data.NewData.Gear);
            packet[6] = (byte)(gear & 0xff);
            packet[7] = (byte)((gear >> 8) & 0xff);

            double throttle = data.NewData.Throttle;
            if (throttle > 1.0) throttle /= 100.0;
            if (throttle < 0) throttle = 0;
            if (throttle > 1) throttle = 1;

            WriteFloat(packet, 8, (float)rpm);
            WriteFloat(packet, 12, (float)throttle);
            WriteFloat(packet, 16, (float)load);
            WriteFloat(packet, 20, (float)(data.NewData.SpeedKmh / 3.6));

            socket.Send(packet, packet.Length, destination);
        }

        public void End(PluginManager pluginManager)
        {
            if (socket != null)
            {
                socket.Close();
                socket = null;
            }
        }

        private static short GearNumber(string gear)
        {
            if (string.IsNullOrEmpty(gear)) return 0;
            if (gear == "R" || gear == "r") return -1;
            if (gear == "N" || gear == "n") return 0;
            short value;
            if (short.TryParse(gear, out value)) return value;
            return 0;
        }

        private static bool TryDouble(object target, string name, out double value)
        {
            value = 0;
            PropertyInfo property = target.GetType().GetProperty(name);
            if (property == null) return false;
            object raw = property.GetValue(target, null);
            if (raw == null) return false;
            try
            {
                value = Convert.ToDouble(raw);
                return true;
            }
            catch (InvalidCastException)
            {
                return false;
            }
            catch (FormatException)
            {
                return false;
            }
        }

        private static void WriteFloat(byte[] packet, int offset, float value)
        {
            byte[] bytes = BitConverter.GetBytes(value);
            if (!BitConverter.IsLittleEndian) Array.Reverse(bytes);
            Buffer.BlockCopy(bytes, 0, packet, offset, 4);
        }
    }
}
