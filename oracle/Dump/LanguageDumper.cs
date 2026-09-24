using System;
using System.Collections.Generic;
using System.Reflection;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using HKOracle.Env;

namespace HKOracle.Dump
{
	// language.json: runtime localisation sheets read from Language.currentEntrySheets.
	// Used by the simulator to resolve GetLanguageString / GetLanguageStringProcessed
	// PlayMaker actions against real game text.
	public static class LanguageDumper
	{
		public static int SheetCount;
		public static int EntryCount;

		private static void Log(string m) => HKOracle.Instance.Log("[Dump] " + m);

		public static void Dump(string dir, string scene)
		{
			SheetCount = 0;
			EntryCount = 0;

			Type langType = typeof(global::Language.Language);

			string langCode = "UNKNOWN";
			try
			{
				langCode = global::Language.Language.CurrentLanguage().ToString();
			}
			catch (Exception ex)
			{
				try
				{
					var langField = langType.GetField("currentLanguage", BindingFlags.NonPublic | BindingFlags.Static);
					var val = langField?.GetValue(null);
					if (val != null) langCode = val.ToString();
				}
				catch { }
				if (langCode == "UNKNOWN")
				{
					langCode = $"error: {ex.Message}";
				}
			}

			FieldInfo sheetsField = null;
			try
			{
				sheetsField = langType.GetField("currentEntrySheets", BindingFlags.NonPublic | BindingFlags.Static);
			}
			catch (Exception ex)
			{
				string reason = $"failed to reflect currentEntrySheets field: {HKOracle.DescribeException(ex)}";
				Log($"LanguageDumper: {reason}");
				var unavail = new JObject
				{
					["__language"] = langCode,
					["__unavailable"] = reason,
				};
				ReflectionDumper.WriteJson(dir, "language.json", unavail, Formatting.Indented);
				return;
			}

			if (sheetsField == null)
			{
				string reason = "Language.currentEntrySheets field not found via reflection";
				Log($"LanguageDumper: {reason}");
				var unavail = new JObject
				{
					["__language"] = langCode,
					["__unavailable"] = reason,
				};
				ReflectionDumper.WriteJson(dir, "language.json", unavail, Formatting.Indented);
				return;
			}

			object rawValue = null;
			try
			{
				rawValue = sheetsField.GetValue(null);
			}
			catch (Exception ex)
			{
				string reason = $"Language.currentEntrySheets GetValue threw: {HKOracle.DescribeException(ex)}";
				Log($"LanguageDumper: {reason}");
				var unavail = new JObject
				{
					["__language"] = langCode,
					["__unavailable"] = reason,
				};
				ReflectionDumper.WriteJson(dir, "language.json", unavail, Formatting.Indented);
				return;
			}

			if (rawValue == null)
			{
				string reason = "Language.currentEntrySheets is null";
				Log($"LanguageDumper: {reason}");
				var unavail = new JObject
				{
					["__language"] = langCode,
					["__unavailable"] = reason,
				};
				ReflectionDumper.WriteJson(dir, "language.json", unavail, Formatting.Indented);
				return;
			}

			if (!(rawValue is Dictionary<string, Dictionary<string, string>> sheets))
			{
				string reason = $"Language.currentEntrySheets unexpected type: {rawValue.GetType().FullName}";
				Log($"LanguageDumper: {reason}");
				var unavail = new JObject
				{
					["__language"] = langCode,
					["__unavailable"] = reason,
				};
				ReflectionDumper.WriteJson(dir, "language.json", unavail, Formatting.Indented);
				return;
			}

			var root = new JObject
			{
				["__language"] = langCode,
			};

			foreach (var sheetKvp in sheets)
			{
				string sheetTitle = sheetKvp.Key;
				var entries = sheetKvp.Value;
				var sheetObj = new JObject();
				if (entries != null)
				{
					foreach (var entryKvp in entries)
					{
						sheetObj[entryKvp.Key] = entryKvp.Value;
						EntryCount++;
					}
				}
				root[sheetTitle] = sheetObj;
				SheetCount++;
			}

			ReflectionDumper.WriteJson(dir, "language.json", root, Formatting.Indented);
			Log($"language dump: {SheetCount} sheets, {EntryCount} entries (language={langCode})");
		}
	}
}
