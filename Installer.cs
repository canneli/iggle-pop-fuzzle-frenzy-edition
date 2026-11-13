using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Windows.Forms;

internal static class Installer {
    const string ExeHash = "bddc47bea03000d92ed552c3f0de2537cafefd69bef9b1e4263e63330358cd2e";
    const string ExeName = "IgglePop.exe";
    const string ArchiveName = "IgglePop.saf";
    const string Backup = "IgglePop-HD-original";
    const string Steam = @"C:\Program Files (x86)\Steam\steamapps\common\Iggle Pop Deluxe";

    [STAThread]
    private static void Main(string[] args) {
        try {
            string target = args.Length == 2 && args[0] == "--folder" ? args[1] : PickFolder();
            if (target == null) return;
            Install(Path.GetFullPath(target));
            MessageBox.Show("Patch aplicado. Abra Fases personalizadas no menu inicial do jogo.",
                "Iggle Pop 16:9", MessageBoxButtons.OK, MessageBoxIcon.Information);
        } catch (Exception error) {
            MessageBox.Show(error.Message, "Iggle Pop 16:9", MessageBoxButtons.OK, MessageBoxIcon.Error);
            Environment.ExitCode = 1;
        }
    }
    static string PickFolder() {
        using (var folder = new FolderBrowserDialog()) {
            folder.Description = "Selecione a pasta do Iggle Pop Deluxe da Steam.";
            folder.ShowNewFolderButton = false;
            if (Directory.Exists(Steam)) folder.SelectedPath = Steam;
            return folder.ShowDialog() == DialogResult.OK ? folder.SelectedPath : null;
        }
    }
    static string Hash(byte[] bytes) {
        using (var sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant();
    }
    static byte[] Read(ZipArchive zip, string name) {
        var entry = zip.GetEntry(name);
        if (entry == null) throw new InvalidDataException(name);
        using (var stream = entry.Open())
        using (var data = new MemoryStream()) { stream.CopyTo(data); return data.ToArray(); }
    }
    static void Install(string target) {
        foreach (var game in Process.GetProcesses()) using (game) {
            if (game.ProcessName.StartsWith("popcapgame", StringComparison.OrdinalIgnoreCase) ||
                game.ProcessName.Equals("IgglePop", StringComparison.OrdinalIgnoreCase)) {
                try {
                    if (string.Equals(Path.GetDirectoryName(game.MainModule.FileName), target, StringComparison.OrdinalIgnoreCase))
                        throw new IOException("Feche o Iggle Pop antes de aplicar o patch.");
                } catch (System.ComponentModel.Win32Exception) { }
            }
        }
        string backup = Path.Combine(target, Backup);
        string input = File.Exists(Path.Combine(backup, ExeName)) ? backup : target;
        var exe = File.ReadAllBytes(Path.Combine(input, ExeName));
        var archive = File.ReadAllBytes(Path.Combine(input, ArchiveName));
        if (Hash(exe) != ExeHash) throw new InvalidDataException("Esta versão do executável não corresponde à edição Steam compatível.");
        var files = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        using (var payload = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
        using (var zip = new ZipArchive(payload, ZipArchiveMode.Read)) {
            string expected = System.Text.Encoding.ASCII.GetString(Read(zip, "saf.sha256")).Trim();
            if (Hash(archive) != expected) throw new InvalidDataException("Os recursos do jogo não correspondem à edição Steam compatível.");
            int baseSize = BitConverter.ToInt32(archive, 8);
            foreach (string language in new[] {"en","pt-BR","es"}) {
                byte[] delta = Read(zip, "deltas/" + language);
                var localized = new byte[baseSize + delta.Length - 4];
                Buffer.BlockCopy(archive, 0, localized, 0, baseSize);
                Buffer.BlockCopy(delta, 0, localized, 8, 4);
                Buffer.BlockCopy(delta, 4, localized, baseSize, delta.Length - 4);
                files.Add("iggle-hd/" + language + ".saf", localized);
            }
            foreach (var entry in zip.Entries)
                if (entry.FullName.StartsWith("files/", StringComparison.Ordinal))
                    files.Add(entry.FullName.Substring(6), Read(zip, entry.FullName));
        }
        Buffer.BlockCopy(BitConverter.GetBytes(864), 0, exe, 348578 + 0x468b, 4);
        Buffer.BlockCopy(BitConverter.GetBytes(486), 0, exe, 348578 + 0x4684, 4);
        files.Add(ExeName, exe);
        string active = "en";
        string existingArchive = Path.Combine(target, ArchiveName);
        string currentHash = Hash(File.ReadAllBytes(existingArchive));
        foreach (string language in new[] {"pt-BR","es"})
            if (currentHash == Hash(files["iggle-hd/" + language + ".saf"]) ||
                (File.Exists(Path.Combine(target, "iggle-hd", language + ".saf")) &&
                 currentHash == Hash(File.ReadAllBytes(Path.Combine(target, "iggle-hd", language + ".saf"))))) active = language;
        files["iggle-hd/language.ini"] = System.Text.Encoding.ASCII.GetBytes("[language]\r\nindex=" + (active == "pt-BR" ? "1" : active == "es" ? "2" : "0") + "\r\n");
        files.Add(ArchiveName, files["iggle-hd/" + active + ".saf"]);
        files["splash/loadText.jpg"] = files["iggle-hd/" + active + "-loadText.jpg"];
        var levels = Directory.CreateDirectory(Path.Combine(target, "CustomLevels"));
        var access = levels.GetAccessControl();
        access.SetAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null),
            FileSystemRights.Modify, InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit,
            PropagationFlags.None, AccessControlType.Allow));
        levels.SetAccessControl(access);
        string transaction = Path.Combine(target, ".iggle-patch-" + Guid.NewGuid().ToString("N"));
        var applied = new List<string>();
        Directory.CreateDirectory(transaction);
        try {
            foreach (var file in files) {
                string relative = file.Key;
                string current = Path.Combine(target, relative);
                string previous = Path.Combine(transaction, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(previous));
                Directory.CreateDirectory(Path.GetDirectoryName(current));
                if (File.Exists(current)) {
                    File.Copy(current, previous);
                    if (input == target) {
                        string preserved = Path.Combine(backup, relative);
                        Directory.CreateDirectory(Path.GetDirectoryName(preserved));
                        if (!File.Exists(preserved)) File.Copy(current, preserved);
                    }
                }
                string staged = previous + ".new";
                File.WriteAllBytes(staged, file.Value);
                if (File.Exists(current)) File.Replace(staged, current, null);
                else File.Move(staged, current);
                applied.Add(relative);
            }
        } catch {
            foreach (string relative in applied) {
                string previous = Path.Combine(transaction, relative);
                string current = Path.Combine(target, relative);
                if (File.Exists(previous)) File.Copy(previous, current, true);
                else File.Delete(current);
            }
            throw;
        } finally { Directory.Delete(transaction, true); }
    }
}
