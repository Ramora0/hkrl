using System.Collections.Generic;
using Modding;
using InControl;

namespace HKOracle.Game
{
	public class InputDeviceShim : InputDevice
	{
		private bool KeyUp = false;
		private bool KeyDown = false;
		private bool KeyLeft = false;
		private bool KeyRight = false;
		private bool KeyJump = false;
		private bool KeyAttack = false;
		private bool KeyDash = false;
		// KeyCast drives quickCast (RightBumper), which only ever casts a spell; the
		// focus action holds KeyFocus, which drives HeroActions.cast (Action2,
		// ControllerMapping.cs:16), the path that heals.
		private bool KeyCast = false;
		private bool KeyFocus = false;
		private bool KeyDreamNail = false;
		private bool KeySuperDash = false;

		// Raw key state as a bitset; bit i <=> KeyNames[i]. Recorded per frame
		// (docs/trace-format.md, FRAME.input).
		public static readonly string[] KeyNames = {
			"left", "right", "up", "down", "jump", "attack", "dash", "cast", "dream_nail", "super_dash", "focus" };
		public uint KeyBits() =>
			(KeyLeft ? 1u : 0) | (KeyRight ? 2u : 0) | (KeyUp ? 4u : 0) | (KeyDown ? 8u : 0)
			| (KeyJump ? 16u : 0) | (KeyAttack ? 32u : 0) | (KeyDash ? 64u : 0) | (KeyCast ? 128u : 0)
			| (KeyDreamNail ? 256u : 0) | (KeySuperDash ? 512u : 0) | (KeyFocus ? 1024u : 0);

		// When true, force one tick of key=false before pressing again (tap actions)
		private bool _retapAttack = false;
		private bool _retapCast = false;

		// Hard-commit state for hold actions. When the agent freely chooses a
		// hold (nail_charge / focus / dream_nail / super_dash), action[2] is
		// locked to it for a fixed number of env-steps, then forced to none for
		// one step: the release is what fires the move. Movement / direction /
		// jump stay free (HK itself restricts movement during heal/charge).
		// Durations: ActionDecoder.HoldGameSeconds.
		public enum CommitState : byte { Idle = 0, Locked = 1, Releasing = 2 }
		public CommitState CState = CommitState.Idle;
		public int LockedAction = -1;     // 0..7: which hold is locked
		public int LockedStepsLeft = 0;   // steps remaining in Locked phase
		// Total locked steps for the hold in flight: the denominator of the
		// commit-progress term in the observation.
		public int LockedStepsTotal = 0;

		public void ResetCommit()
		{
			CState = CommitState.Idle;
			LockedAction = -1;
			LockedStepsLeft = 0;
			LockedStepsTotal = 0;
		}

		// The process's single shim (set on construction), for SceneHooks.
		public static InputDeviceShim Attached { get; private set; }

		public InputDeviceShim() :
			base("HKOracleInputShimDevice")
		{
			AddControl(InputControlType.DPadUp, "Up");
			AddControl(InputControlType.DPadDown, "Down");
			AddControl(InputControlType.DPadLeft, "Left");
			AddControl(InputControlType.DPadRight, "Right");
			AddControl(InputControlType.Action1, "Jump");
			AddControl(InputControlType.Action2, "Cast");
			AddControl(InputControlType.Action3, "Attack");
			AddControl(InputControlType.Action4, "DreamNail");
			AddControl(InputControlType.RightTrigger, "Dash");
			AddControl(InputControlType.LeftTrigger, "SuperDash");
			AddControl(InputControlType.RightBumper, "QuickCast");
			Attached = this;
		}

		// Raw press/release of Jump + Attack, bypassing Can* gates, for the
		// SceneHooks wake-up and bench-leave coroutines. HC's 'Dream Return'
		// FSM in 'Ready'/'Ready 2' fires GET UP on any ListenFor* input; Cast /
		// Dash / DreamNail would trigger real actions once HC accepts input.
		public void WakeTap(bool pressed)
		{
			KeyJump = pressed;
			KeyAttack = pressed;
		}

		public override void Update(ulong updateTick, float deltaTime)
		{
			// Retap: force one tick of false to create a fresh press transition
			bool effectiveAttack = KeyAttack;
			bool effectiveCast = KeyCast;
			if (_retapAttack) { effectiveAttack = false; _retapAttack = false; }
			if (_retapCast) { effectiveCast = false; _retapCast = false; }

			UpdateWithState(InputControlType.DPadUp, KeyUp, updateTick, deltaTime);
			UpdateWithState(InputControlType.DPadDown, KeyDown, updateTick, deltaTime);
			UpdateWithState(InputControlType.DPadLeft, KeyLeft, updateTick, deltaTime);
			UpdateWithState(InputControlType.DPadRight, KeyRight, updateTick, deltaTime);
			UpdateWithState(InputControlType.Action1, KeyJump, updateTick, deltaTime);
			UpdateWithState(InputControlType.RightBumper, effectiveCast, updateTick, deltaTime);
			UpdateWithState(InputControlType.Action2, KeyFocus, updateTick, deltaTime);
			UpdateWithState(InputControlType.Action3, effectiveAttack, updateTick, deltaTime);
			UpdateWithValue(InputControlType.RightTrigger, KeyDash ? 1 : 0, updateTick, deltaTime);
			UpdateWithState(InputControlType.Action4, KeyDreamNail, updateTick, deltaTime);
			UpdateWithValue(InputControlType.LeftTrigger, KeySuperDash ? 1 : 0, updateTick, deltaTime);
		}

		private static bool CanDash() =>
			ReflectionHelper.CallMethod<HeroController, bool>(HeroController.instance, "CanDash");

		private static bool CanAttack() =>
			ReflectionHelper.CallMethod<HeroController, bool>(HeroController.instance, "CanAttack");

		private static bool CanJump() =>
			ReflectionHelper.CallMethod<HeroController, bool>(HeroController.instance, "CanJump");

		private static bool CanDoubleJump() =>
			ReflectionHelper.CallMethod<HeroController, bool>(HeroController.instance, "CanDoubleJump");

		private static bool CanCast() =>
			ReflectionHelper.CallMethod<HeroController, bool>(HeroController.instance, "CanCast");

		private static bool CanWallJump() =>
			ReflectionHelper.CallMethod<HeroController, bool>(HeroController.instance, "CanWallJump");

		private static bool CanNailCharge() =>
			ReflectionHelper.CallMethod<HeroController, bool>(HeroController.instance, "CanNailCharge");

		private static bool CanDreamNail() =>
			HeroController.instance.CanDreamNail();

		private static bool CanSuperDash() =>
			HeroController.instance.CanSuperDash();

		public void Reset()
		{
			KeyUp = false;
			KeyDown = false;
			KeyLeft = false;
			KeyRight = false;
			KeyJump = false;
			KeyAttack = false;
			KeyDash = false;
			KeyCast = false;
			KeyFocus = false;
			KeyDreamNail = false;
			KeySuperDash = false;
			_retapAttack = false;
			_retapCast = false;
		}

		public void Left() { KeyLeft = true; KeyRight = false; }
		public void Right() { KeyRight = true; KeyLeft = false; }
		public void Up() { KeyUp = true; KeyDown = false; }
		public void Down() { KeyDown = true; KeyUp = false; }

		public void Jump()
		{
			if (!CanJump() && !CanDoubleJump() && !CanWallJump()) return;
			KeyJump = true;
			KeyDash = false;
		}

		private void FaceDirection()
		{
			if (KeyLeft) HeroController.instance.FaceLeft();
			else if (KeyRight) HeroController.instance.FaceRight();
		}

		/// <summary>Tap attack: release-then-press to guarantee a fresh swing.</summary>
		public void AttackTap()
		{
			if (!CanAttack()) return;
			FaceDirection();
			_retapAttack = KeyAttack; // force release tick only if already held
			KeyAttack = true;
			KeyCast = false;
			KeyFocus = false;
			KeyDreamNail = false;
			KeySuperDash = false;
		}

		/// <summary>Hold attack: keep KeyAttack held for nail art charge.</summary>
		public void NailCharge()
		{
			// Already holding — continue the charge regardless of CanNailCharge
			if (KeyAttack) return;
			if (!CanNailCharge()) return;
			FaceDirection();
			KeyAttack = true;
			KeyCast = false;
			KeyFocus = false;
			KeyDreamNail = false;
			KeySuperDash = false;
		}

		/// <summary>Tap cast: release-then-press for spell.</summary>
		public void SpellTap()
		{
			if (!CanCast()) return;
			FaceDirection();
			_retapCast = KeyCast;
			KeyCast = true;
			KeyAttack = false;
			KeyDreamNail = false;
			KeySuperDash = false;
		}

		/// <summary>Focus/heal: hold KeyFocus (the focus key, input bit 10), not KeyCast.</summary>
		public void Focus()
		{
			if (!KeyFocus && !CanCast()) return;
			FaceDirection();
			KeyFocus = true;
			KeyCast = false;
			KeyAttack = false;
			KeyDreamNail = false;
			KeySuperDash = false;
		}

		public void Dash()
		{
			if (!CanDash()) return;
			FaceDirection();
			KeyDash = true;
			KeyJump = false;
			KeyAttack = false;
			KeyCast = false;
			KeyFocus = false;
			KeyDreamNail = false;
			KeySuperDash = false;
		}

		/// <summary>Hold dream nail.</summary>
		public void DreamNail()
		{
			if (!KeyDreamNail && !CanDreamNail()) return;
			KeyDreamNail = true;
			KeyAttack = false;
			KeyCast = false;
			KeyFocus = false;
			KeySuperDash = false;
		}

		/// <summary>Hold super dash (crystal heart).</summary>
		public void SuperDash()
		{
			if (!KeySuperDash && !CanSuperDash()) return;
			KeySuperDash = true;
			KeyAttack = false;
			KeyCast = false;
			KeyFocus = false;
			KeyDreamNail = false;
		}

		public void StopLR() { KeyLeft = false; KeyRight = false; }
		public void StopUD() { KeyUp = false; KeyDown = false; }
		public void StopJD() { KeyJump = false; KeyDash = false; }
		public void StopActions()
		{
			KeyAttack = false;
			KeyCast = false;
			KeyFocus = false;
			KeyDash = false;
			KeyDreamNail = false;
			KeySuperDash = false;
			_retapAttack = false;
			_retapCast = false;
		}
	}

	public static class ActionDecoder
	{
		// Hard-commit hold durations in game-time seconds, converted to env-steps
		// at apply time (LockedStepsFor). Each entry counts only the locked
		// phase; one release step (action[2]=none) follows to fire the move.
		// The Knight FSMs' charge times plus a few frames (nail art, focus for
		// one mask, dream nail, crystal heart). hksim: sim/hero/hero_input.c.
		private static readonly Dictionary<int, float> HoldGameSeconds = new()
		{
			{ 1, 1.71f },  // nail_charge
			{ 3, 1.51f },  // focus
			{ 5, 1.09f },  // dream_nail
			{ 6, 0.91f },  // super_dash
		};

		private static int LockedStepsFor(int actionIdx, int framesPerWait)
		{
			if (!HoldGameSeconds.TryGetValue(actionIdx, out float gs)) return 0;
			// Steps = ceil(seconds / (framesPerWait * per-frame seconds)), the
			// per-frame seconds being the capture dt TrainingEnv pins.
			float kCaptureDeltaTime = Env.TrainingEnv.kStepDeltaTime;
			float stepGameSeconds = framesPerWait * kCaptureDeltaTime;
			if (stepGameSeconds <= 0f) return 0;
			int n = (int)System.Math.Ceiling(gs / stepGameSeconds);
			return n > 0 ? n : 1;
		}

		/// <summary>
		/// Decode factored action vector into InputDeviceShim calls.
		/// action[0] movement:  0=left, 1=right, 2=none
		/// action[1] direction: 0=up, 1=down, 2=none
		/// action[2] action:    0=attack(tap), 1=charge(hold), 2=spell(tap),
		///                      3=focus(hold), 4=dash, 5=dream_nail(hold),
		///                      6=super_dash(hold), 7=none
		/// action[3] jump:      0=yes, 1=no
		///
		/// Hard commit: when the agent freely picks a hold action, action[2] is
		/// locked to that hold for LockedStepsFor() env-steps, then forced to
		/// none for one release step. The action[] array is mutated in place to
		/// reflect what was actually applied. Returns true iff action[2] was
		/// overridden this step (i.e. the agent didn't make a free choice on
		/// the action head this step).
		///
		/// Apply order: movement -> direction -> jump -> action
		/// so that dash overrides jump when both are requested.
		/// </summary>
		public static bool ApplyAction(InputDeviceShim shim, int[] action,
			int framesPerWait = 1)
		{
			bool committed = false;

			// Resolve action[2] against the commit state machine BEFORE the
			// shim methods are called, so the rest of this function sees the
			// actually-applied value.
			if (shim.CState == InputDeviceShim.CommitState.Releasing)
			{
				action[2] = 7;  // none — triggers StopActions and fires the held move
				committed = true;
				shim.CState = InputDeviceShim.CommitState.Idle;
				shim.LockedAction = -1;
				shim.LockedStepsLeft = 0;
				shim.LockedStepsTotal = 0;
			}
			else if (shim.CState == InputDeviceShim.CommitState.Locked)
			{
				action[2] = shim.LockedAction;
				committed = true;
				shim.LockedStepsLeft--;
				if (shim.LockedStepsLeft <= 0)
				{
					shim.CState = InputDeviceShim.CommitState.Releasing;
				}
			}
			else if (HoldGameSeconds.ContainsKey(action[2]))
			{
				// Idle + free hold pick: lock starting next step.
				int totalLocked = LockedStepsFor(action[2], framesPerWait);
				shim.LockedAction = action[2];
				// This step counts toward the locked phase (the agent picked
				// it freely, KeyAttack/etc gets set true now). Subsequent
				// (totalLocked - 1) steps stay locked, then one release step.
				shim.LockedStepsLeft = totalLocked - 1;
				shim.LockedStepsTotal = totalLocked;
				shim.CState = (shim.LockedStepsLeft > 0)
					? InputDeviceShim.CommitState.Locked
					: InputDeviceShim.CommitState.Releasing;
				// committed stays false — this step's action[2] is the agent's free choice.
			}

			// Movement
			switch (action[0])
			{
				case 0: shim.Left(); break;
				case 1: shim.Right(); break;
				default: shim.StopLR(); break;
			}

			// Direction
			switch (action[1])
			{
				case 0: shim.Up(); break;
				case 1: shim.Down(); break;
				default: shim.StopUD(); break;
			}

			// Jump (applied before action so dash can override)
			switch (action[3])
			{
				case 0: shim.Jump(); break;
				default: break;
			}

			// Action
			switch (action[2])
			{
				case 0: shim.AttackTap(); break;
				case 1: shim.NailCharge(); break;
				case 2: shim.SpellTap(); break;
				case 3: shim.Focus(); break;
				case 4: shim.Dash(); break;
				case 5: shim.DreamNail(); break;
				case 6: shim.SuperDash(); break;
				default:
					shim.StopActions();
					// Only stop jump/dash if no action and no jump requested
					if (action[3] != 0) shim.StopJD();
					break;
			}

			return committed;
		}
	}
}
