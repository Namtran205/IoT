using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Windows.Forms;

namespace SmartClassroom
{
    public sealed class CameraUsbWindow : Form
    {
        private const int BaudRate = 115200;
        private const int MaxJpegBytes = 24576;
        private readonly ComboBox ports = new ComboBox { Width = 110, DropDownStyle = ComboBoxStyle.DropDownList };
        private readonly Button refreshButton = NewButton("Làm mới COM");
        private readonly Button connectButton = NewButton("Kết nối");
        private readonly Button checkinButton = NewButton("Điểm danh");
        private readonly Button enrollButton = NewButton("Đăng ký mặt");
        private readonly PictureBox preview = new PictureBox { Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.Zoom };
        private readonly Label status = new Label { Dock = DockStyle.Top, Height = 72, ForeColor = Color.FromArgb(74, 90, 102) };
        private readonly TextBox activity = new TextBox
        {
            Dock = DockStyle.Fill, ReadOnly = true, Multiline = true, ScrollBars = ScrollBars.Vertical,
            BackColor = Color.White, BorderStyle = BorderStyle.FixedSingle
        };
        private readonly Timer serialTimer = new Timer { Interval = 40 };
        private readonly StringBuilder jpeg = new StringBuilder();
        private readonly StringBuilder serialLine = new StringBuilder();
        private SerialPort port;
        private int jpegBytes;
        private bool lineTooLong;
        private bool waitingForFrame;
        private long nextFrameAt;
        private readonly System.Diagnostics.Stopwatch time = System.Diagnostics.Stopwatch.StartNew();

        public CameraUsbWindow()
        {
            Text = "Camera USB DevKit";
            Font = new Font("Segoe UI", 10);
            ClientSize = new Size(1000, 610);
            MinimumSize = new Size(740, 460);
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Color.FromArgb(241, 245, 248);

            var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(16), RowCount = 2, ColumnCount = 1 };
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            var toolbar = NewBar();
            toolbar.Controls.Add(new Label { Text = "Cổng DevKit", AutoSize = true, Margin = new Padding(0, 8, 8, 0) });
            toolbar.Controls.AddRange(new Control[] { ports, refreshButton, connectButton });

            var body = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Margin = new Padding(0, 12, 0, 0) };
            body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 65));
            body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 35));
            preview.BackColor = Color.FromArgb(21, 46, 59);
            preview.Margin = new Padding(0, 0, 12, 0);
            preview.Paint += PaintPreview;
            var sidebar = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 4 };
            sidebar.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            sidebar.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            sidebar.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            sidebar.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            var actions = NewBar();
            actions.Controls.AddRange(new Control[] { checkinButton, enrollButton });
            sidebar.Controls.Add(status, 0, 0);
            sidebar.Controls.Add(actions, 0, 1);
            sidebar.Controls.Add(new Label
            {
                Text = "Hướng dẫn và kết quả", AutoSize = true, Margin = new Padding(0, 12, 0, 7)
            }, 0, 2);
            sidebar.Controls.Add(activity, 0, 3);
            body.Controls.Add(preview, 0, 0);
            body.Controls.Add(sidebar, 1, 0);
            layout.Controls.Add(toolbar, 0, 0);
            layout.Controls.Add(body, 0, 1);
            Controls.Add(layout);

            refreshButton.Click += (sender, args) => RefreshPorts();
            connectButton.Click += (sender, args) => ToggleConnection();
            checkinButton.Click += (sender, args) => Send("CHECKIN");
            enrollButton.Click += (sender, args) => Send("ENROLL");
            ports.SelectedIndexChanged += (sender, args) => RenderConnection();
            serialTimer.Tick += (sender, args) => PollSerial();
            FormClosed += (sender, args) => Disconnect();
            status.Text = "Cắm DevKit và chọn cổng COM. ESP32-CAM có thể dùng nguồn ngoài.";
            RefreshPorts();
        }

        public static void Run()
        {
            Application.EnableVisualStyles();
            Application.Run(new CameraUsbWindow());
        }

        private static Button NewButton(string text)
        {
            return new Button { Text = text, AutoSize = true, Padding = new Padding(7, 3, 7, 3), Margin = new Padding(3, 0, 3, 4) };
        }

        private static FlowLayoutPanel NewBar()
        {
            return new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Top, Margin = Padding.Empty };
        }

        private void RefreshPorts()
        {
            string previous = ports.SelectedItem as string;
            var names = SerialPort.GetPortNames();
            Array.Sort(names, StringComparer.OrdinalIgnoreCase);
            ports.Items.Clear();
            ports.Items.AddRange(names);
            if (previous != null && ports.Items.Contains(previous)) ports.SelectedItem = previous;
            else if (names.Length == 1) ports.SelectedIndex = 0;
            RenderConnection();
        }

        private void RenderConnection()
        {
            bool connected = port != null;
            ports.Enabled = refreshButton.Enabled = !connected;
            connectButton.Text = connected ? "Ngắt USB" : "Kết nối";
            connectButton.Enabled = connected || ports.SelectedItem != null;
            checkinButton.Enabled = enrollButton.Enabled = connected;
        }

        private void ToggleConnection()
        {
            if (port != null)
            {
                Disconnect();
                status.Text = "Đã ngắt USB.";
                return;
            }
            string selected = ports.SelectedItem as string;
            if (selected == null) return;

            var selectedPort = new SerialPort(selected, BaudRate) { ReadBufferSize = 65536, WriteTimeout = 250 };
            try
            {
                selectedPort.Open();
                port = selectedPort;
                activity.Clear();
                status.Text = "Đã mở " + selected + ". Đang chờ ảnh từ camera.";
                nextFrameAt = time.ElapsedMilliseconds + 500;
                RenderConnection();
                serialTimer.Start();
            }
            catch (UnauthorizedAccessException)
            {
                selectedPort.Dispose();
                status.Text = "Cổng COM đang bận. Đóng Arduino Serial Monitor hoặc mục xem USB trên web rồi thử lại.";
            }
            catch (IOException)
            {
                selectedPort.Dispose();
                status.Text = "Không mở được COM. Cắm lại DevKit và bấm Làm mới COM.";
            }
        }

        private void Disconnect()
        {
            serialTimer.Stop();
            if (port != null)
            {
                try { port.Dispose(); }
                catch (IOException) { }
            }
            port = null;
            serialLine.Clear();
            jpeg.Clear();
            jpegBytes = 0;
            lineTooLong = waitingForFrame = false;
            SetImage(null);
            RenderConnection();
        }

        private void Send(string command)
        {
            try
            {
                port.Write(command + "\n");
            }
            catch (IOException) { ReportDisconnected(); }
            catch (InvalidOperationException) { ReportDisconnected(); }
            catch (TimeoutException) { ReportDisconnected(); }
        }

        private void PollSerial()
        {
            if (port == null) return;
            try
            {
                ProcessSerialText(port.ReadExisting());
                if (time.ElapsedMilliseconds < nextFrameAt) return;
                if (waitingForFrame) status.Text = "Chưa nhận được ảnh. Kiểm tra UART và firmware của hai bo.";
                jpegBytes = 0;
                jpeg.Clear();
                waitingForFrame = true;
                nextFrameAt = time.ElapsedMilliseconds + 9500;
                Send("FRAME");
            }
            catch (IOException) { ReportDisconnected(); }
            catch (InvalidOperationException) { ReportDisconnected(); }
        }

        private void ReportDisconnected()
        {
            Disconnect();
            status.Text = "Mất kết nối USB. Kiểm tra cáp DevKit rồi kết nối lại.";
        }

        private void ProcessSerialText(string input)
        {
            foreach (char character in input)
            {
                if (character == '\n')
                {
                    if (!lineTooLong && serialLine.Length > 0) ProcessLine(serialLine.ToString());
                    serialLine.Clear();
                    lineTooLong = false;
                }
                else if (character != '\r' && !lineTooLong)
                {
                    if (serialLine.Length < 256) serialLine.Append(character);
                    else lineTooLong = true;
                }
            }
        }

        private void ProcessLine(string line)
        {
            if (line.StartsWith("FRAME_BEGIN:", StringComparison.Ordinal))
            {
                if (!int.TryParse(line.Substring(12), out jpegBytes) || jpegBytes < 1 || jpegBytes > MaxJpegBytes)
                    jpegBytes = 0;
                jpeg.Clear();
            }
            else if (line.StartsWith("FRAME_DATA:", StringComparison.Ordinal))
            {
                if (jpegBytes > 0 && jpeg.Length + line.Length - 11 <= 4 * ((jpegBytes + 2) / 3))
                    jpeg.Append(line, 11, line.Length - 11);
                else jpegBytes = 0;
            }
            else if (line == "FRAME_END")
            {
                waitingForFrame = false;
                nextFrameAt = time.ElapsedMilliseconds + 400;
                ShowJpeg();
                jpeg.Clear();
                jpegBytes = 0;
            }
            else if (line.StartsWith("FRAME_ERROR:", StringComparison.Ordinal)
                || line.StartsWith("FRAME: camera UART offline", StringComparison.Ordinal))
            {
                waitingForFrame = false;
                jpeg.Clear();
                jpegBytes = 0;
                nextFrameAt = time.ElapsedMilliseconds + 2000;
                status.Text = line.StartsWith("FRAME:", StringComparison.Ordinal)
                    ? "DevKit đang chờ ESP32-CAM qua UART."
                    : "Camera chưa trả được ảnh. Đang thử lại.";
            }
            else if (line.StartsWith("CHECKIN", StringComparison.Ordinal)
                || line.StartsWith("ENROLL", StringComparison.Ordinal)
                || line.StartsWith("CAMERA", StringComparison.Ordinal)
                || line.StartsWith("1/3 ", StringComparison.Ordinal)
                || line.StartsWith("2/3 ", StringComparison.Ordinal)
                || line.StartsWith("3/3 ", StringComparison.Ordinal))
            {
                if (activity.TextLength > 5000) activity.Clear();
                activity.AppendText(line + Environment.NewLine);
                activity.ScrollToCaret();
            }
        }

        private void ShowJpeg()
        {
            try
            {
                byte[] bytes = Convert.FromBase64String(jpeg.ToString());
                if (jpegBytes < 4 || bytes.Length != jpegBytes || bytes[0] != 0xff || bytes[1] != 0xd8)
                {
                    ShowFrameError();
                    return;
                }
                using (var stream = new MemoryStream(bytes))
                using (var picture = Image.FromStream(stream, false, true))
                    SetImage(new Bitmap(picture));
                status.Text = "Đang xem ảnh qua USB. Căn mặt trong khung nét đứt rồi bấm nút DevKit hoặc Điểm danh.";
            }
            catch (FormatException) { ShowFrameError(); }
            catch (ArgumentException) { ShowFrameError(); }
        }

        private void ShowFrameError()
        {
            status.Text = "Ảnh USB bị thiếu dữ liệu. Đang lấy khung tiếp theo.";
        }

        private void SetImage(Image next)
        {
            Image previous = preview.Image;
            preview.Image = next;
            if (previous != null) previous.Dispose();
            preview.Invalidate();
        }

        private void PaintPreview(object sender, PaintEventArgs args)
        {
            if (preview.Image == null)
            {
                TextRenderer.DrawText(args.Graphics, "Chờ ảnh ESP32-CAM", Font, preview.ClientRectangle,
                    Color.White, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                return;
            }
            float scale = Math.Min((float)preview.Width / preview.Image.Width, (float)preview.Height / preview.Image.Height);
            float guideWidth = preview.Image.Width * scale * 0.58f;
            float guideHeight = preview.Image.Height * scale * 0.84f;
            args.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            using (var pen = new Pen(Color.FromArgb(210, Color.White), 2) { DashStyle = DashStyle.Dash })
                args.Graphics.DrawEllipse(pen, (preview.Width - guideWidth) / 2, (preview.Height - guideHeight) / 2, guideWidth, guideHeight);
        }
    }
}
