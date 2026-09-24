using System;
using System.Reflection;
using MonoMod.RuntimeDetour;
using UnityEngine;

namespace HKOracle.Game
{
	// Freezes the PlayMaker layer whenever the world is frozen.
	//
	// The env pauses the game between agent steps with Time.timeScale = 0
	// (capture-mode deltaTime = 0): physics and tk2d animation stop, but
	// MonoBehaviour.Update still runs every rendered frame, so FSM event logic
	// (NextFrameEvent, collision raycast re-checks, Fsm.Event dispatch) would
	// advance against a frozen physics world. That breaks any FSM shaped "set
	// velocity -> next frame -> re-check collision" (e.g. Gruz Mother's Big Fly
	// Control livelocks at the ceiling). Vanilla never runs FSMs against frozen
	// physics; gating FSM updates on timeScale > 0 restores that invariant.
	public static class FsmPauseGate
	{
		private static Hook _updateHook;
		private static Hook _lateUpdateHook;
		private static Hook _fixedUpdateHook;

		public static void Install()
		{
			if (_updateHook != null) return;
			// The per-frame driver.
			_updateHook = HookIfPresent(typeof(PlayMakerFSM), "Update");
			// LateUpdate/FixedUpdate are NOT methods on PlayMakerFSM — PlayMaker
			// dispatches them through separate proxy behaviours (added in
			// Preprocess/Init for FSMs whose actions request them, e.g.
			// Translate/SetPosition/Rotate with lateUpdate=true). Their handlers
			// run actions AND commit state transitions, so they must be gated
			// too or LateUpdate-driven FSMs keep ticking during the pause.
			var asm = typeof(PlayMakerFSM).Assembly;
			_lateUpdateHook = HookIfPresent(asm.GetType("PlayMakerLateUpdate"), "LateUpdate");
			_fixedUpdateHook = HookIfPresent(asm.GetType("PlayMakerFixedUpdate"), "FixedUpdate");
		}

		private static Hook HookIfPresent(Type type, string methodName)
		{
			if (type == null) return null;
			MethodInfo m = type.GetMethod(
				methodName,
				BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Instance);
			if (m == null) return null;
			return new Hook(m, new Action<Action<MonoBehaviour>, MonoBehaviour>(
				(orig, self) =>
				{
					// <= rather than ==: a fractional/negative leak must not
					// silently reopen the gate.
					if (Time.timeScale <= 0f) return;
					orig(self);
				}));
		}
	}
}
