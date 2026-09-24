using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Text;
using HKOracle.Game;
using HutongGames.PlayMaker;
using MonoMod.RuntimeDetour;
using Newtonsoft.Json.Linq;
using UnityEngine;
using UnityEngine.SceneManagement;
using HKOracle.Env;

namespace HKOracle.Record
{
	// Produces docs/trace-format.md v2. Active iff HK_ORACLE_TRACE is set.
	public static class TraceRecorder
	{
		// Record kinds — docs/trace-format.md §Records.
		private const byte K_FRAME = 0x01, K_FIXED = 0x02,
			K_HC_FIXED_PRE = 0x03, K_HC_FIXED_POST = 0x04,
			K_HC_UPDATE_PRE = 0x05, K_HC_UPDATE_POST = 0x06,
			K_HC_LATE_PRE = 0x07, K_HC_LATE_POST = 0x08,
			K_OBS = 0x09, K_EVENT = 0x10;

		// EVENT sub-kinds — docs/trace-format.md §0x10 EVENT.
		private const byte E_SCENE_LOADED = 0, E_STEP = 1, E_HERO_DAMAGE = 2,
			E_ENEMY_DAMAGE = 3, E_FSM_TRANSITION = 4, E_FSM_EVENT = 5,
			E_SPAWN = 6, E_DESPAWN = 7, E_RNG_SEED = 8, E_LOG = 9,
			E_EPISODE_END = 10, E_RESET_BEGIN = 11, E_SCENE_READY = 12;

		private static TraceWriter _w;
		private static bool _armed;
		// docs/trace-format.md §0x10: 0 update-coroutine, 1 fixed, 2 late, 3 other/hook.
		private static byte _phase = 3;
		public static uint FixedCount;

		private static int? _seed;
		private static string _levelRequested = "";
		private static int _framesPerWait;
		private static Behaviour _mb;

		private struct FieldSpec { public FieldInfo Fi; public bool IsFloat; public string Name; }
		private static FieldSpec[] _heroFields, _pdFields;
		private static FieldInfo[] _cstateFields;
		private static int _heroSkipped, _pdSkipped;

		// analysis/decomp/Assembly-CSharp/HeroController.cs:435  private Rigidbody2D rb2d;
		private static FieldInfo _rb2dFi;
		// analysis/decomp/Assembly-CSharp/HeroController.cs:445  public HeroControllerStates cState;
		private static FieldInfo _cStateFi;

		private static Hook _hcFixed, _hcUpdate, _hcLate, _hcTakeDamage, _hmTakeDamage,
			_fsmSwitch, _fsmEvent;

		private static readonly uint[] _rng = new uint[4];
		private static bool _rngJsonLogged;

		private sealed class EntRow { public string Name; public int Id; public HealthManager Hm; }
		private static readonly List<EntRow> _ents = new List<EntRow>();
		private static readonly Dictionary<int, bool> _prevActive = new Dictionary<int, bool>();
		private static readonly List<int> _gone = new List<int>();
		private static readonly List<string> _pendingSpawn = new List<string>();
		private static readonly List<string> _pendingDespawn = new List<string>();
		private static readonly Dictionary<Fsm, string> _fsmOwnerNames = new Dictionary<Fsm, string>();

		private static void Log(string m) => HKOracle.Instance.Log($"[Trace] {m}");

		private static readonly Dictionary<string, int> _errCounts = new Dictionary<string, int>();
		private static void Err(string site, Exception e)
		{
			_errCounts.TryGetValue(site, out int n);
			if (n >= 5) { _errCounts[site] = n + 1; return; }
			_errCounts[site] = n + 1;
			Log($"ERROR in {site}: {HKOracle.DescribeException(e)}");
		}

		public static void Install()
		{
			string path = Mode.TracePath;
			string seedStr = System.Environment.GetEnvironmentVariable("HK_ORACLE_SEED");
			if (!string.IsNullOrEmpty(seedStr)
				&& int.TryParse(seedStr, NumberStyles.Integer, CultureInfo.InvariantCulture, out int s))
				_seed = s;

			_w = new TraceWriter(path);
			Log($"writing {path} (seed={(_seed.HasValue ? _seed.Value.ToString() : "null")})");

			_heroFields = BuildFieldSpecs(typeof(HeroController), out _heroSkipped);
			_pdFields = BuildFieldSpecs(typeof(PlayerData), out _pdSkipped);
			_cstateFields = BuildCstateFields();
			_rb2dFi = typeof(HeroController).GetField("rb2d",
				BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
			_cStateFi = typeof(HeroController).GetField("cState",
				BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
			if (_rb2dFi == null) Log("UNHOOKED HeroController.rb2d (field not found)");
			if (_cStateFi == null) Log("UNHOOKED HeroController.cState (field not found)");
			Log($"fields: hero={_heroFields.Length} (+{_heroSkipped} non-primitive skipped) "
				+ $"cstate={_cstateFields.Length} playerdata={_pdFields.Length} "
				+ $"(+{_pdSkipped} non-primitive skipped)");
			if (_cstateFields.Length > 64)
				Log($"UNHOOKED cstate bitset: {_cstateFields.Length} bools exceed u64");

			var go = new GameObject("HKOracle.TraceRecorder");
			UnityEngine.Object.DontDestroyOnLoad(go);
			go.hideFlags = HideFlags.HideAndDontSave;
			_mb = go.AddComponent<RecorderBehaviour>();

			UnityEngine.SceneManagement.SceneManager.sceneLoaded += OnSceneLoaded;
			Hooks.ResetBegin += OnResetBegin;
			Hooks.SceneReady += OnSceneReady;
			Hooks.StepBegin += OnStepBegin;
			Hooks.Frame += OnFrame;
			Hooks.EpisodeEnd += OnEpisodeEnd;
			Hooks.Obs += OnObs;

			InstallHooks();
		}

		// ---------------------------------------------------------------- hooks

		private static void InstallHooks()
		{
			// analysis/decomp/Assembly-CSharp/HeroController.cs:910  private void FixedUpdate()
			_hcFixed = HookHcVoid("FixedUpdate", K_HC_FIXED_PRE, K_HC_FIXED_POST, 1);
			// analysis/decomp/Assembly-CSharp/HeroController.cs:904  private void Update()
			_hcUpdate = HookHcVoid("Update", K_HC_UPDATE_PRE, K_HC_UPDATE_POST, 0);
			// HeroController has NO LateUpdate in the decompiled Assembly-CSharp
			// (grep "LateUpdate" over analysis/decomp/Assembly-CSharp/HeroController.cs
			// returns nothing). HookHcVoid logs UNHOOKED and HC_LATE_PRE/POST
			// (0x07/0x08) never appear in the stream.
			_hcLate = HookHcVoid("LateUpdate", K_HC_LATE_PRE, K_HC_LATE_POST, 2);

			// analysis/decomp/Assembly-CSharp/HeroController.cs:1825
			//   public void TakeDamage(GameObject go, CollisionSide damageSide, int damageAmount, int hazardType)
			var mHcTd = typeof(HeroController).GetMethod("TakeDamage",
				BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null,
				new[] { typeof(GameObject), typeof(GlobalEnums.CollisionSide), typeof(int), typeof(int) },
				null);
			if (mHcTd == null) Log("UNHOOKED HeroController.TakeDamage");
			else _hcTakeDamage = new Hook(mHcTd,
				new Action<Action<HeroController, GameObject, GlobalEnums.CollisionSide, int, int>,
					HeroController, GameObject, GlobalEnums.CollisionSide, int, int>(
				(orig, self, src, side, amount, hazard) =>
				{
					orig(self, src, side, amount, hazard);
					try
					{
						if (!BeginEvent(E_HERO_DAMAGE)) return;
						_w.Str16(src != null ? src.name : "");
						_w.I32(amount);
						_w.I32(hazard);
						var pd = PlayerData.instance;
						_w.I32(pd != null ? pd.health : -1);
						_w.EndRecord();
					}
					catch (Exception e) { Err("HeroController.TakeDamage", e); }
				}));

			// analysis/decomp/Assembly-CSharp/HealthManager.cs:430
			//   private void TakeDamage(HitInstance hitInstance)
			// This is the entry point that actually applies damage; Hit() (line 332)
			// filters blocked/evaded contacts and delegates here (line 344).
			var mHmTd = typeof(HealthManager).GetMethod("TakeDamage",
				BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic, null,
				new[] { typeof(HitInstance) }, null);
			if (mHmTd == null) Log("UNHOOKED HealthManager.TakeDamage");
			else _hmTakeDamage = new Hook(mHmTd,
				new Action<Action<HealthManager, HitInstance>, HealthManager, HitInstance>(
				(orig, self, hit) =>
				{
					orig(self, hit);
					try
					{
						if (!BeginEvent(E_ENEMY_DAMAGE)) return;
						_w.Str16(self != null && self.gameObject != null ? self.gameObject.name : "");
						_w.I32((int)hit.AttackType);
						_w.I32(hit.DamageDealt);
						_w.I32(self != null ? self.hp : -1);
						_w.EndRecord();
					}
					catch (Exception e) { Err("HealthManager.TakeDamage", e); }
				}));

			// analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2347
			//   public void SwitchState(FsmState toState)
			// Every real transition funnels here exactly once: DoTransition (2326)
			// sets switchToState and UpdateStateChanges (2314) drains it through
			// SwitchState; SetState (2309) and GotoPreviousState (2365) call it
			// directly. Hooking DoTransition instead would miss those two and would
			// double-count nothing, but DoTransition can also return false without
			// a state change (2331).
			var mSwitch = typeof(Fsm).GetMethod("SwitchState",
				BindingFlags.Instance | BindingFlags.Public, null, new[] { typeof(FsmState) }, null);
			if (mSwitch == null) Log("UNHOOKED Fsm.SwitchState");
			else _fsmSwitch = new Hook(mSwitch,
				new Action<Action<Fsm, FsmState>, Fsm, FsmState>((orig, self, to) =>
				{
					string from = null;
					bool want = _armed && _w != null && to != null;
					if (want) { try { from = self.ActiveStateName; } catch { from = null; } }
					orig(self, to);
					if (!want) return;
					try
					{
						if (!BeginEvent(E_FSM_TRANSITION)) return;
						_w.Str16(OwnerName(self));
						_w.Str16(self.Name ?? "");
						_w.Str16(from ?? "");
						_w.Str16(to.Name ?? "");
						_w.EndRecord();
					}
					catch (Exception e) { Err("Fsm.SwitchState", e); }
				}));

			// analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2192
			//   public void Event(FsmEvent fsmEvent)
			// Named by docs/trace-format.md ev 5. Event(string) (2184) funnels here;
			// Event(FsmEventTarget, FsmEvent) (2126) called directly does not.
			// Recorded PRE-orig so the event precedes any transition it causes.
			var mEvent = typeof(Fsm).GetMethod("Event",
				BindingFlags.Instance | BindingFlags.Public, null, new[] { typeof(FsmEvent) }, null);
			if (mEvent == null) Log("UNHOOKED Fsm.Event(FsmEvent)");
			else _fsmEvent = new Hook(mEvent,
				new Action<Action<Fsm, FsmEvent>, Fsm, FsmEvent>((orig, self, ev) =>
				{
					try
					{
						if (_armed && ev != null && BeginEvent(E_FSM_EVENT))
						{
							_w.Str16(OwnerName(self));
							_w.Str16(self.Name ?? "");
							_w.Str16(ev.Name ?? "");
							_w.EndRecord();
						}
					}
					catch (Exception e) { Err("Fsm.Event", e); }
					orig(self, ev);
				}));
		}

		private static Hook HookHcVoid(string name, byte pre, byte post, byte phase)
		{
			var m = typeof(HeroController).GetMethod(name,
				BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public,
				null, Type.EmptyTypes, null);
			if (m == null) { Log($"UNHOOKED HeroController.{name}"); return null; }
			return new Hook(m, new Action<Action<HeroController>, HeroController>((orig, self) =>
			{
				_phase = phase;
				try { EmitHero(pre, self); } catch (Exception e) { Err($"HC.{name}.pre", e); }
				orig(self);
				_phase = phase;
				try { EmitHero(post, self); } catch (Exception e) { Err($"HC.{name}.post", e); }
			}));
		}

		private static string OwnerName(Fsm fsm)
		{
			if (fsm == null) return "";
			if (_fsmOwnerNames.TryGetValue(fsm, out string n)) return n;
			try { n = fsm.GameObjectName ?? ""; } catch { n = ""; }
			_fsmOwnerNames[fsm] = n;
			return n;
		}

		// ------------------------------------------------------------- behaviour

		private sealed class RecorderBehaviour : MonoBehaviour
		{
			private void FixedUpdate()
			{
				_phase = 1;
				FixedCount++;
				try { EmitHero(K_FIXED, HeroController.instance); }
				catch (Exception e) { Err("RecorderBehaviour.FixedUpdate", e); }
			}

			private void Update() { _phase = 0; }
			private void LateUpdate() { _phase = 2; }
			private void OnApplicationQuit() { CloseTrace(); }
			private void OnDestroy() { CloseTrace(); }
		}

		public static void FlushTrace() { try { _w?.Flush(); } catch { } }

		public static void CloseTrace()
		{
			if (_w == null) return;
			try
			{
				Log($"closing {_w.Path} ({_w.RecordCount} records, headerWritten={_w.HeaderWritten})");
				_w.Close();
			}
			catch (Exception e) { Err("CloseTrace", e); }
			_w = null;
		}

		// ---------------------------------------------------------- hook targets

		private static void OnSceneLoaded(Scene scene, LoadSceneMode mode)
		{
			try
			{
				if (BeginEvent(E_SCENE_LOADED, allowUnarmed: true))
				{
					_w.Str16(scene.name ?? "");
					_w.EndRecord();
				}
				if (_seed.HasValue && !string.IsNullOrEmpty(_levelRequested)
					&& scene.name == _levelRequested)
				{
					UnityEngine.Random.InitState(_seed.Value);
					if (BeginEvent(E_RNG_SEED, allowUnarmed: true))
					{
						_w.I32(_seed.Value);
						_w.EndRecord();
					}
					Log($"Random.InitState({_seed.Value}) applied at sceneLoaded '{scene.name}' "
						+ $"frame={Time.frameCount}");
				}
			}
			catch (Exception e) { Err("OnSceneLoaded", e); }
		}

		private static void OnResetBegin(string level)
		{
			try
			{
				if (!string.IsNullOrEmpty(level)) _levelRequested = level;
				// Scene-load frames are wall-clock dependent (docs/trace-format.md
				// §Arming): disarm until the next SceneReady.
				_armed = false;
				_stepIndex = 0;
				if (BeginEvent(E_RESET_BEGIN, allowUnarmed: true))
				{
					_w.Str16(level ?? "");
					_w.EndRecord();
				}
			}
			catch (Exception e) { Err("OnResetBegin", e); }
		}

		private static void OnSceneReady(Hooks.SceneContext ctx)
		{
			try
			{
				_framesPerWait = ctx != null ? ctx.FramesPerWait : 0;
				if (ctx != null && !string.IsNullOrEmpty(ctx.Level)) _levelRequested = ctx.Level;
				if (_w != null && !_w.HeaderWritten) _w.WriteHeader(BuildHeaderJson());
				_phase = 0;
				_armed = true;
				// Seed the spawn/despawn baseline from the arena as SceneReady found
				// it, so SPAWN/DESPAWN only carry in-episode changes.
				_prevActive.Clear();
				CollectEntities();
				for (int i = 0; i < _ents.Count; i++)
					_prevActive[_ents[i].Id] = _ents[i].Hm != null && _ents[i].Hm.gameObject != null
						&& _ents[i].Hm.gameObject.activeInHierarchy;
				if (BeginEvent(E_SCENE_READY))
				{
					_w.Str16(ctx != null ? (ctx.Level ?? "") : "");
					_w.EndRecord();
				}
				_w?.Flush();
			}
			catch (Exception e) { Err("OnSceneReady", e); }
		}

		private static void OnStepBegin(int step, int[] action, bool committed)
		{
			try
			{
				_stepIndex = step;
				if (!BeginEvent(E_STEP)) return;
				_w.U32((uint)step);
				for (int i = 0; i < 4; i++) _w.I32(action != null && i < action.Length ? action[i] : -1);
				_w.U8(committed);
				_w.EndRecord();
			}
			catch (Exception e) { Err("OnStepBegin", e); }
		}

		private static void OnEpisodeEnd(string info)
		{
			try
			{
				if (BeginEvent(E_EPISODE_END))
				{
					_w.Str16(info ?? "");
					_w.EndRecord();
				}
				_w?.Flush();
			}
			catch (Exception e) { Err("OnEpisodeEnd", e); }
		}

		private static void OnObs(string kind, Env.Message m, int reset, int step)
		{
			try
			{
				if (!_armed || _w == null) return;
				byte[] payload = Env.BinaryProtocol.Pack(m);
				_w.U8(K_OBS);
				_w.U8(kind == "reset" ? (byte)0 : (byte)1);
				_w.U32((uint)reset);
				_w.U32((uint)step);
				_w.U32((uint)Time.frameCount);
				_w.U32((uint)payload.Length);
				_w.Bytes(payload, payload.Length);
				_w.EndRecord();
			}
			catch (Exception e) { Err("OnObs", e); }
		}

		// ------------------------------------------------------------- FRAME

		private static void OnFrame()
		{
			try
			{
				if (!_armed || _w == null) return;
				_phase = 0;
				var hc = HeroController.instance;

				_w.U8(K_FRAME);
				_w.U32((uint)Time.frameCount);
				_w.U32(FixedCount);
				_w.F32(Time.time);
				_w.F32(Time.deltaTime);
				_w.F32(Time.unscaledDeltaTime);
				_w.F32(Time.fixedTime);
				_w.F32(Time.timeScale);
				ReadRng();
				for (int i = 0; i < 4; i++) _w.U32(_rng[i]);
				var shim = InputDeviceShim.Attached;
				_w.U32(shim != null ? shim.KeyBits() : 0u);
				_w.U32((uint)StepIndex());

				WriteHeroBlock(hc);
				WriteEntityBlock();
				_w.EndRecord();

				// SPAWN/DESPAWN are derived from the ENTITY diff (no hook exists for
				// them); emitted immediately after the FRAME that revealed the change.
				for (int i = 0; i < _pendingSpawn.Count; i++)
				{
					if (!BeginEvent(E_SPAWN)) break;
					_w.Str16(_pendingSpawn[i]);
					_w.EndRecord();
				}
				for (int i = 0; i < _pendingDespawn.Count; i++)
				{
					if (!BeginEvent(E_DESPAWN)) break;
					_w.Str16(_pendingDespawn[i]);
					_w.EndRecord();
				}
				_pendingSpawn.Clear();
				_pendingDespawn.Clear();
			}
			catch (Exception e) { Err("OnFrame", e); }
		}

		// TrainingEnv._stepCount is private; the recorder mirrors it from
		// Hooks.StepBegin (raised once per Step() with that same counter).
		private static int _stepIndex;
		private static int StepIndex() => _stepIndex;

		private static void WriteHeroBlock(HeroController hc)
		{
			GameObject go = hc != null ? hc.gameObject : null;
			Transform tr = go != null ? go.transform : null;
			Rigidbody2D rb = (hc != null && _rb2dFi != null) ? _rb2dFi.GetValue(hc) as Rigidbody2D : null;

			Vector3 p = tr != null ? tr.position : Vector3.zero;
			_w.F32(p.x); _w.F32(p.y);
			_w.F32(tr != null ? tr.localScale.x : 0f);
			_w.F32(rb != null ? rb.position.x : 0f);
			_w.F32(rb != null ? rb.position.y : 0f);
			_w.F32(rb != null ? rb.velocity.x : 0f);
			_w.F32(rb != null ? rb.velocity.y : 0f);
			_w.F32(rb != null ? rb.gravityScale : 0f);
			_w.U8(rb != null && rb.isKinematic);

			object cs = (hc != null && _cStateFi != null) ? _cStateFi.GetValue(hc) : null;
			ulong bits = 0;
			if (cs != null)
			{
				for (int i = 0; i < _cstateFields.Length && i < 64; i++)
				{
					bool v = false;
					try { v = (bool)_cstateFields[i].GetValue(cs); } catch { }
					if (v) bits |= 1UL << i;
				}
			}
			_w.U64(bits);

			WriteFieldValues(_heroFields, hc);
			WriteFieldValues(_pdFields, PlayerData.instance);
			WriteAnim(go);

			int nCol = 0;
			Collider2D[] cols = go != null ? go.GetComponents<Collider2D>() : null;
			if (cols != null) nCol = cols.Length > 255 ? 255 : cols.Length;
			_w.U8((byte)nCol);
			for (int i = 0; i < nCol; i++)
			{
				var c = cols[i];
				_w.Str16(c != null ? c.GetType().Name : "");
				_w.U8(c != null && c.enabled);
				Vector2 off = c != null ? c.offset : Vector2.zero;
				_w.F32(off.x); _w.F32(off.y);
				var box = c as BoxCollider2D;
				_w.F32(box != null ? box.size.x : 0f);
				_w.F32(box != null ? box.size.y : 0f);
			}
		}

		// docs/trace-format.md §ANIM: tk2dSpriteAnimator on the SAME GameObject.
		private static void WriteAnim(GameObject go)
		{
			tk2dSpriteAnimator a = go != null ? go.GetComponent<tk2dSpriteAnimator>() : null;
			tk2dSpriteAnimationClip clip = null;
			try { clip = a != null ? a.CurrentClip : null; } catch { }
			if (a == null || clip == null)
			{
				_w.Str16(""); _w.I32(0); _w.F32(0f); _w.U8(false); _w.F32(0f);
				return;
			}
			_w.Str16(clip.name ?? "");
			int frame = 0; float ct = 0f; bool playing = false;
			try { frame = a.CurrentFrame; } catch { }
			try { ct = a.ClipTimeSeconds; } catch { }
			try { playing = a.Playing; } catch { }
			_w.I32(frame);
			_w.F32(ct);
			_w.U8(playing);
			_w.F32(clip.fps);
		}

		private static void CollectEntities()
		{
			_ents.Clear();
			var all = Resources.FindObjectsOfTypeAll<HealthManager>();
			for (int i = 0; i < all.Length; i++)
			{
				var hm = all[i];
				if (hm == null) continue;
				GameObject go = hm.gameObject;
				if (go == null) continue;
				// Excludes prefab assets; keeps inactive/pooled scene objects.
				if (!go.scene.isLoaded) continue;
				_ents.Add(new EntRow { Name = go.name, Id = go.GetInstanceID(), Hm = hm });
			}
			_ents.Sort((a, b) =>
			{
				int c = string.CompareOrdinal(a.Name, b.Name);
				return c != 0 ? c : a.Id.CompareTo(b.Id);
			});
		}

		private static readonly StringBuilder _pathSb = new StringBuilder(64);
		private static string RelPath(Transform root, Transform t)
		{
			if (t == root || t == null) return "";
			_pathSb.Length = 0;
			var cur = t;
			while (cur != null && cur != root)
			{
				if (_pathSb.Length > 0) _pathSb.Insert(0, '/');
				_pathSb.Insert(0, cur.name);
				cur = cur.parent;
			}
			return _pathSb.ToString();
		}

		private static void WriteEntityBlock()
		{
			CollectEntities();
			int n = _ents.Count > 65535 ? 65535 : _ents.Count;
			_w.U16((ushort)n);
			_gone.Clear();
			foreach (var kv in _prevActive) _gone.Add(kv.Key);

			for (int i = 0; i < n; i++)
			{
				var e = _ents[i];
				var hm = e.Hm;
				GameObject go = hm != null ? hm.gameObject : null;
				bool active = go != null && go.activeInHierarchy;

				_gone.Remove(e.Id);
				if (_prevActive.TryGetValue(e.Id, out bool was))
				{
					if (was != active) (active ? _pendingSpawn : _pendingDespawn).Add(e.Name);
				}
				else if (active) _pendingSpawn.Add(e.Name);
				_prevActive[e.Id] = active;

				_w.Str16(e.Name);
				_w.I32(e.Id);
				_w.U8(active);
				_w.I32(hm != null ? hm.hp : 0);
				_w.U8(hm != null && hm.isDead);
				// Same read HitboxObserver uses for is_invincible
				// (oracle/Game/HitboxObserver.cs).
				_w.U8(hm != null && hm.IsInvincible);
				Transform tr = go != null ? go.transform : null;
				Vector3 p = tr != null ? tr.position : Vector3.zero;
				_w.F32(p.x); _w.F32(p.y);
				_w.F32(tr != null ? tr.localScale.x : 0f);
				var rb = go != null ? go.GetComponent<Rigidbody2D>() : null;
				_w.F32(rb != null ? rb.velocity.x : 0f);
				_w.F32(rb != null ? rb.velocity.y : 0f);
				// ENTITY.rot / rot_t -- both, because they can disagree:
				//   rot   = Rigidbody2D.rotation, the value Box2D derives b2Rot from -> what the SOLVER sees.
				//   rot_t = transform.eulerAngles.z                                  -> what the COLLIDERS,
				//           and therefore the observation, actually follow.
				// A body with freezeRotation/fixedAngle (GG_Grimm_Nightmare's boss has both) pins `rot`
				// at 0 while an FSM writing transform.localEulerAngles -- ACT/FaceAngle.cs:59 does exactly
				// that -- can still move `rot_t`.
				_w.F32(rb != null ? rb.rotation : (tr != null ? tr.eulerAngles.z : 0f));
				_w.F32(tr != null ? tr.eulerAngles.z : 0f);
				WriteAnim(go);

				PlayMakerFSM[] fsms = go != null
					? go.GetComponentsInChildren<PlayMakerFSM>(true) : null;
				int nf = fsms != null ? (fsms.Length > 65535 ? 65535 : fsms.Length) : 0;
				_w.U16((ushort)nf);
				for (int j = 0; j < nf; j++)
				{
					var f = fsms[j];
					_w.Str16(f != null ? RelPath(tr, f.transform) : "");
					_w.Str16(f != null ? (f.FsmName ?? "") : "");
					string st = "";
					try { if (f != null && f.Fsm != null) st = f.Fsm.ActiveStateName ?? ""; } catch { }
					_w.Str16(st);
					_w.U8(f != null && f.isActiveAndEnabled);
					WriteFsmVars(f);
				}
			}

			for (int i = 0; i < _gone.Count; i++)
			{
				if (_prevActive.TryGetValue(_gone[i], out bool was) && was)
					_pendingDespawn.Add("#" + _gone[i].ToString(CultureInfo.InvariantCulture));
				_prevActive.Remove(_gone[i]);
			}
		}

		private static void WriteFsmVars(PlayMakerFSM f)
		{
			FsmVariables v = null;
			try { v = (f != null && f.Fsm != null) ? f.Fsm.Variables : null; } catch { }
			var fl = v != null ? v.FloatVariables : null;
			var iv = v != null ? v.IntVariables : null;
			var bv = v != null ? v.BoolVariables : null;
			int nf = fl != null ? fl.Length : 0;
			int ni = iv != null ? iv.Length : 0;
			int nb = bv != null ? bv.Length : 0;
			int total = nf + ni + nb;
			if (total > 65535) total = 65535;
			_w.U16((ushort)total);
			int written = 0;
			for (int i = 0; i < nf && written < total; i++, written++)
			{
				_w.Str16(fl[i] != null ? (fl[i].Name ?? "") : "");
				_w.U8((byte)0);
				_w.F32(fl[i] != null ? fl[i].Value : 0f);
			}
			for (int i = 0; i < ni && written < total; i++, written++)
			{
				_w.Str16(iv[i] != null ? (iv[i].Name ?? "") : "");
				_w.U8((byte)1);
				_w.F32(iv[i] != null ? iv[i].Value : 0);
			}
			for (int i = 0; i < nb && written < total; i++, written++)
			{
				_w.Str16(bv[i] != null ? (bv[i].Name ?? "") : "");
				_w.U8((byte)2);
				_w.F32(bv[i] != null && bv[i].Value ? 1f : 0f);
			}
		}

		// ------------------------------------------------------ FIXED / HC_*

		private static void EmitHero(byte kind, HeroController hc)
		{
			if (!_armed || _w == null) return;
			GameObject go = hc != null ? hc.gameObject : null;
			Transform tr = go != null ? go.transform : null;
			Rigidbody2D rb = (hc != null && _rb2dFi != null) ? _rb2dFi.GetValue(hc) as Rigidbody2D : null;
			_w.U8(kind);
			_w.U32((uint)Time.frameCount);
			_w.U32(FixedCount);
			_w.F32(Time.fixedTime);
			Vector3 p = tr != null ? tr.position : Vector3.zero;
			_w.F32(p.x); _w.F32(p.y);
			_w.F32(rb != null ? rb.position.x : 0f);
			_w.F32(rb != null ? rb.position.y : 0f);
			_w.F32(rb != null ? rb.velocity.x : 0f);
			_w.F32(rb != null ? rb.velocity.y : 0f);
			_w.EndRecord();
		}

		// -------------------------------------------------------------- EVENT

		private static bool BeginEvent(byte ev, bool allowUnarmed = false)
		{
			if (_w == null) return false;
			if (!_armed && !allowUnarmed) return false;
			_w.U8(K_EVENT);
			_w.U8(ev);
			_w.U32((uint)Time.frameCount);
			_w.U32(FixedCount);
			_w.U8(_phase);
			return true;
		}

		// ------------------------------------------------------------ helpers

		private static FieldSpec[] BuildFieldSpecs(Type t, out int skipped)
		{
			skipped = 0;
			var list = new List<FieldSpec>();
			// Reflection returns fields in metadata order on Mono, i.e. declaration
			// order (docs/trace-format.md §fields).
			foreach (var fi in t.GetFields(BindingFlags.Instance | BindingFlags.Public
				| BindingFlags.NonPublic))
			{
				Type ft = fi.FieldType;
				bool isFloat = ft == typeof(float);
				bool isInt = ft == typeof(bool) || ft == typeof(byte) || ft == typeof(sbyte)
					|| ft == typeof(short) || ft == typeof(ushort)
					|| ft == typeof(int) || ft == typeof(uint) || ft.IsEnum;
				if (!isFloat && !isInt) { skipped++; continue; }
				list.Add(new FieldSpec { Fi = fi, IsFloat = isFloat, Name = fi.Name });
			}
			return list.ToArray();
		}

		// analysis/decomp/Assembly-CSharp/HeroControllerStates.cs — 53 public bools.
		private static FieldInfo[] BuildCstateFields()
		{
			var list = new List<FieldInfo>();
			foreach (var fi in typeof(HeroControllerStates)
				.GetFields(BindingFlags.Instance | BindingFlags.Public))
				if (fi.FieldType == typeof(bool)) list.Add(fi);
			return list.ToArray();
		}

		private static void WriteFieldValues(FieldSpec[] specs, object obj)
		{
			for (int i = 0; i < specs.Length; i++)
			{
				object v = null;
				if (obj != null)
				{
					try { v = specs[i].Fi.GetValue(obj); } catch { v = null; }
				}
				if (specs[i].IsFloat) _w.F32(v is float f ? f : 0f);
				else _w.I32(ToI32(v));
			}
		}

		private static int ToI32(object v)
		{
			if (v == null) return 0;
			if (v is bool b) return b ? 1 : 0;
			if (v is int i) return i;
			try { return unchecked((int)Convert.ToInt64(v, CultureInfo.InvariantCulture)); }
			catch { return 0; }
		}

		// UnityEngine.Random.state is an opaque struct; JsonUtility is the only
		// public serializer for it. The field names observed at runtime are logged
		// once at the first FRAME ([Trace] Random.state JSON: ...). Parsed
		// positionally over the integer literals outside quoted spans, so the four
		// words land in stream order whatever the names are.
		// UnityEngine.JsonUtility (UnityEngine.JSONSerializeModule) is resolved by
		// reflection. Fallback: the four private int fields of
		// UnityEngine.Random.State, read directly; the path taken is logged once.
		private static MethodInfo _toJson;
		private static FieldInfo[] _rngStateFields;
		private static bool _rngResolved;

		private static void ResolveRngReader()
		{
			_rngResolved = true;
			Type jt = Type.GetType("UnityEngine.JsonUtility, UnityEngine.JSONSerializeModule")
				?? Type.GetType("UnityEngine.JsonUtility, UnityEngine");
			if (jt == null)
			{
				foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
				{
					try { jt = asm.GetType("UnityEngine.JsonUtility"); } catch { jt = null; }
					if (jt != null) break;
				}
			}
			if (jt != null) _toJson = jt.GetMethod("ToJson", new[] { typeof(object) });
			var st = typeof(UnityEngine.Random).GetNestedType("State",
				BindingFlags.Public | BindingFlags.NonPublic);
			if (st != null)
				_rngStateFields = st.GetFields(BindingFlags.Instance
					| BindingFlags.Public | BindingFlags.NonPublic);
			var names = new StringBuilder();
			if (_rngStateFields != null)
				foreach (var f in _rngStateFields)
					names.Append(f.FieldType.Name).Append(' ').Append(f.Name).Append("; ");
			Log($"rng reader: JsonUtility.ToJson={( _toJson != null )} "
				+ $"Random.State fields=[{names}]");
			if (_toJson == null && _rngStateFields == null)
				Log("UNHOOKED UnityEngine.Random.state — neither JsonUtility nor "
					+ "Random.State fields reachable; FRAME.rng will be zeros");
		}

		private static void ReadRng()
		{
			_rng[0] = _rng[1] = _rng[2] = _rng[3] = 0u;
			if (!_rngResolved) ResolveRngReader();
			object state;
			try { state = UnityEngine.Random.state; }
			catch (Exception e) { Err("ReadRng.state", e); return; }

			if (_toJson == null)
			{
				if (_rngStateFields == null) return;
				for (int i = 0; i < _rngStateFields.Length && i < 4; i++)
					_rng[i] = unchecked((uint)ToI32(_rngStateFields[i].GetValue(state)));
				return;
			}

			string json;
			try { json = (string)_toJson.Invoke(null, new[] { state }); }
			catch (Exception e) { Err("ReadRng.ToJson", e); return; }
			if (json == null) return;
			if (!_rngJsonLogged) { _rngJsonLogged = true; Log($"Random.state JSON: {json}"); }
			int n = 0;
			bool inStr = false;
			for (int i = 0; i < json.Length && n < 4; i++)
			{
				char c = json[i];
				if (c == '"') { inStr = !inStr; continue; }
				if (inStr) continue;
				if (c == '-' || (c >= '0' && c <= '9'))
				{
					int j = i;
					bool neg = c == '-';
					if (neg) j++;
					long val = 0;
					int digits = 0;
					while (j < json.Length && json[j] >= '0' && json[j] <= '9')
					{
						val = val * 10 + (json[j] - '0');
						j++; digits++;
					}
					i = j - 1;
					if (digits == 0) continue;
					if (neg) val = -val;
					_rng[n++] = unchecked((uint)(int)val);
				}
			}
		}

		private static int SafeBossLevel()
		{
			try { var b = BossSceneController.Instance; return b == null ? -1 : b.BossLevel; } catch { return -1; }
		}

		private static byte[] BuildHeaderJson()
		{
			var capture = new JObject
			{
				["scene"] = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name,
				["level_requested"] = _levelRequested ?? "",
				["unity_version"] = Application.unityVersion,
				["mod_commit"] = ModInfo.Commit,
				["tier_requested"] = System.Environment.GetEnvironmentVariable("HK_ORACLE_TIER") ?? "",
				["tier_loaded"] = global::HKOracle.Game.SceneHooks.LoadedTier,
				["boss_level"] = SafeBossLevel(),
				["timestamp_utc"] = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ",
					CultureInfo.InvariantCulture),
				["capture_dt"] = Time.captureDeltaTime,
				["fixed_dt"] = Time.fixedDeltaTime,
				["frames_per_wait"] = _framesPerWait,
				["launch_args"] = new JArray(System.Environment.GetCommandLineArgs()),
				["mode"] = ScriptDriver.Active ? "script" : "ws",
				["script_path"] = ScriptDriver.Active
					? (JToken)new JValue(ScriptDriver.ScriptPath) : JValue.CreateNull(),
				["script_sha256"] = ScriptDriver.Active
					? (JToken)new JValue(ScriptDriver.ScriptSha256) : JValue.CreateNull(),
				["seed"] = _seed.HasValue ? (JToken)new JValue(_seed.Value) : JValue.CreateNull(),
				["exe_tag"] = System.Diagnostics.Process.GetCurrentProcess().ProcessName,
			};

			var hero = new JArray();
			foreach (var f in _heroFields)
				hero.Add(new JObject { ["name"] = f.Name, ["type"] = f.IsFloat ? "f32" : "i32" });
			var pd = new JArray();
			foreach (var f in _pdFields)
				pd.Add(new JObject { ["name"] = f.Name, ["type"] = f.IsFloat ? "f32" : "i32" });
			var cst = new JArray();
			foreach (var f in _cstateFields) cst.Add(f.Name);
			var inp = new JArray();
			foreach (var k in InputDeviceShim.KeyNames) inp.Add(k);

			var hc = HeroController.instance;
			var fields = new JObject
			{
				["hero"] = hero,
				["cstate"] = cst,
				["playerdata"] = pd,
				["input"] = inp,
				["hero_go"] = hc != null && hc.gameObject != null ? hc.gameObject.name : "",
			};

			var root = new JObject { ["capture"] = capture, ["fields"] = fields };
			string json = root.ToString(Newtonsoft.Json.Formatting.None);
			Log($"header: scene={capture["scene"]} fpw={_framesPerWait} "
				+ $"capture_dt={Time.captureDeltaTime} fixed_dt={Time.fixedDeltaTime} "
				+ $"json_len={Encoding.UTF8.GetByteCount(json)}");
			return Encoding.UTF8.GetBytes(json);
		}
	}
}
