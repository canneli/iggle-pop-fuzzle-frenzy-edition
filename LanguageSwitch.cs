using System;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
internal static class LanguageSwitch {
    [STAThread] private static void Main(string[] args) {
        int pid;
        string[] languages = {"en", "pt-BR", "es"};
        if (args.Length != 3 || args[0] != "--language" || !int.TryParse(args[1], out pid)) return;
        int index = Array.IndexOf(languages, args[2]);
        if (index < 0) return;
        string folder = AppDomain.CurrentDomain.BaseDirectory;
        string assets = Path.Combine(folder, "iggle-hd");
        string selected = Path.Combine(assets, args[2] + ".saf");
        string target = Path.Combine(folder, "IgglePop.saf");
        string temporary = Path.Combine(folder, ".iggle-language-" + Guid.NewGuid().ToString("N"));
        string pending = Path.Combine(assets, "language.pending");
        string token = Guid.NewGuid().ToString("N");
        try {
            if (!File.Exists(selected)) throw new IOException("Language files are missing.");
            File.Copy(selected, temporary);
            File.WriteAllText(pending, token);
            try { using (Process game = Process.GetProcessById(pid)) game.WaitForExit(); }
            catch (ArgumentException) { }
            string identity;
            using (var sha = SHA256.Create()) identity = BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(folder))).Replace("-", "");
            using (var mutex = new Mutex(false, "Local\\IggleLanguage" + identity)) {
                try { mutex.WaitOne(); } catch (AbandonedMutexException) { }
                try {
                    if (!File.Exists(pending) || File.ReadAllText(pending) != token) return;
                    File.Replace(temporary, target, null);
                    string caption = Path.Combine(assets, args[2] + "-loadText.jpg");
                    if (File.Exists(caption)) File.Copy(caption, Path.Combine(folder, "splash", "loadText.jpg"), true);
                    File.WriteAllText(Path.Combine(assets, "language.ini"), "[language]\r\nindex=" + index + "\r\n");
                    File.Delete(pending);
                } finally { mutex.ReleaseMutex(); }
            }
        } catch (Exception error) {
            try { File.WriteAllText(Path.Combine(assets, "language-error.txt"), error.ToString()); } catch { }
        } finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}
