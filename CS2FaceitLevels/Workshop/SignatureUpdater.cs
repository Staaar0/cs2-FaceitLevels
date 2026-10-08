using System.Net.Http;
using System.Text;

namespace CS2FaceitLevels.Workshop;

internal static class SignatureUpdater
{
    internal const string SourceUrl = "https://raw.githubusercontent.com/Staaar0/cs2-FaceitLevels/refs/heads/main/CS2FaceitLevels/workshop.gamedata.json";
    private const int MaximumBytes = 64 * 1024;
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private static readonly HttpClient Client = new(new SocketsHttpHandler { AllowAutoRedirect = false })
    {
        Timeout = Timeout.InfiniteTimeSpan
    };

    internal static Task<bool> Update(string directory, CancellationToken cancellationToken)
        => Update(directory, Client, cancellationToken);

    internal static async Task<bool> Update(string directory, HttpClient client,
        CancellationToken cancellationToken)
    {
        string? temporary = null;
        try
        {
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            deadline.CancelAfter(TimeSpan.FromSeconds(3));
            var token = deadline.Token;
            token.ThrowIfCancellationRequested();

            using var response = await client.GetAsync(SourceUrl, HttpCompletionOption.ResponseHeadersRead, token)
                .ConfigureAwait(false);
            if (!response.IsSuccessStatusCode || response.Content.Headers.ContentLength is > MaximumBytes)
                return false;

            await using var stream = await response.Content.ReadAsStreamAsync(token).ConfigureAwait(false);
            using var content = new MemoryStream();
            var buffer = new byte[4096];
            while (true)
            {
                int length = Math.Min(buffer.Length, MaximumBytes - (int)content.Length + 1);
                int read = await stream.ReadAsync(buffer.AsMemory(0, length), token).ConfigureAwait(false);
                if (read == 0) break;
                if (content.Length + read > MaximumBytes) return false;
                content.Write(buffer, 0, read);
            }

            if (content.Length == 0) return false;
            var bytes = content.ToArray();
            string json = Utf8.GetString(bytes);
            if (json.Length > 0 && json[0] == '\uFEFF') json = json[1..];
            EngineLayout.Parse(json, "linux");
            EngineLayout.Parse(json, "windows");
            token.ThrowIfCancellationRequested();

            string target = Path.Combine(Path.GetFullPath(directory), "workshop.gamedata.json");
            if (await ExistingMatches(target, bytes, token).ConfigureAwait(false)) return false;
            temporary = Path.Combine(Path.GetDirectoryName(target)!, $".workshop.gamedata.{Guid.NewGuid():N}.tmp");
            await using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write,
                FileShare.None, 4096, FileOptions.Asynchronous))
            {
                await file.WriteAsync(bytes.AsMemory(), token).ConfigureAwait(false);
                await file.FlushAsync(token).ConfigureAwait(false);
            }
            token.ThrowIfCancellationRequested();
            File.Move(temporary, target, overwrite: true);
            return true;
        }
        catch (Exception)
        {
            return false;
        }
        finally
        {
            if (temporary != null)
            {
                try { File.Delete(temporary); }
                catch (Exception) { }
            }
        }
    }

    private static async Task<bool> ExistingMatches(string path, byte[] downloaded,
        CancellationToken cancellationToken)
    {
        try
        {
            await using var file = new FileStream(path, FileMode.Open, FileAccess.Read,
                FileShare.Read, 4096, FileOptions.Asynchronous | FileOptions.SequentialScan);
            if (file.Length != downloaded.Length) return false;
            var existing = new byte[downloaded.Length];
            int offset = 0;
            while (offset < existing.Length)
            {
                int read = await file.ReadAsync(existing.AsMemory(offset), cancellationToken).ConfigureAwait(false);
                if (read == 0) return false;
                offset += read;
            }
            return existing.AsSpan().SequenceEqual(downloaded);
        }
        catch (FileNotFoundException)
        {
            return false;
        }
    }
}
