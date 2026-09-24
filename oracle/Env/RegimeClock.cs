using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using MonoMod.Cil;
using MonoMod.RuntimeDetour;
using UnityEngine;
using CilOpCodes = Mono.Cecil.Cil.OpCodes;

namespace HKOracle.Env
{
	// Regime R2: where the game times something on the wall clock inside an episode, it runs on a frame clock
	// instead, so two runs of one script agree frame for frame. Unscaled time is wall clock even in capture mode
	// (analysis/native_specs/native-transform_time.md T3), and audio plays in real time.
	//
	// The frame clock is armed when the requested boss scene loads (sceneLoaded, where HK_ORACLE_SEED is applied)
	// and disarmed when the next reset begins. The Workshop and the transitions keep the wall clock: they wait in
	// real time for asynchronous loading (GameManager.cs:2816 WaitForSecondsRealtime(0.8), SceneLoad.cs:166-183),
	// and on frames those waits would end before the load they wait for. Armed, a frame lasts dt seconds, frozen
	// frames included (every agent step runs exactly one: WebsocketEnv).
	//  - FsmTime.RealtimeSinceStartup (PlayMaker FsmTime.cs, = Time.realtimeSinceStartup), read by every action
	//    with realTime set (Wait, EaseColor, ...): Now, which continues from the wall time at arming so a fade in
	//    flight at arming goes on.
	//  - WaitForSecondsRealtime.keepWaiting (UnityCsReference Runtime/Export/Scripting/WaitForSecondsRealtime.cs),
	//    the AudioManager cues (AudioManager.cs:64): counted in frames from its first poll. A wait begun before
	//    arming ends at arming.
	//  - AudioSource.isPlaying, which the audio thread clears in real time: every call in the game's assemblies
	//    is IL-rewritten to IsPlaying. It decides PlayAudioAndRecycle.Update's recycle (PlayAudioAndRecycle.cs:15),
	//    AudioPlay's finishedEvent / FINISHED (AudioPlay.cs:74-77), AutoRecycleSelf's AUDIO_CLIP_END recycle
	//    (AutoRecycleSelf.cs:55), SpawnableAudioSource's recycle (SpawnableAudioSource.cs:25) and the audio cues.
	//    A clip started by Play plays (clip.length - time) / |pitch| seconds, a PlayOneShot clip.length / |pitch|
	//    (a loop never ends; Stop ends both); a source that plays on awake starts at the OnEnable of its
	//    AutoRecycleSelf (AUDIO_CLIP_END) or SpawnableAudioSource; a disabled source is silent; a sound started
	//    before arming has ended at arming.
	//  - ParticleSystem.useAutoRandomSeed (a fresh seed on every Play) sets particle lifetimes and so IsAlive,
	//    which ParticleSystemAutoRecycle.Update recycles on (ParticleSystemAutoRecycle.cs:24): every particle
	//    system gets a fixed seed.
	// Install failures fail the regime closed (RegimeTweaks.Fail).
	public static class RegimeClock
	{
		private static double _dt;
		private static bool _armed;
		private static float _t0;
		private static int _f0;
		private static int _epoch;
		private static string _level;

		private static readonly List<Hook> _hooks = new List<Hook>();
		private static readonly List<ILHook> _ilHooks = new List<ILHook>();
		private static FieldInfo _waitUntil;

		// Per source, the frame each voice stops: main (Play) and one-shots (PlayOneShot), frame units so the
		// comparison does not depend on the wall time at arming. epoch: the arming they were started in (-1 before
		// any), so a sound left over from the Workshop or the transition has ended once the episode is armed.
		private sealed class Voices { public int epoch = -1; public double main = double.NegativeInfinity; public double shots = double.NegativeInfinity; }
		private static readonly ConditionalWeakTable<AudioSource, Voices> _voices = new ConditionalWeakTable<AudioSource, Voices>();
		private sealed class Started { public int frame; public int epoch; }
		private static readonly ConditionalWeakTable<WaitForSecondsRealtime, Started> _waits = new ConditionalWeakTable<WaitForSecondsRealtime, Started>();
		private static int _untracked;

		private static void Log(string m) => HKOracle.Instance.Log("[RegimeClock] " + m);

		public static float Now => _armed ? _t0 + (float)((Time.frameCount - _f0) * _dt) : Time.realtimeSinceStartup;

		public static void Install(float captureDt)
		{
			_dt = captureDt;
			Hooks.ResetBegin += level => { _armed = false; if (level != null) _level = level; };
			UnityEngine.SceneManagement.SceneManager.sceneLoaded += (sc, mode) =>
			{
				if (_armed || sc.name != _level) return;
				_t0 = Time.realtimeSinceStartup; _f0 = Time.frameCount; _armed = true; _epoch++;
			};
			Hooks.SceneReady += ctx => LogSceneReady();

			var rt = typeof(HutongGames.PlayMaker.FsmTime).GetProperty("RealtimeSinceStartup").GetGetMethod();
			_hooks.Add(new Hook(rt, new Func<Func<float>, float>(orig => _armed ? Now : orig())));

			_waitUntil = typeof(WaitForSecondsRealtime).GetField("m_WaitUntilTime", BindingFlags.Instance | BindingFlags.NonPublic);
			if (_waitUntil == null) RegimeTweaks.Fail("WaitForSecondsRealtime.m_WaitUntilTime not found");
			else
			{
				var keep = typeof(WaitForSecondsRealtime).GetProperty("keepWaiting").GetGetMethod();
				_hooks.Add(new Hook(keep, new Func<Func<WaitForSecondsRealtime, bool>, WaitForSecondsRealtime, bool>(KeepWaiting)));
			}

			var src = typeof(AudioSource);
			_hooks.Add(new Hook(src.GetMethod("Play", Type.EmptyTypes),
				new Action<Action<AudioSource>, AudioSource>((orig, self) => { orig(self); Played(self); })));
			_hooks.Add(new Hook(src.GetMethod("Play", new[] { typeof(ulong) }),
				new Action<Action<AudioSource, ulong>, AudioSource, ulong>((orig, self, d) => { orig(self, d); Played(self); })));
			_hooks.Add(new Hook(src.GetMethod("PlayOneShot", new[] { typeof(AudioClip), typeof(float) }),
				new Action<Action<AudioSource, AudioClip, float>, AudioSource, AudioClip, float>((orig, self, c, v) => { orig(self, c, v); OneShot(self, c); })));
			_hooks.Add(new Hook(src.GetMethod("PlayOneShot", new[] { typeof(AudioClip) }),
				new Action<Action<AudioSource, AudioClip>, AudioSource, AudioClip>((orig, self, c) => { orig(self, c); OneShot(self, c); })));
			_hooks.Add(new Hook(src.GetMethod("Stop", Type.EmptyTypes),
				new Action<Action<AudioSource>, AudioSource>((orig, self) => { orig(self); Silence(self); })));
			// The two components that recycle a pooled sound on isPlaying; a source that plays on awake starts
			// with the activation that enables them.
			On.AutoRecycleSelf.OnEnable += (orig, self) =>
			{
				orig(self);
				if (self.afterEvent == GlobalEnums.AfterEvent.AUDIO_CLIP_END) PlayedOnAwake(self.GetComponent<AudioSource>());
			};
			On.SpawnableAudioSource.OnEnable += (orig, self) => { orig(self); PlayedOnAwake(self.GetComponent<AudioSource>()); };
			PinIsPlaying();

			Hooks.SceneReady += ctx => FixParticleSeeds();
			UnityEngine.SceneManagement.SceneManager.sceneLoaded += (sc, mode) => FixParticleSeeds();
			Log($"installed (dt={_dt}, {_hooks.Count} detours, {_ilHooks.Count} isPlaying sites)");
		}

		// Every call of AudioSource.get_isPlaying in the game's assemblies becomes a call of IsPlaying: same stack,
		// the source in, a bool out.
		private static void PinIsPlaying()
		{
			var get = typeof(AudioSource).GetProperty("isPlaying").GetGetMethod();
			var repl = typeof(RegimeClock).GetMethod(nameof(IsPlaying), BindingFlags.Public | BindingFlags.Static);
			var targets = RegimeTweaks.ScanCallers(mr => mr.Name == "get_isPlaying" && mr.DeclaringType.FullName == "UnityEngine.AudioSource");
			if (targets.Count == 0) RegimeTweaks.Fail("the IL scan found no AudioSource.get_isPlaying call");
			int n = 0;
			foreach (var kv in targets)
			{
				var mb = kv.Key;
				int inMethod = 0;
				try
				{
					_ilHooks.Add(new ILHook(mb, il =>
					{
						var c = new ILCursor(il);
						while (c.TryGotoNext(MoveType.Before, i => i.MatchCallOrCallvirt(get)))
						{
							c.Next.OpCode = CilOpCodes.Call;
							c.Next.Operand = il.Import(repl);
							c.Index++;
							inMethod++;
						}
					}));
				}
				catch (Exception e) { RegimeTweaks.Fail("isPlaying pin on " + kv.Value + ": " + HKOracle.DescribeException(e)); }
				if (inMethod == 0) RegimeTweaks.Fail("isPlaying pin rewrote no call in " + kv.Value);
				n += inMethod;
			}
			Log($"AudioSource.isPlaying on the frame clock at {n} call sites in {targets.Count} methods");
			GC.Collect();
		}

		/// <summary>AudioSource.isPlaying on the frame clock while armed (see the class comment).</summary>
		public static bool IsPlaying(AudioSource s)
		{
			if (!_armed || (object)s == null) return s.isPlaying;
			bool tracked = _voices.TryGetValue(s, out var v);
			if (!s.isActiveAndEnabled)
			{
				if (tracked) { v.main = v.shots = double.NegativeInfinity; }
				return false;
			}
			if (!tracked)
			{
				if (_untracked++ < 10) Log($"isPlaying of a source never started through Play/PlayOneShot ({Path(s)}): wall clock");
				return s.isPlaying;
			}
			if (v.epoch != _epoch) return false;
			return Time.frameCount < Math.Max(v.main, v.shots);
		}

		// WaitForSecondsRealtime.keepWaiting, counted in frames while armed.
		private static bool KeepWaiting(Func<WaitForSecondsRealtime, bool> orig, WaitForSecondsRealtime self)
		{
			if (!_armed)
			{
				_waits.Remove(self);
				return orig(self);
			}
			if (!_waits.TryGetValue(self, out var st) || st.epoch != _epoch)
			{
				// Begun on the wall clock before arming (orig set m_WaitUntilTime): it has ended.
				if ((float)_waitUntil.GetValue(self) >= 0f) { _waitUntil.SetValue(self, -1f); _waits.Remove(self); return false; }
				_waits.Remove(self);
				st = new Started { frame = Time.frameCount, epoch = _epoch };
				_waits.Add(self, st);
			}
			bool wait = (Time.frameCount - st.frame) * _dt < self.waitTime;
			if (!wait) _waits.Remove(self);
			return wait;
		}

		private static Voices VoicesOf(AudioSource s)
		{
			var v = _voices.GetValue(s, _ => new Voices());
			int ep = _armed ? _epoch : -1;
			if (v.epoch != ep) { v.epoch = ep; v.main = v.shots = double.NegativeInfinity; }
			return v;
		}

		private static double EndFrame(AudioSource s, AudioClip c, float from)
		{
			double p = Math.Abs(s.pitch);
			return p > 0 ? Time.frameCount + (c.length - from) / p / _dt : double.PositiveInfinity;
		}

		private static void Played(AudioSource s)
		{
			var c = s.clip;
			if (c == null || !s.isActiveAndEnabled) return;
			VoicesOf(s).main = s.loop ? double.PositiveInfinity : EndFrame(s, c, Mathf.Clamp(s.time, 0f, c.length));
		}

		private static void PlayedOnAwake(AudioSource s)
		{
			if (s != null && s.playOnAwake) Played(s);
		}

		private static void OneShot(AudioSource s, AudioClip c)
		{
			if (c == null || !s.isActiveAndEnabled) return;
			var v = VoicesOf(s);
			v.shots = Math.Max(v.shots, EndFrame(s, c, 0f));
		}

		private static void Silence(AudioSource s)
		{
			if (_voices.TryGetValue(s, out var v)) v.main = v.shots = double.NegativeInfinity;
		}

		private static void FixParticleSeeds()
		{
			int n = 0;
			try
			{
				foreach (var ps in Resources.FindObjectsOfTypeAll<ParticleSystem>())
				{
					if (ps == null || !ps.useAutoRandomSeed) continue;
					bool playing = ps.isPlaying;
					if (playing) ps.Stop(false, ParticleSystemStopBehavior.StopEmittingAndClear);
					ps.useAutoRandomSeed = false;
					ps.randomSeed = 1;
					if (playing) ps.Play(false);
					n++;
				}
			}
			catch (Exception e) { Log("particle seeds failed: " + HKOracle.DescribeException(e)); }
			if (n > 0) Log($"fixed seed on {n} particle systems");
		}

		// The HUD title card is the one live-set FSM whose timer runs through the load (the boss FSM switches it
		// on before SceneReady, e.g. GG_Nosk Mimic Spider `Roar Init`): its Wait.timer at SceneReady is the
		// initial condition hksim restores from the dump (sim/fsm/actions/control.c wait_enter).
		private static void LogSceneReady()
		{
			string title = "absent";
			try
			{
				var at = AreaTitle.instance;
				var fsm = at != null ? FSMUtility.LocateFSM(at.gameObject, "Area Title Control") : null;
				if (fsm != null)
				{
					title = (at.gameObject.activeInHierarchy ? "" : "inactive ") + fsm.ActiveStateName;
					var st = fsm.Fsm.ActiveState;
					if (st != null)
						foreach (var a in st.Actions)
							if (a is HutongGames.PlayMaker.Actions.Wait w)
								title += " Wait.timer=" + ((float)typeof(HutongGames.PlayMaker.Actions.Wait)
									.GetField("timer", BindingFlags.Instance | BindingFlags.NonPublic).GetValue(w)).ToString("R");
				}
			}
			catch (Exception e) { title = "error " + e.GetType().Name; }
			Log($"SceneReady {(_armed ? (Time.frameCount - _f0).ToString() : "unarmed")} frames after arming; Area Title Control: {title}");
		}

		private static string Path(Component c)
		{
			var t = c.transform;
			string p = t.name;
			while ((t = t.parent) != null) p = t.name + "/" + p;
			return p;
		}
	}
}
