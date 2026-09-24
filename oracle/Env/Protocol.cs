using System.Collections.Generic;

namespace HKOracle.Env
{
	public class Message
	{
		public string type;
		public string sender;
		public MessageData data;
	}

	public class MessageData
	{
		// Config (Python -> C#, sent during reset)
		public string level;
		public int? frames_per_wait;
		public int? time_scale;

		// Observation (C# -> Python)
		public List<float[]> combat_hitboxes;
		public List<string> combat_kinds;    // parallel: leaf-kind id string per combat hitbox
		public List<string> combat_parents;  // parallel: HealthManager-root name per combat hitbox ("" if none)
		public List<float[]> terrain_hitboxes;
		// Parallel to terrain_hitboxes: "|seg_idx=N" per segment.
		public List<string> terrain_debug;
		// Debug-only: "<src>|<owner>|<fsm_name>|<state_name>" per relevant FSM, src
		// "B" (boss subtree), "E" (active Enemy collider, incl. pooled projectiles)
		// or "A" (active Attack collider: the knight's nail/spells). FsmObserver.
		public List<string> fsm_snapshots;
		public float[] global_state;

		// Reward / done
		public float? reward;
		public bool? done;
		public string info;

		// Raw reward signals (for Python-side reward computation)
		public float? damage_landed;  // % of boss max HP dealt this step
		public int? hits_taken;
		public float? hp_healed;      // HP restored this step (e.g. via focus)

		// Diagnostic: time elapsed during frame skip
		public float? step_game_time;   // scaled (Time.deltaTime)
		public float? step_real_time;   // unscaled (Time.unscaledDeltaTime)

		// Diagnostic probes, every step: the observation's bucket sizes, the kind cache
		// (grows with every collider ever reported in the episode) and the mono heap.
		public ushort? diag_enemy_count;      // HitboxObserver: Enemy rows at this observation
		public ushort? diag_attack_count;     // HitboxObserver: Attack rows at this observation
		public ushort? diag_terrain_count;    // HitboxObserver: Terrain rows at this observation
		public int? diag_kind_cache_size;     // kindCache dict size (stale Unity refs never GC here)
		public float? diag_gc_heap_mb;        // GC.GetTotalMemory(false) in MB — mono heap total

		// Mode (Python -> C#, sent during reset)
		public bool? eval;
		// Decoded for wire compatibility; every reset reloads the scene anyway.
		public bool? force_full;

		// Action (Python -> C#)
		public int[] action_vec;

		// True iff action[2] was overridden by the hard-commit state machine this
		// step (the trainer masks that head's gradient on such steps).
		public bool? action_committed;

		// Reset-only phase telemetry (trailer in BinaryProtocol.Pack). reset_branch:
		// 0=workshop (no wait), 1=natural_end, 2=unknown. The ms / frames arrays are
		// indexed by ResetPhase.Keys.
		public byte? reset_branch;
		public float[] reset_phase_ms;
		public ushort[] reset_phase_frames;
	}

	public static class ResetPhase
	{
		// Phase keys in wire order; must match the trainer's RESET_PHASES.
		public static readonly string[] Keys = new[] {
			"pre_unload", "transition_out", "settle",
			"load_boss_scene", "recreate_reader",
			"init_boss_refs", "obs_final",
		};
		public const int Count = 7;

		public const byte BranchWorkshop    = 0;
		public const byte BranchNaturalEnd  = 1;
		public const byte BranchUnknown     = 2;

		public static int IndexOf(string key)
		{
			for (int i = 0; i < Keys.Length; i++)
				if (Keys[i] == key) return i;
			return -1;
		}
	}
}
