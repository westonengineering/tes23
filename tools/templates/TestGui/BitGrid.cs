// BitGrid.cs - shows every bit of a message image, coloured by the field it belongs to.
using System;
using System.Drawing;
using System.Windows.Forms;
using IcdLink;

namespace IcdTestGui
{
    internal sealed class BitGrid : Control
    {
        private const int RowHeight = 24, LabelWidth = 96, CellWidth = 40, HeaderHeight = 20;

        internal static readonly Color[] Palette =
        {
            Color.FromArgb(173, 216, 255), Color.FromArgb(255, 214, 165), Color.FromArgb(190, 235, 190),
            Color.FromArgb(240, 195, 240), Color.FromArgb(255, 240, 160), Color.FromArgb(180, 230, 225),
            Color.FromArgb(255, 195, 195), Color.FromArgb(205, 205, 245), Color.FromArgb(225, 225, 180),
            Color.FromArgb(200, 225, 150),
        };

        private readonly ToolTip _tip = new ToolTip();
        private string _lastTip = "";
        private IcdMsgInfo _msg;
        private IcdSide _side;
        private bool _editable;
        private byte[] _image, _compare;
        private int[,] _owner = new int[0, 0];

        internal event Action<int, int> BitClicked;

        internal BitGrid()
        {
            DoubleBuffered = true;
            ResizeRedraw = true;
            BackColor = SystemColors.Window;
            Font = new Font(FontFamily.GenericMonospace, 9f);
        }

        internal void Bind(IcdMsgInfo msg, IcdSide side, bool editable)
        {
            _msg = msg; _side = side; _editable = editable;
            int len = msg.Length(side);
            _owner = new int[len, 8];
            for (int b = 0; b < len; b++)
                for (int i = 0; i < 8; i++) _owner[b, i] = -1;
            for (int f = 0; f < msg.Fields.Length; f++)
            {
                var d = msg.Fields[f];
                if (!d.ExistsOn(side)) continue;
                for (int bit = d.BitLow; bit < d.BitLow + d.BitWidth; bit++)
                {
                    int index = bit / 8;
                    int at = d.ByteOffset + (d.BigEndian ? d.ByteCount - 1 - index : index);
                    if (at < len) _owner[at, bit % 8] = f;
                }
            }
            Size = new Size(LabelWidth + CellWidth * 8 + 1, HeaderHeight + RowHeight * len + 1);
            Invalidate();
        }

        internal void Show(byte[] image, byte[] compare)
        {
            _image = image; _compare = compare;
            Invalidate();
        }

        private static bool Bit(byte[] image, int b, int i) { return (image[b] & (1 << i)) != 0; }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            if (_msg == null || _image == null) return;
            var g = e.Graphics;
            using (var center = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center })
            using (var left = new StringFormat { LineAlignment = StringAlignment.Center })
            using (var diffPen = new Pen(Color.Firebrick, 2f))
            {
                for (int i = 0; i < 8; i++)
                    g.DrawString(i.ToString(), Font, Brushes.DimGray, new RectangleF(LabelWidth + CellWidth * (7 - i), 0, CellWidth, HeaderHeight), center);

                for (int b = 0; b < _image.Length; b++)
                {
                    int y = HeaderHeight + RowHeight * b;
                    g.DrawString(string.Format("[{0}] 0x{1:X2}", b, _image[b]), Font, Brushes.Black, new RectangleF(2, y, LabelWidth - 4, RowHeight), left);
                    for (int i = 7; i >= 0; i--)
                    {
                        var rect = new Rectangle(LabelWidth + CellWidth * (7 - i), y, CellWidth, RowHeight);
                        int owner = _owner[b, i];
                        Color fill = b == 0 ? Color.LightGray : owner >= 0 ? Palette[owner % Palette.Length] : Color.WhiteSmoke;
                        using (var brush = new SolidBrush(fill)) g.FillRectangle(brush, rect);
                        g.DrawRectangle(Pens.Silver, rect);
                        g.DrawString(Bit(_image, b, i) ? "1" : "0", Font, owner >= 0 || b == 0 ? Brushes.Black : Brushes.Gray, rect, center);
                        if (_compare != null && b < _compare.Length && Bit(_image, b, i) != Bit(_compare, b, i))
                            g.DrawRectangle(diffPen, Rectangle.Inflate(rect, -1, -1));
                    }
                }
            }
        }

        private bool HitTest(Point p, out int b, out int i)
        {
            b = i = -1;
            if (_msg == null || p.X < LabelWidth || p.Y < HeaderHeight) return false;
            b = (p.Y - HeaderHeight) / RowHeight;
            i = 7 - (p.X - LabelWidth) / CellWidth;
            return b < _msg.Length(_side) && i >= 0 && i <= 7;
        }

        protected override void OnMouseClick(MouseEventArgs e)
        {
            base.OnMouseClick(e);
            int b, i;
            if (_editable && e.Button == MouseButtons.Left && HitTest(e.Location, out b, out i) && b > 0)
            {
                var handler = BitClicked;
                if (handler != null) handler(b, i);
            }
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            int b, i;
            string text = "";
            if (HitTest(e.Location, out b, out i))
            {
                if (b == 0) text = "Message type";
                else
                {
                    string name = _owner[b, i] >= 0 ? _msg.Fields[_owner[b, i]].Name : "Reserved or undefined";
                    text = string.Format("{0} (byte {1} bit {2})", name, b, i);
                }
            }
            if (text != _lastTip)
            {
                _lastTip = text;
                _tip.SetToolTip(this, text);
            }
        }
    }
}
