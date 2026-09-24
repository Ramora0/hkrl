using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using Mono.Cecil.Cil;
using MonoMod.Cil;
using MonoMod.RuntimeDetour;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using UnityEngine;
using HKOracle.Dump;
using HKOracle.Env;

namespace HKOracle.Record
{
	// Measures Unity's native component lifecycle.  Unity's player loop (BehaviourManager, delayed
	// Start, coroutine scheduler, physics-callback dispatch) is native code with no source; the
	// simulator places every component's Start and Update in its frame loop by hand.
	// This recorder logs, per frame and in order, every lifecycle callback Unity makes on every
	// game component, so tools/lifecycle_rules.py can extract the engine's rules from real fights.
	//
	// Installed in record mode when HK_ORACLE_LIFECYCLE=1 (Mode.cs).  Output per episode (armed at
	// Hooks.SceneReady, flushed at Hooks.EpisodeEnd / quit): <trace base>.lifecycle.gz, format in
	// WriteEpisode below and in tools/lifecycle_rules.py.
	//
	// What is hooked (ILHook, a static call injected at method entry; the body is untouched):
	//   * every DECLARED instance method of a MonoBehaviour-derived type in Assembly-CSharp,
	//     Assembly-CSharp-firstpass and PlayMaker whose name is a Unity message in kNames, with a
	//     parameter shape Unity dispatches (none; or one Collider2D/Collision2D for the physics
	//     messages).  Start returning IEnumerator is included.
	//   * MoveNext of compiler-generated IEnumerator-only iterator classes nested in those types
	//     (coroutine resumes), with the owner read from <>4__this.
	//   * ObjectPool.Spawn / ObjectPool.Recycle (begin/end markers), TrainingEnv step/frame
	//     markers (Hooks.StepBegin / Hooks.Frame).
	//   * the Unity PlayerLoop: a marker system is inserted before every native subsystem and at
	//     the end of every group, so every event is attributable to the native subsystem it ran in
	//     (e.g. EarlyUpdate/ScriptRunDelayedStartupFrame vs Update/ScriptRunBehaviourUpdate).
	//
	// Dispatch fidelity: when a base class and a derived class both declare e.g. Update, Unity
	// calls only the most-derived one; a hooked base method reached through base.Update() is
	// therefore NOT recorded (the per-runtime-type resolution table _declOf).
	//
	// Inertness: the injected code reads Time.frameCount / Time.inFixedTimeStep / Time.timeScale,
	// component identity (name, parent chain, sibling index, GetComponents, instance ids) on first
	// sight of an instance, and appends to an int buffer.  It writes no game state.  The proof is
	// a replay with HK_ORACLE_LIFECYCLE=1 vs =0 compared on the .hktrace observation stream.
	public static class LifecycleRecorder
	{
		// ------------------------------------------------------------------ codes (wire format)
		public const int C_FRAME = 0, C_MARK = 1;
		public const int C_AWAKE = 2, C_ONENABLE = 3, C_START = 4, C_FIXEDUPDATE = 5, C_UPDATE = 6,
			C_LATEUPDATE = 7, C_ONDISABLE = 8, C_ONDESTROY = 9, C_TRIG_ENTER = 10, C_TRIG_STAY = 11,
			C_TRIG_EXIT = 12, C_COLL_ENTER = 13, C_COLL_STAY = 14, C_COLL_EXIT = 15,
			C_VISIBLE = 16, C_INVISIBLE = 17;
		public const int C_COROUTINE = 18, C_STEP = 19, C_ENVFRAME = 20, C_SPAWN_BEGIN = 21,
			C_SPAWN_END = 22, C_RECYCLE_BEGIN = 23, C_RECYCLE_END = 24, C_ARM = 25;
		// EXIT: return from Awake/OnEnable/OnDisable/Start/OnDestroy (w1 = instance, aux = the callback code) -- the
		// nesting structure (a SetActive inside an OnEnable, a Start that disables its children, ...).
		// ACT_BEGIN/ACT_END: PlayMaker ActivateGameObject.DoActivateGameObject / OnExit (the FSM-driven SetActive);
		// END carries the target GameObject and aux = activate | recursive<<1 | onExit<<2.
		public const int C_EXIT = 26, C_ACT_BEGIN = 27, C_ACT_END = 28;
		private const int NCODES = 29;
		// aux on FixedUpdate/Update/LateUpdate: 1 = Unity dispatched the callback but FsmPauseGate
		// (oracle/Game/FsmPauseGate.cs) returned before the body because Time.timeScale <= 0 (seen by an outer detour
		// installed at first arm, after the gate)
		public const int AUX_GATED = 1;

		private static readonly string[] kCodeNames = {
			"FRAME", "MARK", "Awake", "OnEnable", "Start", "FixedUpdate", "Update", "LateUpdate",
			"OnDisable", "OnDestroy", "OnTriggerEnter2D", "OnTriggerStay2D", "OnTriggerExit2D",
			"OnCollisionEnter2D", "OnCollisionStay2D", "OnCollisionExit2D", "OnBecameVisible",
			"OnBecameInvisible", "COROUTINE", "STEP", "ENVFRAME", "SPAWN_BEGIN", "SPAWN_END",
			"RECYCLE_BEGIN", "RECYCLE_END", "ARM", "EXIT", "ACT_BEGIN", "ACT_END" };

		// message name -> code (the hooked set)
		private static readonly Dictionary<string, int> kNames = new Dictionary<string, int> {
			{ "Awake", C_AWAKE }, { "OnEnable", C_ONENABLE }, { "Start", C_START },
			{ "FixedUpdate", C_FIXEDUPDATE }, { "Update", C_UPDATE }, { "LateUpdate", C_LATEUPDATE },
			{ "OnDisable", C_ONDISABLE }, { "OnDestroy", C_ONDESTROY },
			{ "OnTriggerEnter2D", C_TRIG_ENTER }, { "OnTriggerStay2D", C_TRIG_STAY }, { "OnTriggerExit2D", C_TRIG_EXIT },
			{ "OnCollisionEnter2D", C_COLL_ENTER }, { "OnCollisionStay2D", C_COLL_STAY }, { "OnCollisionExit2D", C_COLL_EXIT },
			{ "OnBecameVisible", C_VISIBLE }, { "OnBecameInvisible", C_INVISIBLE } };

		private static bool IsPhysics(int code) => code >= C_TRIG_ENTER && code <= C_COLL_EXIT;
		private static bool HasExit(int code) => code == C_AWAKE || code == C_ONENABLE || code == C_ONDISABLE || code == C_START || code == C_ONDESTROY;

		// ------------------------------------------------------------------ instance table
		private sealed class RefEq : IEqualityComparer<object>
		{
			public new bool Equals(object a, object b) => ReferenceEquals(a, b);
			public int GetHashCode(object o) => RuntimeHelpers.GetHashCode(o);
		}

		private sealed class Inst
		{
			public int idx;
			public int[] decl;       // per code: the hooked-method id Unity dispatches for this runtime type (-2 = unhooked declarer, -1 = none)
			public int owner = -1;   // iterators: the owning component's index
			public JArray row;
		}

		private const int K_MB = 0, K_ITER = 1, K_COL = 2, K_GO = 3, K_OTHER = 4;

		private static readonly Dictionary<object, Inst> _inst = new Dictionary<object, Inst>(1 << 14, new RefEq());
		private static readonly List<JArray> _rows = new List<JArray>(1 << 14);
		private static readonly Dictionary<Type, int> _typeIdx = new Dictionary<Type, int>();
		private static readonly List<string> _typeNames = new List<string>();
		private static readonly Dictionary<Type, int[]> _declOf = new Dictionary<Type, int[]>();

		// hooked methods: id -> (declaring type, code)
		private static readonly List<MethodBase> _hookedMethods = new List<MethodBase>();
		private static readonly Dictionary<MethodBase, int> _hookedId = new Dictionary<MethodBase, int>();
		private static readonly List<string> _iterNames = new List<string>();
		private static readonly List<ILHook> _ilhooks = new List<ILHook>();
		private static readonly List<Hook> _hooks = new List<Hook>();

		// ------------------------------------------------------------------ event buffer
		private const int kChunk = 1 << 20;   // ints per chunk (4 MB)
		private static readonly List<int[]> _chunks = new List<int[]>();
		private static int[] _cur;
		private static int _pos;
		private static long _events;

		private static bool _armed;
		private static int _lastFrame = int.MinValue;
		private static int _episode;
		private static string _base;
		private static int _mainThread;
		private static readonly List<string> _loopNames = new List<string>();
		private static JObject _installInfo = new JObject();
		private static readonly Dictionary<string, int> _errs = new Dictionary<string, int>();

		private static void Log(string m) => HKOracle.Instance.Log("[Lifecycle] " + m);

		private static void Err(string site, Exception e)
		{
			_errs.TryGetValue(site, out int n);
			_errs[site] = n + 1;
			if (n < 3) Log($"ERROR {site}: {HKOracle.DescribeException(e)}");
		}

		// ================================================================== install
		public static void Install()
		{
			_base = Mode.SidePath("");
			_mainThread = System.Threading.Thread.CurrentThread.ManagedThreadId;
			try { string dir = Path.GetDirectoryName(_base); if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir); }
			catch (Exception e) { Log($"cannot create output dir for {_base}: {e.Message}"); return; }

			var sw = System.Diagnostics.Stopwatch.StartNew();
			HookAssemblies();
			HookPool();
			HookActivate();
			InstallLoopMarkers();
			_installInfo["install_ms"] = sw.ElapsedMilliseconds;

			Hooks.SceneReady += ctx => { Flush("scene_ready_before_arm"); Arm(); };
			Hooks.EpisodeEnd += info => { Flush(info); };
			Hooks.StepBegin += (step, action, committed) => Mark(C_STEP, step, committed ? 1 : 0);
			Hooks.Frame += () => Mark(C_ENVFRAME, 0, -1);
			Application.quitting += () => Flush("quit");
			Log($"armed at scene_ready; writing {_base}[.eN].lifecycle.gz; {_installInfo.ToString(Formatting.None)}");
		}

		private static bool _gateObserved;
		private static void Arm()
		{
			if (!_gateObserved) { _gateObserved = true; HookGateObserver(); }
			ResetBuffers();
			_armed = true;
			Mark(C_ARM, 1, -1);
		}

		private static void ResetBuffers()
		{
			_inst.Clear(); _rows.Clear(); _chunks.Clear();
			_cur = new int[kChunk]; _chunks.Add(_cur); _pos = 0; _events = 0;
			_lastFrame = int.MinValue;
		}

		private static IEnumerable<Assembly> TargetAssemblies()
		{
			var want = new HashSet<string> { "Assembly-CSharp", "Assembly-CSharp-firstpass", "PlayMaker" };
			foreach (var a in AppDomain.CurrentDomain.GetAssemblies())
			{
				string n;
				try { n = a.GetName().Name; } catch { continue; }
				if (want.Contains(n)) yield return a;
			}
		}

		private static Type[] SafeTypes(Assembly a)
		{
			try { return a.GetTypes(); }
			catch (ReflectionTypeLoadException e) { var l = new List<Type>(); foreach (var t in e.Types) if (t != null) l.Add(t); return l.ToArray(); }
		}

		// Unity's parameter shapes for the hooked messages.
		private static bool ShapeOk(MethodInfo m, int code)
		{
			var ps = m.GetParameters();
			if (!IsPhysics(code)) return ps.Length == 0;
			if (ps.Length == 0) return true;
			if (ps.Length != 1) return false;
			var want = code <= C_TRIG_EXIT ? typeof(Collider2D) : typeof(Collision2D);
			return ps[0].ParameterType == want;
		}

		private static readonly MethodInfo _recM = typeof(LifecycleRecorder).GetMethod(nameof(Rec));
		private static readonly MethodInfo _recPhysM = typeof(LifecycleRecorder).GetMethod(nameof(RecPhys));
		private static readonly MethodInfo _recCoM = typeof(LifecycleRecorder).GetMethod(nameof(RecCo));
		private static readonly MethodInfo _recExitM = typeof(LifecycleRecorder).GetMethod(nameof(RecExit));

		private static void HookAssemblies()
		{
			int types = 0, methods = 0, iters = 0, failed = 0, badShape = 0, genericSkipped = 0;
			var fails = new JArray();
			var perAsm = new JObject();
			var mbType = typeof(MonoBehaviour);
			foreach (var asm in TargetAssemblies())
			{
				int asmHooks = 0;
				foreach (var t in SafeTypes(asm))
				{
					bool isMb;
					try { isMb = t.IsClass && mbType.IsAssignableFrom(t); } catch { continue; }
					if (!isMb) continue;
					if (t.ContainsGenericParameters) { genericSkipped++; continue; }
					types++;
					MethodInfo[] ms;
					try { ms = t.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly); }
					catch { continue; }
					foreach (var m in ms)
					{
						if (!kNames.TryGetValue(m.Name, out int code)) continue;
						if (m.IsAbstract || m.ContainsGenericParameters) continue;
						if (!ShapeOk(m, code)) { badShape++; continue; }
						System.Reflection.MethodBody body = null;
						try { body = m.GetMethodBody(); } catch { }
						if (body == null) continue;
						int id = _hookedMethods.Count;
						try
						{
							var h = new ILHook(m, il => InjectRec(il, code, id, IsPhysics(code) && m.GetParameters().Length == 1 ? m.GetParameters()[0].ParameterType : null));
							_ilhooks.Add(h);
							_hookedMethods.Add(m); _hookedId[m] = id;
							methods++; asmHooks++;
						}
						catch (Exception e)
						{
							failed++;
							if (fails.Count < 200) fails.Add($"{t.FullName}.{m.Name}: {e.GetType().Name}: {e.Message}");
						}
					}
					// coroutine iterators nested in this type
					Type[] nested;
					try { nested = t.GetNestedTypes(BindingFlags.NonPublic | BindingFlags.Public); } catch { continue; }
					foreach (var nt in nested)
					{
						try
						{
							if (nt.ContainsGenericParameters) continue;
							if (!nt.IsDefined(typeof(CompilerGeneratedAttribute), false)) continue;
							if (!typeof(IEnumerator).IsAssignableFrom(nt) || typeof(IEnumerable).IsAssignableFrom(nt)) continue;
							var mn = nt.GetMethod("MoveNext", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly, null, Type.EmptyTypes, null);
							if (mn == null || mn.GetMethodBody() == null) continue;
							var thisF = nt.GetField("<>4__this", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
							int iterId = _iterNames.Count;
							string nm = nt.Name; int lt = nm.IndexOf('<'), gt = nm.IndexOf('>');
							string meth = (lt >= 0 && gt > lt) ? nm.Substring(lt + 1, gt - lt - 1) : nm;
							var h = new ILHook(mn, il => InjectCo(il, thisF, iterId));
							_ilhooks.Add(h);
							_iterNames.Add(t.FullName + "::" + meth);
							iters++; asmHooks++;
						}
						catch (Exception e)
						{
							failed++;
							if (fails.Count < 200) fails.Add($"{nt.FullName}.MoveNext: {e.GetType().Name}: {e.Message}");
						}
					}
				}
				perAsm[asm.GetName().Name] = asmHooks;
			}
			_installInfo["mb_types"] = types;
			_installInfo["hooked_methods"] = methods;
			_installInfo["hooked_iterators"] = iters;
			_installInfo["failed"] = failed;
			_installInfo["skipped_bad_shape"] = badShape;
			_installInfo["skipped_generic_types"] = genericSkipped;
			_installInfo["per_assembly"] = perAsm;
			_installInfo["failures"] = fails;
			Log($"hooked {methods} lifecycle methods + {iters} coroutine MoveNext over {types} MonoBehaviour types; failed {failed}; skipped bad-shape {badShape}, generic types {genericSkipped}");
			foreach (var f in fails) Log("  FAILED " + f);
		}

		private static void InjectRec(ILContext il, int code, int id, Type physParam)
		{
			var c = new ILCursor(il);
			c.Goto(0);
			c.MoveBeforeLabels();
			c.Emit(OpCodes.Ldarg_0);
			if (IsPhysics(code))
			{
				if (physParam != null)
				{
					c.Emit(OpCodes.Ldarg_1);
					if (physParam.IsValueType) c.Emit(OpCodes.Box, physParam);
				}
				else c.Emit(OpCodes.Ldnull);
				c.Emit(OpCodes.Ldc_I4, code);
				c.Emit(OpCodes.Ldc_I4, id);
				c.Emit(OpCodes.Call, _recPhysM);
			}
			else
			{
				c.Emit(OpCodes.Ldc_I4, code);
				c.Emit(OpCodes.Ldc_I4, id);
				c.Emit(OpCodes.Call, _recM);
			}
			if (!HasExit(code)) return;
			// before every ret: record the return (branches to the ret now land on the inserted call: AfterLabel)
			while (c.TryGotoNext(MoveType.AfterLabel, i => i.MatchRet()))
			{
				c.Emit(OpCodes.Ldarg_0);
				c.Emit(OpCodes.Ldc_I4, code);
				c.Emit(OpCodes.Ldc_I4, id);
				c.Emit(OpCodes.Call, _recExitM);
				c.Index++;   // step over the ret
			}
		}

		private static void InjectCo(ILContext il, FieldInfo thisF, int iterId)
		{
			var c = new ILCursor(il);
			c.Goto(0);
			c.MoveBeforeLabels();
			c.Emit(OpCodes.Ldarg_0);
			if (thisF != null) { c.Emit(OpCodes.Ldarg_0); c.Emit(OpCodes.Ldfld, thisF); }
			else c.Emit(OpCodes.Ldnull);
			c.Emit(OpCodes.Ldc_I4, iterId);
			c.Emit(OpCodes.Call, _recCoM);
		}

		// The oracle's FsmPauseGate detours PlayMakerFSM.Update / PlayMakerFixedUpdate.FixedUpdate / PlayMakerLateUpdate.
		// LateUpdate and returns before the original body while Time.timeScale <= 0, so the entry hook never sees those
		// calls on frozen frames.  A detour added AFTER the gate (here, at first arm; the gate is installed at env setup)
		// wraps it and records the dispatch with aux = AUX_GATED.  It only reads Time.timeScale and calls through.
		private static void HookGateObserver()
		{
			var asm = typeof(PlayMakerFSM).Assembly;
			var targets = new[] {
				new KeyValuePair<Type, int>(typeof(PlayMakerFSM), C_UPDATE),
				new KeyValuePair<Type, int>(asm.GetType("PlayMakerFixedUpdate"), C_FIXEDUPDATE),
				new KeyValuePair<Type, int>(asm.GetType("PlayMakerLateUpdate"), C_LATEUPDATE) };
			foreach (var kv in targets)
			{
				string name = kCodeNames[kv.Value];
				try
				{
					var m = kv.Key?.GetMethod(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly, null, Type.EmptyTypes, null);
					if (m == null || !_hookedId.TryGetValue(m, out int id)) { Log($"gate observer: {kv.Key?.Name}.{name} not hooked"); continue; }
					int cc = kv.Value, ii = id;
					_hooks.Add(new Hook(m, new Action<Action<MonoBehaviour>, MonoBehaviour>((orig, self) =>
					{
						if (_armed && Time.timeScale <= 0f) RecFlag(self, cc, ii, AUX_GATED);
						orig(self);
					})));
				}
				catch (Exception e) { Log($"gate observer {name} failed: {HKOracle.DescribeException(e)}"); }
			}
			Log("gate observer installed");
		}

		private static FieldInfo _actTarget;
		private static void HookActivate()
		{
			try
			{
				var t = typeof(HutongGames.PlayMaker.Actions.ActivateGameObject);
				_actTarget = t.GetField("activatedGameObject", BindingFlags.Instance | BindingFlags.NonPublic);
				var mDo = t.GetMethod("DoActivateGameObject", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
				var mExit = t.GetMethod("OnExit", BindingFlags.Instance | BindingFlags.Public | BindingFlags.DeclaredOnly);
				var ms = new[] { new KeyValuePair<MethodInfo, int>(mDo, 0), new KeyValuePair<MethodInfo, int>(mExit, 4) };
				foreach (var kv in ms)
				{
					if (kv.Key == null) { Log("UNHOOKED ActivateGameObject method"); continue; }
					int flag = kv.Value;
					_hooks.Add(new Hook(kv.Key, new Action<Action<HutongGames.PlayMaker.Actions.ActivateGameObject>, HutongGames.PlayMaker.Actions.ActivateGameObject>((orig, self) =>
					{
						if (_armed) Mark(C_ACT_BEGIN, -1, flag);
						orig(self);
						if (_armed)
						{
							try
							{
								var go = _actTarget?.GetValue(self) as GameObject;
								int f = flag | ((self.activate != null && self.activate.Value) ? 1 : 0) | ((self.recursive != null && self.recursive.Value) ? 2 : 0);
								Emit(C_ACT_END, go == null ? -1 : Lookup(go).idx, f);
							}
							catch (Exception e) { Err("ActEnd", e); }
						}
					})));
				}
			}
			catch (Exception e) { Log("ActivateGameObject hooks failed: " + HKOracle.DescribeException(e)); }
		}

		private static void HookPool()
		{
			try
			{
				var mSpawn = typeof(ObjectPool).GetMethod("Spawn", BindingFlags.Public | BindingFlags.Static, null,
					new[] { typeof(GameObject), typeof(Transform), typeof(Vector3), typeof(Quaternion) }, null);
				if (mSpawn != null)
					_hooks.Add(new Hook(mSpawn, new Func<Func<GameObject, Transform, Vector3, Quaternion, GameObject>, GameObject, Transform, Vector3, Quaternion, GameObject>(
						(orig, prefab, parent, pos, rot) =>
						{
							if (_armed) MarkObj(C_SPAWN_BEGIN, prefab);
							var r = orig(prefab, parent, pos, rot);
							if (_armed) MarkObj(C_SPAWN_END, r);
							return r;
						})));
				else Log("UNHOOKED ObjectPool.Spawn");
				var mRec = typeof(ObjectPool).GetMethod("Recycle", BindingFlags.Public | BindingFlags.Static, null, new[] { typeof(GameObject) }, null);
				if (mRec != null)
					_hooks.Add(new Hook(mRec, new Action<Action<GameObject>, GameObject>((orig, go) =>
					{
						if (_armed) MarkObj(C_RECYCLE_BEGIN, go);
						orig(go);
						if (_armed) MarkObj(C_RECYCLE_END, go);
					})));
				else Log("UNHOOKED ObjectPool.Recycle");
			}
			catch (Exception e) { Log("pool hooks failed: " + HKOracle.DescribeException(e)); }
		}

		// ------------------------------------------------------------------ player loop markers
		private sealed class LifecycleLoopMarker { }

		private static void InstallLoopMarkers()
		{
			try
			{
				_loopNames.AddRange(LoopMarkers.Install(typeof(LifecycleLoopMarker), id => Mark(C_MARK, id, -1)));
				_installInfo["loop_markers"] = _loopNames.Count;
				Log($"player loop: {_loopNames.Count} marker systems inserted");
			}
			catch (Exception e) { Log("player loop markers failed: " + HKOracle.DescribeException(e)); }
		}

		// ================================================================== hot path
		private static void Put(int w0, int w1)
		{
			if (_pos + 2 > kChunk) { _cur = new int[kChunk]; _chunks.Add(_cur); _pos = 0; }
			_cur[_pos] = w0; _cur[_pos + 1] = w1; _pos += 2;
			_events++;
		}

		private static void FrameCheck()
		{
			int f = Time.frameCount;
			if (f != _lastFrame)
			{
				_lastFrame = f;
				Put(C_FRAME | ((Time.timeScale > 0f ? 1 : 0) + 1) << 9, f);
			}
		}

		private static void Emit(int code, int w1, int aux)
		{
			FrameCheck();
			int fx = Time.inFixedTimeStep ? 256 : 0;
			Put(code | fx | ((aux + 1) << 9), w1);
		}

		private static void Mark(int code, int w1, int aux)
		{
			if (!_armed) return;
			try { Emit(code, w1, aux); } catch (Exception e) { Err("Mark", e); }
		}

		private static void MarkObj(int code, GameObject go)
		{
			try { Emit(code, go == null ? -1 : Lookup(go).idx, -1); } catch (Exception e) { Err("MarkObj", e); }
		}

		public static void Rec(object self, int code, int id)
		{
			if (!_armed) return;
			try
			{
				if (System.Threading.Thread.CurrentThread.ManagedThreadId != _mainThread) return;
				var r = Lookup(self);
				if (r.decl != null && r.decl[code] != id) return;   // a base-class declaration reached via base.X()
				Emit(code, r.idx, -1);
			}
			catch (Exception e) { Err("Rec", e); }
		}

		public static void RecPhys(object self, object other, int code, int id)
		{
			if (!_armed) return;
			try
			{
				if (System.Threading.Thread.CurrentThread.ManagedThreadId != _mainThread) return;
				var r = Lookup(self);
				if (r.decl != null && r.decl[code] != id) return;
				object o = other;
				if (o is Collision2D col) o = col.collider;
				int oi = (o is UnityEngine.Object uo && uo != null) ? Lookup(o).idx : -1;
				Emit(code, r.idx, oi);
			}
			catch (Exception e) { Err("RecPhys", e); }
		}

		public static void RecExit(object self, int code, int id)
		{
			if (!_armed) return;
			try
			{
				if (System.Threading.Thread.CurrentThread.ManagedThreadId != _mainThread) return;
				var r = Lookup(self);
				if (r.decl != null && r.decl[code] != id) return;
				Emit(C_EXIT, r.idx, code);
			}
			catch (Exception e) { Err("RecExit", e); }
		}

		private static void RecFlag(object self, int code, int id, int aux)
		{
			try
			{
				var r = Lookup(self);
				if (r.decl != null && r.decl[code] != id) return;
				Emit(code, r.idx, aux);
			}
			catch (Exception e) { Err("RecFlag", e); }
		}

		public static void RecCo(object iter, object owner, int iterId)
		{
			if (!_armed) return;
			try
			{
				if (System.Threading.Thread.CurrentThread.ManagedThreadId != _mainThread) return;
				Inst it;
				if (!_inst.TryGetValue(iter, out it))
				{
					int oi = owner != null ? Lookup(owner).idx : -1;
					it = new Inst { idx = _rows.Count, owner = oi };
					it.row = new JArray { K_ITER, iterId, "", "", 0, 0, "", -1, oi, false, false, "", _events };
					_rows.Add(it.row); _inst[iter] = it;
				}
				Emit(C_COROUTINE, it.idx, it.owner);
			}
			catch (Exception e) { Err("RecCo", e); }
		}

		// ------------------------------------------------------------------ instance rows
		// row: [kind, typeIdx | iterId, path, fsmName, instanceId, goInstanceId, siblingChain,
		//       componentIndexOnGo, ownerIdx, activeInHierarchy, enabled, scene, firstEventIdx]
		private static Inst Lookup(object o)
		{
			if (_inst.TryGetValue(o, out Inst r)) return r;
			r = new Inst { idx = _rows.Count };
			var t = o.GetType();
			int kind = o is MonoBehaviour ? K_MB : o is Collider2D ? K_COL : o is GameObject ? K_GO : K_OTHER;
			if (kind == K_MB) r.decl = DeclOf(t);
			string path = "", fsm = "", sib = "", scene = "";
			int uid = 0, goid = 0, ci = -1; bool act = false, en = false;
			try
			{
				GameObject go = null;
				if (o is Component comp) go = comp.gameObject; else if (o is GameObject g) go = g;
				if (o is UnityEngine.Object uo) uid = uo.GetInstanceID();
				if (go != null)
				{
					goid = go.GetInstanceID();
					path = ReflectionDumper.Path(go.transform) ?? "";
					act = go.activeInHierarchy;
					try { scene = go.scene.name ?? ""; } catch { }
					var sb = new StringBuilder();
					for (var tr = go.transform; tr != null; tr = tr.parent)
					{
						if (sb.Length > 0) sb.Insert(0, '/');
						sb.Insert(0, tr.GetSiblingIndex());
					}
					sib = sb.ToString();
					if (o is Component c2)
					{
						var all = go.GetComponents<Component>();
						for (int i = 0; i < all.Length; i++) if (ReferenceEquals(all[i], c2)) { ci = i; break; }
					}
				}
				if (o is Behaviour b) en = b.enabled;
				if (o is PlayMakerFSM pm) fsm = pm.FsmName ?? "";
			}
			catch (Exception e) { Err("Lookup.describe", e); }
			r.row = new JArray { kind, TypeIdx(t), path, fsm, uid, goid, sib, ci, -1, act, en, scene, _events };
			_rows.Add(r.row);
			_inst[o] = r;
			return r;
		}

		private static int TypeIdx(Type t)
		{
			if (_typeIdx.TryGetValue(t, out int i)) return i;
			i = _typeNames.Count; _typeNames.Add(t.FullName); _typeIdx[t] = i;
			return i;
		}

		// For a runtime type, which hooked method Unity dispatches per message: the most-derived
		// declaration of that name with a dispatchable shape, walking up to MonoBehaviour.
		private static int[] DeclOf(Type t)
		{
			if (_declOf.TryGetValue(t, out int[] d)) return d;
			d = new int[NCODES];
			for (int k = 0; k < NCODES; k++) d[k] = -1;
			foreach (var kv in kNames)
			{
				int code = kv.Value;
				for (var bt = t; bt != null && bt != typeof(MonoBehaviour); bt = bt.BaseType)
				{
					MethodInfo found = null;
					try
					{
						foreach (var m in bt.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
							if (m.Name == kv.Key && ShapeOk(m, code)) { found = m; break; }
					}
					catch { }
					if (found == null) continue;
					d[code] = _hookedId.TryGetValue(found, out int id) ? id : -2;
					break;
				}
			}
			_declOf[t] = d;
			return d;
		}

		// per recorded MonoBehaviour type: the messages Unity dispatches to it and whether the declaring method is hooked
		private static JObject TypeDecl()
		{
			var o = new JObject();
			foreach (var kv in _typeIdx)
			{
				if (!_declOf.TryGetValue(kv.Key, out int[] d)) continue;
				var hooked = new JArray(); var unhooked = new JArray();
				for (int k = 0; k < d.Length && k < kCodeNames.Length; k++)
				{
					if (d[k] >= 0) hooked.Add(kCodeNames[k]);
					else if (d[k] == -2) unhooked.Add(kCodeNames[k]);
				}
				o[kv.Key.FullName] = new JObject { ["hooked"] = hooked, ["unhooked"] = unhooked };
			}
			return o;
		}

		// ================================================================== output
		// File: gzip( "HKLC" | int32 version=1 | int32 jsonLen | json(utf8) | int64 nWords | int32[nWords] )
		// Event = two int32 words: w0 = code | inFixedTimeStep<<8 | (aux+1)<<9, w1 = instance index
		// (FRAME: w1 = Time.frameCount, aux = timeScale>0; MARK: w1 = player-loop marker id;
		// STEP: w1 = step index, aux = committed; physics: aux = other collider's instance index;
		// COROUTINE: w1 = iterator instance index, aux = owner instance index).
		private static void Flush(string why)
		{
			if (!_armed && _events == 0) return;
			_armed = false;
			if (_events == 0) { ResetBuffers(); return; }
			string path = _episode == 0 ? _base + ".lifecycle.gz" : $"{_base}.e{_episode}.lifecycle.gz";
			try
			{
				var hdr = new JObject
				{
					["version"] = 1,
					["episode"] = _episode,
					["why"] = why,
					["armed_at"] = "scene_ready",
					["level"] = Hooks.Current?.Level,
					["trace_base"] = _base,
					["events"] = _events,
					["codes"] = new JArray(kCodeNames),
					["loop"] = new JArray(_loopNames.ToArray()),
					["types"] = new JArray(_typeNames.ToArray()),
					["iter_types"] = new JArray(_iterNames.ToArray()),
					["inst_fields"] = new JArray("kind", "type", "path", "fsm", "uid", "go_uid", "sib", "comp_idx", "owner", "active", "enabled", "scene", "first_event"),
					["inst_kinds"] = new JArray("mb", "iter", "col", "go", "other"),
					["insts"] = new JArray(_rows.ToArray()),
					["install"] = _installInfo,
					["errors"] = JObject.FromObject(_errs),
					["type_decl"] = TypeDecl(),
				};
				byte[] json = Encoding.UTF8.GetBytes(hdr.ToString(Formatting.None));
				long words = 0; foreach (var ch in _chunks) words += ch == _cur ? _pos : kChunk;
				using (var fs = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16))
				using (var gz = new GZipStream(fs, System.IO.Compression.CompressionLevel.Fastest))
				using (var bw = new BinaryWriter(gz))
				{
					bw.Write(Encoding.ASCII.GetBytes("HKLC"));
					bw.Write(1);
					bw.Write(json.Length);
					bw.Write(json);
					bw.Write(words);
					var buf = new byte[kChunk * 4];
					foreach (var ch in _chunks)
					{
						int n = ch == _cur ? _pos : kChunk;
						Buffer.BlockCopy(ch, 0, buf, 0, n * 4);
						bw.Write(buf, 0, n * 4);
					}
				}
				Log($"flushed episode {_episode} ({why}) -> {path}: {_events} events, {_rows.Count} instances");
			}
			catch (Exception e) { Log($"flush to {path} failed: {HKOracle.DescribeException(e)}"); }
			_episode++;
			ResetBuffers();
		}
	}
}
