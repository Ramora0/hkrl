using System;
using System.Collections;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using Modding;
using Mono.Cecil.Cil;
using MonoMod.Cil;
using MonoMod.RuntimeDetour;
using UnityEngine;

namespace HKOracle.Game
{
	public class TimeScale
	{
		private float timeScale;

		public TimeScale(float TimeScale = 1f)
		{
			this.timeScale = TimeScale;
			Time.timeScale = timeScale;

			On.GameManager.SetTimeScale_float += GameManager_SetTimeScale_Shim;

			_coroutineHooks = new ILHook[FreezeCoroutines.Length];

			foreach ((MethodInfo coro, int idx) in FreezeCoroutines.Select((mi, idx) => (mi, idx)))
			{
				_coroutineHooks[idx] = new ILHook(coro, ScaleFreeze);
			}
		}

		public void Dispose()
		{
			foreach (ILHook hook in _coroutineHooks)
				hook.Dispose();

			Time.timeScale = 1f;

			On.GameManager.SetTimeScale_float -= GameManager_SetTimeScale_Shim;
		}

		private readonly MethodInfo[] FreezeCoroutines = (
			from method in typeof(GameManager).GetMethods()
			where method.Name.StartsWith("FreezeMoment")
			where method.ReturnType == typeof(IEnumerator)
			select method.GetCustomAttribute<IteratorStateMachineAttribute>() into attr
			select attr.StateMachineType into type
			select type.GetMethod("MoveNext", BindingFlags.NonPublic | BindingFlags.Instance)
		).ToArray();

		private ILHook[] _coroutineHooks;

		private void ScaleFreeze(ILContext il)
		{
			// The one Time.unscaledDeltaTime read of each FreezeMoment loop (GameManager.cs:2891/2903/2916).
			// Matched alone: WallClockCounter inserts its Hit call right before it.
			var cursor = new ILCursor(il);

			cursor.GotoNext
			(
				MoveType.After,
				x => x.MatchCall<Time>("get_unscaledDeltaTime")
			);

			cursor.EmitDelegate<Func<float>>(() => this.timeScale);
			cursor.Emit(OpCodes.Mul);
		}

		private void GameManager_SetTimeScale_Shim(On.GameManager.orig_SetTimeScale_float orig, GameManager self, float newTimeScale)
		{
			if (ReflectionHelper.GetField<GameManager, int>(self, "timeSlowedCount") > 1)
				newTimeScale = Math.Min(newTimeScale, TimeController.GenericTimeScale);

			// Log near-zero writes with their call stack: with the Min clamp
			// above, one can leave GenericTimeScale stuck at 0.
			if (newTimeScale <= 0.01f)
				LogZeroWrite(newTimeScale);

			TimeController.GenericTimeScale = (newTimeScale <= 0.01f ? 0f : newTimeScale) * this.timeScale;
		}

		private static System.IO.StreamWriter _zeroLog;
		private static void LogZeroWrite(float value)
		{
			try
			{
				if (_zeroLog == null)
				{
					string path = System.IO.Path.Combine(
						UnityEngine.Application.persistentDataPath,
						$"HKOracle_timescale_zero_{System.Diagnostics.Process.GetCurrentProcess().Id}.txt");
					_zeroLog = new System.IO.StreamWriter(path, append: true) { AutoFlush = true };
				}
				_zeroLog.WriteLine($"f={Time.frameCount} SetTimeScale({value:0.####})"
					+ $" gts_before={TimeController.GenericTimeScale:0.###}\n{System.Environment.StackTrace}\n");
			}
			catch { }
		}
	}
}
