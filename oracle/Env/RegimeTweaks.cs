using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using Mono.Cecil;
using Mono.Cecil.Cil;
using MonoMod.Cil;
using MonoMod.RuntimeDetour;
using UnityEngine;
using CilOpCodes = Mono.Cecil.Cil.OpCodes;

namespace HKOracle.Env
{
	// Regime R2 (docs/frame-order.md), held for the whole episode by construction: the game's own
	// writes are rewritten at their call sites, so no code path can leave the regime.
	//
	// No Rigidbody2D interpolation. With it, transform.position = rb2d.position + v * (Time.time -
	// Time.fixedTime), a per-run residual that boss FSMs read. Every managed call of
	// Rigidbody2D.set_interpolation in the game's assemblies is IL-hooked to pass None: 6 sites, one per
	// method: HeroController's hazard respawn (HeroController.cs:2829, in <HazardRespawn>d__N.MoveNext),
	// HeroPlatformStick.OnCollisionEnter2D / OnCollisionExit2D (HeroPlatformStick.cs:22, :44), and the
	// PlayMaker actions SetInterpolate / SetExtrapolate / SetInterpolateNone (SetInterpolate.cs:30,
	// SetExtrapolate.cs:30, SetInterpolateNone.cs:30; the Knight's "Control Interpolation" FSM runs
	// SetInterpolate 0.5 s after every LEVEL LOADED). HeroPlatformStick:22 and SetInterpolateNone already
	// pass None. The
	// serialized value (the Knight and Grimmchild prefabs are Interpolate) is forced to None on every loaded
	// Rigidbody2D, prefab assets included so Instantiate copies None, at each scene load and SceneReady.
	//
	// ShakePositionV2 without its frame-rate limit: UpdateShaking rate-limits on Time.unscaledTime and draws
	// Random.Range x3 per update (ShakePositionV2.cs:81-96), so the number of shared-RNG draws per camera shake
	// would be wall-clock dependent. Its FpsLimit reads are IL-hooked to read 0, which skips that branch (:82)
	// for every instance, pooled and Instantiated ones included; hksim traps on FpsLimit > 0.
	//
	// Wall-clock time inside the episode runs on frames: RegimeClock.
	//
	// A pin that fails leaves the instance outside R2, so it fails closed: Failure is set and HKOracle does
	// not start the env, so the instance never connects to a trainer or eval (the reason is in the log).
	public static class RegimeTweaks
	{
		private static readonly List<ILHook> _hooks = new List<ILHook>();
		private static MethodInfo _setInterp;

		/// <summary>Why the instance is not in regime R2; null once every pin is installed.</summary>
		public static string Failure { get; private set; } = "RegimeTweaks.Install did not run";

		private static void Log(string m) => HKOracle.Instance.Log("[Regime] " + m);

		internal static void Fail(string why)
		{
			Log("FAILED: " + why);
			Failure = Failure == null ? why : Failure + "; " + why;
		}

		public static void Install()
		{
			Failure = null;
			try
			{
				_setInterp = typeof(Rigidbody2D).GetProperty("interpolation").GetSetMethod();
				PinInterpolationSites();
				PinShakeFpsLimit();
				RegimeClock.Install(TrainingEnv.kStepDeltaTime);
				Hooks.SceneReady += ctx => ForceNoInterpolation("SceneReady");
				UnityEngine.SceneManagement.SceneManager.sceneLoaded += (sc, mode) => ForceNoInterpolation("sceneLoaded " + sc.name);
			}
			catch (Exception e) { Fail("Install threw: " + HKOracle.DescribeException(e)); }
		}

		// The methods of Assembly-CSharp, -firstpass and PlayMaker whose IL calls a method `match` accepts, with
		// their Cecil names. Read from the assemblies on disk with Mono.Cecil; nothing is loaded.
		internal static List<KeyValuePair<MethodBase, string>> ScanCallers(Func<MethodReference, bool> match)
		{
			var managed = Path.GetDirectoryName(typeof(GameManager).Assembly.Location);
			var resolver = new DefaultAssemblyResolver();
			resolver.AddSearchDirectory(managed);
			var found = new List<KeyValuePair<MethodBase, string>>();
			var seen = new HashSet<MethodBase>();
			foreach (var asm in new[] { typeof(GameManager).Assembly, typeof(HutongGames.PlayMaker.Fsm).Assembly, FirstPass() })
			{
				if (asm == null) continue;
				using (var mod = ModuleDefinition.ReadModule(asm.Location, new ReaderParameters { AssemblyResolver = resolver }))
				{
					foreach (TypeDefinition td in mod.GetTypes())
						foreach (MethodDefinition md in td.Methods)
						{
							if (!md.HasBody) continue;
							foreach (Instruction ins in md.Body.Instructions)
							{
								if ((ins.OpCode.Code == Code.Call || ins.OpCode.Code == Code.Callvirt) && ins.Operand is MethodReference mr
									&& match(mr))
								{
									var mb = asm.ManifestModule.ResolveMethod(md.MetadataToken.ToInt32());
									if (mb != null && seen.Add(mb)) found.Add(new KeyValuePair<MethodBase, string>(mb, md.FullName));
									break;
								}
							}
						}
				}
			}
			return found;
		}

		// Every call of Rigidbody2D.set_interpolation in Assembly-CSharp, -firstpass and PlayMaker.
		private static void PinInterpolationSites()
		{
			var targets = new List<MethodBase>();
			var names = new List<string>();
			foreach (var kv in ScanCallers(mr => mr.Name == "set_interpolation" && mr.DeclaringType.FullName == "UnityEngine.Rigidbody2D"))
			{
				targets.Add(kv.Key);
				names.Add(kv.Value);
			}
			// Every method the scan found calls set_interpolation, so each hook must rewrite at least one site.
			if (targets.Count == 0) Fail("the IL scan found no Rigidbody2D.set_interpolation call");
			int n = 0;
			foreach (var mb in targets)
			{
				int inMethod = 0;
				try
				{
					_hooks.Add(new ILHook(mb, il =>
					{
						var c = new ILCursor(il);
						while (c.TryGotoNext(MoveType.Before, i => i.MatchCallOrCallvirt(_setInterp)))
						{
							c.Emit(CilOpCodes.Pop);
							c.Emit(CilOpCodes.Ldc_I4_0);   // RigidbodyInterpolation2D.None
							c.Index++;
							inMethod++;
						}
					}));
				}
				catch (Exception e) { Fail("interpolation pin on " + mb.DeclaringType + "." + mb.Name + ": " + HKOracle.DescribeException(e)); }
				if (inMethod == 0) Fail("interpolation pin rewrote no call in " + mb.DeclaringType + "." + mb.Name);
				n += inMethod;
			}
			Log($"interpolation pinned to None at {n} call sites in {targets.Count} methods: {string.Join("; ", names.ToArray())}");
			GC.Collect();
		}

		private static Assembly FirstPass()
		{
			foreach (var a in AppDomain.CurrentDomain.GetAssemblies())
				if (a.GetName().Name == "Assembly-CSharp-firstpass") return a;
			return null;
		}

		private static void PinShakeFpsLimit()
		{
			var t = typeof(HutongGames.PlayMaker.Actions.ShakePositionV2);
			var m = t.GetMethod("UpdateShaking", BindingFlags.Instance | BindingFlags.NonPublic);
			var fps = t.GetField("FpsLimit");
			var getValue = typeof(HutongGames.PlayMaker.FsmFloat).GetProperty("Value").GetGetMethod();
			int n = 0;
			try
			{
				_hooks.Add(new ILHook(m, il =>
				{
					var c = new ILCursor(il);
					while (c.TryGotoNext(MoveType.After, i => i.MatchLdfld(fps), i => i.MatchCallOrCallvirt(getValue)))
					{
						c.Emit(CilOpCodes.Pop);
						c.Emit(CilOpCodes.Ldc_R4, 0f);
						n++;
					}
				}));
			}
			catch (Exception e) { Fail("ShakePositionV2 FpsLimit pin: " + HKOracle.DescribeException(e)); }
			Log($"ShakePositionV2.UpdateShaking: {n} FpsLimit reads pinned to 0");
			// ShakePositionV2.cs:82 (the test) and :88 (1f / FpsLimit.Value) are the method's two reads.
			if (n != 2) Fail($"ShakePositionV2.UpdateShaking: {n} FpsLimit reads pinned, want 2 (ShakePositionV2.cs:82, :88)");
		}

		private static void ForceNoInterpolation(string when)
		{
			int n = 0;
			try
			{
				foreach (var rb in Resources.FindObjectsOfTypeAll<Rigidbody2D>())
				{
					if (rb == null || rb.interpolation == RigidbodyInterpolation2D.None) continue;
					rb.interpolation = RigidbodyInterpolation2D.None;
					n++;
				}
			}
			catch (Exception e) { Log("interpolation sweep failed: " + HKOracle.DescribeException(e)); }
			if (n > 0) Log($"{when}: interpolation=None on {n} bodies");
		}
	}
}
