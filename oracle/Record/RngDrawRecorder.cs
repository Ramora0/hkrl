using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using HutongGames.PlayMaker;
using Mono.Cecil;
using Mono.Cecil.Cil;
using MonoMod.Cil;
using MonoMod.RuntimeDetour;
using UnityEngine;
using CilOpCodes = Mono.Cecil.Cil.OpCodes;
using HKOracle.Env;

namespace HKOracle.Record
{
	// Records EVERY UnityEngine.Random draw the game's managed code makes, attributed to a canonical
	// site key, into <trace>.rngdraws.jsonl (format 2).  Installed in record mode (Mode.cs);
	// HK_ORACLE_RNG_DRAWS=0 disables it so a reference recording can prove it inert.
	//
	// HOW.  UnityEngine.Random's members are native icalls, so they are not hooked themselves.  Instead every
	// managed CALL SITE of UnityEngine.Random is found by reading the loaded assemblies' IL (Mono.Cecil), and
	// the calling method is IL-hooked: `Pre(this|null, site)` goes right before the call and `Post*(result,
	// site)` right after it.  Pre/Post only read Random.state (a pure getter) and write the log; the call itself
	// is untouched, so the game draws exactly what it drew before.  `site` indexes a table built while hooking:
	// the calling method and k, the index of this call among that method's Random calls in IL order.
	//
	// WHO OWNS A DRAW.  The owner is the `this` of the method that makes the call, when that is a Component,
	// a GameObject or a PlayMaker action (an iterator's / closure's <>4__this counts as its `this`).  Static
	// methods, struct methods and ScriptableObject / plain-class methods (FlingUtils.SpawnAndFling,
	// ActionHelpers.GetRandomWeightedIndex, AudioEvent.SelectPitch, RandomAudioClipTable.SelectPitch, ...) have
	// no owner of their own: each call to such a method from a method that HAS an owner is also IL-hooked with
	// PushOwner(this) / PopOwner(), so a draw inside it is owned by its innermost owning caller.
	//
	// THE KEY (hashed by FNV-1a exactly like hk_rng_site(), sim/core/rng.c):
	//   owner is a PlayMaker action:  ownerPath | fsmName | stateName | index of the action in its state
	//   anything else:                ownerPath | Type.Method | "" | k      (ownerPath "" when nothing owns it)
	// Type.Method is the method that contains the Random call (an iterator's MoveNext is named after the
	// iterator method; an overloaded name carries its parameter types).
	//
	// OWNER PATH (ScenePaths.Canon).  The simulator names an object by its path in the scene dump, which DumpDriver
	// takes at SceneReady, and never renames it (sim/fsm/fsm_world.c:1516 hashes sc->gos[go].path, a static table
	// string).  The game renames objects as it plays: ObjectPool.Spawn reparents a pooled clone out of
	// _GameManager/GlobalPool (ObjectPool.cs:488 `obj.parent = parent`, usually null) and Recycle puts it back
	// (:250), so the same FSM's draws were filed under 'Fireball2 Top(Clone)' or
	// '_GameManager/GlobalPool/Fireball2 Top(Clone)' depending on when they happened.  So the canonical owner
	// path is the object's path AT SceneReady (a snapshot of every transform, taken where the dump is taken);
	// an object created after SceneReady is named by its nearest snapshotted ancestor (or, if it is a pool
	// clone, by the pool) plus its own dynamic path below that.
	//
	// WHAT IS WRITTEN.  One line per Random call that consumed at least one word:
	//   {"q":seq,"f":frame,"fc":fixed_count,"ep":episode,"kind":"pm"|"cs","o":owner,"n":fsm-or-method,
	//    "s":state,"i":index-or-k,"m":method,"k":k,"api":api,"t":owner type,"via":"self"|"ctx"|"none",
	//    "b":[state before],"w":[words drawn],"v":result}
	// The words are recovered by stepping xorshift128 from the state before the call to the state after it,
	// so a call that draws several words (insideUnitCircle, ColorHSV, ...) lists all of them.  Markers:
	// {"ev":"scene_ready"} (the episode boundary: the sim restores the dump taken there), "reset_begin",
	// "episode_end", "reseed" (InitState / state setter at a hooked site), and "gap": the state moved between
	// two hooked calls, i.e. a draw no hook saw (native code, or a call site this scan missed).  n words if
	// the new state is reachable, a jump otherwise (Random.InitState from TraceRecorder at sceneLoaded).
	public static class RngDrawRecorder
	{
		public const int FormatVersion = 2;

		private static string _path;
		private static FileStream _fs;
		private static StreamWriter _writer;
		private static int _sinceFlush;
		private static float _lastFlushRealtime;
		private const int kFlushEvery = 256;
		private const float kFlushEverySeconds = 2f;

		private static readonly StringBuilder _sb = new StringBuilder(512);
		private static readonly Dictionary<string, int> _errCounts = new Dictionary<string, int>();

		// ---- site table (index = the int baked into each hooked call site)
		private sealed class Site
		{
			public string Method;   // canonical Type.Method of the method containing the call
			public int K;           // index among that method's drawing Random calls, IL order (-1 for reseeds)
			public string Api;      // e.g. Range(Single,Single)
			public bool Reseed;
		}
		private static readonly List<Site> _sites = new List<Site>();
		private static readonly List<ILHook> _ilHooks = new List<ILHook>();
		private static MethodInfo _miPre, _miPush, _miPop, _miPostVoid, _miPostF, _miPostI, _miPostV2, _miPostV3, _miPostQ, _miPostC;

		// ---- Pre/Post state
		private static int _preSite = -1;
		private static object _preSelf;
		private static uint _b0, _b1, _b2, _b3;
		private static bool _haveLast;
		private static uint _l0, _l1, _l2, _l3;
		private static int _mainThread;

		// ---- owner context (innermost owning caller of an owner-less drawing method)
		private static object[] _ctx = new object[256];
		private static int _ctxN;
		private static int _ctxLeaks;

		private static readonly Dictionary<Type, FieldInfo> _thisField = new Dictionary<Type, FieldInfo>();
		private static int _episode = -1;

		// ---- counters
		private static long _seq, _nCalls, _nWords, _nGaps, _nGapWords, _nJumps, _nOffThread;


		private static void Log(string m) => HKOracle.Instance.Log($"[RngDraws] {m}");

		private static void Err(string where, Exception e)
		{
			_errCounts.TryGetValue(where, out int n);
			_errCounts[where] = n + 1;
			if (n < 5) Log($"ERROR in {where}: {HKOracle.DescribeException(e)}");
		}

		public static void Install()
		{
			if (System.Environment.GetEnvironmentVariable("HK_ORACLE_RNG_DRAWS") == "0") { Log("disabled (HK_ORACLE_RNG_DRAWS=0)"); return; }
			_path = Mode.SidePath(".rngdraws.jsonl");

			try
			{
				string dir = Path.GetDirectoryName(_path);
				if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
				_fs = new FileStream(_path, FileMode.Create, FileAccess.Write, FileShare.Read, 1 << 16);
				_writer = new StreamWriter(_fs, new UTF8Encoding(false), 1 << 16);
			}
			catch (Exception e) { Err("OpenFile", e); _writer = null; return; }

			_mainThread = System.Threading.Thread.CurrentThread.ManagedThreadId;
			try { RngState.Resolve(); Log($"RNG state reader bound ({RngState.Describe})"); }
			catch (Exception e) { Err("ResolveRngReader", e); }
			BindTargets();

			_sb.Length = 0;
			_sb.Append("{\"rngdraws\":").Append(FormatVersion).Append(",\"recorder\":\"callsite-ilhook\",\"dll_sha256\":");
			AppendJsonString(_sb, SelfHash());
			_sb.Append(",\"mod_commit\":");
			AppendJsonString(_sb, ModInfo.Commit);
			_sb.Append(",\"unity\":");
			AppendJsonString(_sb, Application.unityVersion);
			_sb.Append('}');
			EmitLine();

			var go = new GameObject("HKOracle.RngDrawRecorder");
			UnityEngine.Object.DontDestroyOnLoad(go);
			go.hideFlags = HideFlags.HideAndDontSave;
			go.AddComponent<RngDrawRecorderBehaviour>();

			Hooks.ResetBegin += OnResetBegin;
			Hooks.SceneReady += OnSceneReady;
			Hooks.EpisodeEnd += OnEpisodeEnd;

			InstallCallSiteHooks();
			Log($"writing {_path}");
		}

		private static string SelfHash()
		{
			try
			{
				using (var sha = SHA256.Create())
				using (var f = File.OpenRead(typeof(RngDrawRecorder).Assembly.Location))
				{
					var h = sha.ComputeHash(f);
					var s = new StringBuilder(64);
					foreach (byte b in h) s.Append(b.ToString("x2"));
					return s.ToString();
				}
			}
			catch { return ""; }
		}

		// =====================================================================================  scan + hook

		private sealed class MInfo
		{
			public int Token;
			public Assembly Asm;
			public bool Ownerless;
			public bool Draws;        // contains a UnityEngine.Random draw or reseed call
			public List<string> Callees;
		}

		private static readonly HashSet<string> _drawOwnerless = new HashSet<string>();   // method keys (Cecil FullName)

		// 1 = read and hook, 0 = skip.  Unity's engine modules are not read: a Random call in them (an offline scan
		// finds three: WWWForm..ctor, UnityWebRequest.GenerateBoundary, none on a gameplay path) could not be
		// attributed anyway, and would show up as a "gap" (the state moved between two hooked calls).  Reading
		// all 75 assemblies cost 11 s and a heap that slowed every later scene load ~20x (Boehm GC).
		private static int ScanMode(Assembly asm)
		{
			if (asm == typeof(RngDrawRecorder).Assembly) return 1;
			string n;
			try { n = asm.GetName().Name; } catch { return 0; }
			try { if (string.IsNullOrEmpty(asm.Location)) return 0; } catch { return 0; }
			if (n == "Unity.Timeline") return 1;
			if (n.StartsWith("UnityEngine", StringComparison.Ordinal) || n.StartsWith("Unity.", StringComparison.Ordinal)) return 0;
			if (n == "mscorlib" || n == "netstandard" || n == "websocket-sharp" || n.StartsWith("System", StringComparison.Ordinal)
				|| n.StartsWith("Mono.", StringComparison.Ordinal) || n.StartsWith("MonoMod", StringComparison.Ordinal)
				|| n.StartsWith("MMHOOK", StringComparison.Ordinal) || n.StartsWith("Newtonsoft", StringComparison.Ordinal)
				|| n.StartsWith("Microsoft", StringComparison.Ordinal)) return 0;
			return 1;
		}

		private static bool IsRandomType(TypeReference t) => t != null && t.FullName == "UnityEngine.Random";

		private static bool IsReseedName(string name) => name == "InitState" || name == "set_state" || name == "set_seed";

		private static bool IsDrawName(string name) => !(IsReseedName(name) || name == "get_state" || name == "get_seed" || name == ".ctor" || name == ".cctor");

		private static string RefKey(MethodReference mr)
		{
			try
			{
				if (mr is GenericInstanceMethod gim) return gim.ElementMethod.FullName;
				return mr.FullName;
			}
			catch { return null; }
		}

		private static bool OwnerRootName(string n) =>
			n == "UnityEngine.Component" || n == "UnityEngine.Behaviour" || n == "UnityEngine.MonoBehaviour"
			|| n == "UnityEngine.GameObject" || n == "HutongGames.PlayMaker.FsmStateAction";

		private static bool DerivesFromOwnerRoot(TypeReference tr)
		{
			for (int guard = 0; tr != null && guard < 40; guard++)
			{
				string n = tr is GenericInstanceType git ? git.ElementType.FullName : tr.FullName;
				if (OwnerRootName(n)) return true;
				if (n == "System.Object" || n == "UnityEngine.Object" || n == "UnityEngine.ScriptableObject" || n == "System.ValueType") return false;
				TypeDefinition d;
				try { d = tr.Resolve(); } catch { return false; }
				if (d == null) return false;
				tr = d.BaseType;
			}
			return false;
		}

		private static bool CecilOwnerful(TypeDefinition td)
		{
			if (td.IsValueType) return false;
			if (td.IsNested && td.Name.StartsWith("<", StringComparison.Ordinal))
			{
				foreach (var f in td.Fields)
					if (f.Name == "<>4__this") return DerivesFromOwnerRoot(f.FieldType);
				return false;
			}
			return DerivesFromOwnerRoot(td);
		}

		private static void InstallCallSiteHooks()
		{
			var sw = System.Diagnostics.Stopwatch.StartNew();
			var infos = new Dictionary<string, MInfo>();
			var resolver = new DefaultAssemblyResolver();
			var managed = Path.GetDirectoryName(typeof(GameObject).Assembly.Location);
			if (!string.IsNullOrEmpty(managed)) resolver.AddSearchDirectory(managed);
			var modsDir = Path.GetDirectoryName(typeof(RngDrawRecorder).Assembly.Location);
			if (!string.IsNullOrEmpty(modsDir)) resolver.AddSearchDirectory(modsDir);
			int nAsm = 0, nMethods = 0;
			var scanned = new List<string>();
			foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
			{
				int mode = ScanMode(asm);
				if (mode == 0) continue;
				ModuleDefinition mod;
				try { mod = ModuleDefinition.ReadModule(asm.Location, new ReaderParameters { AssemblyResolver = resolver, ReadSymbols = false }); }
				catch (Exception e) { Err("ReadModule " + asm.GetName().Name, e); continue; }
				nAsm++;
				scanned.Add(asm.GetName().Name);
				foreach (TypeDefinition td in mod.GetTypes())
				{
					if (td.FullName.StartsWith(typeof(RngDrawRecorder).FullName, StringComparison.Ordinal)
						|| td.FullName.StartsWith(typeof(Dump.SceneDumper).FullName, StringComparison.Ordinal)) continue;
					bool ownerfulType;
					try { ownerfulType = CecilOwnerful(td); } catch { ownerfulType = false; }
					foreach (MethodDefinition md in td.Methods)
					{
						if (!md.HasBody) continue;
						nMethods++;
						var mi = new MInfo { Token = md.MetadataToken.ToInt32(), Asm = asm, Ownerless = md.IsStatic || !ownerfulType };
						foreach (var ins in md.Body.Instructions)
						{
							var code = ins.OpCode.Code;
							if (code != Code.Call && code != Code.Callvirt && code != Code.Newobj) continue;
							if (!(ins.Operand is MethodReference mr)) continue;
							if (IsRandomType(mr.DeclaringType))
							{
								if (IsDrawName(mr.Name) || IsReseedName(mr.Name)) mi.Draws = true;
								continue;
							}
							string k = RefKey(mr);
							if (k == null) continue;
							(mi.Callees ?? (mi.Callees = new List<string>())).Add(k);
						}
						string key = md.FullName;
						if (!infos.ContainsKey(key)) infos[key] = mi;
					}
				}
			}

			// Owner-less methods that draw, directly or through other owner-less methods.
			foreach (var kv in infos) if (kv.Value.Ownerless && kv.Value.Draws) _drawOwnerless.Add(kv.Key);
			for (bool grew = true; grew;)
			{
				grew = false;
				foreach (var kv in infos)
				{
					var mi = kv.Value;
					if (!mi.Ownerless || mi.Callees == null || _drawOwnerless.Contains(kv.Key)) continue;
					foreach (var c in mi.Callees)
						if (_drawOwnerless.Contains(c)) { _drawOwnerless.Add(kv.Key); grew = true; break; }
				}
			}

			int nDirect = 0, nCtx = 0, nHooked = 0;
			var failed = new List<string>();
			foreach (var kv in infos)
			{
				var mi = kv.Value;
				bool ctxSite = false;
				if (!mi.Ownerless && mi.Callees != null)
					foreach (var c in mi.Callees) if (_drawOwnerless.Contains(c)) { ctxSite = true; break; }
				if (!mi.Draws && !ctxSite) continue;
				if (mi.Draws) nDirect++;
				if (ctxSite) nCtx++;
				MethodBase mb = null;
				try { mb = mi.Asm.ManifestModule.ResolveMethod(mi.Token); }
				catch (Exception e) { failed.Add(kv.Key + " (resolve: " + e.GetType().Name + ")"); continue; }
				if (mb == null) { failed.Add(kv.Key + " (resolve: null)"); continue; }
				if (mb.IsGenericMethodDefinition || (mb.DeclaringType != null && mb.DeclaringType.ContainsGenericParameters))
				{
					failed.Add(kv.Key + " (generic)");
					continue;
				}
				try
				{
					var target = mb;
					_ilHooks.Add(new ILHook(target, il => Manipulate(il, target)));
					nHooked++;
				}
				catch (Exception e) { failed.Add(kv.Key + " (" + e.GetType().Name + ": " + e.Message + ")"); }
			}

			int drawSites = 0, reseedSites = 0;
			foreach (var s in _sites) { if (s.Reseed) reseedSites++; else drawSites++; }
			Log($"scanned {nAsm} assemblies [{string.Join(",", scanned.ToArray())}], {nMethods} methods in {sw.ElapsedMilliseconds} ms: "
				+ $"{nDirect} call Random, {_drawOwnerless.Count} owner-less drawing methods, {nCtx} owner context sites; "
				+ $"hooked {nHooked} methods, {drawSites} draw sites + {reseedSites} reseed sites, {failed.Count} FAILED");
			foreach (var f in failed) Log("UNHOOKED " + f);

			// The site table goes into the log too: it is the list of every managed Random call site the game has.
			_sb.Length = 0;
			_sb.Append("{\"ev\":\"install\",\"assemblies\":").Append(nAsm).Append(",\"methods\":").Append(nMethods)
				.Append(",\"hooked\":").Append(nHooked).Append(",\"draw_sites\":").Append(drawSites)
				.Append(",\"reseed_sites\":").Append(reseedSites).Append(",\"ms\":").Append(sw.ElapsedMilliseconds)
				.Append(",\"unhooked\":[");
			for (int i = 0; i < failed.Count; i++) { if (i > 0) _sb.Append(','); AppendJsonString(_sb, failed[i]); }
			_sb.Append("]}");
			EmitLine();
			for (int i = 0; i < _sites.Count; i++)
			{
				var s = _sites[i];
				_sb.Length = 0;
				_sb.Append("{\"ev\":\"site\",\"id\":").Append(i).Append(",\"m\":");
				AppendJsonString(_sb, s.Method);
				_sb.Append(",\"k\":").Append(s.K).Append(",\"api\":");
				AppendJsonString(_sb, s.Api);
				_sb.Append('}');
				EmitLine();
			}
			Flush();
			infos.Clear();
			GC.Collect();
		}

		private static bool RuntimeOwnerful(Type t)
		{
			if (t == null || t.IsValueType) return false;
			if (typeof(Component).IsAssignableFrom(t) || typeof(FsmStateAction).IsAssignableFrom(t) || typeof(GameObject).IsAssignableFrom(t)) return true;
			if (t.IsNested && t.Name.StartsWith("<", StringComparison.Ordinal))
			{
				var f = t.GetField("<>4__this", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
				return f != null && RuntimeOwnerful(f.FieldType);
			}
			return false;
		}

		private static string TypeKey(Type t)
		{
			// Unity/C# display name: nested types joined by '+', namespaces kept.
			return t == null ? "" : (t.FullName ?? t.Name).Replace('/', '+');
		}

		private static string MethodKey(MethodBase mb)
		{
			Type dt = mb.DeclaringType;
			string name = mb.Name;
			// An iterator's body is its MoveNext; name it after the iterator method (the method itself holds no
			// Random call, so no key collides): GrimmballControl+<Tween>d__5.MoveNext -> GrimmballControl.Tween.
			if (dt != null && dt.IsNested && name == "MoveNext" && dt.Name.StartsWith("<", StringComparison.Ordinal))
			{
				int close = dt.Name.IndexOf('>');
				if (close > 1) return TypeKey(dt.DeclaringType) + "." + dt.Name.Substring(1, close - 1);
			}
			string key = TypeKey(dt) + "." + name;
			try
			{
				int same = 0;
				foreach (var m in dt.GetMethods(BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
					if (m.Name == name) same++;
				if (mb is ConstructorInfo)
					same = dt.GetConstructors(BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic).Length;
				if (same > 1)
				{
					var ps = mb.GetParameters();
					var s = new StringBuilder(key).Append('(');
					for (int i = 0; i < ps.Length; i++) { if (i > 0) s.Append(','); s.Append(ps[i].ParameterType.Name); }
					key = s.Append(')').ToString();
				}
			}
			catch { }
			return key;
		}

		private static string ApiName(MethodReference mr)
		{
			var s = new StringBuilder(mr.Name);
			if (mr.HasParameters)
			{
				s.Append('(');
				for (int i = 0; i < mr.Parameters.Count; i++) { if (i > 0) s.Append(','); s.Append(mr.Parameters[i].ParameterType.Name); }
				s.Append(')');
			}
			return s.ToString();
		}

		private static int AddSite(string method, int k, string api, bool reseed)
		{
			lock (_sites)
			{
				_sites.Add(new Site { Method = method, K = k, Api = api, Reseed = reseed });
				return _sites.Count - 1;
			}
		}

		private static void BindTargets()
		{
			const BindingFlags F = BindingFlags.Public | BindingFlags.Static;
			var t = typeof(RngDrawRecorder);
			_miPre = t.GetMethod(nameof(Pre), F);
			_miPush = t.GetMethod(nameof(PushOwner), F);
			_miPop = t.GetMethod(nameof(PopOwner), F);
			_miPostVoid = t.GetMethod(nameof(PostVoid), F);
			_miPostF = t.GetMethod(nameof(PostF), F);
			_miPostI = t.GetMethod(nameof(PostI), F);
			_miPostV2 = t.GetMethod(nameof(PostV2), F);
			_miPostV3 = t.GetMethod(nameof(PostV3), F);
			_miPostQ = t.GetMethod(nameof(PostQ), F);
			_miPostC = t.GetMethod(nameof(PostC), F);
		}

		// Labels that targeted `ins` were moved onto `first` by MoveType.AfterLabel; exception-handler
		// boundaries are plain Instruction references that ILCursor does not retarget, so do it here.
		private static void FixHandlers(ILContext il, Instruction ins, Instruction first)
		{
			if (!il.Body.HasExceptionHandlers) return;
			foreach (var h in il.Body.ExceptionHandlers)
			{
				if (h.TryStart == ins) h.TryStart = first;
				if (h.TryEnd == ins) h.TryEnd = first;
				if (h.HandlerStart == ins) h.HandlerStart = first;
				if (h.HandlerEnd == ins) h.HandlerEnd = first;
				if (h.FilterStart == ins) h.FilterStart = first;
			}
		}

		private static void Manipulate(ILContext il, MethodBase mb)
		{
			// `this` is passed only where it is a constructed reference: not static, not a struct (ldarg.0 is a
			// byref there), not a constructor (field initialisers run before the base constructor).
			bool objThis = !mb.IsStatic && !mb.IsConstructor && mb.DeclaringType != null && !mb.DeclaringType.IsValueType;
			bool ownerful = objThis && RuntimeOwnerful(mb.DeclaringType);
			string methodKey = MethodKey(mb);
			var targets = new List<Instruction>();
			foreach (var ins in il.Instrs)
			{
				var code = ins.OpCode.Code;
				if ((code == Code.Call || code == Code.Callvirt || code == Code.Newobj) && ins.Operand is MethodReference)
					targets.Add(ins);
			}
			var c = new ILCursor(il);
			int k = 0;
			foreach (var ins in targets)
			{
				var mr = (MethodReference)ins.Operand;
				if (IsRandomType(mr.DeclaringType))
				{
					bool reseed = IsReseedName(mr.Name);
					if (!reseed && !IsDrawName(mr.Name)) continue;
					int site = AddSite(methodKey, reseed ? -1 : k++, ApiName(mr), reseed);
					c.Goto(ins, MoveType.AfterLabel);
					c.Emit(objThis ? CilOpCodes.Ldarg_0 : CilOpCodes.Ldnull);
					var first = c.Prev;
					c.Emit(CilOpCodes.Ldc_I4, site);
					c.Emit(CilOpCodes.Call, _miPre);
					FixHandlers(il, ins, first);
					c.Goto(ins, MoveType.After);
					MethodInfo post;
					switch (mr.ReturnType.FullName)
					{
						case "System.Single": post = _miPostF; break;
						case "System.Int32": post = _miPostI; break;
						case "UnityEngine.Vector2": post = _miPostV2; break;
						case "UnityEngine.Vector3": post = _miPostV3; break;
						case "UnityEngine.Quaternion": post = _miPostQ; break;
						case "UnityEngine.Color": post = _miPostC; break;
						default: post = null; break;
					}
					if (post != null)
					{
						c.Emit(CilOpCodes.Dup);
						c.Emit(CilOpCodes.Ldc_I4, site);
						c.Emit(CilOpCodes.Call, post);
					}
					else
					{
						c.Emit(CilOpCodes.Ldc_I4, site);
						c.Emit(CilOpCodes.Call, _miPostVoid);
					}
				}
				else if (ownerful)
				{
					string key = RefKey(mr);
					if (key == null || !_drawOwnerless.Contains(key)) continue;
					c.Goto(ins, MoveType.AfterLabel);
					c.Emit(CilOpCodes.Ldarg_0);
					var first = c.Prev;
					c.Emit(CilOpCodes.Call, _miPush);
					FixHandlers(il, ins, first);
					c.Goto(ins, MoveType.After);
					c.Emit(CilOpCodes.Call, _miPop);
				}
			}
		}

		// =====================================================================================  runtime

		public static void PushOwner(object self)
		{
			if (_ctxN >= _ctx.Length) Array.Resize(ref _ctx, _ctx.Length * 2);
			_ctx[_ctxN++] = self;
		}

		public static void PopOwner()
		{
			if (_ctxN > 0) _ctx[--_ctxN] = null;
		}

		public static void Pre(object self, int site)
		{
			if (_writer == null) return;
			try
			{
				RngState.Read(out _b0, out _b1, out _b2, out _b3);
				_preSite = site;
				_preSelf = self;
				if (_haveLast && (_b0 != _l0 || _b1 != _l1 || _b2 != _l2 || _b3 != _l3)) Gap();
			}
			catch (Exception e) { Err("Pre", e); }
		}

		public static void PostVoid(int site) { Post(site, 0); }
		public static void PostF(float v, int site) { Post(site, 1, v); }
		public static void PostI(int v, int site) { Post(site, 2, 0f, v); }
		public static void PostV2(Vector2 v, int site) { Post(site, 3, v.x, 0, v.y); }
		public static void PostV3(Vector3 v, int site) { Post(site, 4, v.x, 0, v.y, v.z); }
		public static void PostQ(Quaternion v, int site) { Post(site, 5, v.x, 0, v.y, v.z, v.w); }
		public static void PostC(Color v, int site) { Post(site, 5, v.r, 0, v.g, v.b, v.a); }

		private static void Post(int site, int kind, float f0 = 0f, int i0 = 0, float f1 = 0f, float f2 = 0f, float f3 = 0f)
		{
			if (_writer == null) return;
			try
			{
				if (site != _preSite) { Err("Post", new InvalidOperationException($"site {site} != pre {_preSite}")); }
				uint x0, x1, x2, x3;
				RngState.Read(out x0, out x1, out x2, out x3);
				var S = (site >= 0 && site < _sites.Count) ? _sites[site] : null;
				object self = _preSelf;
				_preSelf = null;
				_preSite = -1;
				if (System.Threading.Thread.CurrentThread.ManagedThreadId != _mainThread) _nOffThread++;

				if (S != null && S.Reseed)
				{
					_sb.Length = 0;
					_sb.Append("{\"ev\":\"reseed\",\"q\":").Append(_seq++).Append(",\"f\":").Append(Time.frameCount)
						.Append(",\"m\":");
					AppendJsonString(_sb, S.Method);
					_sb.Append(",\"api\":");
					AppendJsonString(_sb, S.Api);
					_sb.Append(",\"a\":[").Append(x0).Append(',').Append(x1).Append(',').Append(x2).Append(',').Append(x3).Append("]}");
					EmitLine();
					SetLast(x0, x1, x2, x3);
					return;
				}

				// words drawn: step from the state before the call to the state after it
				uint y0 = _b0, y1 = _b1, y2 = _b2, y3 = _b3;
				int n = 0;
				const int cap = 64;
				uint[] words = _words;
				while (!(y0 == x0 && y1 == x1 && y2 == x2 && y3 == x3) && n < cap)
				{
					uint t = y0 ^ (y0 << 11);
					y0 = y1; y1 = y2; y2 = y3;
					y3 = y3 ^ (y3 >> 19) ^ t ^ (t >> 8);
					words[n++] = y3;
				}
				bool reachable = y0 == x0 && y1 == x1 && y2 == x2 && y3 == x3;
				SetLast(x0, x1, x2, x3);
				if (reachable && n == 0) return;   // Range(int) with min == max draws nothing
				_nCalls++;
				_nWords += n;

				// owner
				string via = "self";
				object owner = Unwrap(self);
				if (!IsOwner(owner))
				{
					owner = null;
					via = "none";
					for (int i = _ctxN - 1; i >= 0; i--)
					{
						var o = Unwrap(_ctx[i]);
						if (IsOwner(o)) { owner = o; via = "ctx"; break; }
					}
				}

				_sb.Length = 0;
				_sb.Append("{\"q\":").Append(_seq++).Append(",\"f\":").Append(Time.frameCount)
					.Append(",\"fc\":").Append(TraceRecorder.FixedCount).Append(",\"ep\":").Append(_episode);
				if (owner is FsmStateAction a)
				{
					string path = "", fsm = "", state = "";
					int idx = -1;
					try
					{
						var fs = a.Fsm;
						if (fs != null) { path = ScenePaths.Canon(fs.GameObject); fsm = fs.Name ?? ""; }
						var st = a.State ?? fs?.ActiveState;
						if (st != null) { state = st.Name ?? ""; if (st.Actions != null) idx = Array.IndexOf(st.Actions, a); }
					}
					catch (Exception e) { Err("pm-key", e); }
					_sb.Append(",\"kind\":\"pm\",\"o\":");
					AppendJsonString(_sb, path);
					_sb.Append(",\"n\":");
					AppendJsonString(_sb, fsm);
					_sb.Append(",\"s\":");
					AppendJsonString(_sb, state);
					_sb.Append(",\"i\":").Append(idx);
				}
				else
				{
					string path = "";
					try
					{
						if (owner is Component comp) path = ScenePaths.Canon(comp.gameObject);
						else if (owner is GameObject g) path = ScenePaths.Canon(g);
					}
					catch (Exception e) { Err("cs-key", e); }
					_sb.Append(",\"kind\":\"cs\",\"o\":");
					AppendJsonString(_sb, path);
					_sb.Append(",\"n\":");
					AppendJsonString(_sb, S?.Method ?? "");
					_sb.Append(",\"s\":\"\",\"i\":").Append(S?.K ?? -1);
				}
				_sb.Append(",\"m\":");
				AppendJsonString(_sb, S?.Method ?? "");
				_sb.Append(",\"k\":").Append(S?.K ?? -1).Append(",\"api\":");
				AppendJsonString(_sb, S?.Api ?? "");
				_sb.Append(",\"t\":");
				AppendJsonString(_sb, owner != null ? owner.GetType().Name : "");
				_sb.Append(",\"via\":\"").Append(via).Append('"');
				_sb.Append(",\"b\":[").Append(_b0).Append(',').Append(_b1).Append(',').Append(_b2).Append(',').Append(_b3).Append(']');
				if (reachable)
				{
					_sb.Append(",\"w\":[");
					for (int i = 0; i < n; i++) { if (i > 0) _sb.Append(','); _sb.Append(words[i]); }
					_sb.Append(']');
				}
				else
				{
					_sb.Append(",\"w\":null,\"a\":[").Append(x0).Append(',').Append(x1).Append(',').Append(x2).Append(',').Append(x3).Append(']');
				}
				switch (kind)
				{
					case 1: _sb.Append(",\"v\":").Append(f0.ToString("R", CultureInfo.InvariantCulture)); break;
					case 2: _sb.Append(",\"v\":").Append(i0); break;
					case 3: _sb.Append(",\"v\":[").Append(F(f0)).Append(',').Append(F(f1)).Append(']'); break;
					case 4: _sb.Append(",\"v\":[").Append(F(f0)).Append(',').Append(F(f1)).Append(',').Append(F(f2)).Append(']'); break;
					case 5: _sb.Append(",\"v\":[").Append(F(f0)).Append(',').Append(F(f1)).Append(',').Append(F(f2)).Append(',').Append(F(f3)).Append(']'); break;
				}
				_sb.Append('}');
				EmitLine();
			}
			catch (Exception e) { Err("Post", e); }
		}

		private static readonly uint[] _words = new uint[64];

		private static string F(float v) => v.ToString("R", CultureInfo.InvariantCulture);

		private static void SetLast(uint a, uint b, uint c, uint d) { _l0 = a; _l1 = b; _l2 = c; _l3 = d; _haveLast = true; }

		// The state changed between the previous hooked call and this one: something drew (or reseeded) where
		// no hook sees it.
		private static void Gap()
		{
			uint y0 = _l0, y1 = _l1, y2 = _l2, y3 = _l3;
			int n = 0;
			const int cap = 1 << 16;
			while (!(y0 == _b0 && y1 == _b1 && y2 == _b2 && y3 == _b3) && n < cap)
			{
				uint t = y0 ^ (y0 << 11);
				y0 = y1; y1 = y2; y2 = y3;
				y3 = y3 ^ (y3 >> 19) ^ t ^ (t >> 8);
				n++;
			}
			bool reachable = n < cap;
			_nGaps++;
			if (reachable) _nGapWords += n; else _nJumps++;
			_sb.Length = 0;
			_sb.Append("{\"ev\":\"gap\",\"q\":").Append(_seq++).Append(",\"f\":").Append(Time.frameCount)
				.Append(",\"fc\":").Append(TraceRecorder.FixedCount).Append(",\"ep\":").Append(_episode)
				.Append(",\"n\":").Append(reachable ? n : -1)
				.Append(",\"from\":[").Append(_l0).Append(',').Append(_l1).Append(',').Append(_l2).Append(',').Append(_l3).Append(']')
				.Append(",\"to\":[").Append(_b0).Append(',').Append(_b1).Append(',').Append(_b2).Append(',').Append(_b3).Append("]}");
			EmitLine();
		}

		private static bool IsOwner(object o) => o is Component || o is GameObject || o is FsmStateAction;

		private static object Unwrap(object o)
		{
			for (int guard = 0; o != null && guard < 8; guard++)
			{
				if (o is Component || o is GameObject || o is FsmStateAction) return o;
				Type t = o.GetType();
				if (!t.IsNested || !t.Name.StartsWith("<", StringComparison.Ordinal)) return o;
				FieldInfo f;
				if (!_thisField.TryGetValue(t, out f))
				{
					f = t.GetField("<>4__this", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
					_thisField[t] = f;
				}
				if (f == null) return o;
				o = f.GetValue(o);
			}
			return o;
		}


		// ---- markers

		private static void OnSceneReady(Hooks.SceneContext ctx)
		{
			try
			{
				int nsnap = ScenePaths.Snapshot();
				_episode++;
				uint s0, s1, s2, s3;
				RngState.Read(out s0, out s1, out s2, out s3);
				_sb.Length = 0;
				_sb.Append("{\"ev\":\"scene_ready\",\"q\":").Append(_seq++).Append(",\"f\":").Append(Time.frameCount)
					.Append(",\"fc\":").Append(TraceRecorder.FixedCount).Append(",\"ep\":").Append(_episode)
					.Append(",\"reset\":").Append(ctx?.ResetCount ?? -1).Append(",\"level\":");
				AppendJsonString(_sb, ctx?.Level ?? "");
				_sb.Append(",\"snapshot\":").Append(nsnap)
					.Append(",\"state\":[").Append(s0).Append(',').Append(s1).Append(',').Append(s2).Append(',').Append(s3).Append("]}");
				EmitLine();
				Flush();
			}
			catch (Exception e) { Err("OnSceneReady", e); }
		}

		private static void Marker(string ev, string info)
		{
			if (_writer == null) return;
			_sb.Length = 0;
			_sb.Append("{\"ev\":\"").Append(ev).Append("\",\"q\":").Append(_seq++).Append(",\"f\":").Append(Time.frameCount)
				.Append(",\"fc\":").Append(TraceRecorder.FixedCount).Append(",\"ep\":").Append(_episode).Append(",\"info\":");
			AppendJsonString(_sb, info ?? "");
			_sb.Append('}');
			EmitLine();
			Flush();
		}

		private static void OnResetBegin(string level) { try { Marker("reset_begin", level); } catch { } }
		private static void OnEpisodeEnd(string info) { try { Marker("episode_end", info); } catch { } }


		// ---- output

		private static void AppendJsonString(StringBuilder sb, string s)
		{
			if (s == null) { sb.Append("null"); return; }
			sb.Append('"');
			foreach (char ch in s)
			{
				switch (ch)
				{
					case '"': sb.Append("\\\""); break;
					case '\\': sb.Append("\\\\"); break;
					case '\n': sb.Append("\\n"); break;
					case '\r': sb.Append("\\r"); break;
					case '\t': sb.Append("\\t"); break;
					default:
						if (ch < ' ') sb.Append("\\u").Append(((int)ch).ToString("x4"));
						else sb.Append(ch);
						break;
				}
			}
			sb.Append('"');
		}

		private static void EmitLine()
		{
			if (_writer == null) return;
			_writer.WriteLine(_sb.ToString());
			if (++_sinceFlush >= kFlushEvery)
			{
				float now = Time.realtimeSinceStartup;
				if (now - _lastFlushRealtime >= kFlushEverySeconds) Flush();
			}
		}

		public static void Flush()
		{
			if (_writer == null) return;
			try { _writer.Flush(); _fs?.Flush(true); } catch { }
			_sinceFlush = 0;
			_lastFlushRealtime = Time.realtimeSinceStartup;
		}

		public static void Close()
		{
			if (_writer == null) return;
			try
			{
				_sb.Length = 0;
				_sb.Append("{\"ev\":\"close\",\"calls\":").Append(_nCalls).Append(",\"words\":").Append(_nWords)
					.Append(",\"gaps\":").Append(_nGaps).Append(",\"gap_words\":").Append(_nGapWords)
					.Append(",\"jumps\":").Append(_nJumps).Append(",\"ctx_leaks\":").Append(_ctxLeaks)
					.Append(",\"off_thread\":").Append(_nOffThread).Append(",\"errors\":").Append(_errCounts.Count).Append('}');
				EmitLine();
				Log($"closing {_path}: {_nCalls} calls, {_nWords} words, {_nGaps} gaps ({_nGapWords} unhooked words, {_nJumps} jumps), {_ctxLeaks} context leaks");
				Flush();
				_writer.Close();
				_fs?.Close();
			}
			catch (Exception e) { Err("Close", e); }
			finally { _writer = null; _fs = null; }
		}

		private sealed class RngDrawRecorderBehaviour : MonoBehaviour
		{
			// Every PushOwner is paired with a PopOwner after the call; only an exception thrown through an
			// owner-less drawing method can leave one behind.  No such call is in progress when this Update runs,
			// so anything still on the stack is a leak: count it and clear it.
			private void Update()
			{
				if (_ctxN != 0)
				{
					_ctxLeaks++;
					while (_ctxN > 0) _ctx[--_ctxN] = null;
				}
			}
			private void OnApplicationQuit() { Close(); }
			private void OnDestroy() { Close(); }
		}
	}
}
