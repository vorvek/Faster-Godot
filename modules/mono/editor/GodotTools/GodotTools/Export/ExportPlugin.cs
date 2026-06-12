using Godot;
using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using GodotTools.Build;
using GodotTools.Internals;
using Directory = GodotTools.Utils.Directory;
using File = GodotTools.Utils.File;
using OS = GodotTools.Utils.OS;
using Path = System.IO.Path;
using System.Globalization;

namespace GodotTools.Export
{
    public partial class ExportPlugin : EditorExportPlugin
    {
        public override string _GetName() => "C#";

        private List<string> _tempFolders = new List<string>();

        private static bool ProjectContainsDotNet()
        {
            return File.Exists(GodotSharpDirs.ProjectSlnPath);
        }

        public override string[] _GetExportFeatures(EditorExportPlatform platform, bool debug)
        {
            if (!ProjectContainsDotNet())
                return Array.Empty<string>();

            return new string[] { "dotnet" };
        }

        public override Godot.Collections.Array<Godot.Collections.Dictionary> _GetExportOptions(EditorExportPlatform platform)
        {
            var exportOptionList = new Godot.Collections.Array<Godot.Collections.Dictionary>();

            exportOptionList.Add
            (
                new Godot.Collections.Dictionary()
                {
                    {
                        "option", new Godot.Collections.Dictionary()
                        {
                            { "name", "dotnet/include_scripts_content" },
                            { "type", (int)Variant.Type.Bool }
                        }
                    },
                    { "default_value", false }
                }
            );
            exportOptionList.Add
            (
                new Godot.Collections.Dictionary()
                {
                    {
                        "option", new Godot.Collections.Dictionary()
                        {
                            { "name", "dotnet/include_debug_symbols" },
                            { "type", (int)Variant.Type.Bool }
                        }
                    },
                    { "default_value", true }
                }
            );
            exportOptionList.Add
            (
                new Godot.Collections.Dictionary()
                {
                    {
                        "option", new Godot.Collections.Dictionary()
                        {
                            { "name", "dotnet/embed_build_outputs" },
                            { "type", (int)Variant.Type.Bool }
                        }
                    },
                    { "default_value", false }
                }
            );
            return exportOptionList;
        }

        private void AddExceptionMessage(EditorExportPlatform platform, Exception exception)
        {
            string? exceptionMessage = exception.Message;
            if (string.IsNullOrEmpty(exceptionMessage))
            {
                exceptionMessage = $"Exception thrown: {exception.GetType().Name}";
            }

            platform.AddMessage(EditorExportPlatform.ExportMessageType.Error, "Export .NET Project", exceptionMessage);

            // We also print exceptions as we receive them to stderr.
            Console.Error.WriteLine(exception);
        }

        // With this method we can override how a file is exported in the PCK
        public override void _ExportFile(string path, string type, string[] features)
        {
            base._ExportFile(path, type, features);

            if (type != Internal.CSharpLanguageType)
                return;

            if (Path.GetExtension(path) != Internal.CSharpLanguageExtension)
                throw new ArgumentException(
                    $"Resource of type {Internal.CSharpLanguageType} has an invalid file extension: {path}",
                    nameof(path));

            if (!ProjectContainsDotNet())
            {
                GetExportPlatform().AddMessage(EditorExportPlatform.ExportMessageType.Error, "Export .NET Project", $"This project contains C# files but no solution file was found at the following path: {GodotSharpDirs.ProjectSlnPath}\n" +
                    "A solution file is required for projects with C# files. Please ensure that the solution file exists in the specified location and try again.");
                throw new InvalidOperationException($"{path} is a C# file but no solution file exists.");
            }

            // TODO: What if the source file is not part of the game's C# project?

            bool includeScriptsContent = (bool)GetOption("dotnet/include_scripts_content");

            if (!includeScriptsContent)
            {
                // We don't want to include the source code on exported games.

                // Sadly, Godot prints errors when adding an empty file (nothing goes wrong, it's just noise).
                // Because of this, we add a file which contains a line break.
                AddFile(path, System.Text.Encoding.UTF8.GetBytes("\n"), remap: false);

                // Tell the Godot exporter that we already took care of the file.
                Skip();
            }
        }

        public override void _ExportBegin(string[] features, bool isDebug, string path, uint flags)
        {
            base._ExportBegin(features, isDebug, path, flags);

            try
            {
                _ExportBeginImpl(features, isDebug, path, flags);
            }
            catch (Exception e)
            {
                AddExceptionMessage(GetExportPlatform(), e);
            }
        }

        private void _ExportBeginImpl(string[] features, bool isDebug, string path, long flags)
        {
            _ = path; // Unused.
            _ = flags; // Unused.

            if (!ProjectContainsDotNet())
                return;

            string osName = GetExportPlatform().GetOsName();

            if (!TryDeterminePlatformFromOSName(osName, out string? platform))
                throw new NotSupportedException("Target platform not supported.");

            if (!new[] { OS.Platforms.Windows, OS.Platforms.LinuxBSD }.Contains(platform))
            {
                throw new NotSupportedException("Target platform not supported by this build.");
            }

            PublishConfig publishConfig = new()
            {
                BuildConfig = isDebug ? "ExportDebug" : "ExportRelease",
                IncludeDebugSymbols = (bool)GetOption("dotnet/include_debug_symbols"),
                RidOS = OS.DotNetOSPlatformMap[platform],
                Archs = [],
                UseTempDir = true,
            };

            if (features.Any(feature => feature is "x86_32" or "arm32" or "arm64" or "universal"))
            {
                throw new NotSupportedException("Only x86_64 .NET exports are supported by this build.");
            }

            if (features.Contains("x86_64"))
            {
                publishConfig.Archs.Add("x86_64");
            }

            if (publishConfig.Archs.Count == 0)
            {
                publishConfig.Archs.Add("x86_64");
            }

            bool embedBuildResults = (bool)GetOption("dotnet/embed_build_outputs");

            string ridOS = publishConfig.RidOS;
            string buildConfig = publishConfig.BuildConfig;
            bool includeDebugSymbols = publishConfig.IncludeDebugSymbols;

            foreach (string arch in publishConfig.Archs)
            {
                string ridArch = DetermineRuntimeIdentifierArch(arch);
                string runtimeIdentifier = $"{ridOS}-{ridArch}";
                string projectDataDirName = $"data_{GodotSharpDirs.CSharpProjectName}_{platform}_{arch}";

                string publishOutputDir;

                if (publishConfig.UseTempDir)
                {
                    publishOutputDir = Path.Combine(Path.GetTempPath(), "godot-publish-dotnet",
                        $"{System.Environment.ProcessId}-{buildConfig}-{runtimeIdentifier}");
                    _tempFolders.Add(publishOutputDir);
                }
                else
                {
                    publishOutputDir = Path.Combine(GodotSharpDirs.ProjectBaseOutputPath, "godot-publish-dotnet",
                        $"{buildConfig}-{runtimeIdentifier}");
                }

                if (!Directory.Exists(publishOutputDir))
                    Directory.CreateDirectory(publishOutputDir);

                if (!BuildManager.PublishProjectBlocking(buildConfig, platform,
                        runtimeIdentifier, publishOutputDir, includeDebugSymbols))
                {
                    throw new InvalidOperationException("Failed to build project. Check MSBuild panel for details.");
                }

                string soExt = ridOS switch
                {
                    OS.DotNetOS.Win or OS.DotNetOS.Win10 => "dll",
                    _ => "so"
                };

                string assemblyPath = Path.Combine(publishOutputDir, $"{GodotSharpDirs.ProjectAssemblyName}.dll");
                string nativeAotPath = Path.Combine(publishOutputDir,
                    $"{GodotSharpDirs.ProjectAssemblyName}.{soExt}");

                if (!File.Exists(assemblyPath) && !File.Exists(nativeAotPath))
                {
                    throw new NotSupportedException(
                        $"Publish succeeded but project assembly not found at '{assemblyPath}' or '{nativeAotPath}'.");
                }

                var manifest = new StringBuilder();

                RecursePublishContents(publishOutputDir,
                    filterDir: _ => true,
                    filterFile: _ => true,
                    recurseDir: _ => true,
                    addEntry: (entryPath, isFile) =>
                    {
                        // We get called back for both directories and files, but we only package files for now.
                        if (!isFile)
                            return;

                        if (embedBuildResults)
                        {
                            string filePath = SanitizeSlashes(Path.GetRelativePath(publishOutputDir, entryPath));
                            byte[] fileData = File.ReadAllBytes(entryPath);
                            string hash = Convert.ToBase64String(SHA512.HashData(fileData));

                            manifest.Append(CultureInfo.InvariantCulture, $"{filePath}\t{hash}\n");

                            AddFile($"res://.godot/mono/publish/{arch}/{filePath}", fileData, false);
                        }
                        else
                        {
                            AddSharedObject(entryPath, tags: null,
                                Path.Join(projectDataDirName,
                                    Path.GetRelativePath(publishOutputDir,
                                        Path.GetDirectoryName(entryPath)!)));
                        }
                    });

                if (embedBuildResults)
                {
                    byte[] fileData = Encoding.Default.GetBytes(manifest.ToString());
                    AddFile($"res://.godot/mono/publish/{arch}/.dotnet-publish-manifest", fileData, false);
                }
            }
        }

        private static void RecursePublishContents(string path, Func<string, bool> filterDir,
            Func<string, bool> filterFile, Func<string, bool> recurseDir,
            Action<string, bool> addEntry)
        {
            foreach (string file in Directory.GetFiles(path, "*", SearchOption.TopDirectoryOnly))
            {
                if (filterFile(file))
                {
                    addEntry(file, true);
                }
            }

            foreach (string dir in Directory.GetDirectories(path, "*", SearchOption.TopDirectoryOnly))
            {
                if (filterDir(dir))
                {
                    addEntry(dir, false);
                    if (recurseDir(dir))
                    {
                        RecursePublishContents(dir, filterDir, filterFile, recurseDir, addEntry);
                    }
                }
            }
        }

        private string SanitizeSlashes(string path)
        {
            if (Path.DirectorySeparatorChar == '\\')
                return path.Replace('\\', '/');
            return path;
        }

        private string DetermineRuntimeIdentifierArch(string arch)
        {
            return arch switch
            {
                "x86" => "x86",
                "x86_32" => "x86",
                "x64" => "x64",
                "x86_64" => "x64",
                _ => throw new ArgumentOutOfRangeException(nameof(arch), arch, "Unexpected architecture")
            };
        }

        public override void _ExportEnd()
        {
            base._ExportEnd();

            string aotTempDir = Path.Combine(Path.GetTempPath(), $"godot-aot-{System.Environment.ProcessId}");

            if (Directory.Exists(aotTempDir))
                Directory.Delete(aotTempDir, recursive: true);

            foreach (string folder in _tempFolders)
            {
                Directory.Delete(folder, recursive: true);
            }
            _tempFolders.Clear();
        }

        /// <summary>
        /// Tries to determine the platform from the export preset's platform OS name.
        /// </summary>
        /// <param name="osName">Name of the export operating system.</param>
        /// <param name="platform">Platform name for the recognized supported platform.</param>
        /// <returns>
        /// <see langword="true"/> when the platform OS name is recognized as a supported platform,
        /// <see langword="false"/> otherwise.
        /// </returns>
        private static bool TryDeterminePlatformFromOSName(string osName, [NotNullWhen(true)] out string? platform)
        {
            if (OS.PlatformFeatureMap.TryGetValue(osName, out platform))
            {
                return true;
            }

            platform = null;
            return false;
        }

        private struct PublishConfig
        {
            public bool UseTempDir;
            public string RidOS;
            public HashSet<string> Archs;
            public string BuildConfig;
            public bool IncludeDebugSymbols;
        }
    }
}
