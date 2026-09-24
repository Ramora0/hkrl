using System;
using System.Collections.Generic;
using UnityEngine;
using GlobalEnums;
using UnityEngine.SceneManagement;

namespace HKOracle.Game
{
	// Per-reset caches of the observer (recreated with each boss scene): entity kind, clip key,
	// HealthManager and max HP per collider, the previous knight-relative position, and the
	// collider's static membership facts (Facts). Membership itself is not cached: the observer
	// evaluates it on the live world at every observation (HitboxObserver.Classify).
	public class HitboxReader : MonoBehaviour
	{
		// Cached per-collider kind/parent strings + HealthManager ref. Computed
		// once on first classification (cheap parent walk) and reused thereafter,
		// since HK reparents rarely.
		public readonly Dictionary<Collider2D, string> kindCache = new();
		public readonly Dictionary<Collider2D, string> parentCache = new();
		// Nearest-ancestor animator per collider (null = none reachable) and the
		// "entity|clip" vocab string per clip asset. Animator refs are stable
		// per collider; clip keys are stable per clip asset.
		public readonly Dictionary<Collider2D, tk2dSpriteAnimator> animCache = new();
		private readonly Dictionary<tk2dSpriteAnimationClip, string> clipKeyCache = new();
		public readonly Dictionary<Collider2D, HealthManager> hmCache = new();
		// HK's HealthManager has no public maxHp field — max is whatever the
		// prefab serialized into `hp` at OnEnable. We cache the highest hp
		// we've ever seen per HM, populated on first sight (when hp == max in
		// every normal flow) and bumped if a phase refill ever pushes it higher.
		// Lives on the reader so it dies with the scene, same as the other caches.
		public readonly Dictionary<HealthManager, int> hmMaxHpCache = new();

		// Previous knight-relative center per combat collider, stamped with the
		// observation tick it was written on. Differencing across consecutive
		// ticks yields per-step relative displacement (closing rate).
		// This has to happen here rather than downstream: no per-row instance
		// identity crosses the wire, so nothing on the Python side can match a
		// row to the same collider one step later.
		// The tick stamp guards pooled colliders that deactivate and come back:
		// a gap emits 0 instead of a displacement spanning many steps.
		public readonly Dictionary<Collider2D, (Vector2 rel, long tick)> prevRelCache = new();
		public long MotionTick = 0;

		// The components a collider's own GameObject carries that decide its bucket and its damage:
		// the damages_hero FSM (HeroBox.cs:43-45) or else the DamageHero (HeroBox.cs:58), and the
		// Attack markers. Components are not added or removed during a fight.
		public sealed class Facts
		{
			public bool Supported;          // Box / Polygon / Edge / Circle
			public PlayMakerFSM DamagesHeroFsm;
			public DamageHero DamageHero;
			public bool Attack;
			public bool DamageSource => DamagesHeroFsm != null || DamageHero != null;
		}
		private readonly Dictionary<Collider2D, Facts> factsCache = new();

		public Facts FactsOf(Collider2D col)
		{
			if (factsCache.TryGetValue(col, out var f)) return f;
			var go = col.gameObject;
			f = new Facts { Supported = col is BoxCollider2D or PolygonCollider2D or EdgeCollider2D or CircleCollider2D };
			if (f.Supported)
			{
				f.DamagesHeroFsm = FSMUtility.LocateFSM(go, "damages_hero");
				f.DamageHero = go.GetComponent<DamageHero>();
				f.Attack = go.GetComponent<DamageEnemies>() || go.LocateMyFSM("damages_enemy")
					|| go.name == "Damager" && go.LocateMyFSM("Damage");
			}
			factsCache[col] = f;
			return f;
		}

		/// <summary>
		/// Visual-entity identity for a combat collider: the stripped name of the
		/// nearest ancestor (including self) carrying a tk2dSpriteAnimator — i.e.
		/// the sprite a player actually sees ("Hornet Boss 1", "False Knight",
		/// a needle, a nail slash). Falls back to the collider's own stripped
		/// name when no animator is reachable. The animator found here is cached
		/// per collider and reused by GetClipKey for the animation channel.
		/// </summary>
		public string GetKind(Collider2D col)
		{
			if (col == null) return "unknown";
			if (kindCache.TryGetValue(col, out string cached)) return cached;

			string result = ClassifyEntity(col);
			kindCache[col] = result;
			return result;
		}

		/// <summary>
		/// Animation channel: "entity|clip" key for the collider's cached
		/// animator plus normalized phase within the clip (CurrentFrame /
		/// frames.Length, so a finished Once clip reads 1.0). "none" / 0 when
		/// no animator or no clip. The key is entity-qualified on purpose:
		/// Hornet's antic and FK's antic are unrelated vocab rows, matching
		/// what a player sees rather than sharing structure by clip name.
		/// </summary>
		public string GetClipKey(Collider2D col, out float phase)
		{
			phase = 0f;
			if (col == null) return "none";
			if (!animCache.TryGetValue(col, out tk2dSpriteAnimator anim))
			{
				GetKind(col);  // populates animCache
				animCache.TryGetValue(col, out anim);
			}
			if (anim == null) return "none";
			var clip = anim.CurrentClip;
			if (clip == null) return "none";
			int len = clip.frames != null ? clip.frames.Length : 0;
			if (len > 0)
				phase = Mathf.Clamp01((float)anim.CurrentFrame / len);
			// Clip assets belong to one creature's animation library in HK, so
			// caching the qualified key per clip object is safe.
			if (!clipKeyCache.TryGetValue(clip, out string key))
			{
				key = Strip(anim.gameObject.name) + "|" + clip.name;
				clipKeyCache[clip] = key;
			}
			return key;
		}

		/// <summary>
		/// Walk parents to the nearest HealthManager and return its stripped
		/// root name (e.g. "Mega Moss Charger"). Empty string if none reachable
		/// (detached projectiles, knight-owned colliders, terrain). Used as the
		/// "parent identity" channel in the factored kind embedding: leaf kind
		/// pools across bosses, parent name specializes per boss.
		/// </summary>
		public string GetParentKind(Collider2D col)
		{
			if (col == null) return "";
			if (parentCache.TryGetValue(col, out string cached)) return cached;

			ClassifyParent(col, out string name, out HealthManager hm);
			parentCache[col] = name;
			hmCache[col] = hm;  // may be null
			return name;
		}

		/// <summary>Returns the cached HealthManager (or null) for a collider.
		/// Populated by GetParentKind on first lookup.</summary>
		public HealthManager GetParentHm(Collider2D col)
		{
			if (col == null) return null;
			if (hmCache.TryGetValue(col, out HealthManager cached)) return cached;
			// Force population.
			GetParentKind(col);
			return hmCache.TryGetValue(col, out cached) ? cached : null;
		}

		/// <summary>Return the observed max HP for an HM: max of (cached, current).
		/// First sight populates the cache; phase refills (current > cached) bump it.</summary>
		public int ObserveMaxHp(HealthManager hm)
		{
			if (hm == null) return 0;
			int cur = hm.hp;
			if (hmMaxHpCache.TryGetValue(hm, out int seen))
			{
				if (cur > seen) { hmMaxHpCache[hm] = cur; return cur; }
				return seen;
			}
			hmMaxHpCache[hm] = cur;
			return cur;
		}

		private void ClassifyParent(Collider2D col, out string name, out HealthManager hm)
		{
			Transform t = col.transform;
			int depth = 0;
			while (t != null && depth < 8)
			{
				var found = t.GetComponent<HealthManager>();
				if (found != null)
				{
					name = Strip(t.gameObject.name);
					hm = found;
					return;
				}
				t = t.parent;
				depth++;
			}
			name = "";
			hm = null;
		}

		private string ClassifyEntity(Collider2D col)
		{
			Transform t = col.transform;
			int depth = 0;
			while (t != null && depth < 8)
			{
				var anim = t.GetComponent<tk2dSpriteAnimator>();
				if (anim != null)
				{
					animCache[col] = anim;
					return Strip(t.gameObject.name);
				}
				t = t.parent;
				depth++;
			}

			animCache[col] = null;
			string n = Strip(col.gameObject.name);
			return string.IsNullOrEmpty(n) ? "unknown" : n;
		}

		private static string Strip(string name)
		{
			if (string.IsNullOrEmpty(name)) return "";
			int i = name.IndexOf("(Clone)");
			if (i >= 0) name = name.Substring(0, i);
			return name.Trim();
		}
	}

	// The observation's membership is one predicate evaluated on the live world at observation time
	// (Classify), not a set of colliders registered as they appear: every Collider2D in a loaded scene
	// (DontDestroyOnLoad included) whose object is active is classified by the first matching rule.
	//   Enemy   its GameObject carries a damages_hero FSM or a DamageHero -- the same-object test
	//           HeroBox.CheckForDamage applies (HeroBox.cs:43, :58) -- and its layer collides with the
	//           HeroBox's (Physics2D layer matrix). A disabled collider is an armed row: a hazard
	//           telegraphing before it hurts (NKG's spikes in Spike Ready).
	//   Terrain layer 8 (PhysLayers.TERRAIN), not a trigger, enabled.
	//   Knight  on the hero's own GameObject, not a trigger, enabled.
	//   Attack  DamageEnemies, a damages_enemy FSM, or a "Damager" with a "Damage" FSM, enabled.
	// Only Box/Polygon/Edge/Circle colliders are classified. hksim: sim/fsm/runtime/observer.c.
	public class HitboxObserver
	{
		private HitboxReader _reader;
		private HeroBox _heroBox;

		public void Load()
		{
			Unload();
			UnityEngine.SceneManagement.SceneManager.activeSceneChanged += CreateReader;
			CreateReader();
		}

		public void Unload()
		{
			UnityEngine.SceneManagement.SceneManager.activeSceneChanged -= CreateReader;
			DestroyReader();
		}

		/// <summary>Force-recreate the per-reset caches for the current scene
		/// (workaround for missed activeSceneChanged events under multi-instance load).</summary>
		public void RecreateReader() => CreateReader();

		private void CreateReader(Scene current, Scene next) => CreateReader();

		private void CreateReader()
		{
			DestroyReader();
			if (GameManager.instance.IsGameplayScene())
				_reader = new GameObject("HKOracle.HitboxReader").AddComponent<HitboxReader>();
		}

		private void DestroyReader()
		{
			if (_reader != null)
			{
				UnityEngine.Object.Destroy(_reader.gameObject);
				_reader = null;
			}
		}

		public sealed class Buckets
		{
			public readonly List<Collider2D> Knight = new();
			public readonly List<Collider2D> Enemy = new();   // live and armed rows
			public readonly List<Collider2D> Attack = new();
			public readonly List<Collider2D> Terrain = new();
		}
		private Buckets _last = new Buckets();
		/// <summary>The buckets of the last Classify (the observation just built).</summary>
		public Buckets Last => _last;

		private int HeroBoxLayer()
		{
			var hc = HeroController.instance;
			if (_heroBox == null && hc != null) _heroBox = hc.GetComponentInChildren<HeroBox>(true);
			return _heroBox != null ? _heroBox.gameObject.layer : (int)PhysLayers.HERO_BOX;
		}

		/// <summary>The buckets now, in Resources.FindObjectsOfTypeAll order within each bucket.</summary>
		public Buckets Classify()
		{
			var b = new Buckets();
			if (_reader == null) return _last = b;
			var heroGo = HeroController.instance != null ? HeroController.instance.gameObject : null;
			int heroBox = HeroBoxLayer();
			foreach (var col in Resources.FindObjectsOfTypeAll<Collider2D>())
			{
				if (col == null) continue;
				var go = col.gameObject;
				if (!go.scene.IsValid() || !go.activeInHierarchy) continue;   // prefab assets; inactive objects
				var f = _reader.FactsOf(col);
				if (!f.Supported) continue;
				if (f.DamageSource && !Physics2D.GetIgnoreLayerCollision(go.layer, heroBox)) { b.Enemy.Add(col); continue; }
				if (!col.enabled) continue;
				if (go.layer == (int)PhysLayers.TERRAIN && !col.isTrigger) b.Terrain.Add(col);
				else if (go == heroGo && !col.isTrigger) b.Knight.Add(col);
				else if (f.Attack) b.Attack.Add(col);
			}
			return _last = b;
		}

		/// <summary>An Enemy collider hurts the knight on contact now: its hit gets past the source-dependent gates
		/// of the damage path. HeroBox.CheckForDamage skips a DamageHero that is a shadowDashHazard while the
		/// knight shadow dashes (HeroBox.cs:59). HeroController.TakeDamage returns at damage &lt;= 0
		/// (HeroController.cs:1829) and, for hazardType 1, in HAZARD_ONLY damage mode, while shadow dashing, or
		/// while parryInvulnTimer &gt; 0 (HeroController.cs:1847). The damages_hero FSM's damageDealt and
		/// hazardType win over a DamageHero on the same object (HeroBox.cs:43-57). CanTakeDamage
		/// (HeroController.cs:1845) is the same for every row and is not part of the feature.</summary>
		public bool GivesDamage(Collider2D col)
		{
			if (!col.enabled) return false;
			var f = _reader.FactsOf(col);
			var hc = HeroController.instance;
			bool dashing = hc != null && hc.cState.shadowDashing;
			int damage, hazardType;
			if (f.DamagesHeroFsm != null)
			{
				damage = FSMUtility.GetInt(f.DamagesHeroFsm, "damageDealt");      // HeroBox.cs:46
				hazardType = FSMUtility.GetInt(f.DamagesHeroFsm, "hazardType");   // HeroBox.cs:47
			}
			else
			{
				if (f.DamageHero.shadowDashHazard && dashing) return false;       // HeroBox.cs:59
				damage = f.DamageHero.damageDealt;                                // HeroBox.cs:61
				hazardType = f.DamageHero.hazardType;                             // HeroBox.cs:62
			}
			if (damage <= 0) return false;                                        // HeroController.cs:1829
			bool immune1 = hc != null && (hc.damageMode == DamageMode.HAZARD_ONLY || dashing || hc.parryInvulnTimer > 0f);
			return !(hazardType == 1 && immune1);                                 // HeroController.cs:1847
		}

		public struct CacheSizes
		{
			public int EnemyCount;
			public int AttackCount;
			public int TerrainCount;
			public int KindCacheCount;
		}

		/// <summary>Bucket sizes of the last observation and the kind cache size (diag block).</summary>
		public CacheSizes GetCacheSizes()
		{
			return new CacheSizes
			{
				EnemyCount = _last.Enemy.Count, AttackCount = _last.Attack.Count, TerrainCount = _last.Terrain.Count,
				KindCacheCount = _reader != null ? _reader.kindCache.Count : 0,
			};
		}

		public struct SplitObservation
		{
			public List<float[]> CombatHitboxes;
			public List<string> CombatKinds;
			public List<string> CombatParents;
			public List<float[]> TerrainHitboxes;
			// Parallel to TerrainHitboxes: "|seg_idx=N" per segment (N counts within its collider).
			public List<string> TerrainDebug;
			public float KnightWidth;
			public float KnightHeight;
		}

		/// <summary>
		/// Decompose a terrain collider into a flat list of line segments in
		/// knight-relative world space, emitting one 8-float feature row per
		/// segment:
		///   [mx, my, hdx, hdy, npx, npy, dist, is_trigger]
		/// mx,my     — segment midpoint (knight frame).
		/// hdx,hdy   — half-vector from midpoint to one endpoint, canonicalized
		///             to hdx >= 0 (hdy >= 0 on vertical segments). Length and
		///             orientation recoverable as (2·|hd|, atan2(hdy, hdx)).
		/// npx,npy   — closest point on the *segment* (clamped, not infinite line)
		///             to the knight at origin. Lets the encoder read "how far
		///             above/below/left/right is the nearest surface" as a
		///             single linear readout.
		/// dist      — L2 norm of (npx, npy). Redundant with (npx, npy) in
		///             principle, but sqrt(x² + y²) is multi-layer to approximate
		///             from raw inputs, so pre-computing it means the attention
		///             key-space can gate on distance as a single linear readout
		///             from step 1 of training instead of waiting for phi to
		///             learn L2 norm.
		/// is_trigger — 0/1 passthrough, not normalized.
		/// BoxCollider2D → 4 edges (respecting rotation via TransformPoint).
		/// EdgeCollider2D → one segment per consecutive point pair.
		/// PolygonCollider2D → segments around each path (closed).
		/// CircleCollider2D → 12-gon approximation.
		/// </summary>
		private static void EmitTerrainSegments(
			Collider2D col, Vector3 knightPos,
			List<float[]> outFeatures, List<string> outDebug)
		{
			if (col == null) return;
			var pairs = new List<(Vector2 a, Vector2 b)>();
			Transform tr = col.transform;

			if (col is EdgeCollider2D ec)
			{
				var pts = ec.points;
				for (int i = 0; i + 1 < pts.Length; i++)
				{
					Vector2 p0 = tr.TransformPoint(new Vector3(pts[i].x + ec.offset.x, pts[i].y + ec.offset.y, 0));
					Vector2 p1 = tr.TransformPoint(new Vector3(pts[i + 1].x + ec.offset.x, pts[i + 1].y + ec.offset.y, 0));
					pairs.Add((p0, p1));
				}
			}
			else if (col is PolygonCollider2D pc)
			{
				for (int pi = 0; pi < pc.pathCount; pi++)
				{
					var pts = pc.GetPath(pi);
					if (pts == null || pts.Length == 0) continue;
					for (int i = 0; i < pts.Length; i++)
					{
						var p0 = pts[i];
						var p1 = pts[(i + 1) % pts.Length];
						Vector2 a = tr.TransformPoint(new Vector3(p0.x + pc.offset.x, p0.y + pc.offset.y, 0));
						Vector2 b = tr.TransformPoint(new Vector3(p1.x + pc.offset.x, p1.y + pc.offset.y, 0));
						pairs.Add((a, b));
					}
				}
			}
			else if (col is BoxCollider2D bc)
			{
				Vector2 half = bc.size * 0.5f;
				Vector2 o = bc.offset;
				Vector2 bl = tr.TransformPoint(new Vector3(o.x - half.x, o.y - half.y, 0));
				Vector2 br = tr.TransformPoint(new Vector3(o.x + half.x, o.y - half.y, 0));
				Vector2 tl = tr.TransformPoint(new Vector3(o.x - half.x, o.y + half.y, 0));
				Vector2 trc = tr.TransformPoint(new Vector3(o.x + half.x, o.y + half.y, 0));
				pairs.Add((bl, br));  // bottom
				pairs.Add((br, trc)); // right
				pairs.Add((trc, tl)); // top
				pairs.Add((tl, bl));  // left
			}
			else if (col is CircleCollider2D cc)
			{
				// 12-gon approximation. lossyScale captures non-uniform parent
				// scales; exact for axis-aligned scaling.
				int N = 12;
				float r = cc.radius;
				var prev = Vector2.zero;
				for (int i = 0; i <= N; i++)
				{
					float ang = 2f * Mathf.PI * i / N;
					Vector2 p = tr.TransformPoint(new Vector3(
						cc.offset.x + Mathf.Cos(ang) * r,
						cc.offset.y + Mathf.Sin(ang) * r, 0));
					if (i > 0) pairs.Add((prev, p));
					prev = p;
				}
			}
			else return;

			float isTrigger = col.isTrigger ? 1f : 0f;
			int segIdx = 0;
			foreach (var (aw, bw) in pairs)
			{
				float ax = aw.x - knightPos.x, ay = aw.y - knightPos.y;
				float bx = bw.x - knightPos.x, by = bw.y - knightPos.y;
				float mx = 0.5f * (ax + bx);
				float my = 0.5f * (ay + by);
				float hdx = 0.5f * (bx - ax);
				float hdy = 0.5f * (by - ay);
				// Canonicalize direction: hdx >= 0, tie-break to hdy >= 0 on
				// vertical segments. Matches the parameterization contract so
				// the running normalizer sees a consistent sign across frames.
				if (hdx < 0f || (hdx == 0f && hdy < 0f))
				{
					hdx = -hdx;
					hdy = -hdy;
				}
				// Closest point on the SEGMENT (not infinite line) to origin.
				// t = proj(origin - a, b - a) / |b - a|^2, clamped to [0, 1].
				float dxs = bx - ax, dys = by - ay;
				float denom = dxs * dxs + dys * dys;
				float t = 0f;
				if (denom > 1e-12f)
				{
					t = (-ax * dxs + -ay * dys) / denom;
					if (t < 0f) t = 0f;
					else if (t > 1f) t = 1f;
				}
				float npx = ax + t * dxs;
				float npy = ay + t * dys;
				float dist = Mathf.Sqrt(npx * npx + npy * npy);

				outFeatures.Add(new float[] { mx, my, hdx, hdy, npx, npy, dist, isTrigger });
				outDebug.Add("|seg_idx=" + segIdx);
				segIdx++;
			}
		}

		/// <summary>
		/// Extract hitbox features split by type.
		/// Combat (Enemy + Attack): 14 floats per hitbox —
		///   [rel_x, rel_y, width, height, vel_x, vel_y, is_trigger,
		///    gives_damage, takes_damage, is_target, is_invincible,
		///    hp_raw, hp_max_raw, anim_phase]
		///   Column order matters: the Python normalizer z-scores a leading
		///   prefix of continuous columns (combat_normalized_dims), passes the
		///   binary flags through raw, log1p-compresses the hp pair, and takes
		///   anim_phase raw (already bounded). Keep that grouping.
		///   vel_x, vel_y  = knight-relative displacement since the previous
		///                   observation (0 on first sight or after a gap).
		///   gives_damage = the collider hurts the knight on contact now (Enemy bucket, GivesDamage).
		///   takes_damage = a HealthManager is reachable from this collider.
		///   is_target    = that HealthManager is in the supplied bossHms set.
		///   is_invincible = that HealthManager is currently untouchable.
		///   hp_raw       = current HP of the reached HealthManager (0 if none).
		///   hp_max_raw   = observed max HP (cached on first sight, bumped on refills).
		///   anim_phase   = normalized position in the owner's current animation
		///                  clip, [0,1] (0 = no animator/clip). See GetClipKey.
		/// Both hp_raw and hp_max_raw are emitted RAW; the Python side log1p-compresses
		/// them so the network sees a sane magnitude while preserving high resolution
		/// in the "1-2 nail hits from death" regime.
		/// CombatKinds / CombatParents: parallel string lists — visual-entity
		/// identity and "entity|clip" animation key respectively (the player-
		/// visible pose channel: what creature, which animation, how far in).
		/// Terrain: 8 floats per SEGMENT — see EmitTerrainSegments.
		///   Each terrain collider is decomposed into one or more line segments
		///   (boxes → 4 edges, edge colliders → their polyline, polygons →
		///   closed paths, circles → 12-gon). Colliders with usedByComposite
		///   are skipped — they're absorbed into a CompositeCollider2D and
		///   don't physically collide on their own.
		/// Knight: bounds only (width, height), folded into global state.
		/// </summary>
		// col.bounds is empty while a collider is disabled, so an armed row's box
		// comes from the shape: the same formula as hksim's col_shape_bounds
		// (sim/fsm/runtime/physics.c), so both sides report the same numbers.
		private static Bounds ShapeBounds(Collider2D col)
		{
			var tr = col.transform;
			Vector3 p = tr.position, sc = tr.lossyScale;
			float ang = tr.eulerAngles.z * Mathf.Deg2Rad;
			float cs = Mathf.Cos(ang), sn = Mathf.Sin(ang);
			float minX = float.PositiveInfinity, minY = float.PositiveInfinity;
			float maxX = float.NegativeInfinity, maxY = float.NegativeInfinity;
			void Add(float x, float y)
			{
				float lx = x * sc.x, ly = y * sc.y;
				float wx = p.x + lx * cs - ly * sn, wy = p.y + lx * sn + ly * cs;
				if (wx < minX) minX = wx;
				if (wx > maxX) maxX = wx;
				if (wy < minY) minY = wy;
				if (wy > maxY) maxY = wy;
			}
			Vector2 off = col.offset;
			if (col is BoxCollider2D box)
			{
				float hx = box.size.x * 0.5f, hy = box.size.y * 0.5f;
				Add(off.x - hx, off.y - hy); Add(off.x + hx, off.y - hy);
				Add(off.x + hx, off.y + hy); Add(off.x - hx, off.y + hy);
			}
			else if (col is CircleCollider2D circ)
			{
				float r = circ.radius * Mathf.Max(Mathf.Abs(sc.x), Mathf.Abs(sc.y));
				float cx = p.x + (off.x * sc.x) * cs - (off.y * sc.y) * sn;
				float cy = p.y + (off.x * sc.x) * sn + (off.y * sc.y) * cs;
				return new Bounds(new Vector3(cx, cy, p.z), new Vector3(2f * r, 2f * r, 0f));
			}
			else if (col is PolygonCollider2D poly)
			{
				for (int k = 0; k < poly.pathCount; k++)
					foreach (var q in poly.GetPath(k)) Add(q.x + off.x, q.y + off.y);
			}
			else if (col is EdgeCollider2D edge)
			{
				foreach (var q in edge.points) Add(q.x + off.x, q.y + off.y);
			}
			if (minX > maxX) return new Bounds(p, Vector3.zero);
			return new Bounds(new Vector3((minX + maxX) * 0.5f, (minY + maxY) * 0.5f, p.z),
			                  new Vector3(maxX - minX, maxY - minY, 0f));
		}

		public SplitObservation GetSplitFeatures(System.Collections.Generic.HashSet<HealthManager> bossHms = null)
		{
			var b = Classify();
			var reader = _reader;
			// Advance the motion clock once per observation. Combat rows below
			// difference against the previous tick's cached relative position.
			if (reader != null) reader.MotionTick++;
			var knightPos = HeroController.instance.transform.position;
			var o = new SplitObservation
			{
				CombatHitboxes = new List<float[]>(), CombatKinds = new List<string>(), CombatParents = new List<string>(),
				TerrainHitboxes = new List<float[]>(), TerrainDebug = new List<string>(),
			};

			foreach (var col in b.Knight)
			{
				var kb = col.bounds;
				o.KnightWidth = kb.size.x;
				o.KnightHeight = kb.size.y;
			}
			foreach (var col in b.Enemy) CombatRow(o, reader, col, true, knightPos, bossHms);
			foreach (var col in b.Attack) CombatRow(o, reader, col, false, knightPos, bossHms);
			foreach (var col in b.Terrain)
			{
				// Composite-absorbed colliders don't collide independently;
				// the parent CompositeCollider2D owns the collision, and we
				// either pick it up as a PolygonCollider2D path list or miss
				// it entirely. Emitting the absorbed child would double-count
				// + introduce physics ghosts.
				bool absorbed = false;
				try { absorbed = col.usedByComposite; } catch { }
				if (absorbed) continue;
				EmitTerrainSegments(col, knightPos, o.TerrainHitboxes, o.TerrainDebug);
			}
			return o;
		}

		private void CombatRow(SplitObservation o, HitboxReader reader, Collider2D col, bool enemy, Vector3 knightPos,
			HashSet<HealthManager> bossHms)
		{
			// An armed row (a disabled Enemy collider) has no col.bounds: its box comes from the shape.
			bool enabledNow = col.isActiveAndEnabled;
			var bounds = enabledNow ? col.bounds : ShapeBounds(col);
			float relX = bounds.center.x - knightPos.x;
			float relY = bounds.center.y - knightPos.y;
			float w = bounds.size.x;
			float h = bounds.size.y;
			float isTrigger = col.isTrigger ? 1f : 0f;

			HealthManager hm = reader != null ? reader.GetParentHm(col) : null;
			float givesDamage = enemy && GivesDamage(col) ? 1f : 0f;
			float takesDamage = hm != null ? 1f : 0f;
			float isTarget = (hm != null && bossHms != null && bossHms.Contains(hm)) ? 1f : 0f;
			float hpRaw = hm != null ? (float)hm.hp : 0f;
			float hpMaxRaw = hm != null && reader != null ? (float)reader.ObserveMaxHp(hm) : 0f;
			float animPhase = 0f;
			string clipKey = reader != null ? reader.GetClipKey(col, out animPhase) : "none";
			// Whether this target is currently untouchable (post-hit
			// iframes / stagger). Without it a boss mid-stagger is
			// observationally identical to a hittable one, so swings
			// that could not have connected look like swings that
			// missed, injecting noise into the attack reward exactly
			// when the agent has just landed a hit.
			float isInvincible = (hm != null && hm.IsInvincible) ? 1f : 0f;

			// Per-step knight-relative displacement; 0 on first sight
			// or after an inactive gap. See prevRelCache.
			float velX = 0f, velY = 0f;
			if (reader != null)
			{
				if (reader.prevRelCache.TryGetValue(col, out var prev)
					&& prev.tick == reader.MotionTick - 1)
				{
					velX = relX - prev.rel.x;
					velY = relY - prev.rel.y;
				}
				reader.prevRelCache[col] =
					(new Vector2(relX, relY), reader.MotionTick);
			}

			o.CombatHitboxes.Add(new float[] {
				relX, relY, w, h, velX, velY,
				isTrigger, givesDamage, takesDamage, isTarget, isInvincible,
				hpRaw, hpMaxRaw, animPhase
			});
			o.CombatKinds.Add(reader != null ? reader.GetKind(col) : "unknown");
			o.CombatParents.Add(clipKey);
		}
	}
}
