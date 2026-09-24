using System.Collections;
using System.Collections.Generic;
using System.Text;
using HKOracle.Game;
using HutongGames.PlayMaker;
using HutongGames.PlayMaker.Actions;
using InControl;
using Modding;
using UnityEngine;

namespace HKOracle.Env
{
	public class TrainingEnv : WebsocketEnv
	{
		private string _level;
		private int _frameSkipCount;
		private int _hitsTakenInStep;
		private float _damageLandedInStep;
		private float _hpHealedInStep;
		// Per-episode knight-hit accounting in EVENTS and HP. hits_taken on
		// the wire is HP (per-hit damage varies 1-2 by boss), so hit COUNTS
		// are only recoverable from these log-side tallies ([EpisodeHits]).
		private int _hitEventsInEpisode;
		private int _hpLostInEpisode;
		private int _knightHpAtStepStart;

		private bool _bossDied;
		private bool _episodeDone;
		private string _episodeResult;
		// The boss HealthManagers: is_target in the obs and the damage_landed credit.
		private readonly HashSet<HealthManager> _bossHMs = new();
		// Max HP at bind time per boss: multi-boss fights (Oro/Mato, God Tamer) have
		// asymmetric pools and damage is normalized per boss.
		private readonly Dictionary<HealthManager, int> _bossMaxHPs = new();
		// OnDeath subscribers per HM, kept to unsubscribe on the next reset. OnDeath
		// fires only on the final death (multi-phase staggers bypass it), the same
		// signal BossSceneController ends the scene on.
		private readonly Dictionary<HealthManager, HealthManager.DeathEvent> _bossDeathHandlers = new();

		// FreezeMoment kill delegates, installed once in Setup(), kept for Dispose.
		private On.GameManager.hook_FreezeMoment_float_float_float_float _killFreezeFloat;
		private On.GameManager.hook_FreezeMoment_float_float_float_bool _killFreezeBool;
		private On.GameManager.hook_FreezeMoment_int _killFreezeInt;
		private On.GameManager.hook_FreezeMomentGC _killFreezeMomentGC;

		private int _resetCount;
		private int _stepCount;
		// Wall-clock phase timing (Time.realtimeSinceStartup ignores timeScale).
		private float _phaseStart;
		private float _phaseLast;
		// Per-phase ms / frame counts for the current Reset, in ResetPhase.Keys
		// order, shipped on the reset message.
		private readonly float[] _resetPhaseMs = new float[ResetPhase.Count];
		private readonly ushort[] _resetPhaseFrames = new ushort[ResetPhase.Count];
		private byte _resetBranch;
		// Frame at the last LogResetPhase call.
		private int _phaseLastFrame;

		private HitboxObserver _hitboxObserver = new();
		private FsmObserver _fsmObserver = new();
		private InputDeviceShim _inputShim = new();
		private Game.TimeScale _timeManager;
		// Heartbeat counter for FsmDiag logging in SnapshotFsms.
		private int _fsmDiagTicks;

		// Game-time per Unity frame (Time.captureDeltaTime), pinned in Reset() and held
		// for the episode: 0.02 = fixedDeltaTime (regime R2: one FixedUpdate per frame).
		// The inter-step pause is timeScale = 0, so deltaTime is 0 and nothing ticks
		// between decisions. Capture mode needs a renderer: -batchmode works,
		// -nographics does not.
		public const float kStepDeltaTime = 0.02f;

		public TrainingEnv(string url, params string[] protocols) : base(url, protocols) { }

		protected override IEnumerator OnMessage(Message message)
		{
			switch (message.type)
			{
				case "action":
					yield return Step(message.data);
					break;
				case "reset":
					yield return Reset(message.data);
					break;
			}
		}

		// pause / resume / close take no frame. Step() owns freeze/unfreeze: unfreezing on
		// resume would let held inputs tick frames and leak damage into the next step.
		protected override bool HandleNow(Message message)
		{
			switch (message.type)
			{
				case "close":
					_terminate = true;
					return true;
				case "pause":
					Time.timeScale = 0;
					SendMessage(new Message { type = "pause", data = message.data });
					return true;
				case "resume":
					SendMessage(new Message { type = "resume", data = message.data });
					return true;
			}
			return false;
		}

		private IEnumerator Reset(MessageData data)
		{
			PhaseBegin();
			Hooks.RaiseResetBegin(data.level);
			Log($"[ResetEntry] reset#{_resetCount + 1} requestedLevel={data.level ?? "(null)"} "
				+ $"eval={data.eval} fpw={data.frames_per_wait}");
			// Entry state before any field changes (e.g. was the scene still playing).
			{
				var _gm = GameManager.instance;
				string _entryScene = UnityEngine.SceneManagement.SceneManager
					.GetActiveScene().name;
				string _gmState = "?";
				try { _gmState = _gm != null ? _gm.gameState.ToString() : "?"; }
				catch { }
				bool _inTrans = _gm != null && _gm.IsInSceneTransition;
				int _hpNow = PlayerData.instance != null ? PlayerData.instance.health : -1;
				var _bossHpStr = new StringBuilder();
				foreach (var hm in _bossHMs)
				{
					if (_bossHpStr.Length > 0) _bossHpStr.Append(",");
					_bossHpStr.Append(hm == null ? "null" : hm.hp.ToString());
				}
				Log($"[ResetDiag] reset#{_resetCount + 1} entryScene={_entryScene} "
					+ $"gmState={_gmState} inTransition={_inTrans} knightHp={_hpNow} "
					+ $"prevResult={_episodeResult ?? "(none)"} bossHps=[{_bossHpStr}] "
					+ $"stepCount={_stepCount} bossDied={_bossDied} "
					+ $"episodeDone={_episodeDone} level={data.level ?? _level}");
			}

			for (int i = 0; i < ResetPhase.Count; i++)
			{
				_resetPhaseMs[i] = 0f;
				_resetPhaseFrames[i] = 0;
			}
			_resetBranch = ResetPhase.BranchUnknown;

			_level = data.level ?? _level;
			_frameSkipCount = data.frames_per_wait ?? _frameSkipCount;
			_hitsTakenInStep = 0;
			_damageLandedInStep = 0;
			_hpHealedInStep = 0;
			_hitEventsInEpisode = 0;
			_hpLostInEpisode = 0;
			_stepCount = 0;
			_bossDied = false;
			_episodeDone = false;
			_episodeResult = null;
			_resetCount++;

			LogResetPhase("pre_unload", "PRE-UNLOAD");

			// Release held inputs and any hard-commit lock before the transition
			// unfreezes time, or a stuck key runs the knight through it.
			_inputShim.ResetCommit();
			ActionDecoder.ApplyAction(_inputShim, new int[] { 2, 2, 7, 1 },
				_frameSkipCount);

			// Unpause for the scene transition; captureDeltaTime makes per-frame
			// game-time independent of wall-clock fps.
			Time.timeScale = 1f;
			Time.captureDeltaTime = kStepDeltaTime;

			// Either already in the Workshop, or HK's dream-return / hero-death
			// transition is queued: wait until the arena is left, then
			// SceneHooks.LoadBossScene handles whatever state HK lands in.
			var preScene = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name;
			if (preScene == "GG_Workshop")
			{
				_resetBranch = ResetPhase.BranchWorkshop;
				LogResetPhase("transition_out", "ALREADY-IN-WORKSHOP");
			}
			else
			{
				_resetBranch = ResetPhase.BranchNaturalEnd;
				yield return WaitForSceneChange(preScene);
				var afterScene = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name;
				LogResetPhase("transition_out", $"NATURAL-END (pre={preScene} post={afterScene})");
			}

			// Empty phase: keeps the 7-slot reset_phase wire layout. LoadBossScene
			// does its own entry wait.
			LogResetPhase("settle", "settle (skipped)");

			yield return SceneHooks.LoadBossScene(_level);
			LogResetPhase("load_boss_scene", "LoadBossScene");

			// Rebuild the hitbox reader explicitly (activeSceneChanged is unreliable under
			// multi-instance load) and yield a frame for its Start() to scan.
			_hitboxObserver.RecreateReader();
			yield return null;
			LogResetPhase("recreate_reader", "RecreateReader+frame");

			InitBossRefs();
			// BossSceneController.bosses can populate lazily: retry until colliders enable.
			int wakeFrames = 0;
			const int kMaxWakeFrames = 600;
			// Grace for the two native registration paths before the live scan may
			// bind (a boss's colliders can enable a frame before its HM registers).
			const int kUnboundGraceFrames = 30;
			while (wakeFrames < kMaxWakeFrames)
			{
				if (_bossHMs.Count == 0) InitBossRefs();
				// FSM-ended scenes (e.g. GG_Grimm_Nightmare) leave BossSceneController.bosses
				// empty by design, and ReportHealth only fills BossHealthLookup for HMs
				// already in that array, so neither native path ever binds. Fall back to
				// the live scan once the boss is awake and the native paths had their grace.
				if (_bossHMs.Count == 0 && wakeFrames >= kUnboundGraceFrames
					&& HasActiveCombatHitboxes())
				{
					ScanBindBossHMs($"wake#{wakeFrames}");
				}
				if (_bossHMs.Count > 0 && HasActiveCombatHitboxes()) break;
				yield return null;
				wakeFrames++;
			}
			bool bossAwake = HasActiveCombatHitboxes();
			LogResetPhase("init_boss_refs", "InitBossRefs+BossWake");
			Log($"[BounceCheck] reset#{_resetCount} level={_level} "
				+ $"bossAwake={bossAwake} wakeFrames={wakeFrames} "
				+ $"bossHMs={_bossHMs.Count} knightHp={PlayerData.instance.health}");
			ClampBossLevel();

			UnhookDamage();
			HookDamage();

			// TimeScale shim at multiplier 1 (pass-through). Built once: each
			// dispose/reapply re-JITs detoured bodies into memory mono never reclaims.
			if (_timeManager == null) _timeManager = new Game.TimeScale(1f);

			var obs = _hitboxObserver.GetSplitFeatures(_bossHMs);
			var gs = StateExtractor.GetGlobalState(
				obs.KnightWidth, obs.KnightHeight, _inputShim);

			data.combat_hitboxes = obs.CombatHitboxes;
			data.combat_kinds = obs.CombatKinds;
			data.combat_parents = obs.CombatParents;
			data.terrain_hitboxes = obs.TerrainHitboxes;
			data.terrain_debug = obs.TerrainDebug;
			data.global_state = gs;
			data.fsm_snapshots = SnapshotFsms();

			Time.timeScale = 0;
			// Re-zero after every frame this reset ticked: frames during the load run
			// with the damage hooks attached and must not be charged to step 1.
			_hitsTakenInStep = 0;
			_damageLandedInStep = 0;
			_hpHealedInStep = 0;
			LogResetPhase("obs_final", "obs+freeze (final)");
			float resetTotalMs = (Time.realtimeSinceStartup - _phaseStart) * 1000f;
			Log($"[Reset-Timing] reset#{_resetCount} TOTAL {resetTotalMs:F0}ms level={_level}");
			data.reset_branch = _resetBranch;
			data.reset_phase_ms = (float[])_resetPhaseMs.Clone();
			data.reset_phase_frames = (ushort[])_resetPhaseFrames.Clone();
			Hooks.RaiseSceneReady(new Hooks.SceneContext {
				Level = _level, BossHMs = _bossHMs, Hitboxes = _hitboxObserver,
				Shim = _inputShim, FramesPerWait = _frameSkipCount, ResetCount = _resetCount });
			Hooks.RaiseObs("reset", new Message { type = "reset", data = data }, _resetCount, _stepCount);
			SendMessage(new Message { type = "reset", data = data });
			yield break;
		}

		private IEnumerator Step(MessageData data)
		{
			PhaseBegin();
			_stepCount++;
			// Late-bind boss refs: bosses with long entrance cinematics (Broken Vessel,
			// Soul Master) register after the reset-time wake window.
			if (_bossHMs.Count == 0 && _stepCount % 24 == 0)
			{
				InitBossRefs();
				if (_bossHMs.Count > 0)
				{
					Log($"[LateBind] reset#{_resetCount} step#{_stepCount} "
						+ $"bossHMs={_bossHMs.Count}");
				}
				else if (_stepCount == 24)
				{
					var _bsc = BossSceneController.Instance;
					Log($"[LateBind/diag] reset#{_resetCount} bsc={_bsc != null} "
						+ $"bossesLen={(_bsc?.bosses?.Length ?? -1)} "
						+ $"lookup={(_bsc?.BossHealthLookup?.Count ?? -1)}");
				}
				if (_bossHMs.Count == 0 && _stepCount >= 240
					&& _stepCount % 240 == 0)
				{
					ScanBindBossHMs($"step#{_stepCount}");
				}
			}
			// If episode already ended, keep returning done
			if (_episodeDone)
			{
				data.done = true;
				data.info = _episodeResult;
				data.combat_hitboxes = new List<float[]>();
				data.terrain_hitboxes = new List<float[]>();
				data.global_state = new float[StateExtractor.GlobalStateDim];
				data.fsm_snapshots = new List<string>();
				data.damage_landed = 0;
				data.hits_taken = 0;
				data.hp_healed = 0;
				data.step_game_time = 0;
				data.step_real_time = 0;
				Hooks.RaiseObs("step", new Message { type = "step", data = data }, _resetCount, _stepCount);
			SendMessage(new Message { type = "step", data = data });
				yield break;
			}

			// timeScale must be restored BEFORE ApplyAction: HeroController.LookForQueueInput
			// consumes inputs in Update, which also runs on frozen frames.
			Time.timeScale = 1f;

			bool committedThisStep = ActionDecoder.ApplyAction(
				_inputShim, data.action_vec, _frameSkipCount);
			data.action_committed = committedThisStep;
			Hooks.RaiseStepBegin(_stepCount, data.action_vec, committedThisStep);

			_knightHpAtStepStart = PlayerData.instance.health;

			float frameSkipT0 = Time.realtimeSinceStartup;
			float gameTimeElapsed = 0f;
			float realTimeElapsed = 0f;
			int frameSkipFrames = 0;
			for (int i = 0; i < _frameSkipCount; i++)
			{
				yield return null;
				frameSkipFrames++;				Hooks.RaiseFrame();

				gameTimeElapsed += Time.deltaTime;
				realTimeElapsed += Time.unscaledDeltaTime;
				if (_bossDied || PlayerData.instance.health <= 0)
					break;
			}
			float frameSkipMs = (Time.realtimeSinceStartup - frameSkipT0) * 1000f;

			Time.timeScale = 0;
			data.step_game_time = gameTimeElapsed;
			data.step_real_time = realTimeElapsed;

			if (_bossDied)
			{
				_episodeDone = true;
				_episodeResult = "win";
			}
			else if (PlayerData.instance.health <= 0)
			{
				_episodeDone = true;
				_episodeResult = "loss";
			}

			if (_episodeDone)
			{
				Log($"[EpisodeHits] reset#{_resetCount} result={_episodeResult} "
					+ $"hitEvents={_hitEventsInEpisode} hpLost={_hpLostInEpisode}");
				_hitEventsInEpisode = 0;
				_hpLostInEpisode = 0;
			}

			// HP healed this step. Never paid on the episode's last step.
			int hpNow = PlayerData.instance.health;
			int hpDelta = hpNow - _knightHpAtStepStart;
			_hpHealedInStep = (hpDelta > 0 && !_episodeDone) ? (float)hpDelta : 0f;

			data.damage_landed = _damageLandedInStep;
			data.hits_taken = _hitsTakenInStep;
			data.hp_healed = _hpHealedInStep;
			_hitsTakenInStep = 0;
			_damageLandedInStep = 0;
			_hpHealedInStep = 0;

			if (_episodeDone) Hooks.RaiseEpisodeEnd(_episodeResult);

			// Leak probes; cheap (field reads, non-collecting GC query).
			var sizes = _hitboxObserver.GetCacheSizes();
			data.diag_enemy_count = (ushort)System.Math.Min(sizes.EnemyCount, ushort.MaxValue);
			data.diag_attack_count = (ushort)System.Math.Min(sizes.AttackCount, ushort.MaxValue);
			data.diag_terrain_count = (ushort)System.Math.Min(sizes.TerrainCount, ushort.MaxValue);
			data.diag_kind_cache_size = sizes.KindCacheCount;
			data.diag_gc_heap_mb = System.GC.GetTotalMemory(false) / (1024f * 1024f);

			float stepWallMs = (Time.realtimeSinceStartup - _phaseStart) * 1000f;
			if (stepWallMs > 1000f)
			{
				Log($"[Step-Timing] reset#{_resetCount} step#{_stepCount} "
					+ $"total={stepWallMs:F0}ms frameSkip={frameSkipMs:F0}ms"
					+ $"({frameSkipFrames}f) "
					+ $"gameTime={gameTimeElapsed * 1000:F0}ms "
					+ $"realTime={realTimeElapsed * 1000:F0}ms done={_episodeDone}");
			}

			if (_episodeDone)
			{
				data.done = true;
				data.info = _episodeResult;
				data.combat_hitboxes = new List<float[]>();
				data.terrain_hitboxes = new List<float[]>();
				data.global_state = new float[StateExtractor.GlobalStateDim];
				data.fsm_snapshots = new List<string>();
				Hooks.RaiseObs("step", new Message { type = "step", data = data }, _resetCount, _stepCount);
			SendMessage(new Message { type = "step", data = data });
				yield break;
			}

			var obs = _hitboxObserver.GetSplitFeatures(_bossHMs);
			var gs = StateExtractor.GetGlobalState(
				obs.KnightWidth, obs.KnightHeight, _inputShim);

			data.combat_hitboxes = obs.CombatHitboxes;
			data.combat_kinds = obs.CombatKinds;
			data.combat_parents = obs.CombatParents;
			data.terrain_hitboxes = obs.TerrainHitboxes;
			data.terrain_debug = obs.TerrainDebug;
			data.global_state = gs;
			data.fsm_snapshots = SnapshotFsms();
			data.done = false;

			Hooks.RaiseObs("step", new Message { type = "step", data = data }, _resetCount, _stepCount);
			SendMessage(new Message { type = "step", data = data });
			yield break;
		}

		// FSM states for the viewer (fsm_snapshots; not used by training): every FSM
		// under each boss, on active Enemy colliders (pooled projectiles leave the
		// boss subtree), and on Attack colliders (the knight's nail / spells), from
		// the observation just built (HitboxObserver.Last).
		private List<string> SnapshotFsms()
		{
			var enemies = _hitboxObserver.Last.Enemy;
			var attacks = _hitboxObserver.Last.Attack;

			List<string> result;
			try
			{
				result = _fsmObserver.Snapshot(_bossHMs, enemies, attacks);
			}
			catch (System.Exception e)
			{
				Log($"[FsmDiag] Snapshot threw: {e.GetType().Name}: {e.Message}\n{e.StackTrace}");
				result = new List<string>();
			}

			// Heartbeat log: first step, then every 60.
			_fsmDiagTicks++;
			if (_fsmDiagTicks == 1 || _fsmDiagTicks % 60 == 0)
			{
				int eCount = enemies.Count;
				int aCount = attacks.Count;
				Log($"[FsmDiag] tick={_fsmDiagTicks} bossHMs={_bossHMs.Count} "
					+ $"enemyColliders={eCount} attackColliders={aCount} "
					+ $"snapshot={result.Count}");
			}
			return result;
		}

		private void Log(string msg) => HKOracle.Instance.Log($"[TrainingEnv] {msg}");

		private void PhaseBegin()
		{
			_phaseStart = Time.realtimeSinceStartup;
			_phaseLast = _phaseStart;
			_phaseLastFrame = Time.frameCount;
		}

		// Accumulate wall ms + frames since the last call into phase `phaseKey`
		// (one of ResetPhase.Keys); `label` is for the log only.
		private void LogResetPhase(string phaseKey, string label)
		{
			float now = Time.realtimeSinceStartup;
			int frameNow = Time.frameCount;
			float deltaMs = (now - _phaseLast) * 1000f;
			int deltaFrames = frameNow - _phaseLastFrame;
			float totalMs = (now - _phaseStart) * 1000f;
			_phaseLast = now;
			_phaseLastFrame = frameNow;
			int idx = ResetPhase.IndexOf(phaseKey);
			if (idx >= 0)
			{
				_resetPhaseMs[idx] += deltaMs;
				int frames = _resetPhaseFrames[idx] + deltaFrames;
				_resetPhaseFrames[idx] = (ushort)(frames > ushort.MaxValue ? ushort.MaxValue : frames);
			}
			Log($"[Phase-Timing] Reset#{_resetCount} {label}: +{deltaMs:F0}ms (total {totalMs:F0}ms)");
		}

		protected override IEnumerator Setup()
		{
			Connect();
			yield return WaitForInit();
			socket.UnreadMessages.TryDequeue(out Message message);
			if (message.type != "init")
			{
				Log($"Setup: expected init, got '{message.type}' — retrying");
				yield return Setup();
				yield break;
			}

			// Uncap fps; captureDeltaTime (pinned in Reset) fixes dt.
			QualitySettings.vSyncCount = 0;
			// FK_REALTIME=1 caps rendering at 1/kStepDeltaTime fps so a graphical run plays
			// at real time. The simulation is unchanged.
			if (System.Environment.GetEnvironmentVariable("FK_REALTIME") == "1")
			{
				Application.targetFrameRate = Mathf.RoundToInt(1f / kStepDeltaTime);
				Log($"[Setup] FK_REALTIME=1 — render capped at "
					+ $"{Application.targetFrameRate} fps (real-time playback)");
			}
			else
			{
				Application.targetFrameRate = -1;
			}
			// The preload thread runs at this priority, and the main thread integrates loaded data for up to
			// 2/4/10/50 ms per frame at Low/BelowNormal/Normal/High (UP!0x180773aa0 PreloadManager::UpdatePreloading).
			// Below High, uncapped main threads of concurrent instances starve the loader: a load that takes 5 s
			// alone took minutes with 13 instances. Loads only; the game sets it nowhere else (OpeningSequence.cs:60).
			Application.backgroundLoadingPriority = UnityEngine.ThreadPriority.High;

			// Kill HK's hit-stop. HeroController.StartRecoil runs gm.FreezeMoment, whose
			// ramp through TimeController.GenericTimeScale spans our step boundaries and
			// overwrites our Time.timeScale writes (TimeController.cs:69-80), leaking a
			// fractional timeScale into the next step. All overloads become no-ops.
			_killFreezeFloat = (orig, self, rd, w, ru, ts) => EmptyCoroutine();
			_killFreezeBool = (orig, self, rd, w, ru, gc) => EmptyCoroutine();
			_killFreezeMomentGC = (orig, self, rd, w, ru, ts) => EmptyCoroutine();
			_killFreezeInt = (orig, self, type) => { /* no-op */ };
			On.GameManager.FreezeMoment_float_float_float_float += _killFreezeFloat;
			On.GameManager.FreezeMoment_float_float_float_bool += _killFreezeBool;
			On.GameManager.FreezeMomentGC += _killFreezeMomentGC;
			On.GameManager.FreezeMoment_int += _killFreezeInt;
			Log("[Setup] FreezeMoment killed (hit-stop disabled)");

			// Stop the PlayMaker layer during the inter-step pause (see FsmPauseGate).
			Game.FsmPauseGate.Install();
			Log("[Setup] PlayMakerFSM updates gated on timeScale > 0");

			On.GameManager.SaveGame += SaveFileProxy.DisableSaveGame;
			SaveFileProxy.LoadCompletedSave();
			GameManager.instance.ContinueGame();
			yield return new SceneHooks.WaitForSceneLoad("GG_Workshop");
			yield return new SceneHooks.WaitForEntryFinished();
			yield return new WaitForSeconds(2f);

			_hitboxObserver.Load();
			InputManager.AttachDevice(_inputShim);
			SendMessage(message);
		}

		protected override IEnumerator Dispose()
		{
			UnhookDamage();
			foreach (var kvp in _bossDeathHandlers)
			{
				if (kvp.Key != null) kvp.Key.OnDeath -= kvp.Value;
			}
			_bossDeathHandlers.Clear();
			if (_killFreezeFloat != null) On.GameManager.FreezeMoment_float_float_float_float -= _killFreezeFloat;
			if (_killFreezeBool != null) On.GameManager.FreezeMoment_float_float_float_bool -= _killFreezeBool;
			if (_killFreezeMomentGC != null) On.GameManager.FreezeMomentGC -= _killFreezeMomentGC;
			if (_killFreezeInt != null) On.GameManager.FreezeMoment_int -= _killFreezeInt;
			_timeManager?.Dispose();
			InputManager.DetachDevice(_inputShim);
			_hitboxObserver.Unload();
			CloseSocket();
			yield break;
		}

		private static IEnumerator EmptyCoroutine() { yield break; }

		private void HookDamage()
		{
			ModHooks.AfterTakeDamageHook += OnKnightDamaged;
			On.HealthManager.TakeDamage += OnBossDamaged;
		}

		private void UnhookDamage()
		{
			ModHooks.AfterTakeDamageHook -= OnKnightDamaged;
			On.HealthManager.TakeDamage -= OnBossDamaged;
		}

		// AfterTakeDamageHook runs past HeroController.TakeDamage's iframe check, so it
		// counts only hits that land (TakeDamageHook would count blocked contacts).
		// The return value is the applied damage: pass it through unchanged.
		private int OnKnightDamaged(int damageType, int damage)
		{
			if (damage > 0)
			{
				_hitsTakenInStep += damage;
				_hitEventsInEpisode++;
				_hpLostInEpisode += damage;
			}
			return damage;
		}

		private void OnBossDamaged(On.HealthManager.orig_TakeDamage orig, HealthManager self, HitInstance hitInstance)
		{
			// Only the boss set is credited; everything else passes through.
			if (!_bossMaxHPs.TryGetValue(self, out int maxHP))
			{
				orig(self, hitInstance);
				return;
			}

			// Each of N bosses is worth 100/N percent when killed, whatever its HP pool.
			int n = _bossHMs.Count;
			_damageLandedInStep += hitInstance.DamageDealt / (float)(n * maxHP) * 100f;

			orig(self, hitInstance);
		}

		// Enforce the loaded tier at every episode boundary: at BossLevel 2
		// HeroController.TakeDamage turns every hit into 9999 damage, and scene
		// code can rewrite BossLevel (see also the set_BossLevel hook in HKOracle.cs).
		private void ClampBossLevel()
		{
			try
			{
				var bsc = BossSceneController.Instance;
				// The tier the canonical load selected (SceneHooks.LoadedTier): a `_V` scene
				// only exists as a Tier2Scene, and HK_ORACLE_TIER can request any tier.
				int want = SceneHooks.LoadedTier >= 0 ? SceneHooks.LoadedTier : 0;
				if (bsc != null && bsc.BossLevel != want)
				{
					Log($"[TierClamp] reset#{_resetCount} level={_level} "
						+ $"BossLevel={bsc.BossLevel} -> {want} (loaded tier)");
					bsc.BossLevel = want;
				}
			}
			catch { }
		}

		// Boss-death signals: BossSceneController.OnBossesDead (below) and each bound
		// HM's OnDeath (OnBossActualDeath), which fires only once Die() reaches
		// SendDeathEvent, i.e. never on a phase stagger.
		private BossSceneController _bscSubscribed;
		private void OnBossesDeadSignal()
		{
			if (_bossDied) return;
			_bossDied = true;
			Log($"[BossDied] reset#{_resetCount} step#{_stepCount} via=OnBossesDead (BossSceneController.EndBossScene)");
		}

		private void OnBossActualDeath(HealthManager hm)
		{
			bool allDead = true;
			foreach (var other in _bossHMs)
			{
				if (other == null) continue;
				if (other == hm) continue;
				if (other.hp > 0) { allDead = false; break; }
			}
			if (allDead)
			{
				_bossDied = true;
				Log($"[BossDied] reset#{_resetCount} step#{_stepCount} "
					+ $"hm={(hm != null ? hm.gameObject.name : "null")} via=OnDeath");
			}
		}

		// Wait for HK's own end-of-episode transition (dream return on a win or a
		// death) to leave `fromScene`, so our scene load does not race it. The
		// destination does not matter: LoadBossScene bounces through the Workshop.
		private IEnumerator WaitForSceneChange(string fromScene)
		{
			int timeoutFrames = 5000;
			while (UnityEngine.SceneManagement.SceneManager.GetActiveScene().name == fromScene
				&& --timeoutFrames > 0)
			{
				yield return null;
			}
			if (timeoutFrames <= 0)
			{
				Log($"WaitForSceneChange: TIMEOUT (still in {fromScene})");
			}
		}

		private bool HasActiveCombatHitboxes()
		{
			foreach (var col in _hitboxObserver.Classify().Enemy)
			{
				if (col.isActiveAndEnabled)
					return true;
			}
			return false;
		}

		// Last resort when neither native path names the boss: bind every live
		// HealthManager with hp >= 100 (Hall of Gods bosses are 500+, adds under 100).
		// FindObjectsOfType sees only active objects, so a boss still buried in its
		// entrance is not bound before HealthManager.Start has applied hpScale.
		private void ScanBindBossHMs(string why)
		{
			foreach (var shm in UnityEngine.Object.FindObjectsOfType<HealthManager>())
			{
				if (shm == null || shm.isDead || shm.hp < 100) continue;
				if (_bossHMs.Contains(shm)) continue;
				_bossHMs.Add(shm);
				_bossMaxHPs[shm] = shm.hp;
				var capturedScan = shm;
				HealthManager.DeathEvent scanHandler =
					() => OnBossActualDeath(capturedScan);
				_bossDeathHandlers[shm] = scanHandler;
				shm.OnDeath += scanHandler;
				Log($"[BossBind/scan] reset#{_resetCount} {why} bound "
					+ $"{shm.gameObject.name} hp={shm.hp}");
			}
		}

		private void InitBossRefs()
		{
			// Unsubscribe the previous episode's OnDeath handlers (destroyed HMs are null).
			foreach (var kvp in _bossDeathHandlers)
			{
				if (kvp.Key != null) kvp.Key.OnDeath -= kvp.Value;
			}
			_bossDeathHandlers.Clear();
			_bossHMs.Clear();
			_bossMaxHPs.Clear();
			try
			{
				var bsc = BossSceneController.Instance;
				if (bsc == null) return;
				// The game's own "fight over" signal. HM.OnDeath never fires for a
				// hasSpecialDeath boss (HealthManager.cs:567-571 returns before
				// isDead/OnDeath): NKG ends its fight through the FSM action
				// EndGGBossScene -> BossSceneController.EndBossScene -> OnBossesDead
				// (BossSceneController.cs:215-226), and CheckBossesDead routes every
				// bosses[] death through the same call.
				if (_bscSubscribed != null)
				{
					try { _bscSubscribed.OnBossesDead -= OnBossesDeadSignal; } catch { }
					_bscSubscribed = null;
				}
				bsc.OnBossesDead += OnBossesDeadSignal;
				_bscSubscribed = bsc;
				// Union of the two native registration paths: the serialized `bosses`
				// array, and BossHealthLookup (ReportHealth(forceAdd), and HMs that
				// register only after their entrance cinematic).
				var seen = new HashSet<HealthManager>();
				if (bsc.bosses != null)
				{
					foreach (var b in bsc.bosses)
					{
						if (b != null) seen.Add(b);
					}
				}
				if (bsc.BossHealthLookup != null)
				{
					foreach (var hm in bsc.BossHealthLookup.Keys)
					{
						if (hm != null) seen.Add(hm);
					}
				}
				foreach (var hm in seen)
				{
					_bossHMs.Add(hm);
					_bossMaxHPs[hm] = hm.hp;
					var capturedHm = hm;
					HealthManager.DeathEvent handler = () => OnBossActualDeath(capturedHm);
					_bossDeathHandlers[hm] = handler;
					hm.OnDeath += handler;
				}
			}
			catch { }
		}
	}
}
