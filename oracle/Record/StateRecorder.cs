using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using HutongGames.PlayMaker;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using UnityEngine;
using UnityEngine.LowLevel;
using HKOracle.Env;

namespace HKOracle.Record
{
	// The per-frame full-state recorder for the lockstep acceptance test (docs/state-record.md).
	//
	// Installed in record mode when HK_ORACLE_STATE=1 (Mode.cs).  From Hooks.SceneReady to Hooks.EpisodeEnd, at the
	// end of every frame (a player-loop system appended after PostLateUpdate's last subsystem, so frozen frames are
	// recorded too) it writes the whole gameplay state to <trace base>[.eN].hkstate: every GameObject of the loaded
	// scenes and DontDestroyOnLoad (identity, active flags, transform as float bits), every component (PlayMakerFSM
	// with its variables and every state's actions, Unity physics and animation components, every game
	// MonoBehaviour's fields by reflection, the plain objects those fields reach), the static fields of the game
	// types in play, the clocks and Random.state, and the engine's native state (NativeState).
	//
	// Inertness: the recorder reads fields (never properties, except the listed Unity getters that only read) and
	// native memory; it writes no game state and draws no Random.  The proof is the window task that replays the
	// same corpus with HK_ORACLE_STATE=1 and without and compares the OBS streams.
	//
	// Every value is read every frame, except values that are a function of inputs read this frame and unchanged
	// since the last put: a Transform's world pose and euler angles (its local TRS and its ancestors'), a string or
	// Unity-object field whose reference is unchanged, a GameObject's path (parent path and name).  A field that is
	// not put keeps its last value (StateWriter.Commit).
	public static partial class StateRecorder
	{
		private const int NS_UNITY = 1;
		private const int MaxPlainDepth = 4, ListCap = 256, NumListCap = 65536, PlainBudget = 60000, LayoutCheckEvery = 500;

		private static StateWriter _w;
		private static NativeState _nat;
		private static bool _armed;
		private static string _closeWhy;
		private static string _base;
		private static int _episode;
		private static int _step = -1;
		private static int _framesInEpisode;
		private static Hooks.SceneContext _ctx;
		private static readonly Stopwatch _sw = new Stopwatch();
		private static double _msTotal, _msMax;
		private static int _plainLeft;
		private static int _plainTruncated, _budgetHits;
		private static readonly Dictionary<string, int> _errs = new Dictionary<string, int>();
		private static readonly Dictionary<string, int> _pathBirths = new Dictionary<string, int>();
		private static readonly List<GameObject> _gos = new List<GameObject>();
		private static readonly List<Component> _comps = new List<Component>();
		private static readonly List<Type> _staticTypes = new List<Type>();
		private static readonly HashSet<Type> _staticSeen = new HashSet<Type>();

		// per-writer caches (classes and slots belong to one StateWriter)
		private static readonly Dictionary<Type, StateWriter.Cls> _clsOf = new Dictionary<Type, StateWriter.Cls>();
		private static readonly Dictionary<Type, StateWriter.Cls> _actionCls = new Dictionary<Type, StateWriter.Cls>();
		private static readonly Dictionary<Type, StateWriter.Cls> _plainCls = new Dictionary<Type, StateWriter.Cls>();
		private static readonly Dictionary<Type, StateWriter.Cls> _staticCls = new Dictionary<Type, StateWriter.Cls>();
		private static StateWriter.Cls _goCls, _stateCls, _delayedCls;

		// Capture time by section (Stopwatch ticks, summed over the episode), for the trailer and a periodic log line.
		private static readonly string[] ProfNames = { "layout", "globals", "gameobjects", "fsm", "mono", "unity", "statics", "engine", "endframe" };
		private const int PR_Layout = 0, PR_Globals = 1, PR_Go = 2, PR_Fsm = 3, PR_Mono = 4, PR_Unity = 5, PR_Statics = 6, PR_Engine = 7, PR_End = 8;
		private static readonly long[] _prof = new long[9];
		private static int _gosThisFrame, _compsThisFrame;
		// the costliest component types and slow-path fields (ticks, calls), for the log
		private static readonly Dictionary<object, long[]> _typeTicks = new Dictionary<object, long[]>(), _siteTicks = new Dictionary<object, long[]>();
		private static int _gcAtArm;
		private static void Tally(Dictionary<object, long[]> d, object k, long ticks)
		{
			if (!d.TryGetValue(k, out long[] v)) d[k] = v = new long[2];
			v[0] += ticks; v[1]++;
		}
		private static string Top(Dictionary<object, long[]> d, int n)
		{
			var l = new List<KeyValuePair<object, long[]>>(d);
			l.Sort((a, b) => b.Value[0].CompareTo(a.Value[0]));
			var sb = new StringBuilder();
			for (int i = 0; i < l.Count && i < n; i++)
				sb.Append(l[i].Key is Type ty ? ty.Name : l[i].Key).Append('=').Append((l[i].Value[0] * 1000.0 / Stopwatch.Frequency / Math.Max(1, _framesInEpisode)).ToString("F2"))
					.Append("ms/").Append(l[i].Value[1] / Math.Max(1, _framesInEpisode)).Append(' ');
			return sb.ToString();
		}

		private static JObject ProfJson()
		{
			var o = new JObject();
			for (int i = 0; i < _prof.Length; i++)
				o[ProfNames[i]] = _framesInEpisode == 0 ? 0 : Math.Round(_prof[i] * 1000.0 / Stopwatch.Frequency / _framesInEpisode, 3);
			return o;
		}

		private static void Log(string m) => HKOracle.Instance.Log("[State] " + m);

		// Main-thread CPU cycles: the capture's, and the game's own between two captures.  Their ratio is the
		// recorder's slowdown independent of how many other processes share the machine.
		[System.Runtime.InteropServices.DllImport("kernel32.dll")] private static extern bool QueryThreadCycleTime(IntPtr thread, out ulong cycles);
		[System.Runtime.InteropServices.DllImport("kernel32.dll")] private static extern IntPtr GetCurrentThread();
		private static ulong Cycles() { QueryThreadCycleTime(GetCurrentThread(), out ulong c); return c; }
		private static ulong _cyclesEnd, _cyclesCapture, _cyclesGame;
		private static string CycleText() => _framesInEpisode < 2 ? "" :
			$"main-thread Mcycles/frame: game {_cyclesGame / 1e6 / (_framesInEpisode - 1):F1}, capture {_cyclesCapture / 1e6 / _framesInEpisode:F1}, "
			+ $"slowdown x{(_cyclesGame + _cyclesCapture * (double)(_framesInEpisode - 1) / _framesInEpisode) / Math.Max(1.0, _cyclesGame):F2}";

		private static void Err(string site, Exception e)
		{
			_errs.TryGetValue(site, out int n);
			_errs[site] = n + 1;
			if (n < 3) Log($"ERROR {site}: {HKOracle.DescribeException(e)}");
		}

		// ================================================================ install / arm / close
		public static void Install()
		{
			_base = Mode.SidePath("");
			string dir = Path.GetDirectoryName(_base);
			if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
			InstallLoopSystem();
			Hooks.SceneReady += ctx => { Close("scene_ready_before_arm"); Arm(ctx); };
			Hooks.EpisodeEnd += info => { if (_armed) _closeWhy = info; };
			Hooks.StepBegin += (step, action, committed) => _step = step;
			Application.quitting += () => Close("quit");
			foreach (string n in ExtraStaticTypes) { var t = FindType(n); if (t != null) AddStatic(t, true); else Log("static type not found: " + n); }
			Log($"armed at scene_ready; writing {_base}[.eN].hkstate");
		}

		// Static state outside the component types in play: PlayMaker's globals and event registry, the iTween and
		// pool registries, InControl's devices and tick.  Their static initializers only build containers, and the
		// game has run them before SceneReady.
		private static readonly string[] ExtraStaticTypes = {
			"HutongGames.PlayMaker.Fsm", "HutongGames.PlayMaker.FsmEvent", "HutongGames.PlayMaker.FsmExecutionStack",
			"PlayMakerFSM", "PlayMakerGlobals", "iTween", "iTweenFSMEvents", "ObjectPool", "InControl.InputManager",
			"HeroBox", "HeroController", "GameManager", "BossSceneController", "StaticVariableList" };

		private static Type FindType(string name)
		{
			foreach (var a in AppDomain.CurrentDomain.GetAssemblies())
			{
				string an;
				try { an = a.GetName().Name; } catch { continue; }
				if (an != "Assembly-CSharp" && an != "Assembly-CSharp-firstpass" && an != "PlayMaker") continue;
				var t = a.GetType(name, false);
				if (t != null) return t;
			}
			return null;
		}

		private sealed class StateRecorderLoopSystem { }

		private static void InstallLoopSystem()
		{
			var root = PlayerLoop.GetCurrentPlayerLoop();
			var subs = root.subSystemList;
			for (int i = 0; i < subs.Length; i++)
			{
				if (subs[i].type != typeof(UnityEngine.PlayerLoop.PostLateUpdate)) continue;
				var list = new List<PlayerLoopSystem>(subs[i].subSystemList ?? new PlayerLoopSystem[0]);
				list.Add(new PlayerLoopSystem { type = typeof(StateRecorderLoopSystem), updateDelegate = EndOfFrame });
				subs[i].subSystemList = list.ToArray();
				root.subSystemList = subs;
				PlayerLoop.SetPlayerLoop(root);
				return;
			}
			Log("PostLateUpdate not found in the player loop; nothing will be recorded");
		}

		private static void Arm(Hooks.SceneContext ctx)
		{
			_ctx = ctx;
			string path = _episode == 0 ? _base + ".hkstate" : $"{_base}.e{_episode}.hkstate";
			try
			{
				var header = HeaderJson(ctx);
				_w = new StateWriter(new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.Read, 1 << 16), header.ToString(Formatting.None), async: true);
				_clsOf.Clear(); _actionCls.Clear(); _plainCls.Clear(); _staticCls.Clear(); _goInfo.Clear(); _varsInfo.Clear();
				_goCls = _w.Class("GameObject"); _stateCls = _w.Class("FsmState"); _delayedCls = _w.Class("DelayedEvent");
				_nat = new NativeState(_w, Log) { Plain = PlainEid };
				_nat.Init();
				_armed = true;
				_closeWhy = null;
				_step = -1;
				_framesInEpisode = 0;
				_msTotal = _msMax = 0;
				_plainTruncated = _budgetHits = 0;
				Array.Clear(_prof, 0, _prof.Length);
				_cyclesCapture = _cyclesGame = 0;
				_typeTicks.Clear(); _siteTicks.Clear();
				_gcAtArm = GC.CollectionCount(0);
				_errs.Clear();
				_pathBirths.Clear();
				Log($"episode {_episode} -> {path}");
				if (_episode == 0) Bench();
			}
			catch (Exception e) { Log("arm failed: " + HKOracle.DescribeException(e)); _armed = false; }
		}

		private static JObject HeaderJson(Hooks.SceneContext ctx)
		{
			string E(string k) => System.Environment.GetEnvironmentVariable(k) ?? "";
			return new JObject
			{
				["format"] = "hkstate",
				["writer_version"] = StateWriter.Version,
				["episode"] = _episode,
				["level"] = ctx?.Level,
				["scene"] = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name,
				["trace_base"] = _base,
				["frames_per_wait"] = ctx?.FramesPerWait ?? 0,
				["reset_count"] = ctx?.ResetCount ?? 0,
				["seed"] = E("HK_ORACLE_SEED"),
				["mod_commit"] = ModInfo.Commit,
				["unity_version"] = Application.unityVersion,
				["capture_dt"] = Time.captureDeltaTime,
				["fixed_dt"] = Time.fixedDeltaTime,
				["armed_frame"] = Time.frameCount,
				["exe"] = Process.GetCurrentProcess().ProcessName,
				["timestamp_utc"] = DateTime.UtcNow.ToString("o"),
			};
		}

		private static void Close(string why)
		{
			if (!_armed) return;
			_armed = false;
			try
			{
				var trailer = new JObject
				{
					["why"] = why,
					["frames"] = _framesInEpisode,
					["births"] = _w.Births, ["deaths"] = _w.Deaths, ["sets"] = _w.Sets,
					["bytes_uncompressed"] = _w.BytesUncompressed,
					["capture_ms_mean"] = _framesInEpisode == 0 ? 0 : _msTotal / _framesInEpisode,
					["capture_ms_max"] = _msMax,
					["capture_ms_by_section"] = ProfJson(),
					["main_thread_mcycles_per_frame"] = new JObject { ["game"] = _framesInEpisode < 2 ? 0 : _cyclesGame / 1e6 / (_framesInEpisode - 1),
						["capture"] = _framesInEpisode == 0 ? 0 : _cyclesCapture / 1e6 / _framesInEpisode },
					["plain_truncated"] = _plainTruncated, ["plain_budget_hits"] = _budgetHits,
					["statics_skipped"] = new JArray(_staticsSkipped.ToArray()),
					["missing_fields"] = new JArray(Cache.Missing.ToArray()),
					["errors"] = JObject.FromObject(_errs),
					["native"] = _nat?.Info(),
					["layout_mismatches"] = _nat?.CheckFailures ?? -1,
				};
				_w.Close(trailer.ToString(Formatting.None));
				Log($"episode {_episode} closed ({why}): {_framesInEpisode} frames, {_w.Births} births, {_w.BytesUncompressed} bytes raw, "
					+ $"capture {(_framesInEpisode == 0 ? 0 : _msTotal / _framesInEpisode):F1} ms/frame (max {_msMax:F0}), "
					+ $"layout mismatches {_nat?.CheckFailures}, native {(_nat != null && _nat.Enabled ? "on" : "off: " + _nat?.DisabledWhy)}; "
					+ $"ms/frame by section {ProfJson().ToString(Formatting.None)}; {CycleText()}");
			}
			catch (Exception e) { Log("close failed: " + HKOracle.DescribeException(e)); }
			_w = null;
			_episode++;
		}

		private static void EndOfFrame()
		{
			if (!_armed) return;
			try { Capture(); }
			catch (Exception e) { Err("Capture", e); }
			if (_closeWhy != null) Close(_closeWhy);
		}

		// ================================================================ one frame
		private static void Capture()
		{
			ulong c0 = Cycles();
			if (_framesInEpisode > 0) _cyclesGame += c0 - _cyclesEnd;
			_sw.Reset(); _sw.Start();
			long t0 = Stopwatch.GetTimestamp(), t1;
			_gosThisFrame = _compsThisFrame = 0;
			int flags = Time.deltaTime > 0f ? 1 : 0;
			_w.BeginFrame(Time.frameCount, _step, (int)TraceRecorder.FixedCount, flags);
			_plainLeft = PlainBudget;
			_nat.BeginFrame();
			// validate the native layouts before this frame's native reads
			if (_nat.Enabled && (_framesInEpisode % LayoutCheckEvery == 0 || _closeWhy != null))
			{
				try
				{
					CollectLive(_gos, _comps);
					var hc = Hero();
					if (hc != null) _nat.SetScene(hc.GetComponent<Rigidbody2D>());
					_nat.Check(_gos, _comps);
				}
				catch (Exception e) { Err("layout check", e); }
			}
			t1 = Stopwatch.GetTimestamp(); _prof[PR_Layout] += t1 - t0; t0 = t1;
			int envEid = 0;
			try { envEid = CaptureGlobals(); } catch (Exception e) { Err("globals", e); }
			t1 = Stopwatch.GetTimestamp(); _prof[PR_Globals] += t1 - t0; t0 = t1;
			long inner = _prof[PR_Fsm] + _prof[PR_Mono] + _prof[PR_Unity];
			try { CaptureObjects(); } catch (Exception e) { Err("objects", e); }
			t1 = Stopwatch.GetTimestamp(); _prof[PR_Go] += t1 - t0 - (_prof[PR_Fsm] + _prof[PR_Mono] + _prof[PR_Unity] - inner); t0 = t1;
			try { CaptureStatics(); } catch (Exception e) { Err("statics", e); }
			t1 = Stopwatch.GetTimestamp(); _prof[PR_Statics] += t1 - t0; t0 = t1;
			_nat.CaptureEngine(envEid);
			t1 = Stopwatch.GetTimestamp(); _prof[PR_Engine] += t1 - t0; t0 = t1;
			_w.EndFrame();
			t1 = Stopwatch.GetTimestamp(); _prof[PR_End] += t1 - t0;
			_framesInEpisode++;
			_cyclesEnd = Cycles();
			_cyclesCapture += _cyclesEnd - c0;
			_sw.Stop();
			double ms = _sw.Elapsed.TotalMilliseconds;
			_msTotal += ms;
			if (ms > _msMax) _msMax = ms;
			if (_framesInEpisode % 100 == 0)
				Log($"frame {_framesInEpisode}: capture {_msTotal / _framesInEpisode:F1} ms/frame, {_gosThisFrame} gameobjects, {_compsThisFrame} components, "
					+ $"{_w.LiveEntities} entities, {_w.BytesUncompressed} bytes raw; ms/frame by section {ProfJson().ToString(Formatting.None)}"
					+ $"; {CycleText()}; top types {Top(_typeTicks, 12)}; top slow fields {Top(_siteTicks, 12)}; gc {GC.CollectionCount(0) - _gcAtArm} heap {GC.GetTotalMemory(false) >> 20} MB");
		}

		// Cost of one compiled read, against FieldInfo.GetValue, on the PlayerData plan (log only).
		private static void Bench()
		{
			try
			{
				var gm = GameManager.UnsafeInstance;
				object pd = gm == null ? null : Get(gm, "playerData");
				if (pd == null) return;
				var plan = PlanFor(pd.GetType(), typeof(object));
				int nb = 0; long sink = 0;
				var sw = Stopwatch.StartNew();
				for (int rep = 0; rep < 20; rep++)
					foreach (var op in plan.Ops) if (op.Kind == K_BITS) { sink += op.Bits(pd); nb++; }
				double il = sw.Elapsed.TotalMilliseconds * 1e6 / Math.Max(1, nb);
				var fields = new List<FieldInfo>();
				foreach (var f in pd.GetType().GetFields(InstAll)) if (FastAccess.IsBitsLeaf(f.FieldType)) fields.Add(f);
				int nr = 0;
				sw.Restart();
				for (int rep = 0; rep < 20; rep++)
					foreach (var f in fields) { if (f.GetValue(pd) != null) sink++; nr++; }
				double refl = sw.Elapsed.TotalMilliseconds * 1e6 / Math.Max(1, nr);
				sw.Restart();
				ulong c0 = Cycles();
				for (int rep = 0; rep < 20; rep++) RunPlanDry(pd, plan);
				double run = sw.Elapsed.TotalMilliseconds / 20;
				Log($"bench PlayerData: {plan.Ops.Length} ops, IL read {il:F0} ns, FieldInfo.GetValue {refl:F0} ns, full plan read {run:F2} ms "
					+ $"({(Cycles() - c0) / 20e6:F2} Mcycles) (sink {sink & 1})");
			}
			catch (Exception e) { Log("bench failed: " + e.Message); }
		}

		private static void RunPlanDry(object o, Plan p)
		{
			foreach (var op in p.Ops)
				switch (op.Kind)
				{
					case K_BITS: op.Bits(o); break;
					default: op.Get(o); break;
				}
		}

		// HeroController._instance without the FindObjectOfType/DontDestroyOnLoad of HeroController.instance
		// (HeroController.cs:750-775).
		private static readonly FieldInfo _heroInstance = typeof(HeroController).GetField("_instance", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
		private static HeroController Hero() => _heroInstance?.GetValue(null) as HeroController;

		// PlayMakerGlobals.instance without the Initialize() of PlayMakerGlobals.Instance (PlayMakerGlobals.cs:25-31).
		private static readonly FieldInfo _pmGlobals = typeof(PlayMakerGlobals).GetField("instance", BindingFlags.Static | BindingFlags.NonPublic);

		private static StateWriter.Ent Single(string cls, int id) =>
			_w.Visit(StateWriter.Key(9, 1000 + id), _w.Class(cls), 0, 0, cls);

		private static int CaptureGlobals()
		{
			var t = Single("Time", 1);
			_w.I(t, "frameCount", Time.frameCount);
			_w.F(t, "time", Time.time);
			_w.F(t, "deltaTime", Time.deltaTime);
			_w.F(t, "unscaledDeltaTime", Time.unscaledDeltaTime);
			_w.F(t, "fixedTime", Time.fixedTime);
			_w.F(t, "fixedDeltaTime", Time.fixedDeltaTime);
			_w.F(t, "timeScale", Time.timeScale);
			_w.F(t, "captureDeltaTime", Time.captureDeltaTime);
			_w.F(t, "maximumDeltaTime", Time.maximumDeltaTime);
			_w.F(t, "timeSinceLevelLoad", Time.timeSinceLevelLoad);
			_w.B(t, "inFixedTimeStep", Time.inFixedTimeStep);
			_w.Commit(t);

			var r = Single("Random", 2);
			var st = UnityEngine.Random.state;
			var bits = RandomWords(st);
			_w.List(r, "state", 'I', bits);
			_w.Commit(r);

			// PlayMakerGlobals is a ScriptableObject, not a component: its variables and global events.
			var g = Single("PlayMakerGlobals", 4);
			var globals = _pmGlobals?.GetValue(null);
			_w.B(g, "present", globals != null);
			_w.E(g, "vars", globals == null ? 0 : VarsEid(Get(globals, "variables"), g.Eid, "globals"));
			_w.Strs(g, "events", Get(globals, "events") as List<string>);
			_w.Commit(g);

			var env = Single("Env", 3);
			_w.I(env, "step", _step);
			_w.I(env, "fixedCount", (int)TraceRecorder.FixedCount);
			_w.S(env, "activeScene", UnityEngine.SceneManagement.SceneManager.GetActiveScene().name);
			_w.Commit(env);
			return env.Eid;
		}

		// Random.State's int fields in declaration order (the words TraceRecorder writes as FRAME.rng).
		private static FieldInfo[] _rngFields;
		private static long[] RandomWords(UnityEngine.Random.State st)
		{
			if (_rngFields == null)
			{
				_rngFields = typeof(UnityEngine.Random.State).GetFields(InstAll);
				var names = new StringBuilder();
				foreach (var f in _rngFields) names.Append(f.Name).Append(' ');
				Log("Random.State fields: " + names);
			}
			object boxed = st;
			var r = new long[Math.Min(4, _rngFields.Length)];
			for (int i = 0; i < r.Length; i++) r[i] = Convert.ToInt32(_rngFields[i].GetValue(boxed));
			return r;
		}

		// ================================================================ objects
		private static readonly List<GameObject> _roots = new List<GameObject>();

		private static void CaptureObjects()
		{
			for (int si = 0; si < UnityEngine.SceneManagement.SceneManager.sceneCount; si++)
			{
				var sc = UnityEngine.SceneManagement.SceneManager.GetSceneAt(si);
				if (!sc.isLoaded) continue;
				_roots.Clear();
				sc.GetRootGameObjects(_roots);
				for (int i = 0; i < _roots.Count; i++) Walk(_roots[i].transform, null, "", 0, 0, -1, false);
			}
			foreach (var root in DdolRoots()) Walk(root.transform, null, "DDOL/", 0, 0, -1, false);
		}

		// The recorded GameObjects (Walk's set) and their components, for the layout check.
		private static void CollectLive(List<GameObject> gos, List<Component> comps)
		{
			gos.Clear(); comps.Clear();
			var roots = new List<GameObject>();
			for (int si = 0; si < UnityEngine.SceneManagement.SceneManager.sceneCount; si++)
			{
				var sc = UnityEngine.SceneManagement.SceneManager.GetSceneAt(si);
				if (sc.isLoaded) roots.AddRange(sc.GetRootGameObjects());
			}
			roots.AddRange(DdolRoots());
			var stack = new Stack<Transform>();
			foreach (var r in roots) if (!r.name.StartsWith("HKOracle", StringComparison.Ordinal)) stack.Push(r.transform);
			while (stack.Count > 0)
			{
				var t = stack.Pop();
				gos.Add(t.gameObject);
				foreach (var c in t.GetComponents<Component>()) if (c != null) comps.Add(c);
				for (int i = 0; i < t.childCount; i++) stack.Push(t.GetChild(i));
			}
		}

		// DontDestroyOnLoad is not enumerable through SceneManager; its Scene handle is reachable from any object in it.
		private static IEnumerable<GameObject> DdolRoots()
		{
			var gm = GameManager.UnsafeInstance;
			if (gm != null)
			{
				var s = gm.gameObject.scene;
				if (s.IsValid() && s.name == "DontDestroyOnLoad") return s.GetRootGameObjects();
			}
			var seen = new HashSet<int>();
			var roots = new List<GameObject>();
			foreach (var tr in Resources.FindObjectsOfTypeAll<Transform>())
			{
				if (tr == null) continue;
				var root = tr.root;
				if (root.gameObject.scene.name != "DontDestroyOnLoad") continue;
				if (seen.Add(root.GetInstanceID())) roots.Add(root.gameObject);
			}
			return roots;
		}

		// What the walk remembers of a GameObject between frames: the inputs its path and world pose derive from.
		private sealed class GoInfo
		{
			public string Name, Path, ParentPath, Prefix, Tag;
			public int ParentIid = int.MinValue, SceneHandle = int.MinValue;
			public readonly long[] Local = new long[10];
		}
		private static readonly Dictionary<int, GoInfo> _goInfo = new Dictionary<int, GoInfo>();
		private static readonly Dictionary<int, string> _sceneNames = new Dictionary<int, string>();

		private static readonly Schema GoSchema = new Schema(
			"s:name", "s:path", "o:parent", "i:sibling", "i:childCount", "b:activeSelf", "b:activeInHierarchy", "i:layer", "s:tag", "s:scene",
			"f:lp.x", "f:lp.y", "f:lp.z", "f:lr.x", "f:lr.y", "f:lr.z", "f:lr.w", "f:ls.x", "f:ls.y", "f:ls.z",
			"f:wp.x", "f:wp.y", "f:wp.z", "f:wr.x", "f:wr.y", "f:wr.z", "f:wr.w", "f:lossy.x", "f:lossy.y", "f:lossy.z",
			"f:leuler.z", "f:euler.z", "O:components", "i:cloneIndex", "o:iid");
		private const int GO_Name = 0, GO_Path = 1, GO_Parent = 2, GO_Sibling = 3, GO_ChildCount = 4, GO_ActiveSelf = 5, GO_ActiveInHierarchy = 6,
			GO_Layer = 7, GO_Tag = 8, GO_Scene = 9, GO_Local = 10, GO_World = 20, GO_LEuler = 30, GO_Euler = 31, GO_Components = 32,
			GO_CloneIndex = 33, GO_Iid = 34;

		private static readonly List<Component> _compBuf = new List<Component>();
		private static long[] _idBuf = new long[64];
		private static readonly long[] _localNow = new long[10];

		// `sibling` is the child index under the parent (-1 for a root: its sibling index is read).  A world pose is
		// re-read when the object is new, its parent changed, its local TRS changed, or its parent's world changed.
		private static void Walk(Transform t, string parentPath, string prefix, int parentEid, int parentIid, int sibling, bool parentDirty)
		{
			var go = t.gameObject;
			_gosThisFrame++;
			string name = go.name;
			if (parentEid == 0 && name.StartsWith("HKOracle", StringComparison.Ordinal)) return;
			int iid = go.GetInstanceID();
			_goInfo.TryGetValue(iid, out GoInfo gi);
			string path = gi != null && ReferenceEquals(gi.ParentPath, parentPath) && ReferenceEquals(gi.Prefix, prefix) && gi.Name == name ? gi.Path
				: parentPath == null ? prefix + name : parentPath + "/" + name;
			var e = _w.Visit(StateWriter.Key(NS_UNITY, iid), _goCls, 0, parentEid, path);
			if (e == null) return;
			var s = Slots(_goCls, GoSchema);
			if (e.IsNew || gi == null)
			{
				gi = new GoInfo();
				_goInfo[iid] = gi;
			}
			if (e.IsNew)
			{
				_pathBirths.TryGetValue(path, out int n);
				_pathBirths[path] = n + 1;
				_w.Raw(e, s[GO_CloneIndex], n);
				_w.Raw(e, s[GO_Iid], iid);
			}
			bool dirty = parentDirty || e.IsNew;
			int childCount = 0;
			try
			{
				if (gi.Name != name) { _w.Raw(e, s[GO_Name], _w.Str(name)); gi.Name = name; }
				if (!ReferenceEquals(gi.Path, path)) { _w.Raw(e, s[GO_Path], _w.Str(path)); gi.Path = path; }
				gi.ParentPath = parentPath; gi.Prefix = prefix;
				if (gi.ParentIid != parentIid) { dirty = true; gi.ParentIid = parentIid; }
				_w.Raw(e, s[GO_Parent], parentIid);
				_w.Raw(e, s[GO_Sibling], sibling >= 0 ? sibling : t.GetSiblingIndex());
				childCount = t.childCount;
				_w.Raw(e, s[GO_ChildCount], childCount);
				_w.Raw(e, s[GO_ActiveSelf], go.activeSelf ? 1 : 0);
				_w.Raw(e, s[GO_ActiveInHierarchy], go.activeInHierarchy ? 1 : 0);
				_w.Raw(e, s[GO_Layer], go.layer);
				// CompareTag reads the tag without allocating its string (GameObject.tag marshals a new one)
				if (gi.Tag == null || !go.CompareTag(gi.Tag)) { gi.Tag = go.tag; _w.Raw(e, s[GO_Tag], _w.Str(gi.Tag)); }
				int sh = go.scene.handle;
				if (sh != gi.SceneHandle)
				{
					if (!_sceneNames.TryGetValue(sh, out string sn)) _sceneNames[sh] = sn = go.scene.name;
					_w.Raw(e, s[GO_Scene], _w.Str(sn));
					gi.SceneHandle = sh;
				}
				var lp = t.localPosition; var lr = t.localRotation; var ls = t.localScale;
				_localNow[0] = StateWriter.FloatBits(lp.x); _localNow[1] = StateWriter.FloatBits(lp.y); _localNow[2] = StateWriter.FloatBits(lp.z);
				_localNow[3] = StateWriter.FloatBits(lr.x); _localNow[4] = StateWriter.FloatBits(lr.y); _localNow[5] = StateWriter.FloatBits(lr.z);
				_localNow[6] = StateWriter.FloatBits(lr.w);
				_localNow[7] = StateWriter.FloatBits(ls.x); _localNow[8] = StateWriter.FloatBits(ls.y); _localNow[9] = StateWriter.FloatBits(ls.z);
				bool rotChanged = e.IsNew;
				for (int k = 0; k < 10; k++)
				{
					if (_localNow[k] == gi.Local[k] && !e.IsNew) continue;
					dirty = true;
					if (k >= 3 && k <= 6) rotChanged = true;
					gi.Local[k] = _localNow[k];
					_w.Raw(e, s[GO_Local + k], _localNow[k]);
				}
				if (rotChanged) _w.Raw(e, s[GO_LEuler], StateWriter.FloatBits(t.localEulerAngles.z));
				if (dirty)
				{
					var wp = t.position; var wr = t.rotation; var lossy = t.lossyScale;
					_w.Raw(e, s[GO_World + 0], StateWriter.FloatBits(wp.x)); _w.Raw(e, s[GO_World + 1], StateWriter.FloatBits(wp.y));
					_w.Raw(e, s[GO_World + 2], StateWriter.FloatBits(wp.z));
					_w.Raw(e, s[GO_World + 3], StateWriter.FloatBits(wr.x)); _w.Raw(e, s[GO_World + 4], StateWriter.FloatBits(wr.y));
					_w.Raw(e, s[GO_World + 5], StateWriter.FloatBits(wr.z)); _w.Raw(e, s[GO_World + 6], StateWriter.FloatBits(wr.w));
					_w.Raw(e, s[GO_World + 7], StateWriter.FloatBits(lossy.x)); _w.Raw(e, s[GO_World + 8], StateWriter.FloatBits(lossy.y));
					_w.Raw(e, s[GO_World + 9], StateWriter.FloatBits(lossy.z));
					_w.Raw(e, s[GO_Euler], StateWriter.FloatBits(t.eulerAngles.z));
				}
				go.GetComponents(_compBuf);
				int nc = _compBuf.Count;
				if (_idBuf.Length < nc) _idBuf = new long[Math.Max(nc, _idBuf.Length * 2)];
				for (int j = 0; j < nc; j++)
				{
					var c = _compBuf[j];
					_idBuf[j] = 0;
					if (c == null) continue;
					_idBuf[j] = c.GetInstanceID();
				}
				for (int j = 0; j < nc; j++)
				{
					var c = _compBuf[j];
					if (c == null) continue;
					try { EncodeComponent(c, (int)_idBuf[j], e.Eid, j); }
					catch (Exception ex) { Err("component " + c.GetType().FullName, ex); }
				}
				_w.RawList(e, s[GO_Components], _idBuf, nc);
			}
			catch (Exception ex) { Err("gameobject", ex); }
			_w.Commit(e);
			for (int i = 0; i < childCount; i++)
			{
				var ch = t.GetChild(i);
				Walk(ch, path, "", e.Eid, iid, i, dirty);
			}
		}

		// ================================================================ components
		private static readonly Dictionary<Type, bool> _reflected = new Dictionary<Type, bool>();
		private static bool Reflected(Type t)
		{
			if (_reflected.TryGetValue(t, out bool r)) return r;
			string an = t.Assembly.GetName().Name, ns = t.Namespace ?? "";
			r = (an == "Assembly-CSharp" || an == "Assembly-CSharp-firstpass" || an == "PlayMaker")
				&& !ns.StartsWith("TMPro", StringComparison.Ordinal) && !ns.StartsWith("UnityEngine", StringComparison.Ordinal);
			_reflected[t] = r;
			return r;
		}

		private static StateWriter.Cls ClassOf(Dictionary<Type, StateWriter.Cls> cache, Type t, string prefix)
		{
			if (!cache.TryGetValue(t, out StateWriter.Cls c)) cache[t] = c = _w.Class(prefix + t.FullName);
			return c;
		}

		private static readonly Schema CompSchema = new Schema("o:iid", "i:index"), BehSchema = new Schema("b:enabled", "b:isActiveAndEnabled"),
			RendererSchema = new Schema("b:enabled");

		private static void EncodeComponent(Component c, int iid, int goEid, int index)
		{
			var t = c.GetType();
			long t0 = Stopwatch.GetTimestamp();
			_compsThisFrame++;
			int sec = c is PlayMakerFSM ? PR_Fsm : c is MonoBehaviour && Reflected(t) ? PR_Mono : PR_Unity;
			try { EncodeComponentBody(c, t, iid, goEid, index); }
			finally
			{
				long dt = Stopwatch.GetTimestamp() - t0;
				_prof[sec] += dt;
				Tally(_typeTicks, t, dt);
			}
		}

		private static void EncodeComponentBody(Component c, Type t, int iid, int goEid, int index)
		{
			var cls = ClassOf(_clsOf, t, "");
			var e = _w.Visit(StateWriter.Key(NS_UNITY, iid), cls, 0, goEid, t.FullName);
			if (e == null) return;
			var s = Slots(cls, CompSchema);
			if (e.IsNew) _w.Raw(e, s[0], iid);
			_w.Raw(e, s[1], index);
			var b = c as Behaviour;
			if (b != null)
			{
				var bs = Slots(cls, BehSchema);
				_w.Raw(e, bs[0], b.enabled ? 1 : 0);
				_w.Raw(e, bs[1], b.isActiveAndEnabled ? 1 : 0);
			}
			switch (c)
			{
				case Transform tr: _nat.PutTransform(e, tr); break;
				case Rigidbody2D rb: PutRigidbody(e, rb); break;
				case Collider2D col: PutCollider(e, col); break;
				case Animator an: PutAnimator(e, an); break;
				case AudioSource au: _w.B(e, "isPlaying", au.isPlaying); _w.O(e, "clip", Iid(au.clip)); break;
				case Renderer r: _w.Raw(e, Slots(cls, RendererSchema)[0], r.enabled ? 1 : 0); break;
				case PlayMakerFSM pm: PutFsm(e, pm); break;
				case MonoBehaviour mb when Reflected(t):
					AddStatic(t, false);
					RunPlan(e, mb, PlanFor(t, typeof(MonoBehaviour)), 1);
					if (mb is tk2dSpriteAnimator anim) PutTk2d(e, anim);
					break;
			}
			if (b != null) _nat.PutBehaviour(e, b);
			_w.Commit(e);
		}

		private static int Iid(UnityEngine.Object o) => ReferenceEquals(o, null) ? 0 : o.GetInstanceID();

		// Not rb.position / rb.rotation: Rigidbody2D::GetPosition and ::GetRotation call
		// PhysicsManager2D::AutoSyncTransforms (UP!0x180c12290, UP!0x180c126b0), which with autoSyncTransforms on
		// (dumps/<scene>/physics.json) pushes pending Transform changes into Box2D.  NativeState reads what they
		// return (m_Body->m_xf.p, m_sweep.a * 57.29578) without the sync.
		private static readonly Schema RbSchema = new Schema("f:velocity.x", "f:velocity.y", "f:angularVelocity", "f:gravityScale", "f:mass",
			"f:drag", "f:angularDrag", "f:inertia", "f:centerOfMass.x", "f:centerOfMass.y", "f:worldCenterOfMass.x", "f:worldCenterOfMass.y",
			"i:bodyType", "b:isKinematic", "b:simulated", "b:useAutoMass", "b:useFullKinematicContacts", "i:constraints",
			"i:collisionDetectionMode", "i:sleepMode", "i:interpolation", "b:awake", "i:attachedColliderCount");

		private static void PutRigidbody(StateWriter.Ent e, Rigidbody2D rb)
		{
			_nat.PutRigidbodyPose(e, rb);
			var s = Slots(e.Cls, RbSchema);
			var v = rb.velocity;
			_w.Raw(e, s[0], StateWriter.FloatBits(v.x)); _w.Raw(e, s[1], StateWriter.FloatBits(v.y));
			_w.Raw(e, s[2], StateWriter.FloatBits(rb.angularVelocity));
			_w.Raw(e, s[3], StateWriter.FloatBits(rb.gravityScale));
			_w.Raw(e, s[4], StateWriter.FloatBits(rb.mass));
			_w.Raw(e, s[5], StateWriter.FloatBits(rb.drag));
			_w.Raw(e, s[6], StateWriter.FloatBits(rb.angularDrag));
			_w.Raw(e, s[7], StateWriter.FloatBits(rb.inertia));
			var com = rb.centerOfMass;
			_w.Raw(e, s[8], StateWriter.FloatBits(com.x)); _w.Raw(e, s[9], StateWriter.FloatBits(com.y));
			var wc = rb.worldCenterOfMass;
			_w.Raw(e, s[10], StateWriter.FloatBits(wc.x)); _w.Raw(e, s[11], StateWriter.FloatBits(wc.y));
			_w.Raw(e, s[12], (int)rb.bodyType);
			_w.Raw(e, s[13], rb.isKinematic ? 1 : 0);
			_w.Raw(e, s[14], rb.simulated ? 1 : 0);
			_w.Raw(e, s[15], rb.useAutoMass ? 1 : 0);
			_w.Raw(e, s[16], rb.useFullKinematicContacts ? 1 : 0);
			_w.Raw(e, s[17], (int)rb.constraints);
			_w.Raw(e, s[18], (int)rb.collisionDetectionMode);
			_w.Raw(e, s[19], (int)rb.sleepMode);
			_w.Raw(e, s[20], (int)rb.interpolation);
			_w.Raw(e, s[21], rb.IsAwake() ? 1 : 0);
			_w.Raw(e, s[22], rb.attachedColliderCount);
		}

		private static readonly Schema ColSchema = new Schema("b:isTrigger", "f:offset.x", "f:offset.y", "f:density", "b:usedByEffector",
			"b:usedByComposite", "o:sharedMaterial", "o:attachedRigidbody", "i:shapeCount");
		private static readonly Schema BoxSchema = new Schema("f:size.x", "f:size.y", "f:edgeRadius", "b:autoTiling");

		private static void PutCollider(StateWriter.Ent e, Collider2D col)
		{
			var s = Slots(e.Cls, ColSchema);
			_w.Raw(e, s[0], col.isTrigger ? 1 : 0);
			var off = col.offset;
			_w.Raw(e, s[1], StateWriter.FloatBits(off.x)); _w.Raw(e, s[2], StateWriter.FloatBits(off.y));
			_w.Raw(e, s[3], StateWriter.FloatBits(col.density));
			_w.Raw(e, s[4], col.usedByEffector ? 1 : 0);
			_w.Raw(e, s[5], col.usedByComposite ? 1 : 0);
			_w.Raw(e, s[6], Iid(col.sharedMaterial));
			_w.Raw(e, s[7], Iid(col.attachedRigidbody));
			_w.Raw(e, s[8], col.shapeCount);
			switch (col)
			{
				case BoxCollider2D box:
					{
						var bs = Slots(e.Cls, BoxSchema);
						var size = box.size;
						_w.Raw(e, bs[0], StateWriter.FloatBits(size.x)); _w.Raw(e, bs[1], StateWriter.FloatBits(size.y));
						_w.Raw(e, bs[2], StateWriter.FloatBits(box.edgeRadius));
						_w.Raw(e, bs[3], box.autoTiling ? 1 : 0);
						break;
					}
				case CircleCollider2D c:
					_w.F(e, "radius", c.radius);
					break;
				case PolygonCollider2D poly:
					{
						var lens = new List<long>(); var pts = new List<long>();
						for (int i = 0; i < poly.pathCount; i++)
						{
							var path = poly.GetPath(i);
							lens.Add(path.Length);
							foreach (var v in path) { pts.Add(StateWriter.FloatBits(v.x)); pts.Add(StateWriter.FloatBits(v.y)); }
						}
						_w.List(e, "pathLengths", 'I', lens.ToArray());
						_w.List(e, "points", 'F', pts.ToArray());
						break;
					}
				case EdgeCollider2D edge:
					{
						var pts = new List<long>();
						foreach (var v in edge.points) { pts.Add(StateWriter.FloatBits(v.x)); pts.Add(StateWriter.FloatBits(v.y)); }
						_w.List(e, "points", 'F', pts.ToArray());
						_w.F(e, "edgeRadius", edge.edgeRadius);
						break;
					}
			}
		}

		// Collider2D.bounds is not read: Collider2D::GetBounds calls AutoSyncTransforms (UP!0x180c03a50).  The
		// fixtures' shapes and proxy AABBs are in the b2Fixture entities.

		// Mecanim: the controller state per layer and every parameter.  Only read on an initialized Animator with a
		// controller; "valid" says whether the rest of the fields describe this frame.
		private static readonly Schema AnimSchema = new Schema("b:valid", "f:speed", "o:controller", "i:cullingMode", "i:updateMode",
			"b:applyRootMotion", "i:layerCount");
		private static readonly Schema[] AnimLayerSchemas = new Schema[8];
		private sealed class AnimParams { public Schema Schema; public int[] Hash; public AnimatorControllerParameterType[] Type; }
		private static readonly Dictionary<RuntimeAnimatorController, AnimParams> _animParams = new Dictionary<RuntimeAnimatorController, AnimParams>();

		private static void PutAnimator(StateWriter.Ent e, Animator a)
		{
			var s = Slots(e.Cls, AnimSchema);
			var ctl = a.runtimeAnimatorController;
			bool valid = a.isActiveAndEnabled && a.isInitialized && ctl != null;
			_w.Raw(e, s[0], valid ? 1 : 0);
			_w.Raw(e, s[1], StateWriter.FloatBits(a.speed));
			_w.Raw(e, s[2], Iid(ctl));
			_w.Raw(e, s[3], (int)a.cullingMode);
			_w.Raw(e, s[4], (int)a.updateMode);
			_w.Raw(e, s[5], a.applyRootMotion ? 1 : 0);
			if (!valid) return;
			int layers = a.layerCount;
			_w.Raw(e, s[6], layers);
			for (int l = 0; l < layers && l < 8; l++)
			{
				var ls = AnimLayerSchemas[l] ?? (AnimLayerSchemas[l] = LayerSchema(l));
				var q = Slots(e.Cls, ls);
				var st = a.GetCurrentAnimatorStateInfo(l);
				_w.Raw(e, q[0], st.fullPathHash);
				_w.Raw(e, q[1], StateWriter.FloatBits(st.normalizedTime));
				_w.Raw(e, q[2], StateWriter.FloatBits(st.length));
				_w.Raw(e, q[3], StateWriter.FloatBits(st.speed));
				_w.Raw(e, q[4], StateWriter.FloatBits(st.speedMultiplier));
				_w.Raw(e, q[5], st.tagHash);
				_w.Raw(e, q[6], st.loop ? 1 : 0);
				_w.Raw(e, q[7], StateWriter.FloatBits(a.GetLayerWeight(l)));
				bool tr = a.IsInTransition(l);
				_w.Raw(e, q[8], tr ? 1 : 0);
				var n = tr ? a.GetNextAnimatorStateInfo(l) : default(AnimatorStateInfo);
				_w.Raw(e, q[9], n.fullPathHash);
				_w.Raw(e, q[10], StateWriter.FloatBits(n.normalizedTime));
				var ti = tr ? a.GetAnimatorTransitionInfo(l) : default(AnimatorTransitionInfo);
				_w.Raw(e, q[11], ti.fullPathHash);
				_w.Raw(e, q[12], StateWriter.FloatBits(ti.normalizedTime));
				_w.Raw(e, q[13], StateWriter.FloatBits(ti.duration));
			}
			// the parameter list is the controller's (Animator.parameters builds it anew on every call)
			if (!_animParams.TryGetValue(ctl, out AnimParams ap))
			{
				var pars = a.parameters;
				var names = new string[pars.Length];
				var types = new char[pars.Length];
				ap = new AnimParams { Hash = new int[pars.Length], Type = new AnimatorControllerParameterType[pars.Length] };
				for (int i = 0; i < pars.Length; i++)
				{
					names[i] = "p:" + pars[i].name;
					ap.Type[i] = pars[i].type;
					ap.Hash[i] = pars[i].nameHash;
					types[i] = pars[i].type == AnimatorControllerParameterType.Float ? 'f' : pars[i].type == AnimatorControllerParameterType.Int ? 'i' : 'b';
				}
				ap.Schema = new Schema(names, types);
				_animParams[ctl] = ap;
			}
			var ps = Slots(e.Cls, ap.Schema);
			for (int i = 0; i < ap.Hash.Length; i++)
			{
				switch (ap.Type[i])
				{
					case AnimatorControllerParameterType.Float: _w.Raw(e, ps[i], StateWriter.FloatBits(a.GetFloat(ap.Hash[i]))); break;
					case AnimatorControllerParameterType.Int: _w.Raw(e, ps[i], a.GetInteger(ap.Hash[i])); break;
					default: _w.Raw(e, ps[i], a.GetBool(ap.Hash[i]) ? 1 : 0); break;
				}
			}
		}

		private static Schema LayerSchema(int l)
		{
			string p = "L" + l + ".";
			return new Schema("i:" + p + "hash", "f:" + p + "normalizedTime", "f:" + p + "length", "f:" + p + "speed", "f:" + p + "speedMultiplier",
				"i:" + p + "tag", "b:" + p + "loop", "f:" + p + "weight", "b:" + p + "inTransition", "i:" + p + "next.hash",
				"f:" + p + "next.normalizedTime", "i:" + p + "transition.hash", "f:" + p + "transition.normalizedTime", "f:" + p + "transition.duration");
		}

		// tk2dSpriteAnimator's derived getters (tk2dSpriteAnimator.cs:113-183), which only read clipTime/currentClip.
		private static readonly Schema Tk2dSchema = new Schema("s:tk2d.clip", "b:tk2d.playing", "i:tk2d.currentFrame", "f:tk2d.clipTimeSeconds");
		private static void PutTk2d(StateWriter.Ent e, tk2dSpriteAnimator a)
		{
			var s = Slots(e.Cls, Tk2dSchema);
			var clip = a.CurrentClip;
			bool ok = clip != null && clip.frames != null && clip.frames.Length > 0;
			if (!_w.SameRef(e, s[0], clip)) { _w.Raw(e, s[0], _w.Str(clip == null ? "" : clip.name)); _w.Ref(e, s[0], clip); }
			_w.Raw(e, s[1], a.Playing ? 1 : 0);
			_w.Raw(e, s[2], ok ? a.CurrentFrame : -1);
			_w.Raw(e, s[3], StateWriter.FloatBits(ok && (a.ClipFps > 0f || clip.fps > 0f) ? a.ClipTimeSeconds : 0f));
		}

		// ================================================================ PlayMaker
		// Private fields only: Fsm, FsmState and PlayMakerGlobals getters have side effects (PlayMakerFSM.Fsm sets
		// Owner, FsmState.Actions loads actions, Fsm.ActiveState caches, PlayMakerGlobals.Instance initializes).
		private const BindingFlags InstAll = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
		private static readonly Dictionary<Type, Dictionary<string, Func<object, object>>> _getByName = new Dictionary<Type, Dictionary<string, Func<object, object>>>();

		// A field by name on the object's runtime type, for the reads outside the compiled plans (formatting, delayed
		// events, globals).
		private static object Get(object o, string field)
		{
			if (o == null) return null;
			var t = o.GetType();
			if (!_getByName.TryGetValue(t, out var byName)) _getByName[t] = byName = new Dictionary<string, Func<object, object>>();
			if (!byName.TryGetValue(field, out var g))
			{
				var f = FindField(t, field);
				if (f == null) MissingField(t, field);
				byName[field] = g = f == null ? (o2 => null) : RefGetter(new[] { f });
			}
			return g(o);
		}

		private static readonly Func<object, object> PM_Fsm = R(typeof(PlayMakerFSM), "fsm");
		private static readonly Func<object, object> FSM_Name = R(typeof(Fsm), "name"), FSM_ActiveStateName = R(typeof(Fsm), "activeStateName"),
			FSM_Previous = R(typeof(Fsm), "previousActiveState"), FSM_SwitchTo = R(typeof(Fsm), "switchToState"),
			FSM_LastTransition = R(typeof(Fsm), "<LastTransition>k__BackingField"),
			FSM_Delayed = R(typeof(Fsm), "delayedEvents"), FSM_Variables = R(typeof(Fsm), "variables"), FSM_States = R(typeof(Fsm), "states"),
			FSM_ActiveState = R(typeof(Fsm), "activeState");
		private static readonly Func<object, long> FSM_Started = B(typeof(Fsm), "<Started>k__BackingField"), FSM_Finished = B(typeof(Fsm), "<Finished>k__BackingField"),
			FSM_Initialized = B(typeof(Fsm), "initialized"), FSM_ActiveStateEntered = B(typeof(Fsm), "activeStateEntered"),
			FSM_SwitchedState = B(typeof(Fsm), "<SwitchedState>k__BackingField");
		private static readonly Func<object, object> ST_Name = R(typeof(FsmState), "name"), ST_Actions = R(typeof(FsmState), "actions"),
			ST_ActiveAction = R(typeof(FsmState), "activeAction"), ST_ActiveActions = R(typeof(FsmState), "activeActions"),
			ST_FinishedActions = R(typeof(FsmState), "_finishedActions");
		private static readonly Func<object, long> ST_Active = B(typeof(FsmState), "active"), ST_Finished = B(typeof(FsmState), "finished"),
			ST_ActiveActionIndex = B(typeof(FsmState), "activeActionIndex"), ST_StateTime = B(typeof(FsmState), "<StateTime>k__BackingField"),
			ST_RealStartTime = B(typeof(FsmState), "<RealStartTime>k__BackingField"), ST_LoopCount = B(typeof(FsmState), "<loopCount>k__BackingField"),
			ST_MaxLoopCount = B(typeof(FsmState), "<maxLoopCount>k__BackingField");
		// FsmStateAction's own fields: a subclass may declare a field of the same name (an FsmBool `enabled`)
		private static readonly Func<object, long> ACT_Enabled = B(typeof(FsmStateAction), "enabled"), ACT_Active = B(typeof(FsmStateAction), "active"),
			ACT_Finished = B(typeof(FsmStateAction), "finished"), ACT_Entered = B(typeof(FsmStateAction), "<Entered>k__BackingField");

		private static string StateName(object st) => st == null ? "" : (ST_Name(st) as string ?? "");

		private static readonly Schema FsmSchema = new Schema("b:fsm.present", "s:fsm.name", "s:fsm.active", "s:fsm.previous", "s:fsm.switchTo",
			"b:fsm.started", "b:fsm.finished", "b:fsm.initialized", "b:fsm.activeStateEntered", "b:fsm.switchedState", "s:fsm.lastTransition",
			"E:fsm.delayed", "e:fsm.vars", "E:fsm.states", "e:fsm.state");
		// Fsm.EventTarget flattened like an action's FsmEventTarget field (fsm.eventTarget.present, .target, ...)
		// built on first use: plan building reads caches other partial files initialize
		private static Plan _fsmEventTargetPlan;
		private static Plan FsmEventTargetPlan => _fsmEventTargetPlan ?? (_fsmEventTargetPlan = MakeFsmEventTargetPlan());
		private static Plan MakeFsmEventTargetPlan()
		{
			var ops = new List<Op>();
			var f = FindField(typeof(Fsm), "<EventTarget>k__BackingField");
			if (f != null) BuildOps(ops, "fsm.eventTarget", new[] { f }, typeof(FsmEventTarget), 0, "fsm.eventTarget");
			return new Plan(ops);
		}
		private static readonly Dictionary<object, string> _transitionText = new Dictionary<object, string>(new StateWriter.RefEq());
		private static long[] _stateBuf = new long[64], _delayedBuf = new long[16];

		private static void PutFsm(StateWriter.Ent e, PlayMakerFSM pm)
		{
			var s = Slots(e.Cls, FsmSchema);
			var f = PM_Fsm(pm) as Fsm;
			_w.Raw(e, s[0], f != null ? 1 : 0);
			if (f == null) return;
			PutStr(e, s[1], FSM_Name(f));
			PutStr(e, s[2], FSM_ActiveStateName(f));
			PutStr(e, s[3], ST_NameOrEmpty(FSM_Previous(f)));
			PutStr(e, s[4], ST_NameOrEmpty(FSM_SwitchTo(f)));
			_w.Raw(e, s[5], FSM_Started(f));
			_w.Raw(e, s[6], FSM_Finished(f));
			_w.Raw(e, s[7], FSM_Initialized(f));
			_w.Raw(e, s[8], FSM_ActiveStateEntered(f));
			_w.Raw(e, s[9], FSM_SwitchedState(f));
			var lt = FSM_LastTransition(f);
			if (!_w.SameRef(e, s[10], lt))
			{
				if (lt == null) _w.Raw(e, s[10], _w.Str(""));
				else
				{
					if (!_transitionText.TryGetValue(lt, out string text))
						_transitionText[lt] = text = Fmt(Get(lt, "fsmEvent"), 1) + "->" + (Get(lt, "toState") as string ?? "");
					_w.Raw(e, s[10], _w.Str(text));
				}
				_w.Ref(e, s[10], lt);
			}
			RunPlan(e, f, FsmEventTargetPlan, 1);
			var delayed = FSM_Delayed(f) as IList;
			int nd = delayed == null ? 0 : delayed.Count;
			if (_delayedBuf.Length < nd) _delayedBuf = new long[nd * 2];
			for (int i = 0; i < nd; i++) _delayedBuf[i] = DelayedEventEid(delayed[i], e.Eid);
			_w.RawList(e, s[11], _delayedBuf, nd);
			_w.Raw(e, s[12], VarsEid(FSM_Variables(f), e.Eid, "vars"));
			// Every state, not only the active one: an action's private fields persist across visits
			// (SendRandomEventV3.loops is never reset, SendRandomEventV3.cs:23,77-78), and a state entered and left
			// within one frame is never the active state at the end of a frame.
			var states = FSM_States(f) as FsmState[];
			int ns = states == null ? 0 : states.Length;
			if (_stateBuf.Length < ns) _stateBuf = new long[ns * 2];
			for (int i = 0; i < ns; i++) _stateBuf[i] = states[i] == null ? 0 : StateEid(states[i], e.Eid);
			_w.RawList(e, s[13], _stateBuf, ns);
			var st = FSM_ActiveState(f);
			_w.Raw(e, s[14], st == null ? 0 : StateEid(st, e.Eid));
		}

		private static string ST_NameOrEmpty(object st) => st == null ? "" : (ST_Name(st) as string ?? "");

		// A string field by slot; unchanged references are not re-put.
		private static void PutStr(StateWriter.Ent e, int slot, object str)
		{
			if (_w.SameRef(e, slot, str)) return;
			_w.Raw(e, slot, _w.Str(str as string));
			_w.Ref(e, slot, str);
		}

		private static int DelayedEventEid(object d, int parent)
		{
			if (_w.VisitedThisFrame(d)) return _w.EidOf(d);
			var e = _w.Visit(d, _delayedCls, parent, "delayed");
			if (e == null) return _w.EidOf(d);
			_w.S(e, "event", Fmt(Get(d, "fsmEvent"), 1));
			_w.S(e, "target", Fmt(Get(d, "eventTarget"), 0));
			_w.F(e, "timer", (float)(Get(d, "timer") ?? 0f));
			_w.F(e, "delay", (float)(Get(d, "delay") ?? 0f));
			_w.B(e, "fired", (bool)(Get(d, "eventFired") ?? false));
			var data = Get(d, "eventData");
			_w.E(e, "eventData", data == null ? 0 : PlainEid(data, e.Eid, "eventData", 1));
			_w.Commit(e);
			return e.Eid;
		}

		private static readonly string[] VarArrays = { "floatVariables", "intVariables", "boolVariables", "stringVariables",
			"vector2Variables", "vector3Variables", "colorVariables", "rectVariables", "quaternionVariables",
			"gameObjectVariables", "objectVariables", "materialVariables", "textureVariables", "arrayVariables", "enumVariables" };
		private static readonly Func<object, object>[] VarArrayGetters = MakeVarArrayGetters();
		private static Func<object, object>[] MakeVarArrayGetters()
		{
			var r = new Func<object, object>[VarArrays.Length];
			for (int k = 0; k < r.Length; k++) r[k] = R(typeof(FsmVariables), VarArrays[k]);
			return r;
		}

		// What the class and plans of one FsmVariables were built from: its arrays and their items.  Rebuilt when an
		// array or an item is replaced.
		private sealed class VarsInfo
		{
			public object[] Arrays = new object[15];
			public object[] Items;
			public StateWriter.Cls Cls;
			public Plan[] Plans;
		}
		private static readonly Dictionary<object, VarsInfo> _varsInfo = new Dictionary<object, VarsInfo>(new StateWriter.RefEq());
		private static readonly Dictionary<string, Plan> _varPlans = new Dictionary<string, Plan>();
		private static readonly object[] _varArraysNow = new object[15];

		// FsmVariables: one field per variable, "<array>:<name>", holding the variable's stored value.  The class is
		// per variable list, so FSMs with the same variables share it.
		private static int VarsEid(object vars, int parent, string key)
		{
			if (vars == null) return 0;
			if (_w.VisitedThisFrame(vars)) return _w.EidOf(vars);
			_varsInfo.TryGetValue(vars, out VarsInfo vi);
			int count = 0;
			bool same = vi != null;
			for (int k = 0; k < VarArrays.Length; k++)
			{
				var a = VarArrayGetters[k](vars);
				_varArraysNow[k] = a;
				if (same && !ReferenceEquals(a, vi.Arrays[k])) same = false;
				if (a is IList l) count += l.Count;
			}
			if (same)
			{
				if (vi.Items.Length != count) same = false;
				else
				{
					int i = 0;
					for (int k = 0; k < VarArrays.Length && same; k++)
						if (_varArraysNow[k] is IList l)
							for (int j = 0; j < l.Count; j++) if (!ReferenceEquals(l[j], vi.Items[i++])) { same = false; break; }
				}
			}
			if (!same) _varsInfo[vars] = vi = BuildVars(count);
			var e = _w.Visit(vars, vi.Cls, parent, key);
			if (e == null) return _w.EidOf(vars);
			for (int i = 0; i < vi.Items.Length; i++) if (vi.Items[i] != null) RunPlan(e, vi.Items[i], vi.Plans[i], 1);
			_w.Commit(e);
			return e.Eid;
		}

		private static VarsInfo BuildVars(int count)
		{
			var vi = new VarsInfo { Items = new object[count], Plans = new Plan[count] };
			var sig = new StringBuilder();
			int i = 0;
			for (int k = 0; k < VarArrays.Length; k++)
			{
				vi.Arrays[k] = _varArraysNow[k];
				if (!(_varArraysNow[k] is IList arr)) continue;
				sig.Append(VarArrays[k][0]).Append(VarArrays[k][1]);
				string pre = VarArrays[k].Substring(0, VarArrays[k].Length - "Variables".Length) + ":";
				for (int j = 0; j < arr.Count; j++, i++)
				{
					var nv = arr[j];
					string vname = nv == null ? null : Get(nv, "name") as string;
					sig.Append('|').Append(nv == null ? "" : vname);
					vi.Items[i] = nv;
					if (nv == null) continue;
					string name = pre + vname;
					string pkey = nv.GetType().FullName + "\n" + name;
					if (!_varPlans.TryGetValue(pkey, out Plan p)) _varPlans[pkey] = p = NamedVarPlan(nv.GetType(), name);
					vi.Plans[i] = p;
				}
				sig.Append(';');
			}
			vi.Cls = _w.Class("FsmVariables@" + Fnv(sig.ToString()).ToString("x8"));
			return vi;
		}

		private static readonly Schema StateSchema = new Schema("s:name", "b:active", "b:finished", "i:activeActionIndex", "f:stateTime",
			"f:realStartTime", "i:loopCount", "i:maxLoopCount", "b:actionsLoaded", "E:actions", "i:activeAction", "I:activeActions", "I:finishedActions");
		private static long[] _actBuf = new long[64], _idxBuf = new long[64];

		private static int StateEid(object st, int parent)
		{
			if (_w.VisitedThisFrame(st)) return _w.EidOf(st);
			var e = _w.Visit(st, _stateCls, parent, _w.Known(st, _stateCls) ? null : StateName(st));
			if (e == null) return _w.EidOf(st);
			var s = Slots(_stateCls, StateSchema);
			PutStr(e, s[0], ST_Name(st) ?? "");
			_w.Raw(e, s[1], ST_Active(st));
			_w.Raw(e, s[2], ST_Finished(st));
			_w.Raw(e, s[3], ST_ActiveActionIndex(st));
			_w.Raw(e, s[4], ST_StateTime(st));
			_w.Raw(e, s[5], ST_RealStartTime(st));
			_w.Raw(e, s[6], ST_LoopCount(st));
			_w.Raw(e, s[7], ST_MaxLoopCount(st));
			var actions = ST_Actions(st) as FsmStateAction[];
			_w.Raw(e, s[8], actions != null ? 1 : 0);
			int na = actions == null ? 0 : actions.Length;
			if (_actBuf.Length < na) _actBuf = new long[na * 2];
			for (int i = 0; i < na; i++) _actBuf[i] = actions[i] == null ? 0 : ActionEid(actions[i], e.Eid, i);
			_w.RawList(e, s[9], _actBuf, na);
			_w.Raw(e, s[10], IndexOf(actions, ST_ActiveAction(st)));
			PutIndices(e, s[11], actions, ST_ActiveActions(st) as IList);
			PutIndices(e, s[12], actions, ST_FinishedActions(st) as IList);
			_w.Commit(e);
			return e.Eid;
		}

		private static int IndexOf(FsmStateAction[] actions, object a)
		{
			if (a == null || actions == null) return -1;
			for (int i = 0; i < actions.Length; i++) if (ReferenceEquals(actions[i], a)) return i;
			return -1;
		}

		private static void PutIndices(StateWriter.Ent e, int slot, FsmStateAction[] actions, IList l)
		{
			int n = l == null ? 0 : l.Count;
			if (_idxBuf.Length < n) _idxBuf = new long[n * 2];
			for (int i = 0; i < n; i++) _idxBuf[i] = IndexOf(actions, l[i]);
			_w.RawList(e, slot, _idxBuf, n);
		}

		private static readonly Schema ActionSchema = new Schema("b:base.enabled", "b:base.active", "b:base.finished", "b:base.entered");

		private static int ActionEid(FsmStateAction a, int parent, int i)
		{
			var t = a.GetType();
			var cls = ClassOf(_actionCls, t, "action:");
			var e = _w.Visit(a, cls, parent, _w.Known(a, cls) ? null : i + ":" + t.Name);
			if (e == null) return _w.EidOf(a);
			var s = Slots(cls, ActionSchema);
			_w.Raw(e, s[0], ACT_Enabled(a));
			_w.Raw(e, s[1], ACT_Active(a));
			_w.Raw(e, s[2], ACT_Finished(a));
			_w.Raw(e, s[3], ACT_Entered(a));
			RunPlan(e, a, PlanFor(t, typeof(FsmStateAction)), 1);
			_w.Commit(e);
			return e.Eid;
		}

		// ================================================================ reflection (the encoders behind K_SLOW ops)
		private delegate void Enc(StateWriter.Ent e, string name, object v, int depth);

		private static readonly Dictionary<Type, Enc> _enc = new Dictionary<Type, Enc>();

		// The encoder for a declared field type.  The wire type depends only on the declared type, so a field keeps
		// one type for the whole record (StateWriter.Field).
		private static Enc EncFor(Type t)
		{
			if (_enc.TryGetValue(t, out Enc enc)) return enc;
			enc = MakeEnc(t, 0);
			_enc[t] = enc;
			return enc;
		}

		private static Enc MakeEnc(Type t, int structDepth)
		{
			if (t == typeof(bool)) return (e, n, v, d) => _w.B(e, n, v != null && (bool)v);
			if (t.IsEnum)
			{
				var u = Enum.GetUnderlyingType(t);
				if (u == typeof(long) || u == typeof(ulong)) return (e, n, v, d) => _w.L(e, n, v == null ? 0 : Convert.ToInt64(v));
				return (e, n, v, d) => _w.I(e, n, v == null ? 0 : unchecked((int)Convert.ToInt64(v)));
			}
			if (t == typeof(int) || t == typeof(short) || t == typeof(sbyte) || t == typeof(byte) || t == typeof(ushort) || t == typeof(char))
				return (e, n, v, d) => _w.I(e, n, v == null ? 0 : Convert.ToInt32(v));
			if (t == typeof(uint)) return (e, n, v, d) => _w.I(e, n, v == null ? 0 : unchecked((int)(uint)v));
			if (t == typeof(long)) return (e, n, v, d) => _w.L(e, n, v == null ? 0 : (long)v);
			if (t == typeof(ulong)) return (e, n, v, d) => _w.L(e, n, v == null ? 0 : unchecked((long)(ulong)v));
			if (t == typeof(float)) return (e, n, v, d) => _w.F(e, n, v == null ? 0f : (float)v);
			if (t == typeof(double)) return (e, n, v, d) => _w.D(e, n, v == null ? 0.0 : (double)v);
			if (t == typeof(string)) return (e, n, v, d) => _w.S(e, n, v as string);
			if (typeof(UnityEngine.Object).IsAssignableFrom(t)) return (e, n, v, d) => _w.O(e, n, Iid(v as UnityEngine.Object));
			if (typeof(Delegate).IsAssignableFrom(t)) return (e, n, v, d) => _w.Strs(e, n, DelegateNames(v as Delegate));
			if (typeof(NamedVariable).IsAssignableFrom(t)) return (e, n, v, d) => PutNamedVar(e, n, v as NamedVariable, t, d);
			if (t == typeof(FsmOwnerDefault))
				return (e, n, v, d) =>
				{
					_w.I(e, n + ".ownerOption", v == null ? 0 : Convert.ToInt32(Get(v, "ownerOption")));
					var g = Get(v, "gameObject");
					_w.O(e, n + ".gameObject", g == null ? 0 : Iid(Get(g, "value") as UnityEngine.Object));
				};
			if (Opaque(t)) return (e, n, v, d) => _w.S(e, n, v == null ? "null" : Fmt(v, 0));
			if (t.IsValueType)
			{
				if (structDepth >= 3) return (e, n, v, d) => _w.S(e, n, Fmt(v, 0));
				var fs = t.GetFields(InstAll);
				var subs = new Enc[fs.Length];
				for (int i = 0; i < fs.Length; i++) subs[i] = fs[i].FieldType == t ? null : MakeEnc(fs[i].FieldType, structDepth + 1);
				return (e, n, v, d) =>
				{
					for (int i = 0; i < fs.Length; i++)
					{
						if (subs[i] == null) continue;
						subs[i](e, n + "." + fs[i].Name, v == null ? null : fs[i].GetValue(v), d);
					}
				};
			}
			if (typeof(IDictionary).IsAssignableFrom(t)) return (e, n, v, d) => PutFormatted(e, n, DictItems(v as IDictionary));
			var elem = ElementType(t);
			if (elem != null) return ListEnc(elem);
			if (typeof(IEnumerable).IsAssignableFrom(t)) return (e, n, v, d) => PutFormatted(e, n, SeqItems(v as IEnumerable));
			if (t == typeof(object) || t.IsInterface) return (e, n, v, d) => _w.S(e, n, v == null ? "null" : Fmt(v, 0));
			return (e, n, v, d) => _w.E(e, n, PlainEid(v, e.Eid, n, d + 1));
		}

		// A PlayMaker variable's stored value, typed by the variable class (NamedVariable subclasses keep it in a
		// private field `value`; FsmEnum in `intValue`; FsmArray in `values`).
		private static void PutNamedVar(StateWriter.Ent e, string name, NamedVariable nv, Type declared, int depth)
		{
			if (typeof(FsmEnum).IsAssignableFrom(declared)) { _w.I(e, name, nv == null ? 0 : (int)(Get(nv, "intValue") ?? 0)); return; }
			if (typeof(FsmArray).IsAssignableFrom(declared) || typeof(FsmVar).IsAssignableFrom(declared) || declared == typeof(NamedVariable) || declared.IsAbstract)
			{
				_w.S(e, name, nv == null ? "null" : Fmt(nv, 0));
				return;
			}
			var vf = ValueField(declared);
			if (vf == null) { _w.S(e, name, Fmt(nv, 0)); return; }
			EncFor(vf.FieldType)(e, name, nv == null ? null : vf.GetValue(nv), depth);
		}

		private static readonly Dictionary<Type, FieldInfo> _valueField = new Dictionary<Type, FieldInfo>();
		private static FieldInfo ValueField(Type t)
		{
			if (_valueField.TryGetValue(t, out FieldInfo f)) return f;
			for (var bt = t; bt != null && f == null; bt = bt.BaseType) f = bt.GetField("value", InstAll | BindingFlags.DeclaredOnly);
			_valueField[t] = f;
			return f;
		}

		// Types formatted to a string instead of expanded: PlayMaker's static graph (an FSM's structure is dump data,
		// analysis/fsm), reflection and runtime plumbing, yield instructions, curves.
		private static readonly Dictionary<Type, bool> _opaque = new Dictionary<Type, bool>();
		private static bool Opaque(Type t)
		{
			if (_opaque.TryGetValue(t, out bool r)) return r;
			r = OpaqueUncached(t);
			_opaque[t] = r;
			return r;
		}

		private static bool OpaqueUncached(Type t)
		{
			if (t == typeof(Type) || typeof(MemberInfo).IsAssignableFrom(t)) return true;
			if (typeof(YieldInstruction).IsAssignableFrom(t) || typeof(CustomYieldInstruction).IsAssignableFrom(t)) return true;
			if (t == typeof(AnimationCurve) || t == typeof(Gradient)) return true;
			if (t == typeof(Fsm) || t == typeof(FsmState) || typeof(FsmStateAction).IsAssignableFrom(t) || t == typeof(FsmTransition)
				|| t == typeof(FsmEvent) || t == typeof(FsmEventTarget) || t == typeof(FsmVariables) || t == typeof(FsmTemplate)
				|| t == typeof(tk2dSpriteAnimationClip)) return true;
			string ns = t.Namespace ?? "";
			foreach (var p in OpaqueNamespaces) if (ns.StartsWith(p, StringComparison.Ordinal)) return true;
			return false;
		}

		private static readonly string[] OpaqueNamespaces = { "System.Reflection", "System.Threading", "System.IO", "System.Net", "System.Diagnostics",
			"System.Text", "System.Globalization", "System.Runtime", "Mono", "MonoMod", "Newtonsoft", "WebSocketSharp", "Modding",
			"UnityEngine.Events", "UnityEngine.EventSystems", "TMPro", "Steamworks", "GalaxyCSharp", "TeamCherry", "Language" };

		private static Type ElementType(Type t)
		{
			if (t.IsArray) return t.GetElementType();
			if (t.IsGenericType && typeof(IList).IsAssignableFrom(t)) return t.GetGenericArguments()[0];
			if (t == typeof(ArrayList)) return typeof(object);
			return null;
		}

		private static Enc ListEnc(Type el)
		{
			if (el == typeof(bool) || el.IsEnum || el == typeof(int) || el == typeof(short) || el == typeof(sbyte) || el == typeof(byte)
				|| el == typeof(ushort) || el == typeof(char) || el == typeof(uint))
				return (e, n, v, d) => _w.List(e, n, 'I', Nums(v as IList, x => el == typeof(bool) ? ((bool)x ? 1 : 0) : unchecked((int)Convert.ToInt64(x))));
			if (el == typeof(long) || el == typeof(ulong)) return (e, n, v, d) => _w.List(e, n, 'L', Nums(v as IList, x => x is ulong u ? unchecked((long)u) : Convert.ToInt64(x)));
			if (el == typeof(double)) return (e, n, v, d) => _w.List(e, n, 'L', Nums(v as IList, x => BitConverter.DoubleToInt64Bits((double)x)));
			if (el == typeof(float)) return (e, n, v, d) => _w.List(e, n, 'F', Nums(v as IList, x => StateWriter.FloatBits((float)x)));
			if (el == typeof(Vector2) || el == typeof(Vector3) || el == typeof(Vector4) || el == typeof(Quaternion) || el == typeof(Color))
			{
				var fs = el.GetFields(BindingFlags.Instance | BindingFlags.Public);
				return (e, n, v, d) =>
				{
					var l = v as IList;
					var r = new List<long>();
					if (l != null) for (int i = 0; i < l.Count && r.Count < NumListCap; i++) foreach (var f in fs) r.Add(StateWriter.FloatBits((float)f.GetValue(l[i])));
					_w.List(e, n, 'F', r.ToArray());
				};
			}
			if (el == typeof(string)) return (e, n, v, d) => PutFormatted(e, n, SeqItems(v as IEnumerable, raw: true));
			if (typeof(UnityEngine.Object).IsAssignableFrom(el)) return (e, n, v, d) => _w.List(e, n, 'O', Nums(v as IList, x => Iid(x as UnityEngine.Object)));
			if (el.IsValueType || el == typeof(object) || el.IsInterface || Opaque(el) || typeof(Delegate).IsAssignableFrom(el)
				|| typeof(NamedVariable).IsAssignableFrom(el) || ElementType(el) != null || typeof(IDictionary).IsAssignableFrom(el))
				return (e, n, v, d) => PutFormatted(e, n, SeqItems(v as IEnumerable));
			return (e, n, v, d) =>
			{
				var l = v as IList;
				int count = l == null ? 0 : l.Count;
				var r = new long[Math.Min(count, ListCap)];
				for (int i = 0; i < r.Length; i++) r[i] = PlainEid(l[i], e.Eid, n, i, d + 1);
				_w.List(e, n, 'E', r);
				_w.I(e, Suffix(n, 0), count);
			};
		}

		private static long[] Nums(IList l, Func<object, long> f)
		{
			if (l == null) return new long[0];
			var r = new long[Math.Min(l.Count, NumListCap)];
			for (int i = 0; i < r.Length; i++) r[i] = l[i] == null ? 0 : f(l[i]);
			return r;
		}

		// A formatted collection: the first ListCap items, the count and a hash over all items.
		private static void PutFormatted(StateWriter.Ent e, string n, List<string> items)
		{
			uint h = 2166136261;
			foreach (var s in items) h = Fnv(s, h);
			_w.Strs(e, n, items.Count > ListCap ? items.GetRange(0, ListCap) : items);
			_w.I(e, Suffix(n, 0), items.Count);
			_w.I(e, Suffix(n, 1), unchecked((int)h));
		}

		private static List<string> SeqItems(IEnumerable s, bool raw = false)
		{
			var r = new List<string>();
			if (s == null) return r;
			foreach (var x in s) r.Add(raw ? (x as string ?? "null") : Fmt(x, 1));
			return r;
		}

		private static List<string> DictItems(IDictionary d)
		{
			var r = new List<string>();
			if (d == null) return r;
			foreach (DictionaryEntry kv in d) r.Add(Fmt(kv.Key, 1) + "=" + Fmt(kv.Value, 1));
			return r;
		}

		private static List<string> DelegateNames(Delegate d)
		{
			var r = new List<string>();
			if (d == null) return r;
			foreach (var x in d.GetInvocationList())
			{
				string target = x.Target is UnityEngine.Object uo ? "#" + Iid(uo)
					: x.Target is FsmStateAction a ? ActionName(a)
					: x.Target == null ? "static" : x.Target.GetType().FullName;
				r.Add(x.Method.DeclaringType?.FullName + "." + x.Method.Name + "@" + target);
			}
			return r;
		}

		// An action by its place: "#<PlayMakerFSM iid>/<state>/<index in the state's actions>".  The owners of
		// tk2dSpriteAnimator.AnimationCompleted and the PlayMakerUnity2DProxy delegate lists are actions
		// (Tk2dPlayAnimationWithEvents, Collision2dEvent), and one owner must be told from another.
		private static readonly Func<object, object> ACT_FsmState = R(typeof(FsmStateAction), "fsmState"), ACT_Fsm = R(typeof(FsmStateAction), "fsm"),
			FSM_Owner = R(typeof(Fsm), "owner");
		private static string ActionName(FsmStateAction a)
		{
			var st = ACT_FsmState(a);
			var fsm = ACT_Fsm(a);
			var owner = fsm == null ? null : FSM_Owner(fsm) as UnityEngine.Object;
			int index = st == null ? -1 : IndexOf(ST_Actions(st) as FsmStateAction[], a);
			return "#" + Iid(owner) + "/" + StateName(st) + "/" + index;
		}

		// A process-independent text form: floats as bit patterns, Unity objects as instance ids, no object hashes.
		private static string Fmt(object v, int depth)
		{
			switch (v)
			{
				case null: return "null";
				case string s: return "\"" + s + "\"";
				case bool b: return b ? "true" : "false";
				case float f: return "f:" + StateWriter.FloatBits(f).ToString("x8");
				case double dd: return "d:" + BitConverter.DoubleToInt64Bits(dd).ToString("x16");
				case Enum en: return en.GetType().Name + "." + en;
				case UnityEngine.Object uo: return "#" + Iid(uo) + ":" + uo.GetType().Name;
				case Delegate dl: return string.Join("+", DelegateNames(dl).ToArray());
				case FsmEvent fe: return "ev:" + (Get(fe, "name") as string);
				case NamedVariable nv:
					{
						var vf = ValueField(nv.GetType());
						object raw = vf != null ? vf.GetValue(nv) : nv is FsmArray ? Get(nv, "values") : nv is FsmEnum ? Get(nv, "intValue") : null;
						return "var:" + (Get(nv, "name") as string) + "=" + Fmt(raw, depth + 1);
					}
				case FsmState st: return "state:" + StateName(st);
				case FsmStateAction a: return "action:" + a.GetType().Name + ActionName(a);
				case Fsm fsm: return "fsm:" + (Get(fsm, "name") as string);
				case FsmEventTarget et:
					return "target:" + Fmt(Get(et, "target"), depth + 1) + "|" + Fmt(Get(et, "excludeSelf"), depth + 1) + "|"
						+ Fmt(Get(et, "gameObject"), depth + 1) + "|" + Fmt(Get(et, "fsmName"), depth + 1) + "|"
						+ Fmt(Get(et, "sendToChildren"), depth + 1) + "|" + Fmt(Get(et, "fsmComponent"), depth + 1);
				case FsmOwnerDefault od: return "owner:" + Fmt(Get(od, "ownerOption"), depth + 1) + "|" + Fmt(Get(od, "gameObject"), depth + 1);
				case WaitForSeconds w: return "WaitForSeconds(" + Fmt(Get(w, "m_Seconds"), depth + 1) + ")";
				case tk2dSpriteAnimationClip clip: return "clip:" + clip.name;
			}
			var t = v.GetType();
			if (t.IsPrimitive) return Convert.ToString(v, CultureInfo.InvariantCulture);
			if (depth > 3) return t.Name;
			if (v is IDictionary dict)
			{
				var items = DictItems(dict);
				return "{" + string.Join(",", items.GetRange(0, Math.Min(16, items.Count)).ToArray()) + (items.Count > 16 ? ",..." + items.Count : "") + "}";
			}
			if (v is IEnumerable seq && !(v is string))
			{
				var sb = new StringBuilder("[");
				int i = 0;
				foreach (var x in seq)
				{
					if (i < 16) { if (i > 0) sb.Append(','); sb.Append(Fmt(x, depth + 1)); }
					i++;
				}
				if (i > 16) sb.Append(",...").Append(i);
				return sb.Append(']').ToString();
			}
			if (t.IsValueType)
			{
				var sb = new StringBuilder("(");
				bool first = true;
				foreach (var f in t.GetFields(InstAll))
				{
					if (!first) sb.Append(',');
					first = false;
					sb.Append(Fmt(f.GetValue(v), depth + 1));
				}
				return sb.Append(')').ToString();
			}
			return t.FullName;
		}

		private static uint Fnv(string s, uint h = 2166136261)
		{
			foreach (char c in s) { h ^= c; h *= 16777619; }
			return h;
		}

		// "<name>#n" and "<name>#h" without a concatenation per frame
		private static readonly Dictionary<string, string[]> _suffixed = new Dictionary<string, string[]>();
		private static string Suffix(string n, int k)
		{
			if (!_suffixed.TryGetValue(n, out string[] s)) _suffixed[n] = s = new[] { n + "#n", n + "#h" };
			return s[k];
		}

		// A plain (non-Unity) object reached from a recorded field: its own entity, dedup'd by reference within the frame.
		private static int PlainEid(object o, int parent, string key, int depth) => PlainEid(o, parent, key, -1, depth);

		// `index` >= 0: the object is element `index` of list `key` (its key is "key[index]", built at birth only).
		private static int PlainEid(object o, int parent, string key, int index, int depth)
		{
			if (o == null) return 0;
			if (_w.VisitedThisFrame(o)) return _w.EidOf(o);
			if (depth > MaxPlainDepth) { _plainTruncated++; return 0; }
			if (_plainLeft-- <= 0) { _budgetHits++; return 0; }
			var t = o.GetType();
			if (t.IsValueType || Opaque(t) || OracleOwn(t)) return 0;
			var cls = ClassOf(_plainCls, t, "plain:");
			var e = _w.Visit(o, cls, parent, index < 0 || _w.Known(o, cls) ? key : key + "[" + index + "]");
			if (e == null) return _w.EidOf(o);
			RunPlan(e, o, PlanFor(t, typeof(object)), depth);
			_w.Commit(e);
			return e.Eid;
		}

		// The mod's own objects (the env and its coroutines) are not game state; a mod type that extends a game type
		// (InputDeviceShim : InControl.InputDevice) is: the game reads it.
		private static readonly Dictionary<Type, bool> _own = new Dictionary<Type, bool>();
		private static bool OracleOwn(Type t)
		{
			if (t.Assembly != typeof(StateRecorder).Assembly) return false;
			if (_own.TryGetValue(t, out bool r)) return r;
			r = true;
			for (var bt = t.BaseType; bt != null && r; bt = bt.BaseType)
			{
				string an = bt.Assembly.GetName().Name;
				if (an == "Assembly-CSharp" || an == "Assembly-CSharp-firstpass" || an == "PlayMaker") r = false;
			}
			_own[t] = r;
			return r;
		}

		// ================================================================ statics
		// Reading a static field runs the type's static constructor if it has not run yet, which could draw Random
		// or register handlers; so a type with a static constructor is read only when listed (ExtraStaticTypes).
		// The others are named in the trailer (statics_skipped).
		private static readonly List<string> _staticsSkipped = new List<string>();
		private static void AddStatic(Type t, bool listed)
		{
			for (var bt = t; bt != null && bt != typeof(MonoBehaviour) && bt != typeof(object); bt = bt.BaseType)
			{
				if (!_staticSeen.Add(bt)) return;
				if (bt.ContainsGenericParameters) continue;
				if (!listed && bt.TypeInitializer != null) { _staticsSkipped.Add(bt.FullName); continue; }
				var ops = new List<Op>();
				foreach (var f in bt.GetFields(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
				{
					if (f.IsLiteral) continue;
					if (f.IsInitOnly && (f.FieldType.IsPrimitive || f.FieldType.IsEnum || f.FieldType == typeof(string) || f.FieldType.IsValueType)) continue;
					if (f.Name.StartsWith("<>f__am$cache", StringComparison.Ordinal) || f.Name.StartsWith("CS$<>", StringComparison.Ordinal)
						|| f.Name.StartsWith("<>9", StringComparison.Ordinal)) continue;
					BuildOps(ops, f.Name, new[] { f }, f.FieldType, 0, "static " + bt.Name + "." + f.Name);
				}
				if (ops.Count == 0) continue;
				_staticPlans[bt] = new Plan(ops);
				_staticTypes.Add(bt);
			}
		}

		private static void CaptureStatics()
		{
			for (int i = 0; i < _staticTypes.Count; i++)
			{
				var t = _staticTypes[i];
				var e = _w.Visit(t, ClassOf(_staticCls, t, "static:"), 0, t.FullName);
				if (e == null) continue;
				RunPlan(e, null, _staticPlans[t], 1);
				_w.Commit(e);
			}
		}

		// ================================================================ dump mode: native snapshot at SceneReady
		// One frame with only the engine's native state (NativeState), for the sim's scene start (docs/state-record.md
		// "Dump mode").  Called by DumpDriver when HK_ORACLE_DUMP_NATIVE=1.
		public static void DumpNative(string path)
		{
			var header = HeaderJson(Hooks.Current);
			header["dump"] = "native";
			using (var fs = new FileStream(path, FileMode.Create, FileAccess.Write))
			{
				var w = new StateWriter(fs, header.ToString(Formatting.None));
				var nat = new NativeState(w, m => HKOracle.Instance.Log("[DumpNative] " + m));
				nat.Init();
				w.BeginFrame(Time.frameCount, -1, 0, Time.deltaTime > 0f ? 1 : 0);
				var gos = new List<GameObject>();
				var comps = new List<Component>();
				CollectLive(gos, comps);
				var hc = Hero();
				if (hc != null) nat.SetScene(hc.GetComponent<Rigidbody2D>());
				nat.Check(gos, comps);
				nat.CaptureEngine(0);
				w.EndFrame();
				w.Close(new JObject { ["native"] = nat.Info(), ["layout_mismatches"] = nat.CheckFailures }.ToString(Formatting.None));
			}
		}
	}
}
