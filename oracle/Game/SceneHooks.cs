using System;
using System.Collections;
using GlobalEnums;
using HutongGames.PlayMaker;
using UnityEngine;

namespace HKOracle.Game
{
	public static class SceneHooks
	{
		// Tier clamp for the in-flight canonical load. -1 = passthrough.
		// The completed save unlocks every tier, and the panel's auto-selected
		// button can fire its own LoadBoss in the same frame as ours (last
		// SetupEvent wins; a stray tier-3 submit makes the fight Radiant).
		// HKOracle.Initialize hooks BossChallengeUI.LoadBoss and forces
		// `level` to this value while a canonical load is in flight.
		internal static int ForcedTier = -1;

		// HK_ORACLE_TIER=<0|1|2> selects Attuned/Ascended/Radiant: the statue tier whose scene is
		// the requested one is preferred, so GG_Hornet_1 at tier 1 loads through the Ascended
		// slot (HP scaled by HealthManager.GetScaledHP level2, CheckGGBossLevel branches taken).
		// Unset (-1): first match, i.e. tier 0 for every Tier1 scene and tier 1 for the `_V`
		// scenes that only exist as Tier2Scene.
		internal static readonly int RequestedTier = ParseRequestedTier();
		// The tier the canonical load actually selected (-1 until a load has happened).
		// TrainingEnv.ClampBossLevel pins BossSceneController.BossLevel to this.
		internal static int LoadedTier = -1;
		private static int ParseRequestedTier()
		{
			var s = System.Environment.GetEnvironmentVariable("HK_ORACLE_TIER");
			int v;
			if (!string.IsNullOrEmpty(s) && int.TryParse(s, out v) && v >= 0 && v <= 2) return v;
			return -1;
		}

		/// <summary>
		/// Loads a boss from the Hall of Gods given the scene name.
		///
		/// Canonical path: drive the statue's GG Boss UI FSM the way a player
		/// would. Send CONVO START -> the FSM spawns BossChallengeUI -> we call
		/// LoadBoss(level, false) on it, which fires OnLevelSelected -> FSM's
		/// DREAM event -> Take Control -> Set Facing -> Challenge -> Impact ->
		/// Dream Box Down -> Transition (BeginSceneTransition) -> Reset Player
		/// -> Change Scene. HK does all the state setup (bossSceneToLoad,
		/// RecordBossScene, bossReturnEntryGate PD, SetupEvent with statue-
		/// completion PD writes, GG visualization, HUD restore on return).
		/// </summary>
		public static IEnumerator LoadBossScene(string scene_name)
		{
			// Retry wrapper. The chain has soft-fail exits (panel missing,
			// statue not matched); verify we reached the boss scene and re-run
			// the whole chain if not. Its entry re-orients from any scene, so a
			// retry is safe from wherever the failed round left us.
			for (int round = 0; round < 3; round++)
			{
				yield return LoadBossSceneOnce(scene_name, forceReinit: round > 0);
				ForcedTier = -1;  // clamp only while a canonical load is in flight
				string _active = UnityEngine.SceneManagement.SceneManager
					.GetActiveScene().name;
				if (_active == scene_name) yield break;
				HKOracle.Instance.Log(
					$"[CanonicalLoad] round={round} ended in '{_active}' "
					+ $"(want '{scene_name}'); retrying full chain");
				yield return null;
			}
			HKOracle.Instance.Log(
				$"[CanonicalLoad] FAILED to reach {scene_name} after 3 rounds");
		}

		private static IEnumerator LoadBossSceneOnce(string scene_name,
			bool forceReinit = false)
		{
			BossStatue statue = null;

			void Log(string s)
			{
				var fsmActive = statue?.bossUIControlFSM?.Fsm?.ActiveStateName ?? "(no statue)";
				HKOracle.Instance.Log($"[CanonicalLoad] {s} | scene="
					+ UnityEngine.SceneManagement.SceneManager.GetActiveScene().name
					+ $" fsmState={fsmActive}"
					+ $" t={Time.realtimeSinceStartup:F2} f={Time.frameCount}");
			}

			Log("ENTER target=" + scene_name);

			// Bounce only when not already in the workshop: after a fight we
			// arrive in GG_Workshop via DoDreamReturn, and bouncing on top of
			// dream-return causes a workshop->atrium auto-transition.
			string currentScene = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name;
			if (currentScene != "GG_Workshop")
			{
				Log("BOUNCE-START from=" + currentScene);
				yield return BounceThroughWorkshop();
				Log("BOUNCE-END");
			}

			// Wait for the workshop entry coroutine before CONVO START. After a
			// DoDreamReturn the lie-down/wake-up entry is still in flight, and
			// the FSM's Challenge state stalls while the hero is mid-entry
			// (transitionState=WAITING_TO_ENTER_LEVEL). No-op after a bounce.
			yield return new WaitForEntryFinished(maxSeconds: 120f);
			Log("entry-coroutine settled");

			// First reset only: the save puts the knight on RestBench (1)
			// mid-cutscene, where CONVO START auto-cancels to Close UI. Tap
			// jump until the bench FSM stands the knight up. No-op when
			// pd.atBench is false (every later reset arrives via dream-return).
			yield return LeaveBenchIfSitting();
			Log("bench-leave settled");

			// Drive HK's 'Return from boss' wake-up FSM to its terminal state.
			// Leaving GG_Workshop mid-sequence skips its cleanup (renderer,
			// anim control, input restore) and the next wake-up stalls on
			// dirty HC state. Returns immediately when no wake-up is active.
			yield return DriveDreamReturnWakeUp();
			Log("wake-up FSM settled");

			// Find the statue + tier (0/1/2) + dream-flag that owns scene_name.
			// Search both bossScene (regular) and dreamBossScene, then set the
			// statue's UsingDreamVersion to match: BossChallengeUI.LoadBoss
			// reads it at call time (BossChallengeUI.cs:237), so e.g.
			// GG_Failed_Champion loads the dream variant of the False Knight statue.
			int level = 0;
			bool wantDream = false;
			if (RequestedTier >= 0)
			{
				foreach (var s in UnityEngine.Object.FindObjectsOfType<BossStatue>())
				{
					for (int di = 0; di < 2; di++)
					{
						var bs = di == 0 ? s.bossScene : s.dreamBossScene;
						if (bs == null) continue;
						string want = RequestedTier == 0 ? bs.Tier1Scene : RequestedTier == 1 ? bs.Tier2Scene : bs.Tier3Scene;
						if (want == scene_name) { statue = s; level = RequestedTier; wantDream = di == 1; goto found; }
					}
				}
				Log($"[Tier] HK_ORACLE_TIER={RequestedTier} maps no statue tier to {scene_name}; falling back to first match");
			}
			foreach (var s in UnityEngine.Object.FindObjectsOfType<BossStatue>())
			{
				if (s.bossScene != null)
				{
					var bs = s.bossScene;
					if (bs.Tier1Scene == scene_name) { statue = s; level = 0; wantDream = false; goto found; }
					if (bs.Tier2Scene == scene_name) { statue = s; level = 1; wantDream = false; goto found; }
					if (bs.Tier3Scene == scene_name) { statue = s; level = 2; wantDream = false; goto found; }
				}
				if (s.dreamBossScene != null)
				{
					var bs = s.dreamBossScene;
					if (bs.Tier1Scene == scene_name) { statue = s; level = 0; wantDream = true; goto found; }
					if (bs.Tier2Scene == scene_name) { statue = s; level = 1; wantDream = true; goto found; }
					if (bs.Tier3Scene == scene_name) { statue = s; level = 2; wantDream = true; goto found; }
				}
			}
		found:

			// Flip the statue's dream-version flag if it doesn't match what
			// the caller asked for. UsingDreamVersion has a private set, but
			// its public getter reads StatueState.usingAltVersion and the
			// public StatueState setter writes the whole Completion struct
			// back to PlayerData; we round-trip through that.
			if (statue != null && statue.UsingDreamVersion != wantDream)
			{
				var state = statue.StatueState;
				state.usingAltVersion = wantDream;
				statue.StatueState = state;
				Log($"flipped UsingDreamVersion -> {wantDream}");
			}

			// Post-fight the bossUIControlFSM sits in its terminal Change Scene
			// state with Finished=true, where global transitions (CONVO START)
			// do not fire. Reinitialize()+Start() returns it to Inert. Also
			// reinitialize on a retry round or when parked at 'Open UI' (a
			// dropped DREAM event), where CONVO START is a no-op.
			string uiState = statue?.bossUIControlFSM?.Fsm?.ActiveStateName ?? "";
			bool uiParkedMidChain = (uiState == "Open UI");
			if (statue != null
				&& (statue.bossUIControlFSM?.Fsm?.Finished == true
					|| uiParkedMidChain || forceReinit))
			{
				if (uiParkedMidChain || forceReinit)
				{
					HKOracle.Instance.Log(
						$"[CanonicalLoad] reinit: state='{uiState}' "
						+ $"forceReinit={forceReinit}");
				}
				statue.bossUIControlFSM.Fsm.Reinitialize();
				statue.bossUIControlFSM.Fsm.Start();
				yield return null;
			}
			if (statue == null)
			{
				Log("ERROR: no statue matches " + scene_name);
				yield break;
			}
			Log($"FOUND statue={statue.gameObject.name} level={level} (HK_ORACLE_TIER={RequestedTier})");
			ForcedTier = level;
			LoadedTier = level;
			LogHero("pre-CONVO");

			// Topology dump of the boss UI control FSM, for reading a stall
			// against the state graph.
			DumpWakeFsm(statue.bossUIControlFSM, "bossUI");

			// Log every state change of the boss UI FSM, to locate a stall.
			GameManager.instance.StartCoroutine(LogFsmTransitions(statue.bossUIControlFSM, "bossUI", 30f));
			// Heartbeat HC state every second while the bossUI FSM runs.
			GameManager.instance.StartCoroutine(LogHcWhileFsmRuns(statue.bossUIControlFSM, "bossUI", 30f));

			// Suspend HC animation control so Challenge can play 'Challenge
			// Start': with a fully woken knight HeroAnimationController writes
			// Idle every frame, Tk2dWatchAnimationEvents never sees FINISHED,
			// and bossUI stalls at Challenge. The arena's dream-warp entry
			// (Dream Return -> Regain Control) re-enables it.
			HeroController.instance?.StopAnimationControl();
			Log("StopAnimationControl called on HC");

			if (statue.hasNoTiers)
			{
				// hasNoTiers statues (only GG_Statue_Zote/"Mighty Zote Boss
				// Scene" in the roster) have no tier-select panel:
				// BossChallengeUI.Setup calls LoadBoss(0, doHideAnim:false)
				// synchronously and that Hide()s (deactivates) the panel
				// before Setup even returns (BossChallengeUI.cs:118,133-136).
				// The tiered path below is built for a panel that stays
				// active waiting on a button press: it searches for the
				// panel after CONVO START and, when missing, calls
				// Fsm.Reinitialize()+Start() on bossUIControlFSM -- the SAME
				// FSM instance that DREAM (fired inside Setup, since
				// ShowBossChallengeUI subscribes OnLevelSelected before
				// calling Setup: ShowBossChallengeUI.cs:54-61) has already
				// driven into Take Control/Challenge/Transition. Reinit mid-
				// transition resets that FSM to Inert and drops the already-
				// armed scene load, landing back in GG_Workshop -- the
				// observed misfire. Fix: for hasNoTiers, send CONVO START
				// once and let the native auto-load run untouched, exactly
				// as it does for a real player who never sees a tier panel.
				string preSendState = statue.bossUIControlFSM.Fsm?.ActiveStateName ?? "(null)";
				HKOracle.Instance.Log($"[CanonicalLoad] hasNoTiers statue={statue.gameObject.name}; single CONVO START, no panel wait (state={preSendState})");
				try
				{
					statue.bossUIControlFSM.SendEvent("CONVO START");
				}
				catch (System.Exception e)
				{
					HKOracle.Instance.Log($"[CanonicalLoad] hasNoTiers SendEvent threw: {HKOracle.DescribeException(e)}");
				}
				string postSendState = statue.bossUIControlFSM.Fsm?.ActiveStateName ?? "(null)";
				Log($"hasNoTiers post-SendEvent state={preSendState}->{postSendState}");
			}
			else
			{
				// CONVO START is the FSM's global transition into Open UI. Open UI
				// runs Tk2dPlayAnimation + ShowBossChallengeUI + ActivateGameObject
				// synchronously. ShowBossChallengeUI instantiates the panel and
				// hooks panel.OnCancel -> Fsm.Event("FINISHED") and
				// panel.OnLevelSelected -> Fsm.Event("DREAM").
				//
				// Race: LoadBoss(level, false) nulls OnCancel before Hide and then
				// fires OnLevelSelected, so the FSM reaches Take Control only if
				// our LoadBoss runs before anything invokes OnCancel. The panel's
				// spawn coroutine (Select / StartUIInput) can auto-cancel within
				// 1-2 frames, so find the panel in the same frame (it is
				// instantiated synchronously), yield at most once, and call
				// LoadBoss immediately. SendEvent can throw from the Open UI /
				// Close UI actions; catch it, Reinitialize and re-send.
				BossChallengeUI panel = null;
				for (int attempt = 0; attempt < 3 && panel == null; attempt++)
				{
					string preSendState = statue.bossUIControlFSM.Fsm?.ActiveStateName ?? "(null)";
					HKOracle.Instance.Log($"[CanonicalLoad] pre-SendEvent CONVO START attempt={attempt} state={preSendState}");
					System.Exception sendEx = null;
					try
					{
						statue.bossUIControlFSM.SendEvent("CONVO START");
					}
					catch (System.Exception e)
					{
						sendEx = e;
					}
					string postSendState = statue.bossUIControlFSM.Fsm?.ActiveStateName ?? "(null)";
					bool postFinished = statue.bossUIControlFSM.Fsm?.Finished ?? false;
					if (sendEx != null)
					{
						HKOracle.Instance.Log($"[CanonicalLoad] SendEvent attempt={attempt} threw: {HKOracle.DescribeException(sendEx)}");
					}
					HKOracle.Instance.Log($"[CanonicalLoad] post-SendEvent attempt={attempt} state={postSendState} finished={postFinished}");

					panel = UnityEngine.Object.FindObjectOfType<BossChallengeUI>();
					if (panel == null)
					{
						HKOracle.Instance.Log($"[CanonicalLoad] attempt={attempt} panel not same-frame; yielding once and retrying find");
						yield return null;
						panel = UnityEngine.Object.FindObjectOfType<BossChallengeUI>();
					}
					if (panel != null) break;

					// Panel never spawned (bossUI collapsed Inert -> Open UI ->
					// Close UI synchronously). Reinitialize and retry.
					HKOracle.Instance.Log($"[CanonicalLoad] attempt={attempt} retry path: panel missing, fsm at {postSendState} finished={postFinished}; reinitializing");
					try
					{
						statue.bossUIControlFSM.Fsm.Reinitialize();
						statue.bossUIControlFSM.Fsm.Start();
					}
					catch (System.Exception e)
					{
						HKOracle.Instance.Log($"[CanonicalLoad] reinit threw {e.GetType().Name}: {e.Message}");
					}
					yield return null;
				}
				if (panel == null)
				{
					Log("ERROR: BossChallengeUI panel not found after 3 CONVO START attempts");
					HKOracle.Instance.Log($"[CanonicalLoad] panel-missing final-diag "
						+ $"fsmState={statue.bossUIControlFSM.Fsm?.ActiveStateName} "
						+ $"finished={statue.bossUIControlFSM.Fsm?.Finished}");
					yield break;
				}
				Log($"FOUND panel={panel.gameObject.name}");

				// LoadBoss is the canonical "press the button" function. It sets
				// bossSceneToLoad, calls RecordBossScene, swaps bossReturnEntryGate,
				// installs the full SetupEvent delegate (including OnBossesDead PD
				// writes for statue completion + OnBossSceneComplete -> DoDreamReturn),
				// then fires OnLevelSelected. doHideAnim:false skips the hide animation.
				// No yield between FindObjectOfType and LoadBoss (auto-cancel race).
				// The panel is a process-wide singleton reused by every statue, so
				// panel.bossStatue is whichever statue last ran Setup on it; if
				// that isn't ours, LoadBoss arms the wrong boss scene. Verify by
				// reflection and rebind before pressing.
				try
				{
					var bsField = typeof(BossChallengeUI).GetField("bossStatue",
						System.Reflection.BindingFlags.NonPublic
						| System.Reflection.BindingFlags.Instance);
					var panelOwner = bsField?.GetValue(panel) as BossStatue;
					if (panelOwner != statue)
					{
						HKOracle.Instance.Log(
							"[CanonicalLoad] panel owner MISMATCH: "
							+ $"{(panelOwner != null ? panelOwner.gameObject.name : "(null)")}"
							+ $" != {statue.gameObject.name}; rebinding via Setup");
						panel.Setup(statue, "Titles", "", "Titles", "");
					}
				}
				catch (System.Exception ownEx)
				{
					HKOracle.Instance.Log(
						$"[CanonicalLoad] owner-verify failed: {ownEx.Message}");
				}
				string preLoadBossState = statue.bossUIControlFSM.Fsm?.ActiveStateName ?? "(null)";
				HKOracle.Instance.Log($"[CanonicalLoad] pre-LoadBoss state={preLoadBossState}");
				panel.LoadBoss(level, doHideAnim: false);
				string postLoadBossState = statue.bossUIControlFSM.Fsm?.ActiveStateName ?? "(null)";
				Log($"after LoadBoss return (fsmState={postLoadBossState})");
				LogHero("post-LoadBoss");
			}

			// The FSM now drives knight animation (Take Control -> Challenge ->
			// Impact -> Dream Box Down) then BeginSceneTransition in Transition
			// state. Total ~2-3s of in-game animation. Poll for active scene.
			yield return new WaitForSceneLoad(scene_name, maxSeconds: 120f);
			Log("SCENE LOADED");
			yield return new WaitForEntryFinished(maxSeconds: 120f);
			Log("DONE");
		}

		// One log line per FSM state change, until Finished or timeoutSec.
		private static IEnumerator LogFsmTransitions(PlayMakerFSM pmFsm, string tag, float timeoutSec)
		{
			if (pmFsm == null || pmFsm.Fsm == null) yield break;
			float t0 = Time.realtimeSinceStartup;
			string last = "<init>";
			bool lastFinished = false;
			while (Time.realtimeSinceStartup - t0 < timeoutSec)
			{
				var fsm = pmFsm.Fsm;
				if (fsm == null) yield break;
				string cur = fsm.ActiveStateName ?? "(null)";
				bool fin = fsm.Finished;
				if (cur != last || fin != lastFinished)
				{
					HKOracle.Instance.Log(
						$"[CanonicalLoad/{tag}] state '{last}' -> '{cur}' (finished {lastFinished}->{fin})"
						+ $" t={Time.realtimeSinceStartup:F2}");
					last = cur;
					lastFinished = fin;
					if (fin) yield break;
				}
				yield return null;
			}
			HKOracle.Instance.Log($"[CanonicalLoad/{tag}] timeout after {timeoutSec}s, last state='{last}' finished={lastFinished}");
		}

		// HeroController state at a checkpoint (lie-down / no-input / cutscene
		// mode is what stalls the canonical chain).
		private static void LogHero(string tag)
		{
			var hc = HeroController.instance;
			if (hc == null) { HKOracle.Instance.Log($"[CanonicalLoad/hero/{tag}] HeroController.instance=null"); return; }
			string transitionState = "?";
			try { transitionState = hc.transitionState.ToString(); } catch { }
			bool accepting = false;
			try { accepting = hc.acceptingInput; } catch { }
			bool active = hc.gameObject.activeInHierarchy;
			Vector3 pos = hc.transform.position;
			string rendererInfo = "?";
			try {
				var r = hc.GetComponentInChildren<Renderer>(includeInactive: true);
				if (r == null) rendererInfo = "noRenderer";
				else
				{
					string chain = $"{r.gameObject.name}.enabled={r.enabled} selfActive={r.gameObject.activeSelf}";
					var t = r.transform.parent;
					while (t != null && t != hc.transform.parent)
					{
						chain = $"{t.gameObject.name}.selfActive={t.gameObject.activeSelf} > " + chain;
						t = t.parent;
					}
					rendererInfo = chain;
				}
			} catch (System.Exception e) { rendererInfo = $"err:{e.Message}"; }
			var cs = hc.cState;
			string csInfo = "?";
			if (cs != null)
			{
				csInfo = $"facingRight={cs.facingRight} onGround={cs.onGround} dead={cs.dead}"
					+ $" hazardDeath={cs.hazardDeath} transitioning={cs.transitioning}"
					+ $" recoiling={cs.recoiling} invulnerable={cs.invulnerable}";
			}
			string animState = "?";
			try {
				var animCtrl = hc.GetComponentInChildren<tk2dSpriteAnimator>();
				if (animCtrl != null && animCtrl.CurrentClip != null)
					animState = animCtrl.CurrentClip.name;
			} catch { }
			HKOracle.Instance.Log(
				$"[CanonicalLoad/hero/{tag}] hcActive={active} renderer=[{rendererInfo}]"
				+ $" pos=({pos.x:F1},{pos.y:F1}) anim={animState}"
				+ $" acceptingInput={accepting} transitionState={transitionState} {csInfo}");
		}

		private static IEnumerator BounceThroughWorkshop()
		{
			var GM = GameManager.instance;
			// BeginSceneTransitionRoutine silently drops a transition while
			// one is in flight (GameManager.cs: "sceneLoad != null -> yield
			// break"). A native transition (a dream return queued by
			// EndSceneDelayed) can still be finishing here; wait it out.
			int guardFrames = 0;
			while (GM.IsInSceneTransition && guardFrames < 3000)
			{
				guardFrames++;
				yield return null;
			}
			if (guardFrames > 0)
			{
				HKOracle.Instance.Log(
					$"[Bounce] waited {guardFrames} frames for in-flight native "
					+ $"scene transition (still in transition: {GM.IsInSceneTransition})");
			}
			float t0 = Time.realtimeSinceStartup;
			GM.BeginSceneTransition(new GameManager.SceneLoadInfo
			{
				SceneName = "GG_Workshop",
				EntryGateName = "door_dreamReturn",
				EntryDelay = 0,
				Visualization = GameManager.SceneLoadVisualizations.GodsAndGlory,
				PreventCameraFadeOut = true,
				WaitForSceneTransitionCameraFade = false,
				// As WarpToDreamGate does: GG->GG hops never satisfy
				// IsUnloadAssetsRequired on their own, so without this the
				// process never unloads assets and native memory only grows.
				AlwaysUnloadUnusedAssets = true,
			});
			float t1 = Time.realtimeSinceStartup;
			yield return new WaitForSceneLoad("GG_Workshop", maxSeconds: 120f);
			float t2 = Time.realtimeSinceStartup;
			yield return new WaitForEntryFinished(maxSeconds: 120f);
			float t3 = Time.realtimeSinceStartup;
			HKOracle.Instance.Log(
				$"[Phase-Timing] BounceThroughWorkshop: BeginTransition={(t1 - t0) * 1000f:F0}ms"
				+ $" WaitForSceneLoad={(t2 - t1) * 1000f:F0}ms"
				+ $" WaitForFinishedEnteringScene={(t3 - t2) * 1000f:F0}ms"
				+ $" total={(t3 - t0) * 1000f:F0}ms");
		}

		// First-reset entry: save_file.json sets respawnMarkerName="RestBench (1)"
		// and atBench=true, so the knight loads sitting on the godseeker bench,
		// where CONVO START auto-cancels. Leave the bench as a player would: tap
		// Jump+Attack (3 on / 3 off, so InControl sees edges) until atBench is
		// false and HC accepts input. Frame budget, not wall clock: the
		// stand-up runs on pinned game time.
		private static IEnumerator LeaveBenchIfSitting(int frameBudget = 4000)
		{
			var pd = GameManager.instance?.playerData;
			var hc = HeroController.instance;
			if (pd == null || hc == null)
			{
				HKOracle.Instance.Log("[LeaveBench] no PD/HC; skipping");
				yield break;
			}
			bool startedOnBench = false;
			try { startedOnBench = pd.atBench; } catch { }
			if (!startedOnBench)
			{
				HKOracle.Instance.Log("[LeaveBench] pd.atBench=false; nothing to leave");
				yield break;
			}
			var shim = InputDeviceShim.Attached;
			if (shim == null)
			{
				HKOracle.Instance.Log("[LeaveBench] no shim attached; cannot tap");
				yield break;
			}

			HKOracle.Instance.Log("[LeaveBench] tapping Jump+Attack until knight stands");
			float t0 = Time.realtimeSinceStartup;
			int frame = 0;
			bool keysDown = false;
			int lastSec = -1;

			while (frame < frameBudget)
			{
				bool stillBench = true;
				bool accepting = false;
				bool onGround = false;
				try { stillBench = pd.atBench; } catch { }
				try { accepting = hc.acceptingInput; } catch { }
				try { onGround = hc.cState != null && hc.cState.onGround; } catch { }

				// Standing = atBench cleared, input back, grounded.
				if (!stillBench && accepting && onGround) break;

				int sec = (int)(Time.realtimeSinceStartup - t0);
				if (sec != lastSec)
				{
					lastSec = sec;
					string anim = "?";
					try
					{
						var ac = hc.GetComponentInChildren<tk2dSpriteAnimator>();
						if (ac != null && ac.CurrentClip != null) anim = ac.CurrentClip.name;
					}
					catch { }
					Vector3 p = hc.transform.position;
					HKOracle.Instance.Log(
						$"[LeaveBench/sec={sec}] atBench={stillBench} accepting={accepting}"
						+ $" onGround={onGround} anim={anim} pos=({p.x:F1},{p.y:F1})");
				}

				bool wantDown = (frame % 6) < 3;
				if (wantDown != keysDown)
				{
					shim.WakeTap(wantDown);
					keysDown = wantDown;
				}
				frame++;
				yield return null;
			}
			shim.WakeTap(false);

			bool finalBench = true;
			bool finalAccepting = false;
			try { finalBench = pd.atBench; } catch { }
			try { finalAccepting = hc.acceptingInput; } catch { }
			HKOracle.Instance.Log(
				$"[LeaveBench] exit atBench={finalBench} accepting={finalAccepting}"
				+ $" elapsed={(Time.realtimeSinceStartup - t0) * 1000f:F0}ms");
		}

		// Two FSMs run in parallel after a death-return to GG_Workshop:
		//   1. 'Return from boss' on door_dreamReturn_* GameObject —
		//      Pause -> Door Entry -> Wait -> Transition Wait ->
		//      Transition In -> (DREAM WAKE) -> Get Up Wait -> Gotten Up.
		//      Terminal states (Gotten Up / Not Returning) have no
		//      outgoing transitions; FSM parks there without setting
		//      Finished=true.
		//   2. 'Dream Return' on HC — Idle -> ... -> Ready -> (GET UP) ->
		//      Get Up -> Save -> Regain Control -> Idle. HC is DDOL so
		//      its FSM state persists across scenes.
		// The door's DREAM WAKE event is fired by HC.Dream Return's
		// 'Get Up' state (SendEventByName), so HC has to reach Get Up
		// for the door to advance. And HC needs input — Ready listens
		// via ListenForJump/Attack/Down/Up/L/R and fires GET UP.
		//
		// Exit only once HC.Dream Return is back at Idle: exiting mid-Get-Up
		// skips Save / Regain Control and parks the DDOL hero FSM at Get Up;
		// LEVEL LOADED only restarts it from Idle, so the next wake-up
		// deadlocks. Frame budget, not wall clock: the sequence runs on pinned
		// game time, so a wall fuse would trip whenever the process is slow.
		// 4000 frames is >10x the sequence length.
		private static IEnumerator DriveDreamReturnWakeUp(int frameBudget = 4000)
		{
			var shim = InputDeviceShim.Attached;

			// Inventory all 'Return from boss' FSMs; only the one for the boss
			// just fought should be active, the rest sit at Pause.
			int total = 0;
			PlayMakerFSM driving = null;
			string drivingDoor = null;
			var inventoryStates = new System.Collections.Generic.List<string>();
			foreach (var fsm in UnityEngine.Object.FindObjectsOfType<PlayMakerFSM>())
			{
				if (fsm == null || fsm.Fsm == null) continue;
				if (fsm.FsmName != "Return from boss") continue;
				total++;
				string st = fsm.Fsm.ActiveStateName ?? "(null)";
				inventoryStates.Add($"{fsm.gameObject.name}:{st}");
				if (string.IsNullOrEmpty(st)) continue;
				if (st == "Pause" || st == "Not Returning") continue;
				if (driving == null)
				{
					driving = fsm;
					drivingDoor = fsm.gameObject.name;
				}
			}
			HKOracle.Instance.Log(
				$"[WakeUp] inventory total={total} active={(driving == null ? "(none)" : drivingDoor)}");
			// Log only non-Pause door states.
			foreach (var s in inventoryStates)
			{
				if (s.EndsWith(":Pause") || s.EndsWith(":Not Returning")) continue;
				HKOracle.Instance.Log($"[WakeUp/inv-nonpause] {s}");
			}

			// The 'Dream Return' FSM on HC. HC is DDOL, so its variables
			// (Dream Returning, read by HC.IsDreamReturning) persist across
			// scenes and can stall the door FSM if left mid-sequence.
			PlayMakerFSM heroDreamReturnFsm = null;
			var hc = HeroController.instance;
			if (hc != null)
			{
				heroDreamReturnFsm = PlayMakerFSM.FindFsmOnGameObject(hc.gameObject, "Dream Return");
				if (heroDreamReturnFsm != null)
				{
					string ds = heroDreamReturnFsm.Fsm?.ActiveStateName ?? "(null)";
					bool isRet = false;
					try { isRet = hc.IsDreamReturning; } catch { }
					HKOracle.Instance.Log(
						$"[WakeUp/heroFsm] 'Dream Return' on HC state={ds} IsDreamReturning={isRet}");
				}
				else
				{
					HKOracle.Instance.Log("[WakeUp/heroFsm] 'Dream Return' FSM not found on HC");
				}
			}

			if (driving == null)
			{
				HKOracle.Instance.Log("[WakeUp] no active 'Return from boss' FSM; nothing to wake");
				// The DDOL hero FSM can still be parked mid-sequence (LEVEL
				// LOADED only restarts it from Idle). Realign before returning.
				string parkedState = heroDreamReturnFsm?.Fsm?.ActiveStateName ?? "(none)";
				if (heroDreamReturnFsm != null && parkedState != "Idle")
				{
					ForceHeroDreamReturnIdle(heroDreamReturnFsm, "no-door entry");
				}
				yield break;
			}

			HKOracle.Instance.Log(
				$"[WakeUp] driving FSM on {drivingDoor} initialState={driving.Fsm.ActiveStateName}");

			// Topology dump (states, transitions, action types) for the log.
			DumpWakeFsm(driving, drivingDoor);
			if (heroDreamReturnFsm != null)
			{
				DumpWakeFsm(heroDreamReturnFsm, "HC.Dream Return");
			}

			// Log every wake FSM state change, also after this loop exits.
			GameManager.instance.StartCoroutine(
				LogFsmTransitions(driving, "wakeFsm", 30f));
			if (heroDreamReturnFsm != null)
			{
				GameManager.instance.StartCoroutine(
					LogFsmTransitions(heroDreamReturnFsm, "wakeHcFsm", 30f));
			}

			float t0 = Time.realtimeSinceStartup;
			int frame = 0;
			bool keysDown = false;
			int lastHcLogSecond = -1;

			while (frame < frameBudget)
			{
				var fsm = driving.Fsm;
				if (fsm == null) break;
				string state = fsm.ActiveStateName ?? "(null)";
				bool finished = fsm.Finished;

				string heroState = heroDreamReturnFsm?.Fsm?.ActiveStateName ?? "(none)";

				// Exit only when BOTH FSMs are quiescent:
				//   door: Gotten Up / Not Returning (terminal — no outgoing
				//         transitions) or Pause / Inert (pre-active).
				//   HC.Dream Return: Idle (the start state / true terminal).
				// If the hero FSM isn't present at all, fall back to just
				// the door condition.
				bool doorTerminal = (state == "Gotten Up" || state == "Not Returning"
					|| state == "Pause" || state == "Inert");
				bool heroTerminal = (heroDreamReturnFsm == null
					|| heroState == "Idle");
				if (finished) break;
				if (doorTerminal && heroTerminal) break;

				int sec = (int)(Time.realtimeSinceStartup - t0);
				if (sec != lastHcLogSecond)
				{
					lastHcLogSecond = sec;
					LogHcDuringWake(state, finished, sec, heroState);
				}

				// Tap only in Ready / Ready 2, the states whose ListenFor*
				// actions turn input into GET UP; elsewhere a press is ignored
				// or triggers a real hero action.
				bool needInput = (heroState == "Ready" || heroState == "Ready 2");
				bool wantDown = needInput && ((frame % 6) < 3);
				if (wantDown != keysDown && shim != null)
				{
					shim.WakeTap(wantDown);
					keysDown = wantDown;
				}
				frame++;
				yield return null;
			}

			if (shim != null) shim.WakeTap(false);

			string finalDoorState = driving?.Fsm?.ActiveStateName ?? "(null)";
			bool finalFinished = driving?.Fsm?.Finished ?? false;
			string finalHeroState = heroDreamReturnFsm?.Fsm?.ActiveStateName ?? "(none)";
			HKOracle.Instance.Log(
				$"[WakeUp] exit doorState={finalDoorState} finished={finalFinished}"
				+ $" heroFsmState={finalHeroState} frames={frame}"
				+ $" elapsed={(Time.realtimeSinceStartup - t0) * 1000f:F0}ms");

			// Post-condition: HC.'Dream Return' at Idle with control regained,
			// as natively after every wake. If the budget ran out, force it
			// rather than hand a half-woken DDOL hero to the next scene load.
			if (heroDreamReturnFsm != null && finalHeroState != "Idle")
			{
				ForceHeroDreamReturnIdle(heroDreamReturnFsm, "budget-exhausted exit");
			}
		}

		// Drive HC.'Dream Return' to Idle and redo what the skipped tail
		// states (Save / Regain Control) do: clear Dream Returning and give
		// back input and animation control, the native post-wake state.
		private static void ForceHeroDreamReturnIdle(PlayMakerFSM heroFsm, string context)
		{
			string st = heroFsm?.Fsm?.ActiveStateName ?? "(none)";
			HKOracle.Instance.Log(
				$"[WakeUp/recover] {context}: HC.'Dream Return' parked at '{st}'"
				+ " -> forcing Idle + control restore (native post-wake state)");
			try { heroFsm.SetState("Idle"); }
			catch (System.Exception e)
			{
				HKOracle.Instance.Log($"[WakeUp/recover] SetState failed: {e.Message}");
			}
			try
			{
				var b = heroFsm.FsmVariables?.FindFsmBool("Dream Returning");
				if (b != null) b.Value = false;
			}
			catch { }
			var hc = HeroController.instance;
			if (hc != null)
			{
				try { hc.RegainControl(); } catch { }
				try { hc.StartAnimationControl(); } catch { }
				try { hc.AcceptInput(); } catch { }
			}
		}

		// FSM topology dump: per state, its transitions and action type names
		// (which tell what a stalled state waits on).
		private static void DumpWakeFsm(PlayMakerFSM pmFsm, string tag)
		{
			var fsm = pmFsm?.Fsm;
			if (fsm == null) return;
			try
			{
				HKOracle.Instance.Log(
					$"[WakeUpFsm/{tag}] startState={fsm.StartState} active={fsm.ActiveStateName}"
					+ $" finished={fsm.Finished} stateCount={fsm.States?.Length}");

				var evs = new System.Text.StringBuilder();
				evs.Append($"[WakeUpFsm/{tag}/events]");
				if (fsm.Events != null)
				{
					for (int i = 0; i < fsm.Events.Length; i++)
						evs.Append(" ").Append(fsm.Events[i].Name);
				}
				HKOracle.Instance.Log(evs.ToString());

				var gt = new System.Text.StringBuilder();
				gt.Append($"[WakeUpFsm/{tag}/globalTrans]");
				if (fsm.GlobalTransitions != null)
				{
					for (int i = 0; i < fsm.GlobalTransitions.Length; i++)
					{
						var t = fsm.GlobalTransitions[i];
						gt.Append($" {t.EventName}->{t.ToState}");
					}
				}
				HKOracle.Instance.Log(gt.ToString());

				if (fsm.States != null)
				{
					foreach (var state in fsm.States)
					{
						if (state == null) continue;
						var sb = new System.Text.StringBuilder();
						sb.Append($"[WakeUpFsm/{tag}/state] {state.Name} trans=[");
						if (state.Transitions != null)
						{
							for (int i = 0; i < state.Transitions.Length; i++)
							{
								if (i > 0) sb.Append(", ");
								var tr = state.Transitions[i];
								sb.Append($"{tr.EventName}->{tr.ToState}");
							}
						}
						sb.Append("] actions=[");
						var acts = state.Actions;
						if (acts != null)
						{
							for (int i = 0; i < acts.Length; i++)
							{
								if (i > 0) sb.Append(", ");
								sb.Append(acts[i]?.GetType().Name ?? "null");
							}
						}
						sb.Append("]");
						HKOracle.Instance.Log(sb.ToString());
					}
				}
			}
			catch (System.Exception e)
			{
				HKOracle.Instance.Log($"[WakeUpFsm/{tag}] dump err: {e.Message}");
			}
		}

		// HC heartbeat (~1s) while the FSM runs: animator, position, input,
		// transition state.
		private static IEnumerator LogHcWhileFsmRuns(PlayMakerFSM pmFsm, string tag, float timeoutSec)
		{
			if (pmFsm == null) yield break;
			float t0 = Time.realtimeSinceStartup;
			int lastSec = -1;
			while (Time.realtimeSinceStartup - t0 < timeoutSec)
			{
				var fsm = pmFsm.Fsm;
				if (fsm == null || fsm.Finished) yield break;
				int sec = (int)(Time.realtimeSinceStartup - t0);
				if (sec != lastSec)
				{
					lastSec = sec;
					var hc = HeroController.instance;
					if (hc != null)
					{
						string anim = "?";
						try
						{
							var animCtrl = hc.GetComponentInChildren<tk2dSpriteAnimator>();
							if (animCtrl != null && animCtrl.CurrentClip != null)
								anim = animCtrl.CurrentClip.name;
						}
						catch { }
						bool accepting = false;
						try { accepting = hc.acceptingInput; } catch { }
						string ts = "?";
						try { ts = hc.transitionState.ToString(); } catch { }
						bool onGround = false; bool facingRight = false;
						try { onGround = hc.cState != null && hc.cState.onGround; } catch { }
						try { facingRight = hc.cState != null && hc.cState.facingRight; } catch { }
						Vector3 p = hc.transform.position;
						HKOracle.Instance.Log(
							$"[{tag}/hc-sec={sec}] fsmState={fsm.ActiveStateName}"
							+ $" pos=({p.x:F1},{p.y:F1}) anim={anim} accepting={accepting}"
							+ $" onGround={onGround} facingRight={facingRight} transitionState={ts}");
					}
				}
				yield return null;
			}
		}

		// Per-second HC snapshot during the wake loop.
		private static void LogHcDuringWake(string doorState, bool doorFinished, int sec, string heroFsmState)
		{
			var hc = HeroController.instance;
			if (hc == null)
			{
				HKOracle.Instance.Log($"[WakeUp/hc-tick={sec}] HC=null doorState={doorState}");
				return;
			}
			bool rendererEnabled = false;
			try
			{
				var r = hc.GetComponentInChildren<Renderer>(includeInactive: true);
				if (r != null) rendererEnabled = r.enabled;
			}
			catch { }
			string anim = "?";
			try
			{
				var animCtrl = hc.GetComponentInChildren<tk2dSpriteAnimator>();
				if (animCtrl != null && animCtrl.CurrentClip != null)
					anim = animCtrl.CurrentClip.name;
			}
			catch { }
			bool accepting = false;
			try { accepting = hc.acceptingInput; } catch { }
			string ts = "?";
			try { ts = hc.transitionState.ToString(); } catch { }
			bool isDreamRet = false;
			try { isDreamRet = hc.IsDreamReturning; } catch { }
			HKOracle.Instance.Log(
				$"[WakeUp/hc-tick={sec}] doorState={doorState} doorFinished={doorFinished}"
				+ $" heroFsmState={heroFsmState}"
				+ $" renderer={rendererEnabled} anim={anim} accepting={accepting}"
				+ $" transitionState={ts} IsDreamReturning={isDreamRet}");
		}

		private static void DumpStatueFSM(BossStatue statue)
		{
			if (statue == null || statue.bossUIControlFSM == null)
			{
				HKOracle.Instance.Log("[StatueFSM] no statue or no bossUIControlFSM");
				return;
			}
			var fsm = statue.bossUIControlFSM.Fsm;
			var sb = new System.Text.StringBuilder();
			sb.Append($"[StatueFSM] gameObject={statue.gameObject.name} fsmName={fsm.Name} active={fsm.ActiveStateName}");
			sb.Append(" | events=[");
			for (int i = 0; i < fsm.Events.Length; i++)
			{
				if (i > 0) sb.Append(", ");
				sb.Append(fsm.Events[i].Name);
			}
			sb.Append("] | globalTransitions=[");
			for (int i = 0; i < fsm.GlobalTransitions.Length; i++)
			{
				if (i > 0) sb.Append(", ");
				var t = fsm.GlobalTransitions[i];
				sb.Append($"{t.EventName}->{t.ToState}");
			}
			sb.Append("]");
			HKOracle.Instance.Log(sb.ToString());

			foreach (var state in fsm.States)
			{
				var ts = new System.Text.StringBuilder();
				ts.Append($"[StatueFSM]   state={state.Name} transitions=[");
				for (int i = 0; i < state.Transitions.Length; i++)
				{
					if (i > 0) ts.Append(", ");
					var tr = state.Transitions[i];
					ts.Append($"{tr.EventName}->{tr.ToState}");
				}
				ts.Append("]");
				HKOracle.Instance.Log(ts.ToString());
			}
		}

		// The game's WaitForFinishedEnteringScene is a PlayMaker FsmStateAction,
		// not a CustomYieldInstruction (yielding it waits one frame). This polls.
		public class WaitForEntryFinished : CustomYieldInstruction
		{
			private int _polls;
			private float _t0;
			// 0 = wait forever (first-boot Setup). Non-zero = give up after
			// that many wall-clock seconds so a caller with a recovery path (the
			// LoadBossScene retry wrapper) gets control back. Not frames: the
			// load it waits for is asynchronous and fps is uncapped, so a frame
			// budget ran out in ~8 s on a loaded machine.
			private float _maxSeconds;

			public WaitForEntryFinished(float maxSeconds = 0f)
			{
				_t0 = Time.realtimeSinceStartup;
				_maxSeconds = maxSeconds;
			}

			public override bool keepWaiting
			{
				get
				{
					var gm = GameManager.instance;
					var hc = HeroController.instance;
					if (gm == null || hc == null) return false;
					// Two gates. HasFinishedEnteringScene is set right after
					// StartCoroutine(Respawn) on the dream-return path
					// (GameManager.cs:1614-1616), while the hero is still mid-
					// entry; the hero is idle once transitionState ==
					// WAITING_TO_TRANSITION and !cState.transitioning.
					bool gmReady = gm.HasFinishedEnteringScene;
					bool hcReady = false;
					try { hcReady = hc.transitionState == HeroTransitionState.WAITING_TO_TRANSITION
						&& hc.cState != null && !hc.cState.transitioning; }
					catch { }
					if (gmReady && hcReady) return false;
					_polls++;
					if (_maxSeconds > 0f && Time.realtimeSinceStartup - _t0 > _maxSeconds)
					{
						HKOracle.Instance.Log(
							$"[WaitForEntryFinished] GAVE UP after {_polls} frames, {_maxSeconds} s"
							+ $" gmReady={gmReady} hcReady={hcReady}");
						return false;
					}
					if (_polls == 1 || _polls % 60 == 0)
					{
						string ts = "?"; bool transitioning = false; bool accepting = false;
						try { ts = hc.transitionState.ToString(); } catch { }
						try { transitioning = hc.cState != null && hc.cState.transitioning; } catch { }
						try { accepting = hc.acceptingInput; } catch { }
						Vector3 p = hc.transform.position;
						HKOracle.Instance.Log(
							$"[WaitForEntryFinished] polls={_polls} elapsed={(Time.realtimeSinceStartup - _t0) * 1000f:F0}ms"
							+ $" gmReady={gmReady} hcReady={hcReady}"
							+ $" gmState={gm.gameState} hcTransitionState={ts} transitioning={transitioning}"
							+ $" acceptingInput={accepting} pos=({p.x:F1},{p.y:F1})");
					}
					return true;
				}
			}
		}

		public class WaitForSceneLoad : CustomYieldInstruction, IDisposable
		{
			private string sceneName;
			// Heartbeat every 60 polls (one poll per frame), so a stuck
			// transition shows in the log.
			private int _polls;
			private float _t0;

			// 0 = wait forever (first-boot Setup); non-zero = give up after
			// that many wall-clock seconds so callers with a recovery path
			// regain control (seconds, not frames: see WaitForEntryFinished).
			private float _maxSeconds;

			public WaitForSceneLoad(string sn, float maxSeconds = 0f)
			{
				// keepWaiting polls; no activeSceneChanged subscription (a static
				// event handler here would leak one delegate per load).
				sceneName = sn;
				_t0 = Time.realtimeSinceStartup;
				_maxSeconds = maxSeconds;
			}

			public override bool keepWaiting
			{
				get
				{
					string active = UnityEngine.SceneManagement.SceneManager.GetActiveScene().name;
					var gm = GameManager.instance;
					// Both gates: the scene name flips at ActivationComplete,
					// but gm.sceneLoad only nulls in the later Finish callback,
					// and a BeginSceneTransition in that window is rejected.
					// IsInSceneTransition clears inside Finish.
					bool sceneReady = (active == sceneName);
					bool transitionDone = (gm == null || !gm.IsInSceneTransition);
					if (sceneReady && transitionDone) return false;
					_polls++;
					if (_maxSeconds > 0f && Time.realtimeSinceStartup - _t0 > _maxSeconds)
					{
						HKOracle.Instance.Log(
							$"[WaitForSceneLoad] GAVE UP after {_polls} frames, {_maxSeconds} s"
							+ $" target={sceneName} active={active}");
						return false;
					}
					if (_polls % 60 == 0)
					{
						string state = "?";
						try { state = gm != null ? gm.gameState.ToString() : "?"; }
						catch { }
						HKOracle.Instance.Log(
							$"[WaitForSceneLoad] target={sceneName} active={active} "
							+ $"state={state} timeScale={Time.timeScale:F2} "
							+ $"inTransition={(gm != null && gm.IsInSceneTransition)} "
							+ $"polls={_polls} elapsed={(Time.realtimeSinceStartup - _t0) * 1000f:F0}ms");
					}
					return true;
				}
			}

			public void Dispose()
			{
				// Nothing to release; kept for the IDisposable signature.
			}
		}
	}
}
