using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace HKOracle.Record
{
	// Every managed call site whose result depends on wall-clock time, or on how many frames the player renders
	// rather than on game time (docs/frame-order.md "Wall-clock reads").  Pure Mono.Cecil: no UnityEngine type is
	// touched, so tools/wallclock_scan compiles this file on its own and reads the game's assemblies from disk
	// without loading them.  WallClockCounter (in the mod) hooks the same sites at runtime.
	public static class WallClockSites
	{
		public sealed class Site
		{
			public string Assembly;   // assembly name
			public string Type;       // declaring type of the method holding the call (Cecil FullName)
			public string Method;     // canonical Type.Method: an iterator's / closure's body is named after its method
			public string Full;       // Cecil FullName of the method holding the call (unique; the runtime hook key)
			public int Token;         // its metadata token
			public int Offset;        // IL offset of the call
			public int K;             // index of this call among the method's wall-clock calls, in IL order
			public string Api;        // e.g. UnityEngine.Time.get_unscaledTime
			public string Class;      // realtime | unscaled | frames | level_load | audio | datetime | stopwatch | tickcount
		}

		// Declaring type -> member -> class.  "*" = every member of the type.
		private static readonly Dictionary<string, Dictionary<string, string>> Apis = new Dictionary<string, Dictionary<string, string>>
		{
			{ "UnityEngine.Time", new Dictionary<string, string> {
				{ "get_realtimeSinceStartup", "realtime" }, { "get_realtimeSinceStartupAsDouble", "realtime" },
				{ "get_unscaledTime", "unscaled" }, { "get_unscaledTimeAsDouble", "unscaled" },
				{ "get_unscaledDeltaTime", "unscaled" }, { "get_fixedUnscaledTime", "unscaled" },
				{ "get_fixedUnscaledTimeAsDouble", "unscaled" }, { "get_fixedUnscaledDeltaTime", "unscaled" },
				{ "get_smoothDeltaTime", "frames" }, { "get_frameCount", "frames" }, { "get_renderedFrameCount", "frames" },
				{ "get_timeSinceLevelLoad", "level_load" }, { "get_timeSinceLevelLoadAsDouble", "level_load" } } },
			{ "HutongGames.PlayMaker.FsmTime", new Dictionary<string, string> { { "get_RealtimeSinceStartup", "realtime" } } },
			{ "UnityEngine.WaitForSecondsRealtime", new Dictionary<string, string> { { ".ctor", "realtime" } } },
			{ "UnityEngine.AudioSettings", new Dictionary<string, string> { { "get_dspTime", "audio" } } },
			// Audio plays in real time whatever Time.timeScale is, so a gameplay branch on playback state is a wall-clock read.
			{ "UnityEngine.AudioSource", new Dictionary<string, string> {
				{ "get_isPlaying", "audio" }, { "get_time", "audio" }, { "get_timeSamples", "audio" } } },
			{ "System.DateTime", new Dictionary<string, string> { { "get_Now", "datetime" }, { "get_UtcNow", "datetime" }, { "get_Today", "datetime" } } },
			{ "System.Diagnostics.Stopwatch", new Dictionary<string, string> { { "*", "stopwatch" } } },
			{ "System.Environment", new Dictionary<string, string> { { "get_TickCount", "tickcount" } } },
		};

		public static bool IsWallClock(MethodReference mr, out string api, out string cls)
		{
			api = cls = null;
			if (mr == null || mr.DeclaringType == null) return false;
			string t = mr.DeclaringType.FullName;
			if (!Apis.TryGetValue(t, out var members)) return false;
			if (!members.TryGetValue(mr.Name, out cls) && !members.TryGetValue("*", out cls)) return false;
			api = t + "." + mr.Name;
			return true;
		}

		// Type.Method with an iterator's MoveNext / a closure body named after the method that created it:
		// GrimmballControl/<Tween>d__5::MoveNext -> GrimmballControl.Tween; X/<>c__DisplayClass3_0::<Foo>b__0 -> X.Foo.
		public static string CanonicalMethod(MethodDefinition md)
		{
			TypeDefinition t = md.DeclaringType;
			string name = md.Name;
			if (name.StartsWith("<", StringComparison.Ordinal))
			{
				int close = name.IndexOf('>');
				if (close > 1) name = name.Substring(1, close - 1);
			}
			while (t.IsNested && t.Name.StartsWith("<", StringComparison.Ordinal))
			{
				int close = t.Name.IndexOf('>');
				if (close > 1 && name == "MoveNext") name = t.Name.Substring(1, close - 1);
				t = t.DeclaringType;
			}
			return t.FullName.Replace('/', '+') + "." + name;
		}

		public static List<Site> Scan(IEnumerable<string> assemblyPaths, IAssemblyResolver resolver)
		{
			var sites = new List<Site>();
			foreach (var path in assemblyPaths)
			{
				ModuleDefinition mod = ModuleDefinition.ReadModule(path, new ReaderParameters { AssemblyResolver = resolver, ReadSymbols = false });
				string asm = mod.Assembly.Name.Name;
				foreach (TypeDefinition td in mod.GetTypes())
				{
					foreach (MethodDefinition md in td.Methods)
					{
						if (!md.HasBody) continue;
						int k = 0;
						foreach (Instruction ins in md.Body.Instructions)
						{
							var code = ins.OpCode.Code;
							if (code != Code.Call && code != Code.Callvirt && code != Code.Newobj) continue;
							if (!IsWallClock(ins.Operand as MethodReference, out string api, out string cls)) continue;
							sites.Add(new Site
							{
								Assembly = asm, Type = td.FullName, Method = CanonicalMethod(md), Full = md.FullName,
								Token = md.MetadataToken.ToInt32(), Offset = ins.Offset, K = k++, Api = api, Class = cls,
							});
						}
					}
				}
			}
			return sites;
		}

		public static string ToJson(Site s)
		{
			var sb = new StringBuilder(256);
			sb.Append("{\"asm\":"); Str(sb, s.Assembly);
			sb.Append(",\"type\":"); Str(sb, s.Type);
			sb.Append(",\"m\":"); Str(sb, s.Method);
			sb.Append(",\"full\":"); Str(sb, s.Full);
			sb.Append(",\"il\":").Append(s.Offset);
			sb.Append(",\"k\":").Append(s.K);
			sb.Append(",\"api\":"); Str(sb, s.Api);
			sb.Append(",\"class\":"); Str(sb, s.Class);
			return sb.Append('}').ToString();
		}

		public static void Str(StringBuilder sb, string s)
		{
			sb.Append('"');
			foreach (char c in s ?? "")
			{
				if (c == '"' || c == '\\') sb.Append('\\').Append(c);
				else if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
				else sb.Append(c);
			}
			sb.Append('"');
		}

		// The assemblies whose code runs in a fight: the game's two script assemblies and PlayMaker.
		public static readonly string[] GameAssemblies = { "Assembly-CSharp.dll", "Assembly-CSharp-firstpass.dll", "PlayMaker.dll" };

		public static List<string> GamePaths(string managedDir)
		{
			var l = new List<string>();
			foreach (var n in GameAssemblies) l.Add(Path.Combine(managedDir, n));
			return l;
		}
	}
}
