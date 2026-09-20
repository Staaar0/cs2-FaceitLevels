using System.Text.Json;
using Microsoft.Extensions.Logging;

namespace CS2FaceitLevels;

internal static class LanguageReader
{
    private const long MaxLanguageFileBytes = 128 * 1024;
    internal static CS2FaceitLevelsLang Load(string moduleDirectory, string language, ILogger logger)
    {
        var requested = string.IsNullOrWhiteSpace(language) ? "en" : language.Trim();

        var name = Path.GetFileName(requested);
        if (string.IsNullOrEmpty(name))
            name = "en";

        var langDirectory = Path.Combine(moduleDirectory, "lang");
        var path = Path.Combine(langDirectory, name + ".json");

        if (!File.Exists(path))
        {
            logger.LogWarning("[CS2FaceitLevels] Language '{Language}' not found in {Directory}, using English.", name, langDirectory);
            path = Path.Combine(langDirectory, "en.json");
        }

        try
        {
            if (File.Exists(path))
            {
                var info = new FileInfo(path);
                if (info.Length > MaxLanguageFileBytes)
                {
                    logger.LogWarning("[CS2FaceitLevels] Language file {Path} is unexpectedly large ({Bytes} bytes), using built-in English.",
                        path, info.Length);
                    return new CS2FaceitLevelsLang();
                }

                var lang = JsonSerializer.Deserialize<CS2FaceitLevelsLang>(File.ReadAllText(path), FaceitJson.Options);
                if (lang != null)
                    return lang.Normalized();
            }
        }
        catch (Exception ex)
        {
            logger.LogWarning(ex, "[CS2FaceitLevels] Failed to read language file {Path}, using built-in English.", path);
        }

        return new CS2FaceitLevelsLang();
    }

}
