// MainForm.cs - ICD test GUI. It is driven entirely by the generated schema, so a
// new ICD needs no changes here.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;
using IcdLink;
using IcdLink.Generated;

namespace IcdTestGui
{
    internal sealed class MainForm : Form
    {
        private const int ValueColumn = 4;

        private readonly IcdDevice _dev = new IcdDevice();
        private readonly string _screenshotPath;
        private readonly byte[][] _pending;
        private readonly Dictionary<int, IDisposable> _longOps = new Dictionary<int, IDisposable>();
        private readonly Timer _timer = new Timer { Interval = 100 };

        private readonly ComboBox _transport = Combo(110, "Simulated", "FTDI", "COM ports");
        private readonly ComboBox _chip = Combo(170);
        private readonly ComboBox _comC = Combo(80), _comD = Combo(80);
        private readonly TextBox _baudC = Box(70), _baudD = Box(70);
        private readonly Button _connect = Btn("Connect"), _disconnect = Btn("Disconnect"), _rescan = Btn("Rescan");
        private readonly Label _state = Lbl("Disconnected");

        private readonly ComboBox _channel = Combo(50, "C", "D");
        private readonly ComboBox _message = Combo(180);
        private readonly Button _send = Btn("Send once"), _apply = Btn("Apply edits"), _revert = Btn("Revert edits");
        private readonly CheckBox _autoApply = new CheckBox { Text = "Apply on edit", AutoSize = true, Margin = new Padding(8, 6, 3, 3) };
        private readonly CheckBox _cyclic = new CheckBox { Text = "Periodic", AutoSize = true, Margin = new Padding(16, 6, 3, 3) };
        private readonly CheckBox _msgCyclic = new CheckBox { Text = "This message in period", AutoSize = true, Margin = new Padding(8, 6, 3, 3) };
        private readonly TextBox _period = Box(70);
        private readonly Button _setPeriod = Btn("Set us");
        private readonly Button _longOp = Btn("Begin long op");

        private readonly DataGridView _toGrid = FieldGrid(false), _fromGrid = FieldGrid(true);
        private readonly BitGrid _toBits = new BitGrid(), _fromBits = new BitGrid();
        private readonly TextBox _toFrame = FrameBox(), _fromFrame = FrameBox();
        private readonly Label _rxInfo = Lbl("");
        private readonly Label _stats = Mono();
        private readonly ListBox _log = new ListBox { Dock = DockStyle.Fill, HorizontalScrollbar = true, IntegralHeight = false };

        private bool _loading;
        private byte[] _shownRx;
        private ulong _shownSeq = ulong.MaxValue;

        private IcdSchemaInfo Schema { get { return _dev.Schema; } }
        private IcdMsgInfo Current { get { return _message.SelectedItem as MsgItem != null ? ((MsgItem)_message.SelectedItem).Info : null; } }
        private IcdChannel Channel { get { return _channel.SelectedIndex == 1 ? IcdChannel.D : IcdChannel.C; } }

        private sealed class MsgItem
        {
            public IcdMsgInfo Info;
            public override string ToString() { return string.Format("0x{0:X2} {1}", Info.Id, Info.Name); }
        }

        internal MainForm(string screenshotPath)
        {
            _screenshotPath = screenshotPath;
            Text = "ICD Test GUI";
            ClientSize = new Size(1180, 820);
            MinimumSize = new Size(900, 600);
            StartPosition = FormStartPosition.CenterScreen;

            _pending = Schema.Messages.Select(m => _dev.ReadImage(m.Index, IcdSide.To)).ToArray();

            BuildLayout();
            Wire();

            var cfgC = _dev.DefaultConfig(IcdChannel.C);
            var cfgD = _dev.DefaultConfig(IcdChannel.D);
            _baudC.Text = cfgC.Baud.ToString(CultureInfo.InvariantCulture);
            _baudD.Text = cfgD.Baud.ToString(CultureInfo.InvariantCulture);
            _period.Text = cfgC.PeriodUs.ToString(CultureInfo.InvariantCulture);
            _transport.SelectedIndex = 0;
            _channel.SelectedIndex = 0;
            Rescan();
            UpdateEnabled();
            _timer.Start();
            Log("Schema hash 0x" + Schema.Hash.ToString("X8") + ", " + Schema.Messages.Length + " messages");
        }

        private static ComboBox Combo(int width, params string[] items)
        {
            var c = new ComboBox { Width = width, DropDownStyle = ComboBoxStyle.DropDownList, Margin = new Padding(3, 3, 8, 3) };
            c.Items.AddRange(items.Cast<object>().ToArray());
            return c;
        }
        private static TextBox Box(int width) { return new TextBox { Width = width, Margin = new Padding(3, 3, 8, 3) }; }
        private static Button Btn(string text) { return new Button { Text = text, AutoSize = true, Margin = new Padding(3, 2, 3, 2) }; }
        private static Label Lbl(string text) { return new Label { Text = text, AutoSize = true, Margin = new Padding(3, 7, 3, 3) }; }
        private static Label Mono() { return new Label { AutoSize = true, Font = new Font(FontFamily.GenericMonospace, 9f), Margin = new Padding(3, 4, 3, 4) }; }

        private static TextBox FrameBox()
        {
            return new TextBox
            {
                Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Height = 58,
                Anchor = AnchorStyles.Left | AnchorStyles.Right, Font = new Font(FontFamily.GenericMonospace, 9f),
            };
        }

        private static DataGridView FieldGrid(bool readOnly)
        {
            var g = new DataGridView
            {
                Dock = DockStyle.Fill, AllowUserToAddRows = false, AllowUserToDeleteRows = false, AllowUserToResizeRows = false,
                RowHeadersVisible = false, SelectionMode = DataGridViewSelectionMode.CellSelect, MultiSelect = false,
                BackgroundColor = SystemColors.Window, BorderStyle = BorderStyle.FixedSingle,
                ScrollBars = ScrollBars.Both,
            };
            foreach (var column in new[] { "Field", "Byte", "Bits", "Type", "Value", "Raw" })
            {
                var c = new DataGridViewTextBoxColumn { HeaderText = column, ReadOnly = column != "Value" || readOnly, SortMode = DataGridViewColumnSortMode.NotSortable };
                c.Width = column == "Field" ? 170 : column == "Value" ? 110 : column == "Raw" ? 80 : column == "Type" ? 62 : 46;
                g.Columns.Add(c);
            }
            return g;
        }

        private static FlowLayoutPanel Row(params Control[] controls)
        {
            var p = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Dock = DockStyle.Top, WrapContents = true, Margin = new Padding(0) };
            p.Controls.AddRange(controls);
            return p;
        }

        private Control SidePanel(string title, DataGridView grid, BitGrid bits, params Control[] frame)
        {
            var group = new GroupBox { Text = title, Dock = DockStyle.Fill };
            var scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
            scroll.Controls.Add(bits);
            bits.Location = new Point(2, 2);
            var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 3 };
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 42));
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 58));
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            layout.Controls.Add(grid, 0, 0);
            layout.Controls.Add(scroll, 0, 1);
            var footer = new TableLayoutPanel { AutoSize = true, Dock = DockStyle.Top, ColumnCount = 1 };
            footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            foreach (var item in frame) footer.Controls.Add(item);
            layout.Controls.Add(footer, 0, 2);
            group.Controls.Add(layout);
            return group;
        }

        private void BuildLayout()
        {
            var connection = new GroupBox { Text = "Connection", Dock = DockStyle.Top, AutoSize = true };
            connection.Controls.Add(Row(Lbl("Transport"), _transport, Lbl("Chip"), _chip, Lbl("COM C"), _comC, Lbl("COM D"), _comD,
                                        Lbl("Baud C"), _baudC, Lbl("Baud D"), _baudD, _rescan, _connect, _disconnect, _state));

            var msg = new GroupBox { Text = "Message", Dock = DockStyle.Top, AutoSize = true };
            msg.Controls.Add(Row(Lbl("Channel"), _channel, Lbl("Message"), _message, _send, _apply, _revert, _autoApply,
                                 _cyclic, _msgCyclic, Lbl("Period"), _period, _setPeriod, _longOp));

            var sides = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Padding = new Padding(0) };
            sides.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            sides.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
            sides.Controls.Add(SidePanel("To device (edit values or click bits)", _toGrid, _toBits, _toFrame), 0, 0);
            sides.Controls.Add(SidePanel("From device (last good response)", _fromGrid, _fromBits, _fromFrame, _rxInfo), 1, 0);

            var bottom = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 };
            bottom.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            bottom.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            bottom.Controls.Add(_stats, 0, 0);
            bottom.Controls.Add(_log, 0, 1);

            var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 4 };
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 130));
            root.Controls.Add(connection, 0, 0);
            root.Controls.Add(msg, 0, 1);
            root.Controls.Add(sides, 0, 2);
            root.Controls.Add(bottom, 0, 3);
            Controls.Add(root);
        }

        private void Wire()
        {
            _transport.SelectedIndexChanged += (s, e) => UpdateEnabled();
            _rescan.Click += (s, e) => Rescan();
            _connect.Click += (s, e) => Guard(Connect);
            _disconnect.Click += (s, e) => Guard(Disconnect);
            _channel.SelectedIndexChanged += (s, e) => FillMessages();
            _message.SelectedIndexChanged += (s, e) => LoadMessage();
            _send.Click += (s, e) => Send();
            _apply.Click += (s, e) => Guard(Apply);
            _revert.Click += (s, e) => Guard(Revert);
            _cyclic.Click += (s, e) => Guard(ToggleCyclic);
            _msgCyclic.Click += (s, e) => Guard(() => { if (Current != null) _dev.SetCyclicMessage(Current.Index, _msgCyclic.Checked); });
            _setPeriod.Click += (s, e) => Guard(() => _dev.SetPeriod(Channel, uint.Parse(_period.Text, CultureInfo.InvariantCulture)));
            _longOp.Click += (s, e) => Guard(ToggleLongOp);
            _toBits.BitClicked += ToggleBit;
            _toGrid.CellValueChanged += ToCellChanged;
            _toGrid.CurrentCellDirtyStateChanged += (s, e) =>
            {
                if (_toGrid.IsCurrentCellDirty && _toGrid.CurrentCell is DataGridViewComboBoxCell)
                    _toGrid.CommitEdit(DataGridViewDataErrorContexts.Commit);
            };
            _toGrid.DataError += (s, e) => e.ThrowException = false;
            _timer.Tick += (s, e) => Tick();
            Shown += async (s, e) => { if (_screenshotPath != null) await TakeScreenshot(); };
            FormClosed += (s, e) => { _timer.Stop(); foreach (var op in _longOps.Values) op.Dispose(); _dev.Dispose(); };
        }

        private void Guard(Action action)
        {
            try { action(); }
            catch (Exception ex) when (ex is IcdException || ex is FormatException || ex is OverflowException || ex is ArgumentException || ex is InvalidOperationException)
            {
                Log("ERROR: " + ex.Message);
            }
            UpdateEnabled();
        }

        private void Log(string text)
        {
            _log.Items.Insert(0, DateTime.Now.ToString("HH:mm:ss.fff ", CultureInfo.InvariantCulture) + text);
            while (_log.Items.Count > 500) _log.Items.RemoveAt(_log.Items.Count - 1);
        }

        private void Rescan()
        {
            _chip.Items.Clear();
            var chips = IcdDeviceBase.ListFtdi().Select(l => l.Split('\t')).Where(p => p.Length >= 5 && p[4] == "quad" && p[2] == "C").Select(p => p[1]).Distinct().ToArray();
            _chip.Items.AddRange(chips.Cast<object>().ToArray());
            if (_chip.Items.Count > 0) _chip.SelectedIndex = 0;
            var ports = IcdDeviceBase.ListCom().Cast<object>().ToArray();
            _comC.Items.Clear(); _comC.Items.AddRange(ports);
            _comD.Items.Clear(); _comD.Items.AddRange(ports);
            if (_comC.Items.Count > 0) _comC.SelectedIndex = 0;
            if (_comD.Items.Count > 1) _comD.SelectedIndex = 1;
            Log("Found " + chips.Length + " FT4232H chip(s) and " + ports.Length + " COM port(s)");
        }

        private bool Connected { get { return _dev.IsOpen(IcdChannel.C) || _dev.IsOpen(IcdChannel.D); } }

        private void UpdateEnabled()
        {
            bool open = Connected, ftdi = _transport.SelectedIndex == 1, com = _transport.SelectedIndex == 2;
            _transport.Enabled = _rescan.Enabled = _connect.Enabled = !open;
            _baudC.Enabled = _baudD.Enabled = !open;
            _chip.Enabled = ftdi && !open;
            _comC.Enabled = _comD.Enabled = com && !open;
            _disconnect.Enabled = open;
            bool channelOpen = _dev.IsOpen(Channel);
            _send.Enabled = _cyclic.Enabled = _longOp.Enabled = _setPeriod.Enabled = channelOpen;
            _longOp.Text = _longOps.ContainsKey((int)Channel) ? "End long op" : "Begin long op";
            _state.Text = open ? "Connected: " + (_dev.IsOpen(IcdChannel.C) ? "C " : "") + (_dev.IsOpen(IcdChannel.D) ? "D" : "") : "Disconnected";
        }

        private void Connect()
        {
            var cfg = new[] { _dev.DefaultConfig(IcdChannel.C), _dev.DefaultConfig(IcdChannel.D) };
            cfg[0].Baud = uint.Parse(_baudC.Text, CultureInfo.InvariantCulture);
            cfg[1].Baud = uint.Parse(_baudD.Text, CultureInfo.InvariantCulture);
            foreach (IcdChannel ch in new[] { IcdChannel.C, IcdChannel.D })
            {
                try
                {
                    switch (_transport.SelectedIndex)
                    {
                        case 0: _dev.OpenSim(ch, cfg[(int)ch]); break;
                        case 1: _dev.OpenFtdi(ch, _chip.SelectedItem as string, cfg[(int)ch]); break;
                        default: _dev.OpenCom(ch, (ch == IcdChannel.C ? _comC : _comD).SelectedItem as string, cfg[(int)ch]); break;
                    }
                    Log("Channel " + ch + " open");
                    string warning = _dev.TakeWarning();
                    if (warning.Length > 0) Log("WARNING: " + warning);
                }
                catch (IcdException ex) { Log("Channel " + ch + ": " + ex.Message); }
            }
            _cyclic.Checked = false;
        }

        private void Disconnect()
        {
            foreach (var op in _longOps.Values) op.Dispose();
            _longOps.Clear();
            _dev.Close(IcdChannel.C);
            _dev.Close(IcdChannel.D);
            _cyclic.Checked = false;
            Log("Disconnected");
        }

        private void FillMessages()
        {
            char letter = Channel == IcdChannel.C ? 'C' : 'D';
            _message.Items.Clear();
            foreach (var m in Schema.Messages.Where(x => x.Channel == letter))
                _message.Items.Add(new MsgItem { Info = m });
            if (_message.Items.Count > 0) _message.SelectedIndex = 0;
            else LoadMessage();
            UpdateEnabled();
        }

        private static string BitsText(IcdFieldInfo f)
        {
            return f.BitWidth == 1 ? f.BitLow.ToString() : (f.BitLow + f.BitWidth - 1) + "-" + f.BitLow;
        }

        private static string TypeText(IcdFieldInfo f)
        {
            string kind = f.Kind.ToString().ToLowerInvariant();
            return f.IsScaled ? kind + " x" + f.Scale.ToString("G6", CultureInfo.InvariantCulture) : kind;
        }

        private void FillGrid(DataGridView grid, IcdMsgInfo msg, IcdSide side, bool editors)
        {
            grid.Rows.Clear();
            for (int i = 0; i < msg.Fields.Length; i++)
            {
                var f = msg.Fields[i];
                if (!f.ExistsOn(side)) continue;
                int row = grid.Rows.Add(f.Name + (f.Unit.Length > 0 ? " [" + f.Unit + "]" : ""), f.ByteOffset, BitsText(f), TypeText(f), "", "");
                grid.Rows[row].Tag = i;
                grid.Rows[row].Cells[0].Style.BackColor = BitGrid.Palette[i % BitGrid.Palette.Length];
                if (editors && (f.Kind == IcdFieldKind.Bool || f.Kind == IcdFieldKind.Enum))
                {
                    var combo = new DataGridViewComboBoxCell { FlatStyle = FlatStyle.Flat };
                    if (f.Kind == IcdFieldKind.Bool) combo.Items.AddRange("False", "True");
                    else combo.Items.AddRange(f.EnumItems().Select(e => (object)e.Value).ToArray());
                    grid.Rows[row].Cells[ValueColumn] = combo;
                }
            }
        }

        private void LoadMessage()
        {
            _loading = true;
            var m = Current;
            if (m != null)
            {
                FillGrid(_toGrid, m, IcdSide.To, true);
                FillGrid(_fromGrid, m, IcdSide.From, false);
                _toBits.Bind(m, IcdSide.To, true);
                _fromBits.Bind(m, IcdSide.From, false);
                _msgCyclic.Enabled = true;
                _msgCyclic.Checked = m.CyclicByDefault;
                _shownRx = null;
                _shownSeq = ulong.MaxValue;
                ShowTo();
                ShowFrom(true);
            }
            _loading = false;
        }

        private static string Hex(byte[] data, int count) { return BitConverter.ToString(data, 0, count).Replace('-', ' '); }

        private string FrameText(byte[] image)
        {
            return Hex(image, image.Length) + " | CRC " + Hex(Schema.Crc.CrcBytes(image, image.Length), 4);
        }

        private void ShowTo()
        {
            var m = Current;
            if (m == null) return;
            var image = _pending[m.Index];
            bool was = _loading;
            _loading = true;
            foreach (DataGridViewRow row in _toGrid.Rows)
            {
                var f = m.Fields[(int)row.Tag];
                if (!(row.Cells[ValueColumn].IsInEditMode)) row.Cells[ValueColumn].Value = IcdBits.Format(image, f);
                row.Cells[5].Value = "0x" + IcdBits.GetRaw(image, f).ToString("X");
            }
            _loading = was;
            _toBits.Show(image, null);
            _toFrame.Text = "Frame " + FrameText(image);
        }

        private void ShowFrom(bool force)
        {
            var m = Current;
            if (m == null) return;
            var meta = _dev.GetMeta(m.Index);
            if (!force && meta.RxSeq == _shownSeq)
            {
                _rxInfo.Text = RxText(meta);
                return;
            }
            _shownSeq = meta.RxSeq;
            var image = _dev.ReadImage(m.Index, IcdSide.From);
            _shownRx = image;
            var sent = _dev.ReadImage(m.Index, IcdSide.To);
            foreach (DataGridViewRow row in _fromGrid.Rows)
            {
                var f = m.Fields[(int)row.Tag];
                row.Cells[ValueColumn].Value = meta.RxValid != 0 ? IcdBits.Format(image, f) : "-";
                row.Cells[5].Value = meta.RxValid != 0 ? "0x" + IcdBits.GetRaw(image, f).ToString("X") : "-";
                bool differs = meta.RxValid != 0 && f.ExistsOn(IcdSide.To) && IcdBits.GetRaw(image, f) != IcdBits.GetRaw(sent, f);
                row.Cells[ValueColumn].Style.BackColor = differs ? Color.MistyRose : SystemColors.Window;
            }
            _fromBits.Show(image, meta.RxValid != 0 ? sent : null);
            _fromFrame.Text = meta.RxValid != 0 ? "Frame " + FrameText(image) : "No response yet";
            _rxInfo.Text = RxText(meta);
        }

        private static string RxText(IcdMsgMeta meta)
        {
            return meta.RxValid == 0 ? "" : string.Format(CultureInfo.InvariantCulture, "Responses {0}, newest {1:F1} ms ago", meta.RxSeq, meta.RxAgeUs / 1000.0);
        }

        private void AfterEdit()
        {
            ShowTo();
            if (_autoApply.Checked) Guard(Apply);
        }

        private void ToggleBit(int b, int i)
        {
            var m = Current;
            if (m == null) return;
            _pending[m.Index][b] ^= (byte)(1 << i);
            AfterEdit();
        }

        private void ToCellChanged(object sender, DataGridViewCellEventArgs e)
        {
            var m = Current;
            if (_loading || m == null || e.RowIndex < 0 || e.ColumnIndex != ValueColumn) return;
            var row = _toGrid.Rows[e.RowIndex];
            var f = m.Fields[(int)row.Tag];
            double value;
            try
            {
                if (!IcdBits.TryParse(Convert.ToString(row.Cells[ValueColumn].Value, CultureInfo.InvariantCulture), f, out value))
                    throw new FormatException("'" + row.Cells[ValueColumn].Value + "' is not a value for " + f.Name);
                IcdBits.SetValue(_pending[m.Index], f, value);
            }
            catch (Exception ex) when (ex is FormatException || ex is ArgumentException)
            {
                Log("ERROR: " + ex.Message);
            }
            BeginInvoke((Action)AfterEdit);
        }

        // All edits of the selected message reach the device as one update.
        private void Apply()
        {
            var m = Current;
            if (m == null) return;
            var image = (byte[])_pending[m.Index].Clone();
            _dev.Atomic(() => _dev.WriteImage(m.Index, image));
            ShowFrom(true);
        }

        private void Revert()
        {
            var m = Current;
            if (m == null) return;
            _pending[m.Index] = _dev.ReadImage(m.Index, IcdSide.To);
            ShowTo();
        }

        private async void Send()
        {
            var m = Current;
            if (m == null) return;
            try
            {
                Apply();
                _send.Enabled = false;
                var rx = await Task.Run(() => _dev.Send(m.Index, 1000));
                Log(string.Format("{0}: TX {1}  RX {2}", m.Name, FrameText(_pending[m.Index]), m.HasResponse ? FrameText(rx) : "(no response expected)"));
                ShowFrom(true);
            }
            catch (IcdException ex) { Log("ERROR: " + ex.Message); }
            UpdateEnabled();
        }

        private void ToggleCyclic()
        {
            if (_cyclic.Checked) _dev.StartCyclic(Channel);
            else _dev.StopCyclic(Channel);
        }

        private void ToggleLongOp()
        {
            IDisposable op;
            if (_longOps.TryGetValue((int)Channel, out op))
            {
                op.Dispose();
                _longOps.Remove((int)Channel);
                Log("Long operation ended on " + Channel);
            }
            else
            {
                _longOps[(int)Channel] = _dev.BeginLongOperation(Channel);
                Log("Long operation started on " + Channel + ". Periodic traffic is paused.");
            }
        }

        private void Tick()
        {
            if (!Connected) return;
            ShowFrom(false);
            var s = _dev.GetStats(Channel);
            _stats.Text = string.Format(CultureInfo.InvariantCulture,
                "{0}: slots {1}  tx {2}  rx {3}  crc {4}  timeouts {5}  late {6}  skipped {7}  stray {8}  io {9}  worst lateness {10} us",
                Channel, s.Slots, s.TxFrames, s.RxFrames, s.CrcErrors, s.RxTimeouts, s.WindowMisses, s.MissedSlots, s.StrayBytes, s.IoErrors, s.MaxLatenessUs);
        }

        private async Task TakeScreenshot()
        {
            Guard(Connect);
            var m = Current;
            IcdBits.SetRaw(_pending[m.Index], m.Fields[0], 1);
            IcdBits.SetRaw(_pending[m.Index], m.Fields[2], 7);
            ShowTo();
            _cyclic.Checked = true;
            Guard(ToggleCyclic);
            Guard(Apply);
            await Task.Delay(600);
            IcdBits.SetRaw(_pending[m.Index], m.Fields[1], 5);
            ShowTo();
            await Task.Delay(200);
            using (var bitmap = new Bitmap(Width, Height))
            {
                DrawToBitmap(bitmap, new Rectangle(0, 0, Width, Height));
                bitmap.Save(_screenshotPath, System.Drawing.Imaging.ImageFormat.Png);
            }
            Close();
        }
    }
}
