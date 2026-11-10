using System;
using System.Windows.Forms;

namespace IcdTestGui
{
    internal static class Program
    {
        [STAThread]
        private static void Main(string[] args)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            string screenshot = args.Length == 2 && args[0] == "--screenshot" ? args[1] : null;
            Application.Run(new MainForm(screenshot));
        }
    }
}
