using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.CompilerServices;
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
	// The method-level oracle: records single calls of PlayMaker actions and of the ported component methods,
	// with the inputs each call reads and the outputs it writes, so the simulator's port can be checked call by
	// call (tools/method_oracle.py replays every record through sim/fsm/runtime/method_oracle.c).
	//
	// Installed in record mode when HK_ORACLE_METHODS=1 (Mode.cs).  Armed at Hooks.SceneReady, disarmed at
	// Hooks.EpisodeEnd.  Output: <trace base>.methods.jsonl.gz, one JSON object per line; the format is
	// documented in docs/method-oracle.md.
	//
	// WHAT IS HOOKED.
	//   * FsmState's action dispatch (ILHook): the callvirt of FsmStateAction.OnEnter / OnUpdate / OnFixedUpdate /
	//     OnLateUpdate / OnExit / Event in FsmState.ActivateActions / OnUpdate / OnFixedUpdate / OnLateUpdate /
	//     OnExit / OnEvent (PM/FsmState.cs:302,326,337,351,363,633) is replaced by a static wrapper that records
	//     around the same virtual call.  These are the only places PlayMaker calls an action callback, so every
	//     action type is covered and a base.OnEnter() inside an override is not a second record.
	//   * Fsm.Event(FsmEvent) (Fsm.cs:2192), Fsm.Event(FsmEventTarget, FsmEvent) (:2126) and Fsm.DelayedEvent
	//     (:2200, :2207): the events a call sends itself, at its own FsmExecutionStack depth.
	//   * HealthManager.Hit, Recoil.RecoilByDirection / FixedUpdate, tk2dSpriteAnimator.Play(clip, t, fps) /
	//     UpdateAnimation: the component methods sim/fsm/components and sim/fsm/runtime/tk2d.c port.
	//
	// SAMPLING.  An action is recorded per ACTIVATION (its OnEnter through its OnExit), because its private
	// state carries from one callback to the next: the replay runs the whole activation in order.  Per
	// canonical site (owner path | fsm | state | index) the first kSiteFirst activations are taken, then an
	// activation whose hash(site, n, seed) is 0 mod kHashMod, up to kSiteCap per site and kTypeCap per action
	// type.  An activation stops being recorded after kCallCap callbacks.  Component calls are sampled the same
	// way per (method, owner).  No sampling decision draws from UnityEngine.Random.
	//
	// INERTNESS.  The recorder only reads: variable values (NamedVariable getters), transforms, Rigidbody2D
	// velocity, component fields (reflection), Random.state (a pure getter), and it appends to a gzip stream.
	// The proof is a replay with HK_ORACLE_METHODS=1 vs unset compared on the .hktrace observation stream
	// (tools/method_oracle.py inert).
	public static class MethodRecorder
	{
		public const int FormatVersion = 3;
		private const int kSiteFirst = 2, kSiteCap = 12, kHashMod = 8, kTypeCap = 400, kTypeFirstCap = 1500, kCallCap = 160;
		private const int CB_ENTER = 0, CB_UPDATE = 1, CB_FIXED = 2, CB_LATE = 3, CB_EXIT = 4, CB_EVENT = 5;
		private static readonly string[] kCb = { "E", "U", "F", "L", "X", "V" };

		private static StreamWriter _writer;
		private static GZipStream _gz;
		private static FileStream _fs;
		private static string _path;
		private static bool _armed;
		private static int _episode = -1;
		private static int _seed;
		private static long _seq, _nCalls, _nActs, _dispatches;
		private static GameObject _knight;
		private static readonly Dictionary<string, int> _errs = new Dictionary<string, int>();
		private static readonly List<ILHook> _ilHooks = new List<ILHook>();
		private static readonly List<Hook> _hooks = new List<Hook>();
		private static readonly StringBuilder _sb = new StringBuilder(1 << 14);

		private static void Log(string m) => HKOracle.Instance.Log("[Methods] " + m);

		private static void Err(string where, Exception e)
		{
			_errs.TryGetValue(where, out int n);
			_errs[where] = n + 1;
			if (n < 5) Log($"ERROR in {where}: {HKOracle.DescribeException(e)}");
		}

		private sealed class RefEq : IEqualityComparer<object>
		{
			public new bool Equals(object a, object b) => ReferenceEquals(a, b);
			public int GetHashCode(object o) => RuntimeHelpers.GetHashCode(o);
		}

		// ------------------------------------------------------------------ install

		public static void Install()
		{
			_path = Mode.SidePath(".methods.jsonl.gz");
			string dir = Path.GetDirectoryName(_path);
			if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
			_fs = new FileStream(_path, FileMode.Create, FileAccess.Write, FileShare.Read, 1 << 16);
			_gz = new GZipStream(_fs, System.IO.Compression.CompressionLevel.Fastest);
			_writer = new StreamWriter(_gz, new UTF8Encoding(false), 1 << 16);
			RngState.Resolve();
			int.TryParse(Mode.Get("HK_ORACLE_SEED") ?? "0", NumberStyles.Integer, CultureInfo.InvariantCulture, out _seed);

			HookDispatch("ActivateActions", "OnEnter", nameof(WEnter));
			HookDispatch("OnUpdate", "OnUpdate", nameof(WUpdate));
			HookDispatch("OnFixedUpdate", "OnFixedUpdate", nameof(WFixed));
			HookDispatch("OnLateUpdate", "OnLateUpdate", nameof(WLate));
			HookDispatch("OnExit", "OnExit", nameof(WExit));
			HookDispatch("OnEvent", "Event", nameof(WEvent));
			HookEvents();
			HookComponents();

			_sb.Length = 0;
			_sb.Append("{\"methods\":").Append(FormatVersion).Append(",\"mod_commit\":");
			J(_sb, ModInfo.Commit);
			_sb.Append(",\"unity\":");
			J(_sb, Application.unityVersion);
			_sb.Append(",\"seed\":").Append(_seed).Append(",\"hooks\":").Append(_ilHooks.Count + _hooks.Count).Append('}');
			Emit();

			Hooks.SceneReady += OnSceneReady;
			Hooks.EpisodeEnd += OnEpisodeEnd;
			Application.quitting += Close;
			Log($"writing {_path} ({_ilHooks.Count} dispatch hooks, {_hooks.Count} method hooks; RNG {RngState.Describe})");
		}

		// Replace `callvirt FsmStateAction::<cbName>` in FsmState.<method> by `call MethodRecorder.<wrapper>`.
		private static void HookDispatch(string method, string cbName, string wrapper)
		{
			var m = typeof(FsmState).GetMethod(method, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
			var w = typeof(MethodRecorder).GetMethod(wrapper, BindingFlags.Static | BindingFlags.Public);
			if (m == null || w == null) { Log($"UNHOOKED FsmState.{method}"); return; }
			int n = 0;
			_ilHooks.Add(new ILHook(m, il =>
			{
				foreach (var ins in il.Instrs)
				{
					if ((ins.OpCode.Code == Code.Callvirt || ins.OpCode.Code == Code.Call) && ins.Operand is MethodReference mr
						&& mr.Name == cbName && mr.DeclaringType.FullName == "HutongGames.PlayMaker.FsmStateAction")
					{
						ins.OpCode = CilOpCodes.Call;
						ins.Operand = il.Import(w);
						n++;
					}
				}
			}));
			if (n != 1) Log($"FsmState.{method}: {n} {cbName} call sites replaced (expected 1)");
		}

		// ------------------------------------------------------------------ dispatch wrappers

		public static void WEnter(FsmStateAction a) { _dispatches++; var c = _armed ? PreAction(a, CB_ENTER, null) : null; try { a.OnEnter(); } finally { if (c != null) Post(c); } }
		public static void WUpdate(FsmStateAction a) { _dispatches++; var c = _armed ? PreAction(a, CB_UPDATE, null) : null; try { a.OnUpdate(); } finally { if (c != null) Post(c); } }
		public static void WFixed(FsmStateAction a) { _dispatches++; var c = _armed ? PreAction(a, CB_FIXED, null) : null; try { a.OnFixedUpdate(); } finally { if (c != null) Post(c); } }
		public static void WLate(FsmStateAction a) { _dispatches++; var c = _armed ? PreAction(a, CB_LATE, null) : null; try { a.OnLateUpdate(); } finally { if (c != null) Post(c); } }
		public static void WExit(FsmStateAction a) { _dispatches++; var c = _armed ? PreAction(a, CB_EXIT, null) : null; try { a.OnExit(); } finally { if (c != null) Post(c); } }
		public static bool WEvent(FsmStateAction a, FsmEvent e)
		{
			_dispatches++;
			var c = _armed ? PreAction(a, CB_EVENT, e) : null;
			bool r = false;
			try { r = a.Event(e); return r; }
			finally { if (c != null) { c.ret = r ? 1 : 0; Post(c); } }
		}

		// ------------------------------------------------------------------ activations and sampling

		private sealed class Act
		{
			public long id;
			public int n;                                   // callbacks recorded so far
			public Dictionary<string, string> vars;         // the variable values after the previous record
			public bool dead;                               // past kCallCap: nothing more is recorded
		}

		private sealed class Call
		{
			public int kind;                                // 0 action, 1 component
			public FsmStateAction a;
			public Fsm fsm;
			public Act act;
			public int cb;
			public object comp;                             // component calls
			public string method;
			public GameObject owner;
			public List<GameObject> gos;                    // snapshotted GameObjects
			public Dictionary<string, string> vars0;
			public StringBuilder sb;
			public List<string> evl, evt, dly;              // Fsm.Event(FsmEvent) on the owner FSM (or any FSM for components), Event(target, e), DelayedEvent
			public int ret = -1;
			public long disp0;                              // _dispatches at Pre: the action callbacks this call cascaded into
			public int stack0;                              // FsmExecutionStack.StackCount at Pre: the call's own sends happen at it
			public bool hero;                               // the HeroController's fields are recorded (kHeroTypes)
			public Fsm other;                               // the FSM a GetFsm* / SetFsm* action reads or writes
		}

		private static readonly Dictionary<object, Act> _live = new Dictionary<object, Act>(new RefEq());
		private static readonly Dictionary<object, string> _siteOf = new Dictionary<object, string>(new RefEq());
		private static readonly Dictionary<string, int> _siteSeen = new Dictionary<string, int>();
		private static readonly Dictionary<string, int> _siteTaken = new Dictionary<string, int>();
		private static readonly Dictionary<string, int> _typeTaken = new Dictionary<string, int>();
		private static readonly List<Call> _open = new List<Call>();

		private static uint Fnv(string s, uint h = 2166136261u)
		{
			foreach (char ch in s) { h ^= ch; h *= 16777619u; }
			return h;
		}

		// The sampling rule of the header comment; `key` is a site, `type` the per-type budget's key.
		private static bool Take(string key, string type)
		{
			_siteSeen.TryGetValue(key, out int seen);
			_siteSeen[key] = seen + 1;
			_siteTaken.TryGetValue(key, out int taken);
			_typeTaken.TryGetValue(type, out int tt);
			bool take;
			if (taken < kSiteFirst) take = tt < kTypeFirstCap;
			else take = taken < kSiteCap && tt < kTypeCap
				&& (Fnv(key, Fnv(seen.ToString(CultureInfo.InvariantCulture), (uint)_seed * 2654435761u)) % kHashMod) == 0;
			if (!take) return false;
			_siteTaken[key] = taken + 1;
			_typeTaken[type] = tt + 1;
			return true;
		}

		private static Call PreAction(FsmStateAction a, int cb, FsmEvent e)
		{
			try
			{
				Act act;
				if (cb == CB_ENTER)
				{
					_live.Remove(a);
					string site;
					if (!_siteOf.TryGetValue(a, out site))
					{
						var st0 = a.State;
						var f0 = a.Fsm;
						site = ScenePaths.Canon(f0?.GameObject) + "|" + (f0?.Name ?? "") + "|" + (st0?.Name ?? "") + "|"
							+ (st0?.Actions != null ? Array.IndexOf(st0.Actions, a) : -1);
						_siteOf[a] = site;
					}
					string tn = a.GetType().Name;
					if (!Take(site, tn)) return null;
					act = new Act { id = ++_nActs };
					_live[a] = act;
				}
				else if (!_live.TryGetValue(a, out act)) return null;
				if (act.dead) { if (cb == CB_EXIT) _live.Remove(a); return null; }
				if (act.n >= kCallCap) { act.dead = true; return null; }
				return BeginAction(a, act, cb, e);
			}
			catch (Exception ex) { Err("PreAction", ex); return null; }
		}

		private static Call BeginAction(FsmStateAction a, Act act, int cb, FsmEvent e)
		{
			var fsm = a.Fsm;
			var state = a.State;
			var c = new Call { kind = 0, a = a, fsm = fsm, act = act, cb = cb, sb = new StringBuilder(2048), owner = fsm?.GameObject };
			var sb = c.sb;
			sb.Append("{\"q\":").Append(_seq++).Append(",\"ep\":").Append(_episode).Append(",\"f\":").Append(Time.frameCount)
				.Append(",\"fc\":").Append(TraceRecorder.FixedCount).Append(",\"k\":\"pm\",\"t\":");
			J(sb, a.GetType().Name);
			sb.Append(",\"o\":");
			J(sb, ScenePaths.Canon(c.owner));
			sb.Append(",\"n\":");
			J(sb, fsm?.Name ?? "");
			sb.Append(",\"s\":");
			J(sb, state?.Name ?? "");
			sb.Append(",\"i\":").Append(state?.Actions != null ? Array.IndexOf(state.Actions, a) : -1);
			sb.Append(",\"a\":").Append(act.id).Append(",\"j\":").Append(act.n).Append(",\"cb\":\"").Append(kCb[cb]).Append('"');
			sb.Append(",\"d\":").Append(_open.Count);
			if (e != null) { sb.Append(",\"e\":"); J(sb, e.Name ?? ""); }
			Clocks(sb);
			sb.Append(",\"st\":").Append(F(state != null ? state.StateTime : 0f));
			sb.Append(",\"fin0\":").Append(a.Finished ? 1 : 0).Append(",\"sw0\":");
			J(sb, SwitchTo(fsm));
			sb.Append(",\"as0\":");
			J(sb, fsm?.ActiveStateName ?? "");
			sb.Append(",\"ps0\":");
			J(sb, fsm?.PreviousActiveState?.Name ?? "");
			EventData(sb);
			Rng(sb, "r0");
			c.vars0 = Vars(fsm);
			sb.Append(",\"vb\":");
			VarDelta(sb, act.vars, c.vars0);
			sb.Append(",\"pb\":");
			Priv(sb, a);
			c.gos = ActionGos(a, fsm);
			sb.Append(",\"gb\":");
			Gos(sb, c.gos);
			Inputs(sb);
			Pd(sb, "pd");
			c.hero = kHeroTypes.Contains(a.GetType().Name);
			if (c.hero) Hero(sb, "hc");
			c.other = OtherFsm(a, fsm);
			if (c.other != null) OtherVars(sb, "vx", c.other);
			c.disp0 = _dispatches;
			c.stack0 = FsmExecutionStack.StackCount;
			act.n++;
			_nCalls++;
			_open.Add(c);
			return c;
		}

		private static void Post(Call c)
		{
			try
			{
				int k = _open.LastIndexOf(c);
				if (k >= 0) _open.RemoveAt(k);
				var sb = c.sb;
				Rng(sb, "r1");
				if (c.kind == 0)
				{
					var vars1 = Vars(c.fsm);
					sb.Append(",\"va\":");
					VarDelta(sb, c.vars0, vars1);
					c.act.vars = vars1;
					sb.Append(",\"pa\":");
					Priv(sb, c.a);
					foreach (var g in ActionGos(c.a, c.fsm)) if (!c.gos.Contains(g)) c.gos.Add(g);
					sb.Append(",\"fin\":").Append(c.a.Finished ? 1 : 0).Append(",\"sw\":");
					J(sb, SwitchTo(c.fsm));
					sb.Append(",\"as\":");
					J(sb, c.fsm?.ActiveStateName ?? "");
					if (c.ret >= 0) sb.Append(",\"ret\":").Append(c.ret);
					if (c.cb == CB_EXIT) _live.Remove(c.a);
				}
				else
				{
					sb.Append(",\"sa\":");
					Fields(sb, c.comp);
				}
				sb.Append(",\"ga\":");
				Gos(sb, c.gos);
				Pd(sb, "pda");
				if (c.hero) Hero(sb, "hca");
				if (c.other != null) OtherVars(sb, "vxa", c.other);
				sb.Append(",\"nd\":").Append(_dispatches - c.disp0);
				List("evl", sb, c.evl);
				List("evt", sb, c.evt);
				List("dly", sb, c.dly);
				sb.Append('}');
				if (_writer != null) _writer.WriteLine(sb.ToString());
			}
			catch (Exception e) { Err("Post", e); }
		}

		private static void List(string key, StringBuilder sb, List<string> l)
		{
			if (l == null) return;
			sb.Append(",\"").Append(key).Append("\":[");
			for (int i = 0; i < l.Count; i++) { if (i > 0) sb.Append(','); sb.Append(l[i]); }
			sb.Append(']');
		}

		private static void Clocks(StringBuilder sb)
		{
			sb.Append(",\"dt\":").Append(F(Time.deltaTime)).Append(",\"fdt\":").Append(F(Time.fixedDeltaTime))
				.Append(",\"fx\":").Append(Time.inFixedTimeStep ? 1 : 0).Append(",\"tm\":").Append(F(Time.time))
				.Append(",\"ts\":").Append(F(Time.timeScale));
		}

		private static void Rng(StringBuilder sb, string key)
		{
			RngState.Read(out uint a, out uint b, out uint c, out uint d);
			sb.Append(",\"").Append(key).Append("\":[").Append(a).Append(',').Append(b).Append(',').Append(c).Append(',').Append(d).Append(']');
		}

		// Fsm.EventData (PM/Fsm.cs:31), which GetEventInfo-style actions and delayed events read: sender FSM, int, float, string.
		private static void EventData(StringBuilder sb)
		{
			var e = Fsm.EventData;
			if (e == null) return;
			sb.Append(",\"evd\":[");
			J(sb, e.SentByFsm != null ? ScenePaths.Canon(e.SentByFsm.GameObject) : null);
			sb.Append(',');
			J(sb, e.SentByFsm?.Name);
			sb.Append(',').Append(e.IntData).Append(',').Append(F(e.FloatData)).Append(',');
			J(sb, e.StringData);
			sb.Append(']');
		}

		private static FieldInfo _switchTo;
		private static string SwitchTo(Fsm fsm)
		{
			if (fsm == null) return "";
			if (_switchTo == null) _switchTo = typeof(Fsm).GetField("switchToState", BindingFlags.Instance | BindingFlags.NonPublic);
			return (_switchTo?.GetValue(fsm) as FsmState)?.Name ?? "";
		}

		// ------------------------------------------------------------------ events

		private static void HookEvents()
		{
			const BindingFlags I = BindingFlags.Instance | BindingFlags.Public;
			var m1 = typeof(Fsm).GetMethod("Event", I, null, new[] { typeof(FsmEvent) }, null);
			if (m1 != null) _hooks.Add(new Hook(m1, new Action<Action<Fsm, FsmEvent>, Fsm, FsmEvent>((orig, self, ev) =>
			{
				if (_armed && _open.Count > 0 && ev != null) Note(self, 0, ev, null, 0f);
				orig(self, ev);
			})));
			else Log("UNHOOKED Fsm.Event(FsmEvent)");
			var m2 = typeof(Fsm).GetMethod("Event", I, null, new[] { typeof(FsmEventTarget), typeof(FsmEvent) }, null);
			if (m2 != null) _hooks.Add(new Hook(m2, new Action<Action<Fsm, FsmEventTarget, FsmEvent>, Fsm, FsmEventTarget, FsmEvent>((orig, self, t, ev) =>
			{
				if (_armed && _open.Count > 0 && ev != null) Note(self, 1, ev, t, 0f);
				orig(self, t, ev);
			})));
			else Log("UNHOOKED Fsm.Event(FsmEventTarget, FsmEvent)");
			var m3 = typeof(Fsm).GetMethod("DelayedEvent", I, null, new[] { typeof(FsmEvent), typeof(float) }, null);
			if (m3 != null) _hooks.Add(new Hook(m3, new Func<Func<Fsm, FsmEvent, float, DelayedEvent>, Fsm, FsmEvent, float, DelayedEvent>((orig, self, ev, d) =>
			{
				if (_armed && _open.Count > 0 && ev != null) Note(self, 2, ev, null, d);
				return orig(self, ev, d);
			})));
			else Log("UNHOOKED Fsm.DelayedEvent(FsmEvent, float)");
			var m4 = typeof(Fsm).GetMethod("DelayedEvent", I, null, new[] { typeof(FsmEventTarget), typeof(FsmEvent), typeof(float) }, null);
			if (m4 != null) _hooks.Add(new Hook(m4, new Func<Func<Fsm, FsmEventTarget, FsmEvent, float, DelayedEvent>, Fsm, FsmEventTarget, FsmEvent, float, DelayedEvent>((orig, self, t, ev, d) =>
			{
				if (_armed && _open.Count > 0 && ev != null) Note(self, 3, ev, t, d);
				return orig(self, t, ev, d);
			})));
			else Log("UNHOOKED Fsm.DelayedEvent(FsmEventTarget, FsmEvent, float)");
		}

		// Attribute an event to the open call that sends it itself: the one whose FsmExecutionStack depth it is sent at
		// (an event sent from inside the processing of another event is the cascade's, PM/FsmExecutionStack.cs:57).
		// Action calls take the events of their own FSM, component calls every FSM's (with the FSM named).
		private static void Note(Fsm self, int which, FsmEvent ev, FsmEventTarget t, float delay)
		{
			try
			{
				string item = null, itemNamed = null;
				int depth = FsmExecutionStack.StackCount;
				for (int i = 0; i < _open.Count; i++)
				{
					var c = _open[i];
					if (c.stack0 != depth) continue;
					if (c.kind == 0 && !ReferenceEquals(c.fsm, self)) continue;
					string s;
					if (c.kind == 0) s = item ?? (item = EventItem(null, which, ev, t, delay));
					else s = itemNamed ?? (itemNamed = EventItem(self, which, ev, t, delay));
					if (which == 0) (c.evl ?? (c.evl = new List<string>())).Add(s);
					else if (which == 1) (c.evt ?? (c.evt = new List<string>())).Add(s);
					else (c.dly ?? (c.dly = new List<string>())).Add(s);
				}
			}
			catch (Exception e) { Err("Note", e); }
		}

		private static string EventItem(Fsm fsm, int which, FsmEvent ev, FsmEventTarget t, float delay)
		{
			var sb = new StringBuilder(64);
			sb.Append('[');
			J(sb, ev.Name ?? "");
			if (which == 1 || which == 3)
			{
				sb.Append(',').Append(t == null ? -1 : (int)t.target);
			}
			if (which >= 2) sb.Append(',').Append(F(delay));
			if (fsm != null)
			{
				sb.Append(',');
				J(sb, ScenePaths.Canon(fsm.GameObject));
				sb.Append(',');
				J(sb, fsm.Name ?? "");
			}
			sb.Append(']');
			return sb.ToString();
		}

		// ------------------------------------------------------------------ components

		private static void HookComponents()
		{
			const BindingFlags I = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
			var hit = typeof(HealthManager).GetMethod("Hit", I, null, new[] { typeof(HitInstance) }, null);
			if (hit != null) _hooks.Add(new Hook(hit, new Action<Action<HealthManager, HitInstance>, HealthManager, HitInstance>((orig, self, h) =>
			{
				_dispatches++;
				var c = _armed ? PreComp(self, "HealthManager.Hit", sb => HitArgs(sb, h), h.Source) : null;
				try { orig(self, h); } finally { if (c != null) Post(c); }
			})));
			else Log("UNHOOKED HealthManager.Hit");
			var rbd = typeof(Recoil).GetMethod("RecoilByDirection", I, null, new[] { typeof(int), typeof(float) }, null);
			if (rbd != null) _hooks.Add(new Hook(rbd, new Action<Action<Recoil, int, float>, Recoil, int, float>((orig, self, dir, mag) =>
			{
				_dispatches++;
				var c = _armed ? PreComp(self, "Recoil.RecoilByDirection", sb => sb.Append("{\"dir\":").Append(dir).Append(",\"mag\":").Append(F(mag)).Append('}'), null) : null;
				try { orig(self, dir, mag); } finally { if (c != null) Post(c); }
			})));
			else Log("UNHOOKED Recoil.RecoilByDirection");
			var rfu = typeof(Recoil).GetMethod("FixedUpdate", I, null, Type.EmptyTypes, null);
			if (rfu != null) _hooks.Add(new Hook(rfu, new Action<Action<Recoil>, Recoil>((orig, self) =>
			{
				_dispatches++;
				var c = _armed ? PreComp(self, "Recoil.FixedUpdate", null, null) : null;
				try { orig(self); } finally { if (c != null) Post(c); }
			})));
			else Log("UNHOOKED Recoil.FixedUpdate");
			var play = typeof(tk2dSpriteAnimator).GetMethod("Play", I, null, new[] { typeof(tk2dSpriteAnimationClip), typeof(float), typeof(float) }, null);
			if (play != null) _hooks.Add(new Hook(play, new Action<Action<tk2dSpriteAnimator, tk2dSpriteAnimationClip, float, float>, tk2dSpriteAnimator, tk2dSpriteAnimationClip, float, float>((orig, self, clip, t, fps) =>
			{
				_dispatches++;
				var c = _armed ? PreComp(self, "tk2dSpriteAnimator.Play", sb =>
				{
					sb.Append("{\"clip\":"); J(sb, clip?.name); sb.Append(",\"t\":").Append(F(t)).Append(",\"fps\":").Append(F(fps)).Append('}');
				}, null) : null;
				try { orig(self, clip, t, fps); } finally { if (c != null) Post(c); }
			})));
			else Log("UNHOOKED tk2dSpriteAnimator.Play(clip, float, float)");
			var upd = typeof(tk2dSpriteAnimator).GetMethod("UpdateAnimation", I, null, new[] { typeof(float) }, null);
			if (upd != null) _hooks.Add(new Hook(upd, new Action<Action<tk2dSpriteAnimator, float>, tk2dSpriteAnimator, float>((orig, self, dt) =>
			{
				_dispatches++;
				var c = _armed ? PreComp(self, "tk2dSpriteAnimator.UpdateAnimation", sb => sb.Append("{\"dt\":").Append(F(dt)).Append('}'), null) : null;
				try { orig(self, dt); } finally { if (c != null) Post(c); }
			})));
			else Log("UNHOOKED tk2dSpriteAnimator.UpdateAnimation");
		}

		private static void HitArgs(StringBuilder sb, HitInstance h)
		{
			sb.Append("{\"src\":");
			J(sb, h.Source != null ? ScenePaths.Canon(h.Source) : null);
			sb.Append(",\"type\":").Append((int)h.AttackType).Append(",\"circ\":").Append(h.CircleDirection ? 1 : 0)
				.Append(",\"dmg\":").Append(h.DamageDealt).Append(",\"dir\":").Append(F(h.Direction))
				.Append(",\"ign\":").Append(h.IgnoreInvulnerable ? 1 : 0).Append(",\"magm\":").Append(F(h.MagnitudeMultiplier))
				.Append(",\"mova\":").Append(F(h.MoveAngle)).Append(",\"movd\":").Append(h.MoveDirection ? 1 : 0)
				.Append(",\"mul\":").Append(F(h.Multiplier)).Append(",\"spec\":").Append((int)h.SpecialType)
				.Append(",\"extra\":").Append(h.IsExtraDamage ? 1 : 0).Append('}');
		}

		private static Call PreComp(Component self, string method, Action<StringBuilder> args, GameObject other)
		{
			try
			{
				if (self == null) return null;
				var go = self.gameObject;
				string owner = ScenePaths.Canon(go);
				if (!Take(method + "|" + owner, method)) return null;
				var c = new Call { kind = 1, comp = self, method = method, owner = go, sb = new StringBuilder(2048) };
				var sb = c.sb;
				sb.Append("{\"q\":").Append(_seq++).Append(",\"ep\":").Append(_episode).Append(",\"f\":").Append(Time.frameCount)
					.Append(",\"fc\":").Append(TraceRecorder.FixedCount).Append(",\"k\":\"cs\",\"t\":");
				J(sb, method);
				sb.Append(",\"o\":");
				J(sb, owner);
				sb.Append(",\"d\":").Append(_open.Count);
				Clocks(sb);
				Rng(sb, "r0");
				sb.Append(",\"args\":");
				if (args != null) args(sb); else sb.Append("{}");
				sb.Append(",\"sb\":");
				Fields(sb, self);
				c.gos = new List<GameObject> { go };
				if (other != null && other != go) c.gos.Add(other);
				if (_knight != null && !c.gos.Contains(_knight)) c.gos.Add(_knight);
				sb.Append(",\"gb\":");
				Gos(sb, c.gos);
				Inputs(sb);
				Pd(sb, "pd");
				c.disp0 = _dispatches;
				c.stack0 = FsmExecutionStack.StackCount;
				_nCalls++;
				_open.Add(c);
				return c;
			}
			catch (Exception ex) { Err("PreComp " + method, ex); return null; }
		}

		// ------------------------------------------------------------------ variables

		// Every variable of the FSM and of PlayMakerGlobals ("G" prefix), keyed "<bucket>:<name>", valued as a
		// JSON fragment.
		private static Dictionary<string, string> Vars(Fsm fsm)
		{
			var d = new Dictionary<string, string>();
			if (fsm != null) AddVars(d, "", fsm.Variables);
			FsmVariables g = null;
			try { g = FsmVariables.GlobalVariables; } catch { }
			if (g != null) AddVars(d, "G", g);
			return d;
		}

		private static readonly StringBuilder _vs = new StringBuilder(256);

		// The first variable of a name wins, as FsmVariables.GetFsm*(name) returns it (PM/FsmVariables.cs:1495-1504):
		// an FSM can declare two variables with one name (GG_Hornet_1 Control has two GameObject "Area Title").
		private static void Put(Dictionary<string, string> d, string key, string v) { if (!d.ContainsKey(key)) d[key] = v; }

		private static void AddVars(Dictionary<string, string> d, string pre, FsmVariables v)
		{
			if (v == null) return;
			foreach (var x in v.FloatVariables) Put(d, pre + "f:" + x.Name, F(x.Value));
			foreach (var x in v.IntVariables) Put(d, pre + "i:" + x.Name, x.Value.ToString(CultureInfo.InvariantCulture));
			foreach (var x in v.BoolVariables) Put(d, pre + "b:" + x.Name, x.Value ? "1" : "0");
			foreach (var x in v.StringVariables) { _vs.Length = 0; J(_vs, x.Value); Put(d, pre + "s:" + x.Name, _vs.ToString()); }
			foreach (var x in v.Vector2Variables) Put(d, pre + "v2:" + x.Name, "[" + F(x.Value.x) + "," + F(x.Value.y) + "]");
			foreach (var x in v.Vector3Variables) Put(d, pre + "v3:" + x.Name, "[" + F(x.Value.x) + "," + F(x.Value.y) + "," + F(x.Value.z) + "]");
			foreach (var x in v.RectVariables) Put(d, pre + "r:" + x.Name, "[" + F(x.Value.x) + "," + F(x.Value.y) + "," + F(x.Value.width) + "," + F(x.Value.height) + "]");
			foreach (var x in v.QuaternionVariables) Put(d, pre + "q:" + x.Name, "[" + F(x.Value.x) + "," + F(x.Value.y) + "," + F(x.Value.z) + "," + F(x.Value.w) + "]");
			foreach (var x in v.ColorVariables) Put(d, pre + "c:" + x.Name, "[" + F(x.Value.r) + "," + F(x.Value.g) + "," + F(x.Value.b) + "," + F(x.Value.a) + "]");
			foreach (var x in v.GameObjectVariables) { _vs.Length = 0; J(_vs, x.Value != null ? ScenePaths.Canon(x.Value) : null); Put(d, pre + "go:" + x.Name, _vs.ToString()); }
			foreach (var x in v.EnumVariables) Put(d, pre + "e:" + x.Name, x.Value != null ? Convert.ToInt32(x.Value, CultureInfo.InvariantCulture).ToString(CultureInfo.InvariantCulture) : "null");
			foreach (var x in v.ObjectVariables)
			{
				// an AlertRange (FindAlertRange's store) is named by its GameObject: ["AlertRange", path]
				_vs.Length = 0;
				if (x.Value is AlertRange ar) { _vs.Append("[\"AlertRange\","); J(_vs, ScenePaths.Canon(ar.gameObject)); _vs.Append(']'); }
				else J(_vs, x.Value != null ? x.Value.name : null);
				Put(d, pre + "o:" + x.Name, _vs.ToString());
			}
			foreach (var x in v.ArrayVariables)
			{
				_vs.Length = 0;
				_vs.Append('[');
				var vals = x.Values;
				if (vals != null) for (int i = 0; i < vals.Length; i++) { if (i > 0) _vs.Append(','); Val(_vs, vals[i]); }
				_vs.Append(']');
				Put(d, pre + "a:" + x.Name, _vs.ToString());
			}
		}

		private static void Val(StringBuilder sb, object o)
		{
			switch (o)
			{
				case null: sb.Append("null"); break;
				case float f: sb.Append(F(f)); break;
				case int i: sb.Append(i); break;
				case bool b: sb.Append(b ? 1 : 0); break;
				case string s: J(sb, s); break;
				case Vector2 v2: sb.Append('[').Append(F(v2.x)).Append(',').Append(F(v2.y)).Append(']'); break;
				case Vector3 v3: sb.Append('[').Append(F(v3.x)).Append(',').Append(F(v3.y)).Append(',').Append(F(v3.z)).Append(']'); break;
				case GameObject g: J(sb, ScenePaths.Canon(g)); break;
				case UnityEngine.Object uo: J(sb, uo.name); break;
				case Enum e: sb.Append(Convert.ToInt32(e, CultureInfo.InvariantCulture)); break;
				default: J(sb, o.ToString()); break;
			}
		}

		// {key: value} for every key of `now` whose value differs from `prev` (all of `now` when prev is null).
		private static void VarDelta(StringBuilder sb, Dictionary<string, string> prev, Dictionary<string, string> now)
		{
			sb.Append('{');
			bool first = true;
			foreach (var kv in now)
			{
				string p;
				if (prev != null && prev.TryGetValue(kv.Key, out p) && p == kv.Value) continue;
				if (!first) sb.Append(',');
				first = false;
				J(sb, kv.Key);
				sb.Append(':').Append(kv.Value);
			}
			sb.Append('}');
		}

		// ------------------------------------------------------------------ private and component fields

		private static readonly Dictionary<Type, FieldInfo[]> _privOf = new Dictionary<Type, FieldInfo[]>();
		private static readonly Dictionary<Type, FieldInfo[]> _fieldsOf = new Dictionary<Type, FieldInfo[]>();

		private static bool Simple(Type t) => t == typeof(float) || t == typeof(int) || t == typeof(bool) || t == typeof(double)
			|| t == typeof(string) || t == typeof(Vector2) || t == typeof(Vector3) || t.IsEnum;

		// An action's non-public instance fields of simple types, declared below FsmStateAction: its private state.
		private static void Priv(StringBuilder sb, FsmStateAction a)
		{
			var t = a.GetType();
			FieldInfo[] fs;
			if (!_privOf.TryGetValue(t, out fs))
			{
				var l = new List<FieldInfo>();
				for (var cur = t; cur != null && cur != typeof(FsmStateAction); cur = cur.BaseType)
					foreach (var f in cur.GetFields(BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
						if (Simple(f.FieldType)) l.Add(f);
				fs = l.ToArray();
				_privOf[t] = fs;
			}
			WriteFields(sb, fs, a);
		}

		// A component's instance fields (public and private) of simple types.
		private static void Fields(StringBuilder sb, object comp)
		{
			var t = comp.GetType();
			FieldInfo[] fs;
			if (!_fieldsOf.TryGetValue(t, out fs))
			{
				var l = new List<FieldInfo>();
				for (var cur = t; cur != null && cur != typeof(MonoBehaviour); cur = cur.BaseType)
					foreach (var f in cur.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
						if (Simple(f.FieldType)) l.Add(f);
				fs = l.ToArray();
				_fieldsOf[t] = fs;
			}
			WriteFields(sb, fs, comp);
			if (comp is tk2dSpriteAnimator an)
			{
				sb.Length--;   // reopen the object for the animator's reference fields
				sb.Append(",\"clip\":");
				J(sb, an.CurrentClip?.name);
				sb.Append(",\"frame\":").Append(SafeFrame(an)).Append(",\"sprite\":").Append(an.Sprite != null ? an.Sprite.spriteId : -1).Append('}');
			}
		}

		private static int SafeFrame(tk2dSpriteAnimator an)
		{
			try { return an.CurrentClip != null ? an.CurrentFrame : -1; } catch { return -1; }
		}

		private static void WriteFields(StringBuilder sb, FieldInfo[] fs, object o)
		{
			sb.Append('{');
			for (int i = 0; i < fs.Length; i++)
			{
				if (i > 0) sb.Append(',');
				J(sb, fs[i].Name);
				sb.Append(':');
				object v;
				try { v = fs[i].GetValue(o); } catch { v = null; }
				if (v is double dd) sb.Append(dd.ToString("R", CultureInfo.InvariantCulture)); else Val(sb, v);
			}
			sb.Append('}');
		}

		// ------------------------------------------------------------------ input and PlayerData

		// HeroActions in the simulator's PA_* order (sim/hero/hero.h): bit 0 IsPressed, 1 WasPressed, 2 WasReleased.
		private static readonly string[] kPa = { "left", "right", "up", "down", "rs_up", "rs_down", "rs_left", "rs_right", "jump", "attack",
			"evade", "dash", "superDash", "dreamNail", "cast", "focus", "quickMap", "quickCast" };
		private static FieldInfo[] _paFields;

		private static void Inputs(StringBuilder sb)
		{
			var ih = InputHandler.Instance;
			var acts = ih != null ? ih.inputActions : null;
			if (acts == null) return;
			if (_paFields == null)
			{
				_paFields = new FieldInfo[kPa.Length];
				for (int i = 0; i < kPa.Length; i++) _paFields[i] = typeof(HeroActions).GetField(kPa[i], BindingFlags.Instance | BindingFlags.Public);
			}
			sb.Append(",\"in\":[");
			for (int i = 0; i < _paFields.Length; i++)
			{
				if (i > 0) sb.Append(',');
				var pa = _paFields[i]?.GetValue(acts) as InControl.PlayerAction;
				int bits = pa == null ? 0 : (pa.IsPressed ? 1 : 0) | (pa.WasPressed ? 2 : 0) | (pa.WasReleased ? 4 : 0);
				sb.Append(bits);
			}
			sb.Append(']');
		}

		// PlayerData's int / bool / float fields that differ from their SceneReady values (written in full in the
		// scene_ready marker), so each record carries the whole store in a few entries.
		private static FieldInfo[] _pdFields;
		private static object[] _pdBase;
		private static FieldInfo _pdInstance;

		private static PlayerData PdInstance()
		{
			if (_pdInstance == null) _pdInstance = typeof(PlayerData).GetField("_instance", BindingFlags.Static | BindingFlags.NonPublic);
			return _pdInstance?.GetValue(null) as PlayerData;   // the backing field: the getter creates one
		}

		private static void PdBaseline(StringBuilder sb)
		{
			var pd = PdInstance();
			if (_pdFields == null)
			{
				var l = new List<FieldInfo>();
				foreach (var f in typeof(PlayerData).GetFields(BindingFlags.Instance | BindingFlags.Public))
					if (f.FieldType == typeof(int) || f.FieldType == typeof(bool) || f.FieldType == typeof(float)) l.Add(f);
				_pdFields = l.ToArray();
			}
			_pdBase = new object[_pdFields.Length];
			sb.Append(",\"pd\":{");
			for (int i = 0; i < _pdFields.Length; i++)
			{
				_pdBase[i] = pd != null ? _pdFields[i].GetValue(pd) : null;
				if (i > 0) sb.Append(',');
				J(sb, _pdFields[i].Name);
				sb.Append(':');
				Val(sb, _pdBase[i]);
			}
			sb.Append("},\"pdt\":{");
			for (int i = 0; i < _pdFields.Length; i++)
			{
				if (i > 0) sb.Append(',');
				J(sb, _pdFields[i].Name);
				var ft = _pdFields[i].FieldType;
				sb.Append(ft == typeof(float) ? ":\"f\"" : ft == typeof(int) ? ":\"i\"" : ":\"b\"");
			}
			sb.Append('}');
		}

		private static void Pd(StringBuilder sb, string key)
		{
			var pd = PdInstance();
			if (pd == null || _pdBase == null) return;
			sb.Append(",\"").Append(key).Append("\":{");
			bool first = true;
			for (int i = 0; i < _pdFields.Length; i++)
			{
				object v = _pdFields[i].GetValue(pd);
				if (Equals(v, _pdBase[i])) continue;
				if (!first) sb.Append(',');
				first = false;
				J(sb, _pdFields[i].Name);
				sb.Append(':');
				Val(sb, v);
			}
			sb.Append('}');
		}

		// ------------------------------------------------------------------ GameObjects

		private static readonly Dictionary<Type, FieldInfo[]> _goFieldsOf = new Dictionary<Type, FieldInfo[]>();
		private static FieldInfo _lseSent;

		// The owner, the knight, and every GameObject the action's fields name (FsmOwnerDefault, FsmGameObject and
		// arrays of them), resolved as the action resolves them (Fsm.GetOwnerDefaultTarget, Fsm.cs:2458).
		private static List<GameObject> ActionGos(FsmStateAction a, Fsm fsm)
		{
			var l = new List<GameObject>(4);
			var own = fsm?.GameObject;
			if (own != null) l.Add(own);
			if (_knight != null && !l.Contains(_knight)) l.Add(_knight);
			var t = a.GetType();
			FieldInfo[] fs;
			if (!_goFieldsOf.TryGetValue(t, out fs))
			{
				var fl = new List<FieldInfo>();
				foreach (var f in t.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
				{
					var ft = f.FieldType;
					if (ft == typeof(FsmOwnerDefault) || ft == typeof(FsmGameObject) || ft == typeof(FsmOwnerDefault[]) || ft == typeof(FsmGameObject[])
						|| ft == typeof(FsmObject))
						fl.Add(f);
				}
				fs = fl.ToArray();
				_goFieldsOf[t] = fs;
			}
			foreach (var f in fs)
			{
				object v;
				try { v = f.GetValue(a); } catch { continue; }
				if (v is FsmOwnerDefault od) AddGo(l, fsm?.GetOwnerDefaultTarget(od));
				else if (v is FsmGameObject fg) AddGo(l, fg.Value);
				else if (v is FsmOwnerDefault[] oda) foreach (var x in oda) AddGo(l, x != null ? fsm?.GetOwnerDefaultTarget(x) : null);
				else if (v is FsmGameObject[] fga) foreach (var x in fga) AddGo(l, x?.Value);
				else if (v is FsmObject fo && fo.Value is Component comp) AddGo(l, comp.gameObject);
			}
			if (a.GetType().Name == "TakeDamage")
			{
				// HitTaker.Hit (HitTaker.cs:7-22) hits the IHitResponder of the target and of its next two ancestors:
				// their HealthManagers are inputs too
				for (int k = l.Count - 1; k >= 0; k--)
				{
					var cur = l[k].transform.parent;
					for (int d = 1; d < 3 && cur != null; d++, cur = cur.parent)
						if (cur.GetComponent<HealthManager>() != null) AddGo(l, cur.gameObject);
				}
			}
			return l;
		}

		// Actions whose ported receivers are HeroController methods (CallMethodProper / SendMessage to the knight):
		// they read and write the HeroController's fields and cState, which no GameObject record carries.
		private static readonly HashSet<string> kHeroTypes = new HashSet<string> { "CallMethodProper", "SendMessage" };

		// The FSM a GetFsm* / SetFsm* action addresses (its gameObject and fsmName fields), resolved as
		// ActionHelpers.GetGameObjectFsm does (ActionHelpers.cs:56-75) without its warning log; null when it is the
		// action's own FSM (whose variables vb / va carry) or none.
		private static Fsm OtherFsm(FsmStateAction a, Fsm fsm)
		{
			var t = a.GetType();
			if (!t.Name.StartsWith("GetFsm", StringComparison.Ordinal) && !t.Name.StartsWith("SetFsm", StringComparison.Ordinal)) return null;
			var od = t.GetField("gameObject")?.GetValue(a) as FsmOwnerDefault;
			var fn = t.GetField("fsmName")?.GetValue(a) as FsmString;
			var go = od != null ? fsm?.GetOwnerDefaultTarget(od) : null;
			if (go == null) return null;
			PlayMakerFSM pm = null;
			if (string.IsNullOrEmpty(fn?.Value)) pm = go.GetComponent<PlayMakerFSM>();
			else foreach (var x in go.GetComponents<PlayMakerFSM>()) if (x.FsmName == fn.Value) { pm = x; break; }
			return pm != null && pm.Fsm != fsm ? pm.Fsm : null;
		}

		// {"o": owner path, "n": FSM name, "v": its variables}
		private static void OtherVars(StringBuilder sb, string key, Fsm other)
		{
			var d = new Dictionary<string, string>();
			AddVars(d, "", other.Variables);
			sb.Append(",\"").Append(key).Append("\":{\"o\":");
			J(sb, ScenePaths.Canon(other.GameObject));
			sb.Append(",\"n\":");
			J(sb, other.Name);
			sb.Append(",\"v\":");
			VarDelta(sb, null, d);
			sb.Append('}');
		}

		// {"f": HeroController's simple fields, "cs": its cState's}
		private static void Hero(StringBuilder sb, string key)
		{
			var hc = _knight != null ? _knight.GetComponent<HeroController>() : null;
			if (hc == null) return;
			sb.Append(",\"").Append(key).Append("\":{\"f\":");
			Fields(sb, hc);
			sb.Append(",\"cs\":");
			if (hc.cState != null) Fields(sb, hc.cState); else sb.Append("{}");
			sb.Append('}');
		}

		private static void AddGo(List<GameObject> l, GameObject g)
		{
			if (g != null && !l.Contains(g) && l.Count < 12) l.Add(g);
		}

		private static void Gos(StringBuilder sb, List<GameObject> gos)
		{
			sb.Append('{');
			bool first = true;
			foreach (var g in gos)
			{
				if (g == null) continue;
				if (!first) sb.Append(',');
				first = false;
				J(sb, ScenePaths.Canon(g));
				sb.Append(':');
				Go(sb, g);
			}
			sb.Append('}');
		}

		private static void Go(StringBuilder sb, GameObject g)
		{
			var t = g.transform;
			Vector3 p = t.position, lp = t.localPosition, ls = t.localScale;
			sb.Append("{\"a\":").Append(g.activeSelf ? 1 : 0).Append(",\"h\":").Append(g.activeInHierarchy ? 1 : 0)
				.Append(",\"p\":[").Append(F(p.x)).Append(',').Append(F(p.y)).Append(',').Append(F(p.z)).Append(']')
				.Append(",\"lp\":[").Append(F(lp.x)).Append(',').Append(F(lp.y)).Append(',').Append(F(lp.z)).Append(']')
				.Append(",\"ls\":[").Append(F(ls.x)).Append(',').Append(F(ls.y)).Append(',').Append(F(ls.z)).Append(']')
				.Append(",\"lz\":").Append(F(t.localEulerAngles.z)).Append(",\"z\":").Append(F(t.eulerAngles.z))
				.Append(",\"lq\":[").Append(F(t.localRotation.z)).Append(',').Append(F(t.localRotation.w)).Append(']')
				.Append(",\"par\":");
			J(sb, t.parent != null ? ScenePaths.Canon(t.parent.gameObject) : null);
			if (t.parent != null)
			{
				// the parent chain's local poses, root first: the replay composes the world pose from them
				sb.Append(",\"anc\":[");
				var chain = new List<Transform>();
				for (var cur = t.parent; cur != null; cur = cur.parent) chain.Add(cur);
				for (int k = chain.Count - 1; k >= 0; k--)
				{
					var x = chain[k];
					Vector3 xp = x.localPosition, xs = x.localScale;
					sb.Append('[');
					J(sb, ScenePaths.Canon(x.gameObject));
					sb.Append(",[").Append(F(xp.x)).Append(',').Append(F(xp.y)).Append(',').Append(F(xp.z)).Append("],[")
						.Append(F(xs.x)).Append(',').Append(F(xs.y)).Append(',').Append(F(xs.z)).Append("],").Append(F(x.localEulerAngles.z)).Append(']');
					if (k > 0) sb.Append(',');
				}
				sb.Append(']');
			}
			var rb = g.GetComponent<Rigidbody2D>();
			if (rb != null)
			{
				Vector2 v = rb.velocity;
				sb.Append(",\"v\":[").Append(F(v.x)).Append(',').Append(F(v.y)).Append("],\"g\":").Append(F(rb.gravityScale))
					.Append(",\"kin\":").Append(rb.isKinematic ? 1 : 0);
			}
			var an = g.GetComponent<tk2dSpriteAnimator>();
			if (an != null) { sb.Append(",\"an\":"); Fields(sb, an); }
			var hm = g.GetComponent<HealthManager>();
			if (hm != null) { sb.Append(",\"hm\":"); Fields(sb, hm); }
			var alert = g.GetComponent<AlertRange>();
			if (alert != null) sb.Append(",\"ar\":").Append(alert.IsHeroInRange ? 1 : 0);
			var lse = g.GetComponent<LimitSendEvents>();
			if (lse != null)
			{
				if (_lseSent == null) _lseSent = typeof(LimitSendEvents).GetField("sentList", BindingFlags.Instance | BindingFlags.NonPublic);
				var sent = _lseSent?.GetValue(lse) as List<GameObject>;
				sb.Append(",\"lse\":[");
				if (sent != null) for (int i = 0; i < sent.Count; i++) { if (i > 0) sb.Append(','); J(sb, sent[i] != null ? ScenePaths.Canon(sent[i]) : null); }
				sb.Append(']');
			}
			sb.Append('}');
		}

		// ------------------------------------------------------------------ episode markers

		private static void OnSceneReady(Hooks.SceneContext ctx)
		{
			try
			{
				int n = ScenePaths.Snapshot();
				_episode++;
				_live.Clear();
				_open.Clear();
				_siteOf.Clear();
				try { _knight = HeroController.SilentInstance != null ? HeroController.SilentInstance.gameObject : null; } catch { _knight = null; }
				_sb.Length = 0;
				_sb.Append("{\"ev\":\"scene_ready\",\"ep\":").Append(_episode).Append(",\"f\":").Append(Time.frameCount)
					.Append(",\"fc\":").Append(TraceRecorder.FixedCount).Append(",\"level\":");
				J(_sb, ctx?.Level ?? "");
				_sb.Append(",\"snapshot\":").Append(n).Append(",\"knight\":");
				J(_sb, _knight != null ? ScenePaths.Canon(_knight) : null);
				PdBaseline(_sb);
				_sb.Append('}');
				Emit();
				_armed = _writer != null;
			}
			catch (Exception e) { Err("OnSceneReady", e); }
		}

		private static void OnEpisodeEnd(string info)
		{
			_armed = false;
			try
			{
				_sb.Length = 0;
				_sb.Append("{\"ev\":\"episode_end\",\"ep\":").Append(_episode).Append(",\"f\":").Append(Time.frameCount).Append(",\"info\":");
				J(_sb, info ?? "");
				_sb.Append(",\"calls\":").Append(_nCalls).Append(",\"activations\":").Append(_nActs).Append('}');
				Emit();
				_writer?.Flush();
			}
			catch (Exception e) { Err("OnEpisodeEnd", e); }
		}

		private static void Close()
		{
			if (_writer == null) return;
			_armed = false;
			try
			{
				_sb.Length = 0;
				_sb.Append("{\"ev\":\"close\",\"calls\":").Append(_nCalls).Append(",\"activations\":").Append(_nActs)
					.Append(",\"errors\":").Append(_errs.Count).Append('}');
				Emit();
				Log($"closing {_path}: {_nCalls} calls in {_nActs} activations, {_errs.Count} error sites");
				_writer.Close();
			}
			catch (Exception e) { Err("Close", e); }
			finally { _writer = null; _gz = null; _fs = null; }
		}

		// ------------------------------------------------------------------ output

		private static void Emit() { if (_writer != null) _writer.WriteLine(_sb.ToString()); }

		private static string F(float v) => v.ToString("R", CultureInfo.InvariantCulture);

		private static void J(StringBuilder sb, string s)
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
	}
}
